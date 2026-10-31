/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <mupdf/fitz/buffer.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <signal.h>
#include "engine.h"
#include "incdvi.h"
#include "mydvi.h"
#include "state.h"
#include "synctex.h"
#include "editor.h"
#include "profile.h"
#include "checkpoint_selection.h"

typedef struct
{
  fileentry_t *entry;
  int position;
} fence_t;

typedef struct
{
  fileentry_t *entry;
  int seen, time;
} trace_entry_t;

typedef struct
{
  int pid, fd;
  int trace_len;
  txp_pass_status pass_status;
  bool starting, startup_failed;
  double handshake_started;
  const char *startup_reason;
  int tokens;
  mark_t snap;
  fileentry_t *checkpoint_entry;
  int checkpoint_offset;
  uint64_t anchor;
} process_t;

enum
{
  MAX_PROCESS = TXP_CHECKPOINT_CAPACITY,
  MAX_ROOT_CHILDREN = 64
};

struct tex_engine
{
  struct txp_engine_class *_class;

  char *name;
  char *engine_path;
  char *inclusion_path;
  bool use_texlive;
  bool stream_mode;

  filesystem_t *fs;
  state_t st;
  log_t *log;

  channel_t *c;
  process_t processes[MAX_PROCESS];
  int process_count;
  struct { pid_t pid; bool closed; } root_children[MAX_ROOT_CHILDREN];
  int root_child_count;
  pid_t startup_cleanup_pid;

  trace_entry_t *trace;
  int trace_cap;
  fence_t fences[16];
  int fence_pos;
  mark_t restart;

  incdvi_t *dvi;
  synctex_t *stex;

  struct {
    int trace_len, offset, flush;
  } rollback;

  struct {
    bool active;
    query_t query;
    char path[1024];
  } deferred;

  bool aux_dirty;
  bool finishing;
  int completed_page_count;
  int last_tokens;
  fileentry_t *fork_entry;
  int fork_offset;
  double fork_started;
  int fork_parent_pid;
  bool fork_is_anchor;
  uint64_t anchor_serial;
  struct { fileentry_t *entry; int offset, status; bool retained_fence; } prepare;
};

// Backtrackable process state & VFS representation

static process_t *get_process(struct tex_engine *t)
{
  if (t->process_count == 0)
    mabort();
  return &t->processes[t->process_count-1];
}

// Useful routines

static char *last_index(char *path, char needle)
{
  char *result = path;
  while (*path)
  {
    if (*path == needle)
      result = path + 1;
    path += 1;
  }
  return result;
}

// tex_engine implementation

TXP_ENGINE_DEF_CLASS;
#define SELF struct tex_engine *self = (struct tex_engine*)_self

static void answer_query(fz_context *ctx, struct tex_engine *self, query_t *q);

// Only exec_xelatex's direct children belong to this reaper. Snapshot children
// are reaped by their compiler parents; provider popen/system statuses are not
// ours. Wait only after closing a root's channel, so a reaped/reused PID can
// never still be targeted by close_process.
static void reap_root_children(struct tex_engine *self, bool dispose)
{
  int retained = 0;
  for (int i = 0; i < self->root_child_count; ++i)
  {
    pid_t pid = self->root_children[i].pid;
    if (!dispose && !self->root_children[i].closed)
    {
      self->root_children[retained++] = self->root_children[i];
      continue;
    }
    pid_t result;
    do result = waitpid(pid, NULL, WNOHANG);
    while (result < 0 && errno == EINTR);
    if (dispose && result == 0)
    {
      kill(pid, SIGKILL);
      do result = waitpid(pid, NULL, 0);
      while (result < 0 && errno == EINTR);
    }
    if (result == 0 || (result < 0 && errno != ECHILD))
      self->root_children[retained++] = self->root_children[i];
  }
  self->root_child_count = retained;
}


// Launching processes

static pid_t exec_xelatex_generic(char **args, int *fd)
{
  int sockets[2];
  if (socketpair(PF_UNIX, SOCK_STREAM, 0, sockets) != 0)
  {
    perror("exec_xelatex socketpair");
    mabort();
  }

  char buf[30];
  snprintf(buf, 30, "%d", sockets[1]);
  setenv("TEXPRESSO_FD", buf, 1);

#ifdef __APPLE__
  static int env_init = 0;
  if (!env_init)
  {
    env_init = 1;
    setenv("OBJC_DISABLE_INITIALIZE_FORK_SAFETY", "YES", 1);
  }

  pid_t pid = fork();
#else
  pid_t pid = vfork();
#endif

  if (pid == -1)
  {
    perror("exec_xelatex vfork");
    mabort();
  }

  if (pid == 0)
  {
    /* CHILD */
    if (close(sockets[0]) != 0)
      mabort();
    // Redirect stdout to stderr
    dup2(STDERR_FILENO, STDOUT_FILENO);
    execvp(args[0], args);
    _exit(2);
  }

  /* PARENT */
  if (close(sockets[1]) != 0)
    mabort();
  *fd = sockets[0];
  return pid;
}

static pid_t exec_xelatex(char *engine_path, bool use_texlive, const char *filename, int *fd)
{
  char *args[] = {
    engine_path,
    (use_texlive ? "-texlive" : "-tectonic"),
    "-texpresso",
    (char*)filename,
    NULL
  };

  pid_t pid = exec_xelatex_generic(args, fd);
  fprintf(stderr, "[process] launched pid %d (using %s)\n", pid, engine_path);
  return pid;
}

static void prepare_process(fz_context *ctx, struct tex_engine *self, const char *reason)
{
  if (self->process_count == 0)
  {
    reap_root_children(self, false);
    if (self->root_child_count == MAX_ROOT_CHILDREN)
      fz_throw(ctx, FZ_ERROR_GENERIC, "Compiler workers have not exited; restart the preview");
    self->deferred.active = false;
    log_rollback(ctx, self->log, self->restart);
    self->process_count = 1;
    process_t *p = get_process(self);
    double spawn_start = txp_profile_now();
    p->pid = exec_xelatex(self->engine_path, self->use_texlive, self->name, &p->fd);
    if (spawn_start) {
      double now = txp_profile_now();
      fprintf(stderr, "[profile] {\"stage\":\"compiler-spawn\",\"ms\":%.4f,\"nativeAt\":%.4f,\"request\":-1,\"page\":-1,\"bytes\":0,\"pid\":%d,\"reason\":\"%s\"}\n", now-spawn_start, now, p->pid, reason);
    }
    self->root_children[self->root_child_count].pid = p->pid;
    self->root_children[self->root_child_count++].closed = false;
    p->trace_len = 0;
    p->pass_status = TXP_PASS_RUNNING;
    p->tokens = 0;
    p->checkpoint_entry = NULL;
    p->anchor = 0;
    self->fork_started = 0;
    p->starting = true; p->startup_failed = false;
    p->handshake_started = txp_monotonic_ms();
    p->startup_reason = reason;
    channel_handshake_begin(self->c);
  }
}

// Terminating processes

static void close_process(struct tex_engine *self, process_t *p)
{
  p->starting = false;
  if (p->fd != -1)
  {
    for (int i = 0; i < self->root_child_count; ++i)
      if (self->root_children[i].pid == p->pid) self->root_children[i].closed = true;
    if (p->pass_status == TXP_PASS_RUNNING)
    {
      p->pass_status = TXP_PASS_CANCELLED;
      fprintf(stderr, "[pass] cancelled\n");
    }
    kill(p->pid, SIGTERM);
    close(p->fd);
    p->fd = -1;
  }
}

static int startup_timeout_ms(void)
{
  // A cold first format build can take much longer than the usual 160ms.
  // Tests exercise the same deadline without waiting two minutes.
  const char *value = getenv("TEXPRESSO_TEST_STARTUP_TIMEOUT_MS");
  if (value && *value) {
    char *end;
    long timeout = strtol(value, &end, 10);
    if (!*end && timeout >= 50 && timeout <= 120000) return timeout;
  }
  return 120000;
}

bool txp_engine_startup_pending(txp_engine *_self)
{
  if (_self->_class != &_class) return false;
  SELF;
  return self->startup_cleanup_pid || (self->process_count && get_process(self)->starting);
}

void txp_engine_poll_startup(txp_engine *_self)
{
  if (_self->_class != &_class) return;
  SELF;
  if (self->startup_cleanup_pid) {
    reap_root_children(self, false);
    bool retained = false;
    for (int i = 0; i < self->root_child_count; ++i)
      retained |= self->root_children[i].closed && self->root_children[i].pid == self->startup_cleanup_pid;
    if (!retained) self->startup_cleanup_pid = 0;
  }
  if (!self->process_count || !get_process(self)->starting) return;
  process_t *p = get_process(self);
  double now = txp_monotonic_ms();
  bool timeout = now - p->handshake_started >= startup_timeout_ms();
  enum channel_handshake_status result = timeout ? CHANNEL_HANDSHAKE_FAILED : channel_handshake_poll(self->c, p->fd);
  if (result == CHANNEL_HANDSHAKE_PENDING) return;
  p->starting = false;
  if (txp_profile_now())
    fprintf(stderr, "[profile] {\"stage\":\"compiler-handshake\",\"ms\":%.4f,\"nativeAt\":%.4f,\"request\":-1,\"page\":-1,\"bytes\":0,\"pid\":%d,\"reason\":\"%s\",\"outcome\":\"%s\"}\n",
      now-p->handshake_started, now, p->pid, p->startup_reason, result == CHANNEL_HANDSHAKE_READY ? "ready" : "failed");
  if (result == CHANNEL_HANDSHAKE_FAILED) {
    fprintf(stderr, "[process] Compiler startup %s. Check that texpresso-xetex matches this frontend and that the TeX distribution is available.\n",
      timeout ? "timed out waiting for the worker greeting" : "failed: the worker closed its connection or sent an incompatible greeting");
    p->pass_status = TXP_PASS_FAILED; p->startup_failed = true;
    close_process(self, p);
    // A worker stuck before its greeting cannot perform protocol cleanup. Do
    // not let ignored SIGTERM keep a timed-out startup alive indefinitely.
    kill(p->pid, SIGKILL);
    self->startup_cleanup_pid = p->pid;
    reap_root_children(self, false);
  }
}

static void pop_process(fz_context *ctx, struct tex_engine *self)
{
  process_t *p = get_process(self);
  close_process(self, p);
  reap_root_children(self, false);
  channel_reset(self->c);
  self->process_count -= 1;
  mark_t mark =
    self->process_count > 0 ? get_process(self)->snap : self->restart;
  log_rollback(ctx, self->log, mark);
}

static void cancel_convergence(struct tex_engine *self)
{
  // A source edit cancels the finishing pass, not its last complete auxiliary
  // input. Dropping that input here makes root replays forget resolved labels
  // until another idle pass reaches EOF. Keep the copied previous-pass bytes,
  // just as LaTeX keeps .aux files between normal runs; finish_convergence will
  // replace them when a complete pass changes or removes labels.
  self->aux_dirty = false;
  self->finishing = false;
}

static bool read_query(struct tex_engine *self, channel_t *t, query_t *q)
{
  process_t *p = get_process(self);
  bool result = channel_read_query(t, p->fd, q);
  if (!result)
  {
    if (p->pass_status == TXP_PASS_RUNNING)
      p->pass_status = TXP_PASS_FAILED;
    fprintf(stderr, "[pass] %s\n", p->pass_status == TXP_PASS_SUCCEEDED ? "succeeded"
                    : p->pass_status == TXP_PASS_RECOVERED ? "completed with errors" : "failed");
    fprintf(stderr, "[process] terminating process\n");
    if (self->prepare.retained_fence) {
      self->fence_pos = -1;
      self->prepare.retained_fence = false;
    }
    close_process(self, p);
  }
  return result;
}

static void decimate_processes(struct tex_engine *self)
{
  bool keep[MAX_PROCESS] = {0,};

  txp_checkpoint points[MAX_PROCESS];
  int anchor_count = 0;
  for (int i = 0; i < self->process_count; ++i) {
    process_t *p = &self->processes[i];
    points[i] = (txp_checkpoint){ .trace=p->trace_len, .entry=p->checkpoint_entry,
      .offset=p->checkpoint_offset, .anchor=p->anchor, .live=p->fd>=0 };
    anchor_count += p->anchor != 0;
  }
  int reserved_anchors = 0;
  txp_select_checkpoints(points, self->process_count, keep, &reserved_anchors);
  if (txp_profile_now()) {
    int retained = 0, kept_anchors = 0;
    for (int i = 0; i < self->process_count; ++i) {
      retained += keep[i]; kept_anchors += keep[i] && self->processes[i].anchor != 0;
    }
    fprintf(stderr, "[profile] {\"stage\":\"compiler-decimation\",\"ms\":0,\"nativeAt\":%.4f,\"request\":-1,\"page\":-1,\"bytes\":0,\"before\":%d,\"after\":%d,\"taggedAnchors\":%d,\"reservedAnchors\":%d,\"evictedAnchors\":%d}\n",
            txp_profile_now(), self->process_count, retained, kept_anchors, reserved_anchors, anchor_count-kept_anchors);
  }

  int i = 0;
  for (int j = 0; j < self->process_count; j++)
  {
    if (!keep[j])
    {
      close_process(self, &self->processes[j]);
      continue;
    }
    if (i != j)
      self->processes[i] = self->processes[j];
    i++;
  }
  self->process_count = i;

  fprintf(stderr, "after process decimation:\n");
  for  (int i = 0; i < self->process_count; ++i)
  {
    process_t *p = &self->processes[i];
    fprintf(stderr, "- position %d, time %d tokens [pid %d]\n",
            p->trace_len,
            p->trace_len == 0 ? 0 : self->trace[p->trace_len - 1].time,
            p->pid);
  }
}

// Engine class implementation

static void engine_destroy(txp_engine *_self, fz_context *ctx)
{
  SELF;
  while (self->process_count > 0)
    pop_process(ctx, self);
  reap_root_children(self, true);
  incdvi_free(ctx, self->dvi);
  synctex_free(ctx, self->stex);
  fz_free(ctx, self->name);
  fz_free(ctx, self->engine_path);
  fz_free(ctx, self->inclusion_path);
  fz_free(ctx, self);
}

static const char *expand_path(const char **inclusion_path, const char *name, char buffer[1024])
{
  if (!*inclusion_path || !(*inclusion_path)[0])
    return NULL;

  if (name[0] == '/')
    return NULL;

  if (name[0] == '.' && name[1] == '/')
  {
    name += 2;
    while (*name == '/')
      name += 1;
  }

  char *p = buffer;
  const char *i = *inclusion_path;

  while (*i)
  {
    if (p > buffer + 1024) mabort();
    *p = *i;
    p += 1;
    i += 1;
  }
  *inclusion_path = i+1;

  if (p[-1] != '/')
  {
    if (p > buffer + 1024) mabort();
    p[0] = '/';
    p += 1;
  }

  while (*name)
  {
    if (p > buffer + 1024) mabort();
    *p = *name;
    p += 1;
    name += 1;
  }

  if (p > buffer + 1024) mabort();
  *p = '\0';

  return buffer;
}

static void check_fid(file_id fid)
{
  if (fid < 0 || fid >= MAX_FILES)
    mabort();
}

static void record_seen(struct tex_engine *self, fileentry_t *entry, int seen, int time)
{
  process_t *p = get_process(self);

  if (p->trace_len > 0 && self->trace[p->trace_len-1].entry == entry &&
      (self->process_count <= 1 ||
      self->processes[self->process_count - 2].trace_len != p->trace_len))
  {
    self->trace[p->trace_len-1].time = time;
    entry->seen = seen;
    return;
  }

  if (p->trace_len == self->trace_cap)
  {
    int new_cap = self->trace_cap == 0 ? 8 : self->trace_cap * 2;
    fprintf(stderr, "[info] trace has %d entries, growing to %d\n", self->trace_cap, new_cap);
    trace_entry_t *newtr = calloc(sizeof(trace_entry_t), new_cap);
    if (newtr == NULL) abort();
    if (self->trace)
    {
      memcpy(newtr, self->trace, self->trace_cap * sizeof(trace_entry_t));
      free(self->trace);
    }
    self->trace = newtr;
    self->trace_cap = new_cap;
  }

  self->trace[p->trace_len] = (trace_entry_t){
    .entry = entry,
    .seen = entry->seen,
    .time = time,
  };
  entry->seen = seen;
  p->trace_len += 1;
}

static fz_buffer *entry_data(fileentry_t *e)
{
  if (e->saved.data)
    return e->saved.data;
  if (e->edit_data)
    return e->edit_data;
  return e->fs_data;
}

static fz_buffer *output_data(fileentry_t *e)
{
  if (!e)
    return NULL;
  return e->saved.data;
}

static const char *
lookup_path(struct tex_engine *self, const char *path, char buf[1024], struct stat *st)
{
  struct stat st1;
  if (st == NULL)
    st = &st1;

  const char *fs_path = path;
  const char *inclusion_path = self->inclusion_path;

  do {
    if (stat(fs_path, st) != -1)
      break;
  }
  while ((fs_path = expand_path(&inclusion_path, path, buf)));

  return fs_path;
}

static bool need_snapshot(fz_context *ctx, struct tex_engine *self, int time)
{
  // Fences are pending: don't snapshot now
  if (self->fence_pos != -1)
    return 0;

  int process = self->process_count - 1;

  int last_time;

  if (process > 0)
  {
    // There is already some snapshot, stop if no new event has been traced
    if (self->processes[process].trace_len == self->processes[process-1].trace_len)
      return 0;

    last_time = self->trace[self->processes[process-1].trace_len - 1].time;

    // TODO Alternative
    // Checking that some new event happened avoid entering an infinite fork
    // loop when last event is old, but nothing new is registered, therefore
    // causing an infinite loop.
    // A better solution might be to record a fork as an event.
    // This could be done for instance in READ, when trying to fork again.
  }
  else
  {
    #ifdef __APPLE__
    // Workaround for macOS
    // Due to limitations in the implementation of fork on macOS, it is not
    // possible to load system fonts after fork (without exec). This breaks
    // TeXpresso sooner or later, and there is no obvious solution besides
    // implementing XeTeX snapshotting without fork.
    // The second best thing is to hopefully load all system fonts before the
    // first fork.
    // Therefore we delay forking until output started, hopping that all fonts
    // have been specified at this point.
    if (!incdvi_output_started(self->dvi))
      return 0;
    #endif

    // No snapshot, measure time since root process started
    last_time = 0;
  }

  // q->time is XeTeX token progress, not wall-clock milliseconds.
  return time > 500 + last_time;
}

static void answer_query(fz_context *ctx, struct tex_engine *self, query_t *q)
{
  process_t *p = get_process(self);
  answer_t a;
  self->last_tokens = q->time;
  switch (q->tag)
  {
    case Q_OPRD:
    case Q_OPWR:
    {
      check_fid(q->open.fid);
      filecell_t *cell = &self->st.table[q->open.fid];
      if (cell->entry != NULL) mabort();

      fileentry_t *e = NULL;

      char fs_path_buffer[1024];
      const char *fs_path = NULL;

      if (q->tag == Q_OPRD)
      {
        e = filesystem_lookup(self->fs, q->open.path);
        if (self->stream_mode && e && entry_data(e))
        {
          // Stream mode: VFS data available, skip filesystem lookup
        }
        else if (!e || !entry_data(e))
        {
          fs_path = lookup_path(self, q->open.path, fs_path_buffer, NULL);
          if (!fs_path)
          {
            e = filesystem_lookup_or_create(ctx, self->fs, q->open.path);
            if (e->promised && !self->deferred.active)
            {
              // File was promised and is missing: notify the editor and wait
              // for answer
              self->deferred.active = true;
              self->deferred.query = *q;
              strncpy(self->deferred.path, q->open.path,
                      sizeof(self->deferred.path) - 1);
              self->deferred.path[sizeof(self->deferred.path) - 1] = '\0';
              self->deferred.query.open.path = self->deferred.path;
              editor_notify_lookup(q->open.path, strlen(q->open.path), true,
                                   LOOKUP_PROMISED);
            }
            else
            {
              // File is missing: record this observation and mark the lookup
              // as failed.
              log_fileentry(ctx, self->log, e);
              record_seen(self, e, INT_MAX, q->time);
              a.tag = A_PASS;
              editor_notify_lookup(q->open.path, strlen(q->open.path), true,
                                   LOOKUP_FAILED);
              channel_write_answer(self->c, p->fd, &a);
            }
            break;
          }
        }
      }

      if (!e)
        e = filesystem_lookup_or_create(ctx, self->fs, q->open.path);

      log_filecell(ctx, self->log, cell);
      log_fileentry(ctx, self->log, e);
      cell->entry = e;
      if (e->seen < 0)
        record_seen(self, e, 0, q->time);

      enum accesslevel level =
        (q->tag == Q_OPRD) ? FILE_READ : FILE_WRITE;

      if (level == FILE_READ)
      {
        if (e->saved.level < FILE_READ)
        {
          if (!fs_path && !(self->stream_mode && e->edit_data))
            fs_path = lookup_path(self, q->open.path, fs_path_buffer, NULL);
          if (!fs_path)
          {
            if (!e->edit_data)
            {
              if (self->stream_mode)
              {
                log_fileentry(ctx, self->log, e);
                record_seen(self, e, INT_MAX, q->time);
                a.tag = A_PASS;
                editor_notify_lookup(q->open.path, strlen(q->open.path),
                                     q->tag == Q_OPRD, LOOKUP_FAILED);
                channel_write_answer(self->c, p->fd, &a);
                break;
              }
              mabort("path: %s\nmode:%c\n", q->open.path, (q->tag == Q_OPRD) ? 'r' : 'w');
            }
            e->saved.level = FILE_READ;
            e->saved.read_from_filesystem = false;
            memset(&e->fs_stat, 0, sizeof(e->fs_stat));
          }
          else
          {
            if (fs_path == q->open.path)
              fs_path = e->path;
            e->fs_data = fz_read_file(ctx, fs_path);
            e->saved.level = FILE_READ;
            e->saved.read_from_filesystem = e->edit_data == NULL;
            stat(fs_path, &e->fs_stat);
          }
        }
      }
      else
      {
        e->saved.data = fz_new_buffer(ctx, 1024);
        e->saved.level = level;
      }

      if (level == FILE_READ)
      {
        if (0)
          fprintf(stderr, "[info] opening %s\n", q->open.path);
      }
      else
      {
        fprintf(stderr, "[info] writing %s\n", q->open.path);
        if (strcmp(q->open.path, "stdout") == 0)
        {
          if (self->st.stdout.entry != NULL)
          {
            fprintf(stderr, "[error] two stdouts!\n");
            mabort();
          }
          log_filecell(ctx, self->log, &self->st.stdout);
          self->st.stdout.entry = e;
        }
        else
        {
          char *ext = last_index(q->open.path, '.');
          if (0)
            fprintf(stderr, "extension is %s\n", ext);
          if (!ext);
          else if ((strcmp(ext, "xdv") == 0 ||
                    strcmp(ext, "dvi") == 0 ||
                    strcmp(ext, "pdf") == 0))
          {
            if (self->st.document.entry != NULL)
            {
              fprintf(stderr, "[error] two outputs!\n");
              mabort();
            }
            log_filecell(ctx, self->log, &self->st.document);
            self->st.document.entry = e;
            incdvi_reset(self->dvi);
            fprintf(stderr, "[info] this is the output document\n");
          }
          else if ((strcmp(ext, "synctex") == 0))
          {
            if (self->st.synctex.entry != NULL)
            {
              fprintf(stderr, "[error] two synctex!\n");
              mabort();
            }
            log_filecell(ctx, self->log, &self->st.synctex);
            self->st.synctex.entry = e;
            synctex_rollback(ctx, self->stex, 0);
            fprintf(stderr, "[info] this is the synctex\n");
          }
          else if ((strcmp(ext, "log") == 0))
          {
            if (self->st.log.entry != NULL)
            {
              fprintf(stderr, "[error] two log files!\n");
              mabort();
            }
            log_filecell(ctx, self->log, &self->st.log);
            self->st.log.entry = e;
            fprintf(stderr, "[info] this is the log file\n");
          }
        }
      }

      int n = strlen(q->open.path);
      a.open.path_len = n;
      a.tag = A_OPEN;
      editor_notify_lookup(q->open.path, n, q->tag == Q_OPRD, LOOKUP_SUCCESSFUL);
      memmove(channel_get_buffer(self->c, n), q->open.path, n);
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_READ:
    {
      check_fid(q->read.fid);
      fileentry_t *e = self->st.table[q->read.fid].entry;
      if (e == NULL) mabort();
      if (e->saved.level < FILE_READ) mabort();
      fz_buffer *data = entry_data(e);
      if (e->debug_rollback_invalidation > -1)
      {
        if (q->read.pos > e->debug_rollback_invalidation)
          mabort();
        e->debug_rollback_invalidation = -1;
      }
      if (q->read.pos > data->len)
      {
        fprintf(stderr, "read:%d\ndata->len:%d\n", q->read.pos, (int)data->len);
        mabort();
      }
      ssize_t n = q->read.size;
      if (n > data->len - q->read.pos)
        n = data->len - q->read.pos;

      // A cancelled replay keeps its one fence for the next real demand.
      // A later unread edit may shorten/replace that input before the worker
      // reaches it. Discard only that speculative fence if it cannot fire.
      if (self->prepare.retained_fence && self->fence_pos == 0 &&
          self->fences[0].entry == e &&
          (self->fences[0].position < q->read.pos || self->fences[0].position >= data->len))
      {
        fprintf(stderr, "[prepare-edit] dropping cancelled fence outside current input\n");
        self->fence_pos = -1;
        self->prepare.retained_fence = false;
      }
      int fork = 0;
      if (self->fence_pos >= 0 &&
          self->fences[self->fence_pos].entry == e &&
          self->fences[self->fence_pos].position < q->read.pos + n)
      {
        if (n < 0)
          mabort();
        n = self->fences[self->fence_pos].position - q->read.pos;
        // Weird that n can be negative at this point?!
        fork = (n == 0);
        if (n < 0)
          mabort("n:%d fence_pos:%d read_pos:%d\n", (int)n, self->fences[self->fence_pos].position, q->read.pos);
      }
      bool periodic = !fork && need_snapshot(ctx, self, q->time);
      if ((fork || periodic) && txp_profile_now())
        fprintf(stderr, "[profile] {\"stage\":\"compiler-checkpoint\",\"ms\":0,\"nativeAt\":%.4f,\"request\":-1,\"page\":-1,\"bytes\":0,\"tokens\":%d,\"trace\":%d,\"offset\":%d,\"checkpoints\":%d,\"fence\":%s}\n",
                txp_profile_now(), q->time, p->trace_len, q->read.pos, self->process_count - 1, fork ? "true" : "false");
      if (fork || periodic)
      {
        self->fork_entry = e;
        self->fork_offset = q->read.pos;
        self->fork_started = txp_profile_now();
        self->fork_parent_pid = p->pid;
        self->fork_is_anchor = fork && self->fence_pos == 0;
      }
      if (fork)
      {
        a.tag = A_FORK;
        self->fence_pos -= 1;
        self->prepare.retained_fence = false;
      }
      else if (periodic)
      {
        a.tag = A_FORK;
      }
      else
      {
        memmove(channel_get_buffer(self->c, n), data->data + q->read.pos, n);
        a.tag = A_READ;
        a.read.size = n;
      }
      if (0)
      {
        if (fork)
          fprintf(stderr, "read = fork\n");
        else
          fprintf(stderr, "read = %d\n", (int)n);
      }
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_APND:
    {
      fileentry_t *e = NULL;

      if (q->apnd.fid == -1)
      {
        e = self->st.stdout.entry;
        if (e == NULL)
        {
          e = filesystem_lookup_or_create(ctx, self->fs, "stdout");
          log_fileentry(ctx, self->log, e);
          log_filecell(ctx, self->log, &self->st.stdout);
          self->st.stdout.entry = e;
          if (e->saved.data == NULL)
          {
            e->saved.data = fz_new_buffer(ctx, 1024);
            e->saved.level = FILE_WRITE;
          }
        }
      }
      else
      {
        check_fid(q->apnd.fid);
        e = self->st.table[q->apnd.fid].entry;
      }

      if (e == NULL || e->saved.level != FILE_WRITE) mabort();
      log_fileentry(ctx, self->log, e);

      int pos = e->saved.data->len;
      fz_append_data(ctx, e->saved.data, q->apnd.buf, q->apnd.size);

      if (self->st.document.entry == e)
      {
        int opage = incdvi_page_count(self->dvi);
        incdvi_update(ctx, self->dvi, e->saved.data);
        int npage = incdvi_page_count(self->dvi);
        if (opage != npage)
          fprintf(stderr, "[info] output %d pages long\n", npage);
      }
      else if (self->st.synctex.entry == e)
      {
        int opage = synctex_page_count(self->stex);
        int oinput = synctex_input_count(self->stex);
        synctex_update(ctx, self->stex, e->saved.data);
        int npage = synctex_page_count(self->stex);
        int ninput = synctex_input_count(self->stex);
        if (opage != npage || oinput != ninput)
          fprintf(stderr, "[info] synctex used %d input files, is %d pages long\n", ninput, npage);
      }
      else if (self->st.log.entry == e)
        editor_append(BUF_LOG, output_data(e), pos);
      else if (self->st.stdout.entry == e)
        editor_append(BUF_OUT, output_data(e), pos);
      else
        self->aux_dirty = true;
      a.tag = A_DONE;
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_CLOS:
    {
      check_fid(q->clos.fid);

      filecell_t *cell = &self->st.table[q->clos.fid];
      fileentry_t *e = cell->entry;
      if (e == NULL) mabort();
      log_filecell(ctx, self->log, cell);
      cell->entry = NULL;

      if (0)
        fprintf(stderr, "[info] closing %s\n", e->path);

      if (self->st.stdout.entry == e)
      {
        log_filecell(ctx, self->log, &self->st.stdout);
        self->st.stdout.entry = NULL;
      }

      if (self->st.document.entry == e)
      {
        fprintf(stderr, "[info] finished output\n");
        // log_filecell(ctx, log, &st->document);
        // st->document.entry = NULL;
      }

      if (self->st.log.entry == e)
      {
        log_filecell(ctx, self->log, &self->st.log);
        self->st.log.entry = NULL;
      }

      a.tag = A_DONE;
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_SIZE:
    {
      check_fid(q->clos.fid);
      fileentry_t *e = self->st.table[q->clos.fid].entry;
      if (e == NULL || e->saved.level < FILE_READ) mabort();
      a.tag = A_SIZE;
      a.size.size = entry_data(e)->len;
      if (LOG)
        fprintf(stderr, "SIZE = %d (seen = %d)\n", a.size.size, e->seen);
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_MTIM:
    {
      check_fid(q->clos.fid);
      fileentry_t *e = self->st.table[q->clos.fid].entry;
      if (e == NULL || e->saved.level < FILE_READ) mabort();
      a.tag = A_MTIM;
      a.mtim.mtime = e->fs_stat.st_mtime;
      if (LOG)
        fprintf(stderr, "MTIM = %d\n", a.mtim.mtime);
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_SEEN:
    {
      check_fid(q->seen.fid);
      fileentry_t *e = self->st.table[q->seen.fid].entry;
      if (e == NULL) mabort();
      if (LOG)
        fprintf(stderr, "[info] file %s seen: %d -> %d\n", e->path, e->seen, q->seen.pos);
      if (e->saved.level < FILE_READ) mabort();
      if (self->fence_pos >= 0 &&
          self->fences[self->fence_pos].entry == e &&
          self->fences[self->fence_pos].position < q->seen.pos)
      {
        fprintf(stderr,
                "Seen position invalid wrt fence:\n"
                "  file %s, seen: %d -> %d\n"
                "  fence #%d position: %d\n",
                e->path, e->seen, q->seen.pos,
                self->fence_pos,
                self->fences[self->fence_pos].position);
        mabort();
      }
      if (e->debug_rollback_invalidation != -1 &&
          q->seen.pos >= e->debug_rollback_invalidation)
        mabort();
      if (q->seen.pos <= e->seen)
      {
        // if the same file is reopened, the "new seen position" can be lower
        // if (q->seen.pos < e->seen)
        //   mabort();
      }
      else
      {
        log_fileentry(ctx, self->log, e);
        record_seen(self, e, q->seen.pos, q->time);
      }
      break;
    }
    case Q_GPIC:
    {
      fileentry_t *e = filesystem_lookup(self->fs, q->gpic.path);
      if (e && e->saved.level == FILE_READ &&
          e->pic_cache.type == q->gpic.type &&
          e->pic_cache.page == q->gpic.page)
      {
        a.gpic.bounds[0] = e->pic_cache.bounds[0];
        a.gpic.bounds[1] = e->pic_cache.bounds[1];
        a.gpic.bounds[2] = e->pic_cache.bounds[2];
        a.gpic.bounds[3] = e->pic_cache.bounds[3];
        a.tag = A_GPIC;
      }
      else
        a.tag = A_PASS;
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_SPIC:
    {
      fileentry_t *e = filesystem_lookup(self->fs, q->spic.path);
      if (e && e->saved.level == FILE_READ)
          e->pic_cache = q->spic.cache;
      a.tag = A_DONE;
      channel_write_answer(self->c, p->fd, &a);
      break;
    }

    case Q_FINI:
      p->pass_status = q->fini.result == 1 ? TXP_PASS_SUCCEEDED
                     : q->fini.result == 2 ? TXP_PASS_RECOVERED : TXP_PASS_FAILED;
      a.tag = A_DONE;
      channel_write_answer(self->c, p->fd, &a);
      break;

    case Q_CHLD:
    {
      if (self->process_count == MAX_PROCESS)
      {
        decimate_processes(self);
        p = get_process(self);
      }
      channel_reset(self->c);
      self->process_count += 1;
      process_t *p2 = get_process(self);
      p->snap = log_snapshot(ctx, self->log);
      p->tokens = q->time;
      p->checkpoint_entry = self->fork_entry;
      p->checkpoint_offset = self->fork_offset;
      p->anchor = self->fork_is_anchor ? ++self->anchor_serial : 0;
      p2->checkpoint_entry = NULL;
      p2->anchor = 0;
      p2->pass_status = TXP_PASS_RUNNING;
      p2->starting = p2->startup_failed = false;
      if (self->prepare.status == TXP_PREPARE_PENDING &&
          self->prepare.entry == self->fork_entry &&
          self->prepare.offset == self->fork_offset)
        self->prepare.status = TXP_PREPARE_READY;
      p2->fd = q->chld.fd;
      p2->pid = q->chld.pid;
      p2->trace_len = p->trace_len;
      p2->tokens = q->time;
      a.tag = A_DONE;
      channel_write_answer(self->c, p->fd, &a);
      if (self->fork_started) {
        double now = txp_profile_now();
        fprintf(stderr, "[profile] {\"stage\":\"compiler-fork\",\"ms\":%.4f,\"nativeAt\":%.4f,\"request\":-1,\"page\":-1,\"bytes\":0,\"parentPid\":%d,\"pid\":%d,\"tokens\":%d,\"checkpoints\":%d}\n",
                now-self->fork_started, now, self->fork_parent_pid, p2->pid, q->time, self->process_count-1);
        self->fork_started = 0;
      }
      break;
    }
  }
}

static int output_length(fileentry_t *entry)
{
  if (!entry || !entry->saved.data)
    return 0;
  else
    return entry->saved.data->len;
}

static void revert_trace(trace_entry_t *te)
{
  te->entry->seen = te->seen;
}

static void restore_rollback_outputs(fz_context *ctx, struct tex_engine *self)
{
  double profile_start = txp_profile_now();
  fprintf(stderr, "after rollback: %d bytes of output\n",
    self->st.document.entry
    ? (int)self->st.document.entry->saved.data->len
    : 0
  );

  if (self->st.document.entry)
  {
    fprintf(stderr, "[info] before rollback: %d pages\n", incdvi_page_count(self->dvi));
    incdvi_update(ctx, self->dvi, self->st.document.entry->saved.data);
    fprintf(stderr, "[info] after  rollback: %d pages\n", incdvi_page_count(self->dvi));
  }
  else
    incdvi_reset(self->dvi);
  if (self->st.synctex.entry)
  {
    fprintf(stderr, "[info] before rollback: %d pages in synctex\n", synctex_page_count(self->stex));
    synctex_update(ctx, self->stex, self->st.synctex.entry->saved.data);
    fprintf(stderr, "[info] after  rollback: %d pages in synctex\n", synctex_page_count(self->stex));
  }
  else
    synctex_rollback(ctx, self->stex, 0);
  editor_truncate(BUF_OUT, output_data(self->st.stdout.entry));
  editor_truncate(BUF_LOG, output_data(self->st.log.entry));
  txp_profile_record("compiler-restore", profile_start, -1, -1, output_length(self->st.document.entry));
}

static void rollback_processes(fz_context *ctx, struct tex_engine *self, int reverted, int trace)
{
  double profile_start = txp_profile_now();
  self->fork_started = 0;
  int tokens_before = self->last_tokens;
  self->deferred.active = false;
  cancel_convergence(self);

  fprintf(
    stderr,
    "rolling back to position %d\nbefore rollback: %d bytes of output\n",
    trace,
    output_length(self->st.document.entry)
  );
  if (self->fence_pos < 0)
  {
    fprintf(stderr, "No fences, assuming process finished\n");
    // if (self->process_count > 0)
    //   mabort();
  }

  fprintf(stderr, "Last trace entries:\n");
  for (int i = get_process(self)->trace_len - 1, j = fz_maxi(i - 10, 0); i > j; i--)
  {
    fprintf(stderr, "- %s@%d, %d tokens\n",
            self->trace[i].entry->path,
            self->trace[i].seen,
            self->trace[i].time);
  }

  fprintf(stderr, "Snapshots:\n");
  for  (int i = 0; i < self->process_count; ++i)
  {
    process_t *p = &self->processes[i];
    fprintf(stderr, "- position %d, time %d tokens\n", p->trace_len,
            p->trace_len == 0 ? 0 : self->trace[p->trace_len - 1].time);
  }

  // A worker interrupted before its first observed input can have trace_len
  // zero, equal to the rollback target. It is still unusable once closed:
  // retaining it prevents prepare_process from ever spawning a replacement.
  while (self->process_count > 0 &&
         (get_process(self)->trace_len > trace || get_process(self)->fd < 0))
    pop_process(ctx, self);

  int trace_len = self->process_count == 0 ? 0 : get_process(self)->trace_len;
  if (profile_start)
    fprintf(stderr, "[profile] {\"stage\":\"compiler-rollback\",\"ms\":%.4f,\"nativeAt\":%.4f,\"request\":-1,\"page\":-1,\"bytes\":0,\"tokens\":%d,\"resumeTokens\":%d,\"changedTrace\":%d,\"resumeTrace\":%d,\"checkpoints\":%d}\n",
            txp_profile_now() - profile_start, txp_profile_now(), tokens_before,
            self->process_count ? get_process(self)->tokens : 0,
            reverted, trace_len, self->process_count ? self->process_count - 1 : 0);
  while (reverted > trace_len)
  {
    reverted--;
    revert_trace(&self->trace[reverted]);
  }

  restore_rollback_outputs(ctx, self);
}

static bool possible_fence(trace_entry_t *te)
{
  if (te->seen == INT_MAX || te->seen == -1)
    return 0;
  if (te->entry->saved.level > FILE_READ)
    return 0;
  return 1;
}

static int compute_fences(fz_context *ctx, struct tex_engine *self, int trace, int offset)
{
  // An edit can interrupt a cancelled warm replay before its first trace.
  // No adaptive replacement can be built then: retain its one safe READ fence
  // across root respawns. rollback_add_change invalidates it for earlier or
  // different-file edits, and Q_READ/EOF bound its lifetime.
  if (trace <= 0 && self->prepare.retained_fence)
    return trace;
  self->fence_pos = -1;
  self->prepare.retained_fence = false;

  if (trace <= 0)
    return trace;

  if (get_process(self)->trace_len <= trace)
    mabort();

  self->fence_pos = 0;

  // READ fences stop before the first changed byte. Align for nearby edits,
  // but do not rewind another block: that can cross a costly macro loop.
  offset &= ~(64 - 1);
  if (offset < self->trace[trace].seen)
    offset = self->trace[trace].seen;
  if (offset == -1)
    offset = 0;

  self->fences[0].entry = self->trace[trace].entry;
  self->fences[0].position = offset;

  int process = self->process_count - 1;
  int delta = 50;
  int time = self->trace[trace].time - 10;

  fprintf(stderr,
          "[fence] placing fence %d at trace position %d, file %s, offset %d\n",
          self->fence_pos, trace, self->fences[self->fence_pos].entry->path,
          self->fences[self->fence_pos].position);

  int target_process = self->process_count - 1;
  while (target_process >= 0 && self->processes[target_process].trace_len > trace)
    target_process -= 1;
  int target_trace = target_process >= 0 ? self->processes[target_process].trace_len : -1;
  while (trace > target_trace && self->fence_pos < 15)
  {
    if (self->trace[trace].time <= time && possible_fence(&self->trace[trace]))
    {
      self->fence_pos += 1;
      self->fences[self->fence_pos].entry = self->trace[trace].entry;
      self->fences[self->fence_pos].position = self->trace[trace].seen;
      if (self->fences[self->fence_pos].position == -1)
        self->fences[self->fence_pos].position = 0;
      time -= delta;
      delta *= 2;
      fprintf(stderr, "[fence] placing fence %d at trace position %d, file %s, offset %d\n",
              self->fence_pos, trace,
              self->fences[self->fence_pos].entry->path,
              self->fences[self->fence_pos].position);
    }
    trace -= 1;
  }

  return trace;
}

int txp_engine_prepare_status(txp_engine *_self)
{
  if (_self->_class != &_class) return TXP_PREPARE_SKIPPED;
  SELF;
  return self->prepare.status;
}

void txp_engine_cancel_prepare(txp_engine *_self)
{
  if (_self->_class != &_class) return;
  SELF;
  if (self->prepare.status == TXP_PREPARE_PENDING)
    self->prepare.retained_fence = self->fence_pos == 0 &&
      self->fences[0].entry == self->prepare.entry && self->fences[0].position == self->prepare.offset;
  // Cancellation stops speculative stepping, not a useful checkpoint in the
  // prefix that foreground work must replay anyway. No READY can follow this.
  self->prepare.status = TXP_PREPARE_SKIPPED;
}

int txp_engine_prepare_edit(txp_engine *_self, fz_context *ctx, const char *path, int offset)
{
  if (_self->_class != &_class) return TXP_PREPARE_SKIPPED;
  SELF;
  txp_engine_cancel_prepare(_self);
  if (self->prepare.retained_fence) self->fence_pos = -1;
  self->prepare.retained_fence = false;
  fileentry_t *entry = filesystem_lookup(self->fs, path);
  if (!entry || entry->saved.level != FILE_READ || self->finishing ||
      self->process_count == 0 || offset < 0)
    return TXP_PREPARE_SKIPPED;
  fz_buffer *data = entry_data(entry);
  if (!data || offset >= data->len) return TXP_PREPARE_SKIPPED;
  int bucket = offset & ~63;
  for (int i = 0; i < self->process_count; ++i)
  {
    process_t *p = &self->processes[i];
    if (p->fd >= 0 && p->checkpoint_entry == entry &&
        p->checkpoint_offset <= offset && p->checkpoint_offset >= bucket) {
      p->anchor = ++self->anchor_serial;
      return TXP_PREPARE_CACHED;
    }
  }
  if (entry->seen < offset || bucket >= data->len) return TXP_PREPARE_SKIPPED;
  int length = get_process(self)->trace_len, target = length - 1;
  while (target >= 0 && (self->trace[target].entry != entry || self->trace[target].seen >= bucket))
    --target;
  // Unread files and offsets not represented by a safe READ trace are cheap
  // skips. Never advance arbitrary documents merely to discover a warm target.
  if (target <= 0) return TXP_PREPARE_SKIPPED;
  compute_fences(ctx, self, target, bucket);
  // One target only: don't build the earlier exponential editing fences.
  self->fence_pos = 0;
  self->prepare.entry = entry;
  self->prepare.offset = self->fences[0].position;
  rollback_processes(ctx, self, length, target);
  self->prepare.status = TXP_PREPARE_PENDING;
  fprintf(stderr, "[prepare-edit] replay %s at byte %d\n", path, self->prepare.offset);
  prepare_process(ctx, self, "prewarm");
  return TXP_PREPARE_PENDING;
}

static int engine_page_count(txp_engine *_self)
{
  SELF;
  return incdvi_page_count(self->dvi);
}

static fz_display_list *engine_render_page(txp_engine *_self, fz_context *ctx, int page)
{
  SELF;
  return incdvi_display_list(ctx, self->dvi, self->st.document.entry->saved.data, page);
}

static bool engine_step(txp_engine *_self, fz_context *ctx, bool restart_if_needed)
{
  SELF;
  reap_root_children(self, false);
  if (restart_if_needed)
  {
    // Explicit Resume/edit demand can recover a root that failed before its
    // first open. No source trace/file entry exists to trigger normal rollback.
    if (self->process_count && get_process(self)->startup_failed && get_process(self)->fd < 0)
      pop_process(ctx, self);
    prepare_process(ctx, self, "demand");
  }

  txp_engine_poll_startup(_self);
  if (txp_engine_startup_pending(_self)) return false;

  if (self->deferred.active)
  {
    fileentry_t *e = filesystem_lookup(self->fs, self->deferred.path);
    if (e && e->edit_data)
    {
      e->seen = -1;
      self->deferred.active = false;
      answer_query(ctx, self, &self->deferred.query);
      channel_flush(self->c, get_process(self)->fd);
      return 1;
    }
    return 0;
  }

  if (engine_get_status(_self) == DOC_RUNNING)
  {
    query_t q;
    int fd = get_process(self)->fd;
    if (fd == -1)
      return 0;
    if (!channel_has_pending_query(self->c, fd, 10))
      return 0;
    if (!read_query(self, self->c, &q))
    {
      close(fd);
      get_process(self)->fd = -1;
      return 0;
    }
    answer_query(ctx, self, &q);
    channel_flush(self->c, fd);
    return 1;
  }

  return 0;
}

static int scan_entry(fz_context *ctx, struct tex_engine *self, fileentry_t *e)
{
  if (e->saved.level < FILE_READ || e->fs_stat.st_ino == 0 || e->edit_data)
    return -1;

  struct stat st;

  fprintf(stderr, "[scan] scanning %s\n", e->path);

  const char *inclusion_path = self->inclusion_path;
  char fs_path_buffer[1024];
  const char *fs_path = lookup_path(self, e->path, fs_path_buffer, &st);

  if (!fs_path)
  {
      fprintf(stderr, "[scan] file removed\n");
      return -1;
  }

  if (stat_same(&st, &e->fs_stat))
    return -1;

  e->fs_stat = st;
  fprintf(stderr, "[scan] file %s has changed\n", e->path);

  fz_buffer *buf;
  fz_var(buf);

  fz_try(ctx)
  {
    buf = fz_read_file(ctx, fs_path);
  }
  fz_catch(ctx)
  {
    return -1;
  }

  e->pic_cache.type = -1;

  int olen = e->fs_data->len, nlen = buf->len;
  int len = olen < nlen ? olen : nlen;

  int i = 0;
  while (i < len && e->fs_data->data[i] == buf->data[i])
    i += 1;

  if (i != len)
    fprintf(stderr, "[scan] first changed byte is %d\n", i);
  else if (olen == nlen)
  {
    fprintf(stderr, "[scan] but content has not changed\n");
    fz_drop_buffer(ctx, buf);
    return -1;
  }
  else if (olen < nlen)
    fprintf(stderr, "[scan] content has grown from %d to %d bytes\n", olen, nlen);
  else
    fprintf(stderr, "[scan] content was shrinked from %d to %d bytes\n", olen, nlen);

  fz_drop_buffer(ctx, e->fs_data);
  e->fs_data = buf;

  return i;
}

#define NOT_IN_TRANSACTION (-2)

static void rollback_begin(fz_context *ctx, struct tex_engine *self)
{
  // Check if already in a transaction
  if (self->rollback.trace_len != NOT_IN_TRANSACTION)
    abort();

  // Skip if no worker yet (-stream paused at startup)
  if (self->process_count == 0)
    return;

  self->rollback.trace_len = get_process(self)->trace_len;
  self->rollback.offset = -1;
  self->rollback.flush = 0;
}

static bool rollback_end(fz_context *ctx, struct tex_engine *self, int *tracep, int *offsetp)
{
  int trace_len = self->rollback.trace_len;
  self->rollback.trace_len = NOT_IN_TRANSACTION;

  // No transaction opened: legitimate when begin was a no-op (no worker
  // at begin, possibly spawned mid-iteration by an EDIT_RESUME bootstrap).
  if (trace_len == NOT_IN_TRANSACTION)
    return false;

  process_t *p = get_process(self);

  // Check if nothing changed
  if (trace_len == p->trace_len)
  {
    if (!self->rollback.flush)
      return false;
    if (p->fd > -1)
    {
      if (p->starting) return false;
      ask_t a;
      a.tag = C_FLSH;
      channel_write_ask(self->c, p->fd, &a);
      channel_flush(self->c, p->fd);
      return false;
    }
    if (trace_len > 0)
    {
      trace_len -= 1;
      revert_trace(&self->trace[trace_len]);
    }
    if (trace_len > 0)
      self->rollback.offset = self->trace[trace_len].seen;
  }

  fprintf(stderr, "[change] rewinded trace from %d to %d entries\n",
          get_process(self)->trace_len, trace_len);

  if (tracep)
    *tracep = trace_len;
  if (offsetp)
    *offsetp = self->rollback.offset;

  return true;
}

// Return false if some contents had not been observed: caller should recheck
// for changed contents.
// Return true otherwise (process is ready to be flushed).
static bool process_pending_messages(fz_context *ctx, struct tex_engine *self)
{
  // If the process is marked ready to flush, seen messages have already been
  // consumed
  if (self->rollback.flush)
    return 1;

  process_t *p = get_process(self);

  // If process is dead, nothing has been missed
  if (p->fd == -1 || p->starting)
    return 1;

  // Synchronize with the child process:
  // - kill if stuck
  // - check pending SEEN messages to update vision of the process
  int nothing_seen = 1;
  do {
    if (!channel_has_pending_query(self->c, p->fd, 10))
    {
      fprintf(stderr, "[kill] worker might be stuck, killing\n");
      // The process hasn't answered in 10ms
      // It might be stuck in long computation or a loop, kill it to start from the previous one.
      close_process(self, p);
      break;
    }
    // Process only pending SEEN to have an updated view on process state
    switch (channel_peek_query(self->c, p->fd))
    {
      case Q_SEEN:
        {
          query_t q;
          if (!read_query(self, self->c, &q))
          {
            close(p->fd);
            p->fd = -1;
            break;
          }
          answer_query(ctx, self, &q);
          nothing_seen = 0;
          continue;
        }
      default:
        break;
    }
  } while(0);

  self->rollback.flush = 1;
  return nothing_seen;
}

static void rollback_add_change(fz_context *ctx, struct tex_engine *self, fileentry_t *e, int changed)
{
  int trace_len = self->rollback.trace_len;
  if (self->prepare.retained_fence &&
      (self->fences[0].entry != e || changed < self->fences[0].position))
  {
    self->fence_pos = -1;
    self->prepare.retained_fence = false;
  }
  // if (changed > 0) changed--;

  // No transaction opened: legitimate when begin was a no-op (no worker).
  if (trace_len == NOT_IN_TRANSACTION)
    return;

  // A worker cannot hold bytes (or an existence answer) for an unopened file:
  // Q_OPRD records seen=0 before returning A_OPEN; failed opens use INT_MAX.
  // Do not mistake startup/preamble work for a hung consumer of this input.
  if (e->seen < 0)
  {
    if (get_process(self)->fd < 0)
      self->rollback.flush = 1; // A genuinely dead root still needs replacement.
    else if (txp_profile_now())
      fprintf(stderr, "[profile] {\"stage\":\"compiler-unread-edit\",\"ms\":0,\"nativeAt\":%.4f,\"request\":-1,\"page\":-1,\"bytes\":0,\"offset\":%d,\"trace\":%d}\n", txp_profile_now(), changed, get_process(self)->trace_len);
    return;
  }

  if (e->seen < changed && trace_len == get_process(self)->trace_len)
  {
    // A pending message might update e->seen
    if (process_pending_messages(ctx, self))
      return;
    trace_len = self->rollback.trace_len = get_process(self)->trace_len;
  }
  if (e->seen < changed)
    return;

  while (e->seen >= changed)
  {
    trace_len--;
    revert_trace(&self->trace[trace_len]);
  }

  if (self->trace[trace_len].entry != e)
  {
    fprintf(stderr, "Rollback position: %d. Entries: %d. Seen: %d. Changed: %d. Last trace entries:\n", trace_len, get_process(self)->trace_len, e->seen, changed);
    for (int i = get_process(self)->trace_len - 1, j = fz_maxi(i - 10, 0); i > j; i--)
    {
      fprintf(stderr, "- %s@%d, %d tokens\n",
              self->trace[i].entry->path,
              self->trace[i].seen,
              self->trace[i].time);
    }
    mabort();
  }

  self->rollback.trace_len = trace_len;
  self->rollback.offset = changed;
}

static void engine_notify_file_changes(txp_engine *_self,
                                       fz_context *ctx,
                                       fileentry_t *entry,
                                       int offset)
{
  SELF;
  rollback_add_change(ctx, self, entry, offset);
}

static bool is_system_output(const char *path)
{
  if (strcmp(path, "stdout") == 0)
    return true;
  const char *dot = strrchr(path, '.');
  if (!dot)
    return false;
  return strcmp(dot, ".log") == 0
      || strcmp(dot, ".xdv") == 0
      || strcmp(dot, ".dvi") == 0
      || strcmp(dot, ".pdf") == 0
      || strcmp(dot, ".synctex") == 0;
}

// Reset per-entry `seen` counters before respawning the engine. Without
// this, the new engine's Q_OPRD never enters the `e->seen < 0` branch
// in answer_query, so no first-open trace entry is generated. Later
// rollbacks would walk back through the trace without ever bringing
// entry->seen below the changed offset, walking past trace[0] into OOB
// memory and crashing in revert_trace.
static void reset_seen_for_respawn(struct tex_engine *self)
{
  fileentry_t *e;
  for (int index = 0; (e = filesystem_scan(self->fs, &index));)
    e->seen = -1;
}

static bool engine_aux_dirty(txp_engine *_self)
{
  SELF;
  // Replaying an unchanged prefix for a lens, page or warm checkpoint writes
  // auxiliary bytes again. A converged source generation already proved those
  // bytes stable; only a real edit (which clears completed_page_count) needs
  // another convergence pass.
  return self->aux_dirty && self->completed_page_count < 0;
}

static bool engine_is_finishing(txp_engine *_self)
{
  SELF;
  return self->finishing;
}

// Mark the engine as needing to run to end-of-document. The main loop
// keeps calling engine_step (because need_advance honours is_finishing),
// so the engine processes its remaining queries between SDL events —
// UI stays responsive. When the engine closes its socket, status flips
// to DOC_TERMINATED and finish_convergence does the post-pass work.
static void engine_start_finishing(txp_engine *_self)
{
  SELF;
  self->completed_page_count = -1;
  self->finishing = true;
}

static void profile_convergence(struct tex_engine *self, double start, int old_processes, const char *outcome, bool respawn, size_t bytes)
{
  if (!start) return;
  double now = txp_profile_now();
  fprintf(stderr, "[profile] {\"stage\":\"compiler-convergence\",\"ms\":%.4f,\"nativeAt\":%.4f,\"request\":-1,\"page\":-1,\"bytes\":%zu,\"oldProcesses\":%d,\"processes\":%d,\"outcome\":\"%s\",\"respawn\":%s}\n",
          now-start, now, bytes, old_processes, self->process_count, outcome, respawn ? "true" : "false");
}

static void engine_finish_convergence(txp_engine *_self, fz_context *ctx)
{
  SELF;
  if (!self->finishing)
    return;
  if (engine_get_status(_self) != DOC_TERMINATED)
    return;

  double profile_start = txp_profile_now();
  int old_processes = self->process_count;
  size_t aux_bytes = 0;
  self->finishing = false;
  if (txp_engine_pass_status(_self) != TXP_PASS_SUCCEEDED)
  {
    // Failed/aborted passes can close a truncated .aux. Keep the last completed
    // pass's copied input and wait for the next source edit; retrying here
    // would repeatedly compile the same unfinished document.
    fprintf(stderr, "[rerun] incomplete pass: retaining completed auxiliary input\n");
    self->aux_dirty = false;
    profile_convergence(self, profile_start, old_processes, "incomplete", false, 0);
    return;
  }

  bool converged = true;
  fileentry_t *e;
  for (int index = 0; (e = filesystem_scan(self->fs, &index));)
  {
    if (e->saved.level != FILE_WRITE || !e->saved.data)
      continue;
    if (is_system_output(e->path))
      continue;
    bool stashed_match = e->edit_data && e->edit_data_from_convergence
                         && e->saved.data->len == e->edit_data->len
                         && memcmp(e->saved.data->data, e->edit_data->data,
                                   e->saved.data->len) == 0;
    // A prebuilt .aux/.out can already contain exactly the output of this
    // pass. Requiring a convergence-created stash needlessly discarded all
    // compiler checkpoints after every editing pause, even for stable labels.
    bool filesystem_match = !e->edit_data && e->saved.read_from_filesystem && e->fs_data &&
                            e->saved.data->len == e->fs_data->len &&
                            (!e->saved.data->len || memcmp(e->saved.data->data, e->fs_data->data,
                                   e->saved.data->len) == 0);
    if (filesystem_match)
      fprintf(stderr, "[rerun] %s unchanged from filesystem input\n", e->path);
    if (!stashed_match && !filesystem_match)
    {
      if (txp_profile_now())
        fprintf(stderr, "[rerun] changed %s (output %zu bytes, filesystem %zu bytes, observed %s)\n",
                e->path, e->saved.data->len, e->fs_data ? e->fs_data->len : 0,
                e->saved.read_from_filesystem ? "true" : "false");
      converged = false;
      break;
    }
  }

  if (converged)
  {
    fprintf(stderr, "[rerun] aux byte-stable, convergence reached\n");
    // Pagination describes the completed source, not the worker checkpoint
    // retained below for the next edit. Capture it before restoring that live
    // prefix (whose pass status is RUNNING and may expose fewer pages).
    self->completed_page_count = incdvi_page_count(self->dvi);
    // Finishing left the latest process dead (fd=-1) at the top of the
    // snapshot stack. Strip dead processes so get_process() returns a
    // live snapshot the next user edit can roll back into.
    int reverted = self->process_count ? get_process(self)->trace_len : 0;
    while (self->process_count > 0 && get_process(self)->fd == -1)
      pop_process(ctx, self);
    if (self->process_count > 0)
    {
      // The log restores file handles/output buffers, but seen counters are
      // tracked separately. A late input may not exist in the retained trace;
      // leaving its finished-pass counter would walk rollback before trace[0].
      int retained = get_process(self)->trace_len;
      while (reverted > retained)
        revert_trace(&self->trace[--reverted]);
      restore_rollback_outputs(ctx, self);
    }
    bool respawn = self->process_count == 0;
    if (respawn)
    {
      reset_seen_for_respawn(self);
      incdvi_reset(self->dvi);
      synctex_rollback(ctx, self->stex, 0);
      editor_truncate(BUF_OUT, output_data(self->st.stdout.entry));
      editor_truncate(BUF_LOG, output_data(self->st.log.entry));
      prepare_process(ctx, self, "convergence-stable");
    }
    self->aux_dirty = false;
    profile_convergence(self, profile_start, old_processes, "stable", respawn, 0);
    return;
  }

  for (int index = 0; (e = filesystem_scan(self->fs, &index));)
  {
    if (e->saved.level != FILE_WRITE || !e->saved.data)
      continue;
    if (is_system_output(e->path))
      continue;
    // Don't clobber edit_data the editor pushed (interpret_open sets
    // edit_data without the convergence flag).
    if (e->edit_data && !e->edit_data_from_convergence)
      continue;
    if (e->edit_data)
      fz_drop_buffer(ctx, e->edit_data);
    // Copy, don't share: log_rollback truncates saved.data->len in place.
    e->edit_data = fz_new_buffer_from_copied_data(ctx, e->saved.data->data,
                                                  e->saved.data->len);
    aux_bytes += e->saved.data->len;
    e->edit_data_from_convergence = true;
  }

  while (self->process_count > 0)
    pop_process(ctx, self);

  reset_seen_for_respawn(self);
  incdvi_reset(self->dvi);
  synctex_rollback(ctx, self->stex, 0);
  editor_truncate(BUF_OUT, output_data(self->st.stdout.entry));
  editor_truncate(BUF_LOG, output_data(self->st.log.entry));

  prepare_process(ctx, self, "convergence-changed");
  self->aux_dirty = false;
  profile_convergence(self, profile_start, old_processes, "changed", true, aux_bytes);
}

static void engine_begin_changes(txp_engine *_self, fz_context *ctx)
{
  SELF;
  rollback_begin(ctx, self);
}

static void engine_detect_changes(txp_engine *_self, fz_context *ctx)
{
  SELF;

  fileentry_t *e;
  for (int index = 0; (e = filesystem_scan(self->fs, &index));)
  {
    int changed = scan_entry(ctx, self, e);
    if (changed > -1)
      rollback_add_change(ctx, self, e, changed);
  }
}

static bool engine_end_changes(txp_engine *_self, fz_context *ctx)
{
  SELF;
  int reverted, trace, offset;

  if (!rollback_end(ctx, self, &reverted, &offset))
    return false;

  self->completed_page_count = -1;
  trace = reverted >= 0 ? compute_fences(ctx, self, reverted, offset) : 0;
  rollback_processes(ctx, self, reverted, trace);

  return true;
}

uint64_t txp_engine_page_token(txp_engine *_self, fz_display_list *list)
{
  if (_self->_class != &_class) return 0;
  SELF;
  return incdvi_display_list_token(self->dvi, list);
}

int txp_engine_completed_page_count(txp_engine *_self)
{
  if (_self->_class != &_class) return -1;
  SELF;
  return self->completed_page_count;
}

txp_pass_status txp_engine_pass_status(txp_engine *_self)
{
  if (_self->_class != &_class) return TXP_PASS_UNKNOWN;
  SELF;
  return self->process_count ? get_process(self)->pass_status : TXP_PASS_CANCELLED;
}

static txp_engine_status engine_get_status(txp_engine *_self)
{
  SELF;
  reap_root_children(self, false);
  if (self->process_count == 0)
    return DOC_TERMINATED;
  return get_process(self)->fd > -1 ? DOC_RUNNING : DOC_TERMINATED;
}

static float engine_scale_factor(txp_engine *_self)
{
  SELF;
  return incdvi_tex_scale_factor(self->dvi);
}

static synctex_t *engine_synctex(txp_engine *_self, fz_buffer **buf)
{
  SELF;
  if (buf)
    *buf = self->st.synctex.entry ? entry_data(self->st.synctex.entry) : NULL;
  return self->stex;
}

static fileentry_t *engine_find_file(txp_engine *_self, fz_context *ctx, const char *path)
{
  SELF;
  return filesystem_lookup_or_create(ctx, self->fs, path);
}

txp_engine *txp_create_tex_engine(fz_context *ctx,
                                  const char *engine_path,
                                  bool use_texlive,
                                  bool stream_mode,
                                  const char *inclusion_path,
                                  const char *tex_name,
                                  dvi_reshooks hooks)
{
  struct tex_engine *self = fz_malloc_struct(ctx, struct tex_engine);
  self->_class = &_class;
  self->completed_page_count = -1;

  self->name = fz_strdup(ctx, tex_name);
  self->engine_path = fz_strdup(ctx, engine_path);
  self->inclusion_path = fz_strdup(ctx, inclusion_path ? inclusion_path : "");
  state_init(&self->st);
  self->fs = filesystem_new(ctx);
  self->log = log_new(ctx);
  self->trace = NULL;
  self->trace_cap = 0;
  self->fence_pos = -1;
  self->restart = log_snapshot(ctx, self->log);
  self->c = channel_new();
  self->process_count = 0;

  self->dvi = incdvi_new(ctx, hooks);
  self->use_texlive = use_texlive;
  self->stream_mode = stream_mode;

  self->stex = synctex_new(ctx);
  self->rollback.trace_len = NOT_IN_TRANSACTION;
  self->deferred.active = false;

  return (txp_engine*)self;
}

static fz_buffer *engine_page_annotations(txp_engine *_self, fz_context *ctx)
{
  SELF;
  return incdvi_annotations(ctx, self->dvi);
}

static int engine_destination_page(txp_engine *_self, fz_context *ctx, const char *name)
{
  SELF;
  /* The index is maintained by incdvi_update/reset. A restored checkpoint can
   * precede opening the output file; destination lookup must remain safe then. */
  return incdvi_destination_page(ctx, self->dvi, name);
}
