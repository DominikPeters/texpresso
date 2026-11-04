/* MIT License; Copyright (c) 2026 Dominik Peters. See ../../LICENSE. */
#define _POSIX_C_SOURCE 200809L
#include "providers.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#ifndef MIKTEX_LOOKUP_TIMEOUT_MS
#define MIKTEX_LOOKUP_TIMEOUT_MS 15000
#endif
static bool selected, install;
void miktex_select(bool enabled, bool auto_install) { selected = enabled; install = auto_install; }
bool miktex_selected(void) { return selected; }
bool miktex_auto_install(void) { return install; }
static long long milliseconds(void) {
  struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
  return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
/* No shell, no inherited protocol input, and bounded output/time. findtexmf
 * explicitly calls EnableInstaller(False) unless --must-exist was supplied. */
static bool lookup(const char *name, char *output, size_t capacity, bool version) {
  int pipefd[2]; if (pipe(pipefd)) return false;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  posix_spawn_file_actions_addclose(&actions, pipefd[1]);
  char *args[10]; int count = 0;
  args[count++] = "findtexmf";
  if (version) args[count++] = "--version";
  else {
    args[count++] = "--the-name-of-the-game=xetex";
    /* MiKTeX infers .txt as documentation. XeTeX requests Unicode tables
     * such as UnicodeData.txt from the TeX input tree, not the doc tree. */
    const char *extension = strrchr(name, '.');
    if (extension && !strcmp(extension, ".txt")) args[count++] = "--file-type=tex";
    if (install) args[count++] = "--must-exist";
    args[count++] = "--";
    args[count++] = (char *)name;
  }
  args[count] = NULL;
  pid_t child; int error = posix_spawnp(&child, args[0], &actions, NULL, args, environ);
  posix_spawn_file_actions_destroy(&actions); close(pipefd[1]);
  if (error) { close(pipefd[0]); return false; }
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
  size_t used = 0; bool complete = false, overflow = false;
  long long deadline = milliseconds() + (install && !version ? 180000 : MIKTEX_LOOKUP_TIMEOUT_MS);
  while (milliseconds() < deadline) {
    char buffer[4096]; ssize_t n = read(pipefd[0], buffer, sizeof buffer);
    if (n > 0) {
      if (used + (size_t)n >= capacity) { overflow = true; break; }
      memcpy(output + used, buffer, n); used += n;
    } else if (!n) { complete = true; break; }
    else if (errno != EAGAIN && errno != EINTR) break;
    struct pollfd fd = {pipefd[0], POLLIN | POLLHUP, 0}; poll(&fd, 1, 50);
  }
  close(pipefd[0]);
  int status = 0;
  bool reaped = false;
  while (complete && milliseconds() < deadline) {
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) { reaped = true; break; }
    if (result < 0 && errno != EINTR) break;
    struct timespec delay = {0, 10000000}; nanosleep(&delay, NULL);
  }
  if (!reaped) {
    complete = false; kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  }
  output[used] = 0;
  if (!complete || overflow) fprintf(stderr, "[miktex] lookup exceeded time/output limit\n");
  return complete && !overflow && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
bool miktex_available(void) { char version[16384]; return lookup(NULL, version, sizeof version, true) && strstr(version, "MiKTeX"); }
struct cached_path { struct cached_path *next; char *name, *path; };
static struct cached_path *cache;
const char *miktex_file_path(const char *name, FILE *record) {
  if (!name || strchr(name, '\n') || strchr(name, '\r')) return NULL;
  const char *path = NULL;
  for (struct cached_path *p = cache; p; p = p->next)
    if (!strcmp(name, p->name) && access(p->path, R_OK) == 0) { path = p->path; break; }
  if (!path) {
    char output[16384];
    if (lookup(name, output, sizeof output, false)) {
      char *save, *line = strtok_r(output, "\r\n", &save);
      for (; line; line = strtok_r(NULL, "\r\n", &save)) {
        struct stat st;
        if (*line == '/' && !stat(line, &st) && S_ISREG(st.st_mode) && !access(line, R_OK)) {
          struct cached_path *p = calloc(1, sizeof *p);
          if (!p) break;
          p->name = strdup(name); p->path = strdup(line);
          if (!p->name || !p->path) { free(p->name); free(p->path); free(p); break; }
          p->next = cache; cache = p; path = p->path; break;
        }
        fprintf(stderr, "[miktex] %s\n", line);
      }
    }
  }
  /* Record successful dependencies only. A failed optional lookup must not
   * trigger installation on every format validation. Never cache misses. */
  if (record && path) {
    struct stat st;
    if (!stat(path, &st)) fprintf(record, "%s\n%lld:%lld:%lld\n", path,
      (long long)st.st_size, (long long)st.st_mtime, (long long)st.st_ctime);
  }
  return path;
}
bool miktex_check_dependencies(FILE *record) {
  char path[4096]; long long size, modified, changed; bool any = false;
  while (fgets(path, sizeof path, record)) {
    size_t n = strlen(path); if (!n || path[n-1] != '\n') return false; path[n-1] = 0;
    if (fscanf(record, "%lld:%lld:%lld\n", &size, &modified, &changed) != 3) return false;
    struct stat st;
    if (stat(path, &st) || size != st.st_size || modified != st.st_mtime || changed != st.st_ctime) return false;
    any = true;
  }
  return any && !ferror(record);
}
const char *texmf_file_path(const char *name, FILE *record) {
  return selected ? miktex_file_path(name, record) : texlive_file_path(name, record);
}
