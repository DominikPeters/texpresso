/* MIT License; Copyright (c) 2026 Dominik Peters. See ../../LICENSE. */
#define _POSIX_C_SOURCE 200809L
#include "providers.h"
#include "miktex_windows.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#ifndef MIKTEX_LOOKUP_TIMEOUT_MS
#define MIKTEX_LOOKUP_TIMEOUT_MS 15000
#endif
static bool selected, install, windows_bridge;
void miktex_use_windows(bool enabled) { windows_bridge = enabled; }
bool miktex_windows(void) { return windows_bridge; }
const char *miktex_cache_name(void) {
  static char tag[48];
  if (!windows_bridge) return "miktex";
  const char *bin = getenv("TEXPRESSO_MIKTEX_WINDOWS_BIN");
  if (!bin || !*bin) return "miktex-windows";
  uint32_t hash = 2166136261u;
  for (; *bin; bin++) hash = (hash ^ (unsigned char)*bin) * 16777619u;
  snprintf(tag,sizeof tag,"miktex-windows-%08x",(unsigned)hash); return tag;
}
void miktex_select(bool enabled, bool auto_install) { selected = enabled; install = auto_install; }
bool miktex_selected(void) { return selected; }
bool miktex_auto_install(void) { return install; }
static long long milliseconds(void) {
  struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
  return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static bool run_capture(char *const args[], char *output, size_t capacity,
                        bool diagnostics, long timeout) {
  int pipefd[2];
  if (pipe(pipefd)) return false;
  fcntl(pipefd[0], F_SETFD, FD_CLOEXEC); fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, diagnostics ? STDERR_FILENO : pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  posix_spawn_file_actions_addclose(&actions, pipefd[1]);
  pid_t child; int error = posix_spawnp(&child, args[0], &actions, NULL, args, environ);
  posix_spawn_file_actions_destroy(&actions); close(pipefd[1]);
  if (error) { close(pipefd[0]); return false; }
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
  size_t used = 0; bool complete = false, overflow = false;
  long long deadline = milliseconds() + timeout;
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

static char *base64(const unsigned char *bytes, size_t length) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char *result = malloc(4*((length+2)/3)+1); if (!result) return NULL;
  size_t out = 0;
  for (size_t i=0; i<length; i+=3) {
    unsigned value = (unsigned)bytes[i]<<16;
    if (i+1<length) value |= (unsigned)bytes[i+1]<<8;
    if (i+2<length) value |= bytes[i+2];
    result[out++] = alphabet[value>>18]; result[out++] = alphabet[(value>>12)&63];
    result[out++] = i+1<length ? alphabet[(value>>6)&63] : '=';
    result[out++] = i+2<length ? alphabet[value&63] : '=';
  }
  result[out] = 0; return result;
}
/* One persistent Windows helper per Linux process avoids a PowerShell startup
 * for every package lookup. Its dedicated stdin is also the lifetime signal. */
static pid_t windows_child;
static int windows_input = -1, windows_output = -1;
static void close_windows_helper(void) {
  if (windows_input >= 0) close(windows_input);
  if (windows_output >= 0) close(windows_output);
  windows_input = windows_output = -1;
  if (!windows_child) return;
  long long deadline = milliseconds()+5000; int status;
  while (milliseconds()<deadline) {
    pid_t result = waitpid(windows_child,&status,WNOHANG);
    if (result == windows_child || (result<0 && errno!=EINTR)) { windows_child=0; return; }
    struct timespec delay={0,10000000}; nanosleep(&delay,NULL);
  }
  kill(windows_child,SIGKILL);
  while (waitpid(windows_child,&status,0)<0 && errno==EINTR) {}
  windows_child=0;
}
static bool open_windows_helper(void) {
  if (windows_child) return true;
  size_t n=strlen(miktex_windows_script);
  unsigned char *utf16=calloc(n,2); if (!utf16) return false;
  for (size_t i=0;i<n;i++) utf16[2*i]=(unsigned char)miktex_windows_script[i];
  char *encoded=base64(utf16,n*2); free(utf16);
  if (!encoded) return false;
  if (strlen(encoded)>30000) { free(encoded); return false; }
  int input[2],output[2];
  if (socketpair(AF_UNIX,SOCK_STREAM,0,input)) { free(encoded); return false; }
  if (pipe(output)) { close(input[0]); close(input[1]); free(encoded); return false; }
  for (int i=0;i<2;i++) { fcntl(input[i],F_SETFD,FD_CLOEXEC); fcntl(output[i],F_SETFD,FD_CLOEXEC); }
#ifdef SO_NOSIGPIPE
  int enabled=1; setsockopt(input[1],SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof enabled);
#endif
  posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions,input[0],STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions,output[1],STDOUT_FILENO);
  for (int i=0;i<2;i++) { posix_spawn_file_actions_addclose(&actions,input[i]); posix_spawn_file_actions_addclose(&actions,output[i]); }
  char *args[]={"powershell.exe","-NoLogo","-NoProfile","-NonInteractive","-EncodedCommand",encoded,NULL};
  int error=posix_spawnp(&windows_child,args[0],&actions,NULL,args,environ);
  posix_spawn_file_actions_destroy(&actions); free(encoded); close(input[0]); close(output[1]);
  if (error) { windows_child=0; close(input[1]); close(output[0]); return false; }
  windows_input=input[1]; windows_output=output[0];
  fcntl(windows_input,F_SETFL,O_NONBLOCK); fcntl(windows_output,F_SETFL,O_NONBLOCK);
  static bool registered;
  if (!registered) { atexit(close_windows_helper); registered=true; }
  return true;
}
static bool windows_lookup(char **args, char *output, size_t capacity, bool diagnostics, long timeout) {
  const char *bin=getenv("TEXPRESSO_MIKTEX_WINDOWS_BIN"); if (!bin) bin="";
  const char *tool=diagnostics ? "miktex.exe" : "findtexmf.exe";
  size_t length=strlen(bin)+1+strlen(tool);
  for (int i=1;args[i];i++) length+=strlen(args[i])+1;
  if (length>8192) return false;
  unsigned char *data=malloc(length); if (!data) return false;
  unsigned char *at=data;
  memcpy(at,bin,strlen(bin)); at+=strlen(bin); *at++=0;
  memcpy(at,tool,strlen(tool)); at+=strlen(tool);
  for (int i=1;args[i];i++) { *at++=0; memcpy(at,args[i],strlen(args[i])); at+=strlen(args[i]); }
  char *encoded=base64(data,length); free(data); if (!encoded) return false;
  char request[12000]; snprintf(request,sizeof request,"%ld:%s\n",timeout,encoded); free(encoded);
  if (!open_windows_helper()) return false;
  long long deadline=milliseconds()+timeout+15000;
  size_t sent=0, size=strlen(request), used=0;
  while (sent<size && milliseconds()<deadline) {
#ifdef MSG_NOSIGNAL
    ssize_t n=send(windows_input,request+sent,size-sent,MSG_NOSIGNAL);
#else
    ssize_t n=send(windows_input,request+sent,size-sent,0);
#endif
    if (n>0) sent+=(size_t)n;
    else if (n<0 && errno!=EAGAIN && errno!=EINTR) break;
    struct pollfd fd={windows_input,POLLOUT,0}; poll(&fd,1,50);
  }
  char response[32768]; bool complete=false;
  while (sent==size && used<sizeof response-1 && milliseconds()<deadline) {
    ssize_t n=read(windows_output,response+used,sizeof response-1-used);
    if (n>0) { used+=(size_t)n; if (response[used-1]=='\n') { complete=true; break; } }
    else if (!n || (errno!=EAGAIN && errno!=EINTR)) break;
    struct pollfd fd={windows_output,POLLIN|POLLHUP,0}; poll(&fd,1,50);
  }
  if (!complete) { fprintf(stderr,"[miktex-windows] helper exceeded time/output limit or exited\n"); close_windows_helper(); return false; }
  response[used]=0; char *body; long status=strtol(response,&body,10);
  if (body==response || *body++!=':') { close_windows_helper(); return false; }
  size_t decoded=0; unsigned value=0,bits=0;
  for (;*body && *body!='=' && *body!='\r' && *body!='\n';body++) {
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *digit=strchr(alphabet,*body);
    if (!digit) { close_windows_helper(); return false; }
    value=(value<<6)|(unsigned)(digit-alphabet); bits+=6;
    if (bits>=8) { bits-=8; if (decoded+1>=capacity) { close_windows_helper(); return false; } output[decoded++]=(char)(value>>bits); }
  }
  output[decoded]=0;
  if (diagnostics) { fwrite(output,1,decoded,stderr); output[0]=0; }
  return status==0;
}
/* No shell or protocol stdin is exposed to MiKTeX. findtexmf explicitly
 * disables its installer unless --must-exist was supplied. */
static bool lookup(const char *name, char *output, size_t capacity, int operation) {
  bool version = operation == 1, fontmaps = operation == 2;
  char *args[10]; int count = 0;
  args[count++] = fontmaps ? "miktex" : "findtexmf";
  if (fontmaps) {
    args[count++] = install ? "--enable-installer" : "--disable-installer";
    args[count++] = "fontmaps"; args[count++] = "configure";
  }
  else if (version) args[count++] = "--version";
  else {
    args[count++] = "--the-name-of-the-game=xetex";
    /* MiKTeX infers .txt as documentation. XeTeX requests Unicode tables
     * such as UnicodeData.txt from the TeX input tree, not the doc tree. */
    const char *extension = strrchr(name, '.');
    if (extension && !strcmp(extension, ".txt")) args[count++] = "--file-type=tex";
    else if (extension && !strcmp(extension, ".tec")) args[count++] = "--file-type=misc fonts";
    else if (extension && !strcmp(extension, ".pdf")) args[count++] = "--file-type=graphic/figure";
    if (install) args[count++] = "--must-exist";
    args[count++] = "--";
    args[count++] = (char *)name;
  }
  args[count] = NULL;
  long timeout = install && !version ? 180000 : MIKTEX_LOOKUP_TIMEOUT_MS;
  return windows_bridge ? windows_lookup(args,output,capacity,fontmaps,timeout)
    : run_capture(args,output,capacity,fontmaps,timeout);
}
static bool package_path(char *line, char *converted, size_t capacity) {
  if (!windows_bridge) {
    if (strlen(line)>=capacity) return false;
    strcpy(converted,line); return true;
  }
  size_t n = strlen(line);
  if (n>=2 && line[0]=='"' && line[n-1]=='"') { line[n-1]=0; line++; }
  if (!((line[0] && line[1]==':' && (line[2]=='\\' || line[2]=='/')) || (line[0]=='\\' && line[1]=='\\'))) return false;
  char *command[] = {"wslpath","-u",line,NULL};
  if (!run_capture(command,converted,capacity,false,MIKTEX_LOOKUP_TIMEOUT_MS)) return false;
  converted[strcspn(converted,"\r\n")] = 0;
  return converted[0]=='/';
}
struct cached_path { struct cached_path *next; char *name, *path; };
static struct cached_path *cache;
bool miktex_refresh_fontmaps(void) {
  char output[16384];
  if (!lookup(NULL, output, sizeof output, 2)) return false;
  /* A generated user map can take precedence over a still-existing system
   * map. Re-resolve map names rather than reusing their previous paths. */
  struct cached_path **link = &cache;
  while (*link) {
    struct cached_path *p = *link;
    const char *extension = strrchr(p->name, '.');
    if (extension && !strcmp(extension, ".map")) {
      *link = p->next; free(p->name); free(p->path); free(p);
    } else link = &p->next;
  }
  return true;
}
bool miktex_available(void) { char version[16384]; return lookup(NULL, version, sizeof version, true) && strstr(version, "MiKTeX"); }
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
        char translated[8192];
        if (!package_path(line,translated,sizeof translated)) continue;
        struct stat st;
        line = translated;
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
