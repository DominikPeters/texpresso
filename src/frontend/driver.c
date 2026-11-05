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

#include <unistd.h>
#include <signal.h>
#include <mupdf/fitz.h>
#include "logo.h"
#include "driver.h"
#include "preview.h"
#include "preview_compression.h"
#include "txp_error.h"
#include "parent_watch.h"
#include <errno.h>
#include <stdint.h>

#ifdef __APPLE__
#include <sys/syslimits.h>
#include <mach-o/dyld.h>
#else
#include <linux/limits.h>
#endif

/* Custom SDL events  */

Uint32 custom_event = 0;

void schedule_event(enum custom_events ev)
{
  static char scheduled[EVENT_COUNT] = {0,};

  char *sched = scheduled + ev;
  if (!*sched)
  {
    *sched = 1;
    SDL_Event event;
    SDL_zero(event);
    event.type = custom_event;
    event.user.code = ev;
    event.user.data1 = sched;
    if (SDL_PushEvent(&event) < 0)
      abort();
  }
}

static void signal_usr1(int sig)
{
  (void)sig;

  static volatile int barrier = 0;

  if (!barrier)
  {
    barrier = 1;
    schedule_event(SCAN_EVENT);
    barrier = 0;
  }
}

/* Misc routines */

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

static bool get_executable_path(char path[PATH_MAX])
{
#ifdef __APPLE__
  uint32_t size = PATH_MAX;
  char exe_path[PATH_MAX];
  if (_NSGetExecutablePath(exe_path, &size) != 0)
    return 0;
#else
  char *exe_path = "/proc/self/exe";
#endif
  return realpath(exe_path, path);
}

static bool should_reload_binary(void)
{
  return 0;
}

static void report_engine_failure(fz_context *ctx, bool headless)
{
  const char *message = txp_take_error(ctx);
  fprintf(stderr, "[fatal] TeXpresso engine failure: %s\n", message);
  if (!headless) return;
  // Reporting must also work when the original error was allocation failure.
  fputs("[\"engine-error\",", stdout);
  txp_write_json_string(stdout, message);
  fputs("]\n", stdout); fflush(stdout);
}

static void usage(void)
{
  fprintf(stderr,
          "Usage: texpresso [-I path]* [-json] [-lines] [-texlive] [-tectonic] "
          "[-test-initialize] [-stream] [-headless] root_file.tex\n");
  fprintf(stderr,
          " -I path    Add a path to included directories. \n"
          "    Files are looked up relative to document directory and all "
          "included paths\n");
  fprintf(stderr,
          " -json      Use json rather than s-exp communication protocol\n");
  fprintf(stderr,
          " -lines     Use line-based rather than byte-based synchronization\n");
  fprintf(stderr,
          " -texlive   Load TeX packages from TeXlive installation (using "
          "kpsewhich command)\n");
  fprintf(stderr,
          " -tectonic  Load TeX packages from tectonic installation (using "
          "tectonic command)\n");
  fprintf(stderr,
          " -test-initialize  Run a single cycle for test purposes\n");
  fprintf(stderr,
          " -stream   Skip filesystem lookups; files are pushed via editor commands\n");
  fprintf(stderr,
          " -miktex   Load packages and fonts using MiKTeX findtexmf\n"
          " -miktex-windows Use Windows MiKTeX from WSL (experimental)\n"
          " -miktex-windows-bin PATH Optional Windows MiKTeX binary directory\n"
          " -miktex-auto-install Allow noninteractive installation of missing MiKTeX packages\n"
          " -headless Export preview images over stdout (requires -json)\n"
          " -watch-parent Release isolated compiler group when its editor exits (headless only)\n"
          " -capabilities Print supported preview features as JSON and exit\n"
          " -preview-max-pixels N Set PNG area budget (1..16000000; default 4000000)\n");
}

int main(int argc, const char **argv)
{
  // Unbuffered stderr so debug/[fatal] lines hit disk before a crash.
  setvbuf(stderr, NULL, _IONBF, 0);

  char work_dir[PATH_MAX];

  if (!getcwd(work_dir, PATH_MAX))
  {
    perror("get working directory");
    return 1;
  }

  fprintf(stderr, "[info] working directory: %s\n", work_dir);

  char exe_path[PATH_MAX];

  if (!get_executable_path(exe_path) && !realpath(argv[0], exe_path))
  {
    perror("finding executable path");
    return 1;
  }
  fprintf(stderr, "[info] executable path: %s\n", exe_path);

  const char *doc_arg = NULL;
  enum editor_protocol protocol = EDITOR_SEXP;
  bool line_output = 0;
  bool use_tectonic = 0;
  bool use_texlive = 0, use_miktex = 0, use_miktex_windows = 0, miktex_auto_install = 0;
  bool initialize_only = 0;
  bool stream_mode = 0;
  bool headless = 0;
  bool capabilities = 0;
  bool watch_parent = 0;
  int preview_max_pixels = TXP_PREVIEW_DEFAULT_PIXELS;

  size_t inclusion_path_size = 1;
  for (int i = 1; i < argc; i++)
  {
    const char *arg = argv[i];

    if (arg[0] == '-')
    {
      if (strcmp(arg, "-json") == 0)
      {
        protocol = EDITOR_JSON;
      }
      else if (strcmp(arg, "-headless") == 0)
        headless = 1;
      else if (strcmp(arg, "-watch-parent") == 0)
        watch_parent = 1;
      else if (strcmp(arg, "-capabilities") == 0)
        capabilities = 1;
      else if (strcmp(arg, "-preview-max-pixels") == 0)
      {
        char *end = NULL;
        errno = 0;
        long pixels = ++i < argc ? strtol(argv[i], &end, 10) : 0;
        if (errno || !end || end == argv[i] || *end || pixels < 1 || pixels > TXP_PREVIEW_MAX_PIXELS) {
          fprintf(stderr, "[error] -preview-max-pixels expects an integer in 1..%d\n", TXP_PREVIEW_MAX_PIXELS);
          return 1;
        }
        preview_max_pixels = (int)pixels;
      }
      else if (strcmp(arg, "-I") == 0)
      {
        i += 1;
        if (i == argc)
        {
          fprintf(stderr, "[error] Expecting a path after -I\n");
          usage();
          exit(1);
        }
        size_t length = strlen(argv[i]);
        if (length > SIZE_MAX - inclusion_path_size - 1) {
          fprintf(stderr, "[error] Include path list is too large\n");
          return 1;
        }
        inclusion_path_size += 1 + length;
      }
      else if (strcmp(arg, "-lines") == 0)
      {
        line_output = 1;
      }
      else if (strcmp(arg, "-tectonic") == 0)
      {
        use_tectonic = 1;
      }
      else if (strcmp(arg, "-miktex") == 0)
        use_miktex = 1;
      else if (strcmp(arg, "-miktex-windows") == 0) { use_miktex = 1; use_miktex_windows = 1; }
      else if (strcmp(arg, "-miktex-windows-bin") == 0) {
        if (++i == argc || setenv("TEXPRESSO_MIKTEX_WINDOWS_BIN", argv[i], 1)) {
          fprintf(stderr, "[error] -miktex-windows-bin requires a Windows directory\n"); return 1;
        }
      }
      else if (strcmp(arg, "-miktex-auto-install") == 0)
        miktex_auto_install = 1;
      else if (strcmp(arg, "-texlive") == 0)
      {
        use_texlive = 1;
      }
      else if (strcmp(arg, "-test-initialize") == 0)
      {
        initialize_only = 1;
      }
      else if (strcmp(arg, "-stream") == 0)
      {
        stream_mode = 1;
      }
      else
      {
        fprintf(stderr, "[error] Unknown option %s\n", arg);
        usage();
        exit(1);
      }

      continue;
    }

    if (doc_arg == NULL)
    {
      doc_arg = arg;
      continue;
    }

    fprintf(stderr, "[error] Expecting a single document argument, got %s and %s\n",
            doc_arg, arg);
    usage();
    exit(1);
  }

  if (capabilities)
  {
    printf("{\"previewProtocol\":1,\"maxPreviewPixels\":%d,\"codecs\":[\"png\",\"svg\"],\"synctex\":true,\"synctexPrecision\":1,\"regions\":true,\"links\":true,\"tikzFigures\":true,\"adaptiveRendering\":true,\"pngPatches\":true,\"backgroundRendering\":true,\"prepareEdit\":true,\"requestLifecycle\":true,\"parentWatch\":true,\"packageProviders\":[\"texlive\",\"miktex\",\"miktex-windows\",\"tectonic\"],\"miktexAutoInstall\":true,\"miktexWindowsBridge\":true,\"pngCompression\":%s}\n", TXP_PREVIEW_MAX_PIXELS, TXP_PNG_COMPRESSION);
    return 0;
  }
  if (headless && protocol != EDITOR_JSON)
  {
    fprintf(stderr, "[error] -headless requires -json\n");
    return 1;
  }

  if (doc_arg == NULL)
  {
    usage();
    exit(1);
  }

  if (use_tectonic + use_texlive + use_miktex > 1 || (miktex_auto_install && !use_miktex))
  {
    fprintf(stderr, "[error] Choose one provider; -miktex-auto-install requires -miktex.\n");
    usage();
    exit(1);
  }

  if (watch_parent) {
    if (!headless) {
      fprintf(stderr, "[error] -watch-parent requires -headless\n");
      return 1;
    }
    const int result = txp_watch_parent();
    if (result >= 0) return result;
  }

  // A worker can exit between a protocol read and our response. Treat its
  // closed socket as a failed pass instead of terminating the whole frontend.
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    perror("[error] ignoring broken worker pipes");
    return 1;
  }

  char *inclusion_path = malloc(inclusion_path_size);
  if (!inclusion_path) {
    fprintf(stderr, "[error] Could not allocate include path list\n");
    return 1;
  }

  char *p = inclusion_path;
  for (int i = 1; i < argc; i++)
  {
    if (argv[i][0] == '-' && argv[i][1] == 'I' && argv[i][2] == '\0')
    {
      i += 1;
      p = stpcpy(p, argv[i]) + 1;
    }
  }
  *p = '\0';

  // Move to TeX document directory
  char doc_path[PATH_MAX];
  if (!realpath(doc_arg, doc_path))
  {
    if (stream_mode)
    {
      // In stream mode, the file may not exist on disk yet.
      // Absolute paths used as-is; relative paths resolved against cwd.
      if (doc_arg[0] == '/')
        snprintf(doc_path, PATH_MAX, "%s", doc_arg);
      else
        snprintf(doc_path, PATH_MAX, "%s/%s", work_dir, doc_arg);
    }
    else
    {
      perror("finding document path");
      free(inclusion_path);
      return 1;
    }
  }

  char *doc_name = last_index(doc_path, '/');
  if (doc_path == doc_name) {
    fprintf(stderr, "[error] Document path has no parent directory: %s\n", doc_path);
    free(inclusion_path);
    return 1;
  }
  char doc_name_buffer[PATH_MAX];
  snprintf(doc_name_buffer, sizeof doc_name_buffer, "%s", doc_name);
  if (doc_name == doc_path + 1) doc_path[1] = '\x00';
  else doc_name[-1] = '\x00';
  doc_name = doc_name_buffer;

  fprintf(stderr, "[info] document path: %s\n", doc_path);
  fprintf(stderr, "[info] document name: %s\n", doc_name);

  if (chdir(doc_path) == -1)
  {
    perror("chdir to document path");
    free(inclusion_path);
    return 1;
  }

  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  if (!ctx) {
    fprintf(stderr, "[error] Could not create MuPDF context\n");
    free(inclusion_path);
    return 1;
  }
  fz_try(ctx) { fz_register_document_handlers(ctx); }
  fz_catch(ctx) {
    report_engine_failure(ctx, headless);
    fz_drop_context(ctx); free(inclusion_path);
    return 1;
  }

  bool init = 0;

  //Initialize SDL
  if (init == 0 && SDL_Init(headless ? (SDL_INIT_EVENTS | SDL_INIT_TIMER) : SDL_INIT_VIDEO) < 0)
  {
    fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
    fz_drop_context(ctx); free(inclusion_path);
    return 1;
  }

  custom_event = SDL_RegisterEvents(1);
  if (custom_event == (Uint32)-1) {
    fprintf(stderr, "[error] Could not register SDL engine event: %s\n", SDL_GetError());
    SDL_Quit(); fz_drop_context(ctx); free(inclusion_path);
    return 1;
  }
  signal(SIGUSR1, signal_usr1);

  //Create window
  char window_title[128];
  snprintf(window_title, sizeof window_title, "TeXpresso %.116s", doc_name);

#if SDL_VERSION_ATLEAST(2, 0, 8)
  SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif

  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  if (!headless)
  {
    window = SDL_CreateWindow(window_title,
      SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
      700, 900,
      SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE
    );

    if (window == NULL)
    {
      fprintf(stderr, "Window could not be created! SDL_Error: %s\n", SDL_GetError() );
      SDL_Quit(); fz_drop_context(ctx); free(inclusion_path);
      return 1;
    }

    SDL_Surface *logo = texpresso_logo();
    fprintf(stderr, "texpresso logo: %dx%d\n", logo->w, logo->h);
#ifndef __APPLE__
    // SDL_SetWindowIcon on macOS calls [NSApp setApplicationIconImage:]
    // which scales the image to fill the Dock cell, ignoring transparent
    // padding. On macOS we rely on the .app bundle's AppIcon.icns instead
    // (see scripts/build-macos-app.sh).
    SDL_SetWindowIcon(window, logo);
#endif
    SDL_FreeSurface(logo);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer) {
      // Older software renderers support render targets but not vsync.
      fprintf(stderr, "[warning] SDL renderer with vsync unavailable (%s); retrying without vsync\n", SDL_GetError());
      renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_TARGETTEXTURE);
    }
    if (!renderer) {
      fprintf(stderr, "SDL renderer: %s\n", SDL_GetError());
      SDL_DestroyWindow(window); SDL_Quit(); fz_drop_context(ctx); free(inclusion_path);
      return 1;
    }
  }

  struct persistent_state pstate = {
      .initial = {0,},
      .protocol = protocol,
      .window = window,
      .renderer = renderer,
      .ctx = ctx,
      .exe_path = exe_path,
      .doc_path = doc_path,
      .doc_name = doc_name,
      .inclusion_path = inclusion_path,
      .custom_event = custom_event,
      .schedule_event = &schedule_event,
      .should_reload_binary = &should_reload_binary,

      .line_output = line_output,
      .use_tectonic = use_tectonic,
      .use_texlive = use_texlive,
      .use_miktex = use_miktex,
      .use_miktex_windows = use_miktex_windows,
      .miktex_auto_install = miktex_auto_install,
      .initialize_only = initialize_only,
      .stream_mode = stream_mode,
      .headless = headless,
      .preview_max_pixels = preview_max_pixels,
      // In stream mode, start paused: editor primes the VFS before (resume).
      .paused = stream_mode,
      .rerun_enabled = false,
      .rerun_once_pending = false,
  };

  int exit_code = 0;

  fz_var(exit_code);
  fz_try(ctx) {
  if (initialize_only)
  {
    exit_code = texpresso_main(&pstate);
    fprintf(stderr, "[info] Initialize mode: test completed\n");
  }
  else
  {
    while (texpresso_main(&pstate))
      ;
  }
  }
  fz_catch(ctx) {
    // A known library/input error must not become an uncaught-exception abort.
    // Stop this session: compiler/VFS mutations may already have taken place.
    report_engine_failure(ctx, headless);
    exit_code = 1;
  }

  if (renderer) SDL_DestroyRenderer(renderer);
  if (window) SDL_DestroyWindow(window);
  SDL_Quit();
  fz_drop_context(ctx);
  free(inclusion_path);

  return exit_code;
}
