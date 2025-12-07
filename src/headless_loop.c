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

/*
 * Headless loop implementation - faithfully mirrors the GUI main loop from main.c
 *
 * Key design principles (matching GUI behavior):
 * 1. Non-blocking stdin polling (like GUI's poll_stdin_thread + pipe)
 * 2. Time-sliced compilation (~5ms chunks, like GUI's advance_engine)
 * 3. Event-driven structure with similar state tracking
 * 4. Use of delayed_changes buffering via command_processor
 */

#include "headless_loop.h"
#include "engine_lifecycle.h"
#include "command_processor.h"
#include "core_loop.h"
#include "editor.h"
#include "synctex.h"
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <poll.h>

/* Headless UI state - mirrors ui_state from main.c */
typedef struct {
  txp_engine *eng;
  int page;                    // Current page (for synctex)
  int need_synctex;            // Whether we need synctex data
  bool advancing;              // Whether we're currently advancing the engine
  bool render_dirty;           // Set to true when content may have changed
} headless_state;

/* Event flags - mirrors custom_events from driver.h */
typedef struct {
  bool need_reload;    // Like RELOAD_EVENT - need to check/render pages
  bool need_stdin;     // Like STDIN_EVENT - stdin activity
  bool need_scan;      // Like SCAN_EVENT - filesystem rescan
} headless_events;

/* Non-blocking stdin polling - matches poll_stdin() from main.c */
static bool poll_stdin(void)
{
  struct pollfd fd;
  fd.fd = STDIN_FILENO;
  fd.events = POLLRDNORM;
  fd.revents = 0;
  return (poll(&fd, 1, 0) == 1) && ((fd.revents & POLLRDNORM) != 0);
}

/* Wait for stdin with timeout - for idle waiting when no work to do */
static bool wait_for_stdin(int timeout_ms)
{
  struct pollfd fd;
  fd.fd = STDIN_FILENO;
  fd.events = POLLRDNORM;
  fd.revents = 0;
  int result = poll(&fd, 1, timeout_ms);
  return (result == 1) && ((fd.revents & POLLRDNORM) != 0);
}

/* Check if engine needs more work - mirrors need_advance() from main.c */
static bool need_advance(fz_context *ctx, headless_state *hs)
{
  int need = send(page_count, hs->eng) <= hs->page;

  if (!need)
  {
    fz_buffer *buf;
    synctex_t *stx = send(synctex, hs->eng, &buf);
    need =
      (hs->need_synctex && synctex_page_count(stx) <= hs->page) ||
      synctex_has_target(stx);
  }

  return (need && send(get_status, hs->eng) == DOC_RUNNING);
}

/* Time-bounded engine advancement - mirrors advance_engine() from main.c */
static bool advance_engine(fz_context *ctx, headless_state *hs)
{
  bool need = need_advance(ctx, hs);
  if (!need && hs->advancing)
    editor_flush();
  hs->advancing = need;
  if (!need)
    return false;

  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  int steps = 10;
  while (need)
  {
    if (!send(step, hs->eng, ctx, false))
      break;

    steps -= 1;
    need = need_advance(ctx, hs);

    if (steps == 0)
    {
      steps = 10;

      struct timespec curr;
      clock_gettime(CLOCK_MONOTONIC, &curr);

      int delta =
        (curr.tv_sec - start.tv_sec) * 1000 * 1000 * 1000 +
        (curr.tv_nsec - start.tv_nsec);

      // Break after ~5ms (same as GUI)
      if (delta > 5000000)
        break;
    }
  }
  return need;
}

/* Callback for synctex forward search results */
static void headless_synctex_callback(void *user_data, int page, int x, int y)
{
  const char *path = (const char *)user_data;

  // Output synctex result using editor_synctex()
  editor_synctex("", path, strlen(path), page, x);
  fflush(stdout);

  fprintf(stderr, "[headless] synctex result: page=%d, x=%d, y=%d\n", page, x, y);
}

/* Render all pages to JSON output */
static void render_pages_to_json(struct persistent_state *ps, headless_state *hs)
{
  int page_count = send(page_count, hs->eng);

  // Only re-render if marked dirty (content may have changed)
  if (!hs->render_dirty)
    return;

  hs->render_dirty = false;

  fprintf(stdout, "{\"type\":\"doc.pageCount\",\"count\":%d}\n", page_count);
  fflush(stdout);

  fprintf(stderr, "[headless] rendering %d pages to JSON commands\n", page_count);
  for (int i = 0; i < page_count; i++)
  {
    fz_buffer *output = fz_new_buffer(ps->ctx, 4096);
    fz_try(ps->ctx)
    {
      send(render_page_to_json, hs->eng, ps->ctx, i, output);

      // Output the buffer contents to stdout
      unsigned char *data;
      size_t len = fz_buffer_storage(ps->ctx, output, &data);
      fprintf(stderr, "[headless] rendered page %d: %zu bytes\n", i, len);
      if (len > 0)
      {
        fwrite(data, 1, len, stdout);
        fflush(stdout);
      }
    }
    fz_always(ps->ctx)
    {
      fz_drop_buffer(ps->ctx, output);
    }
    fz_catch(ps->ctx)
    {
      fprintf(stderr, "[headless] error rendering page %d: %s\n",
              i, fz_caught_message(ps->ctx));
      fprintf(stdout, "{\"type\":\"error\",\"code\":\"RENDER_ERROR\",\"page\":%d,\"message\":\"%s\"}\n",
              i, fz_caught_message(ps->ctx));
      fflush(stdout);
    }
  }
  fprintf(stderr, "[headless] rendering complete\n");
}

/* Process a single parsed command - mirrors interpret_command() from main.c */
static void interpret_command(struct persistent_state *ps,
                              headless_state *hs,
                              headless_events *events,
                              struct editor_command *ecmd)
{
  switch (ecmd->tag)
  {
    case EDIT_OPEN:
      fprintf(stderr, "[headless] open file: %s\n", ecmd->open.path);
      command_processor_interpret_open(ps->ctx, hs->eng, ps->doc_path,
                                       ecmd->open.path, ecmd->open.data, ecmd->open.length);
      break;

    case EDIT_CHANGE:
      fprintf(stderr, "[headless] change file: %s\n", ecmd->change.path);
      command_processor_interpret_change(ps->ctx, hs->eng, ps->doc_path, &ecmd->change);
      break;

    case EDIT_CLOSE:
      fprintf(stderr, "[headless] close file: %s\n", ecmd->close.path);
      command_processor_interpret_close(ps->ctx, hs->eng, ps->doc_path, ecmd->close.path);
      break;

    case EDIT_SYNCTEX_FORWARD:
    {
      fprintf(stderr, "[headless] synctex forward: %s:%d\n",
              ecmd->synctex_forward.path, ecmd->synctex_forward.line);

      // Set target like GUI does, then let advance_engine find it
      fz_buffer *buf;
      synctex_t *stx = send(synctex, hs->eng, &buf);

      // Get relative path (simplified - command_processor has full logic)
      const char *path = ecmd->synctex_forward.path;

      synctex_set_target(stx, hs->page, path, ecmd->synctex_forward.line);
      events->need_stdin = true;  // Schedule event like GUI does
      break;
    }

    case EDIT_RESCAN:
      fprintf(stderr, "[headless] rescan filesystem\n");
      events->need_scan = true;
      break;

    case EDIT_PREVIOUS_PAGE:
      synctex_set_target(send(synctex, hs->eng, NULL), 0, NULL, 0);
      if (hs->page > 0)
      {
        hs->page -= 1;
        int page_count = send(page_count, hs->eng);
        if (page_count > 0 && hs->page >= page_count &&
            send(get_status, hs->eng) == DOC_TERMINATED)
          hs->page = page_count - 1;
        events->need_reload = true;
      }
      break;

    case EDIT_NEXT_PAGE:
      synctex_set_target(send(synctex, hs->eng, NULL), 0, NULL, 0);
      hs->page += 1;
      events->need_reload = true;
      break;

    default:
      fprintf(stderr, "[headless] unhandled command type: %d\n", ecmd->tag);
      break;
  }
}

bool headless_loop_run(struct persistent_state *ps)
{
  fprintf(stderr, "[headless] starting headless mode (event-driven)\n");

  // Set up protocol (always use JSON in headless mode)
  editor_set_protocol(EDITOR_JSON);
  editor_set_line_output(ps->line_output);

  // Create protocol parser
  core_loop_state *parser = core_loop_create_parser(ps->ctx, EDITOR_JSON);
  if (!parser)
  {
    fprintf(stderr, "[headless] failed to create parser\n");
    return false;
  }

  // Create engine using shared module
  txp_engine *eng = engine_lifecycle_create_engine(
      ps->ctx, ps->doc_name, ps->doc_path, ps->inclusion_path, ps->exe_path);

  if (!eng)
  {
    fprintf(stderr, "[headless] failed to create engine\n");
    core_loop_free_parser(ps->ctx, parser);
    return false;
  }

  // Initialize headless state - mirrors ui_state initialization in main.c
  headless_state hs = {
    .eng = eng,
    .page = 0,
    .need_synctex = 1,
    .advancing = false,
    .render_dirty = true,  // Force initial render
  };

  headless_events events = {
    .need_reload = false,
    .need_stdin = false,
    .need_scan = false,
  };

  // Send ready message
  fprintf(stdout, "{\"type\":\"status\",\"state\":\"ready\"}\n");
  fflush(stdout);

  // Do initial step - matches GUI: send(step, ui->eng, ps->ctx, true);
  send(step, eng, ps->ctx, true);
  events.need_reload = true;

  bool stdin_eof = false;
  char buffer[4096];

  // Main event loop - structured to match GUI's while (!quit) loop
  while (!stdin_eof)
  {
    // ========== STDIN PROCESSING ==========
    // Matches GUI's stdin processing block (lines 1128-1172 in main.c)

    // Begin transaction
    send(begin_changes, eng, ps->ctx);

    // Read all available stdin (non-blocking) - matches GUI's while loop
    int n = -1;
    while (!stdin_eof && poll_stdin() && (n = read(STDIN_FILENO, buffer, sizeof(buffer))) != 0)
    {
      if (n == -1)
      {
        if (errno == EINTR)
          continue;
        perror("[headless] poll stdin");
        break;
      }

      fprintf(stderr, "[headless] received %d bytes\n", n);

      // Parse JSON commands
      const char *ptr = buffer, *lim = buffer + n;
      fz_try(ps->ctx)
      {
        while ((ptr = prot_parse(ps->ctx, &parser->cmd_parser, parser->cmd_stack, ptr, lim)))
        {
          val cmds = vstack_get_values(ps->ctx, parser->cmd_stack);
          int n_cmds = val_array_length(ps->ctx, parser->cmd_stack, cmds);

          for (int i = 0; i < n_cmds; i++)
          {
            val cmd = val_array_get(ps->ctx, parser->cmd_stack, cmds, i);

            struct editor_command ecmd;
            if (!editor_parse(ps->ctx, parser->cmd_stack, cmd, &ecmd))
            {
              fprintf(stderr, "[headless] failed to parse command\n");
              continue;
            }

            interpret_command(ps, &hs, &events, &ecmd);
          }
        }
      }
      fz_catch(ps->ctx)
      {
        fprintf(stderr, "[headless] error parsing command: %s\n",
                fz_caught_message(ps->ctx));
        fprintf(stdout, "{\"type\":\"error\",\"code\":\"PARSE_ERROR\",\"message\":\"%s\"}\n",
                fz_caught_message(ps->ctx));
        fflush(stdout);
        vstack_reset(ps->ctx, parser->cmd_stack);
        prot_reinitialize(&parser->cmd_parser);
      }
    }

    if (n == 0)
      stdin_eof = true;

    // Flush buffered changes before ending transaction
    command_processor_flush_changes(ps->ctx, eng, ps->doc_path);

    // End transaction - if changes detected, step and schedule reload
    // Matches GUI: if (send(end_changes, ui->eng, ps->ctx)) { ... }
    if (send(end_changes, eng, ps->ctx))
    {
      fprintf(stderr, "[headless] changes detected, triggering recompilation\n");
      fprintf(stdout, "{\"type\":\"status\",\"state\":\"compiling\"}\n");
      fflush(stdout);

      send(step, eng, ps->ctx, true);
      events.need_reload = true;
      hs.render_dirty = true;  // Mark for re-render
    }

    // ========== DOCUMENT PROCESSING ==========
    // Matches GUI's document processing block (lines 1174-1237 in main.c)
    {
      int before_page_count = send(page_count, eng);
      bool advance = advance_engine(ps->ctx, &hs);
      int after_page_count = send(page_count, eng);
      fflush(stdout);

      // Check if current page became available
      if (hs.page >= before_page_count && hs.page < after_page_count)
        events.need_reload = true;

      // If still advancing, continue loop without waiting
      if (advance)
        continue;

      // If no work to do and stdin not closed, wait for input
      // This replaces GUI's SDL_WaitEvent
      if (!stdin_eof && !events.need_reload && !events.need_scan)
      {
        // Wait for stdin with timeout (100ms to allow periodic checks)
        wait_for_stdin(100);
        continue;
      }

      // Process synctex target if any
      fz_buffer *buf;
      synctex_t *stx = send(synctex, eng, &buf);
      int page = -1, x = -1, y = -1;
      if (synctex_find_target(ps->ctx, stx, buf, &page, &x, &y))
      {
        fprintf(stderr, "[headless] synctex forward: hit page %d, coordinates (%d, %d)\n",
                page, x, y);

        if (page != hs.page)
        {
          hs.page = page;
          events.need_reload = true;
        }

        // Output synctex result
        // (In GUI this would scroll the view; here we just report it)
        fprintf(stdout, "{\"type\":\"synctex\",\"page\":%d,\"x\":%d,\"y\":%d}\n",
                page, x, y);
        fflush(stdout);
      }
    }

    // ========== EVENT PROCESSING ==========
    // Matches GUI's event processing block (lines 1367-1415 in main.c)

    if (events.need_scan)
    {
      events.need_scan = false;
      send(begin_changes, eng, ps->ctx);
      command_processor_flush_changes(ps->ctx, eng, ps->doc_path);
      send(detect_changes, eng, ps->ctx);
      if (send(end_changes, eng, ps->ctx))
      {
        send(step, eng, ps->ctx, true);
        events.need_reload = true;
        hs.render_dirty = true;
      }
    }

    if (events.need_reload)
    {
      events.need_reload = false;

      int page_count = send(page_count, eng);

      // Adjust page if beyond available pages (like GUI's RELOAD_EVENT handler)
      if (hs.page >= page_count && send(get_status, eng) == DOC_TERMINATED)
      {
        if (page_count > 0)
          hs.page = page_count - 1;
      }

      // Render pages first (if dirty)
      if (hs.page < page_count)
      {
        render_pages_to_json(ps, &hs);
      }

      // Send "ready" status after rendering is complete
      // We report "ready" when we've rendered all available pages,
      // regardless of DOC_RUNNING vs DOC_TERMINATED
      // (DOC_RUNNING just means the TeX process is still alive for more edits)
      fprintf(stdout, "{\"type\":\"status\",\"state\":\"ready\",\"pages\":%d}\n", page_count);
      fflush(stdout);
    }

    // Reset stdin event flag
    events.need_stdin = false;
  }

  // Cleanup
  core_loop_free_parser(ps->ctx, parser);
  send(destroy, eng, ps->ctx);

  fprintf(stderr, "[headless] exiting\n");
  return false; // Don't reload
}
