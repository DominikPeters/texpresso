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

#include "headless_loop.h"
#include "engine_lifecycle.h"
#include "command_processor.h"
#include "core_loop.h"
#include "editor.h"
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>

// Callback for synctex forward search results
static void headless_synctex_callback(void *user_data, int page, int x, int y)
{
  const char *path = (const char *)user_data;

  // Output synctex result using editor_synctex()
  // This outputs in format: ["synctex", "path", line, column] for JSON
  editor_synctex("", path, strlen(path), page, x);
  fflush(stdout);

  fprintf(stderr, "[headless] synctex result: page=%d, x=%d, y=%d\n", page, x, y);
}

bool headless_loop_run(struct persistent_state *ps)
{
  fprintf(stderr, "[headless] starting headless mode\n");

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

  // Send ready message
  fprintf(stdout, "{\"type\":\"status\",\"state\":\"ready\"}\n");
  fflush(stdout);

  // Do initial compilation - similar to GUI mode's approach
  fprintf(stdout, "{\"type\":\"status\",\"state\":\"compiling\"}\n");
  fflush(stdout);

  send(step, eng, ps->ctx, true);

  // Continue stepping until compilation is complete
  // Use time-bounded stepping (5ms chunks) like the GUI mode does
  int step_count = 0;
  while (engine_lifecycle_step_bounded(ps->ctx, eng, 999, false))
  {
    // engine_lifecycle_step_bounded returns true if more work is needed
    // It internally limits itself to ~5ms of work per call
    step_count++;

    // Output progress every 10 steps to avoid flooding
    if (step_count % 10 == 0)
    {
      int current_pages = engine_lifecycle_get_page_count(eng);
      fprintf(stdout, "{\"type\":\"status\",\"state\":\"compiling\",\"pages\":%d}\n", current_pages);
      fflush(stdout);
    }
  }

  fprintf(stdout, "{\"type\":\"status\",\"state\":\"ready\"}\n");
  fflush(stdout);

  int page_count = engine_lifecycle_get_page_count(eng);
  fprintf(stderr, "[headless] initial compilation complete, %d pages\n", page_count);
  fprintf(stdout, "{\"type\":\"doc.pageCount\",\"count\":%d}\n", page_count);
  fflush(stdout);

  // Output initial rendering commands
  fprintf(stderr, "[headless] rendering %d pages to JSON commands\n", page_count);
  for (int i = 0; i < page_count; i++)
  {
    fz_buffer *output = fz_new_buffer(ps->ctx, 4096);
    fz_try(ps->ctx)
    {
      send(render_page_to_json, eng, ps->ctx, i, output);

      // Output the buffer contents to stdout
      unsigned char *data;
      size_t len = fz_buffer_storage(ps->ctx, output, &data);
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

  // Main event loop - matches GUI version structure
  bool stdin_eof = false;
  char buffer[4096];

  while (!stdin_eof)
  {
    // Begin transaction (matches GUI: send(begin_changes, ...))
    core_loop_begin_transaction(eng, ps->ctx);

    // Read all available stdin (matches GUI while loop pattern)
    ssize_t n = -1;
    while (!stdin_eof && (n = read(STDIN_FILENO, buffer, sizeof(buffer))) != 0)
    {
      if (n == -1)
      {
        if (errno == EINTR)
          continue;
        perror("[headless] read stdin");
        break;
      }

      fprintf(stderr, "[headless] received %zd bytes\n", n);

      // Parse JSON commands (matches GUI pattern)
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

            // Parse the editor command
            struct editor_command ecmd;
            if (!editor_parse(ps->ctx, parser->cmd_stack, cmd, &ecmd))
            {
              fprintf(stderr, "[headless] failed to parse command\n");
              continue;
            }

            fprintf(stderr, "[headless] received command type: %d\n", ecmd.tag);

            // Process the command using shared command processor
            switch (ecmd.tag)
            {
              case EDIT_OPEN:
                fprintf(stderr, "[headless] open file: %s\n", ecmd.open.path);
                command_processor_interpret_open(ps->ctx, eng, ps->doc_path,
                                                ecmd.open.path, ecmd.open.data, ecmd.open.length);
                break;

              case EDIT_CHANGE:
                fprintf(stderr, "[headless] change file: %s\n", ecmd.change.path);
                command_processor_interpret_change(ps->ctx, eng, ps->doc_path, &ecmd.change);
                break;

              case EDIT_CLOSE:
                fprintf(stderr, "[headless] close file: %s\n", ecmd.close.path);
                command_processor_interpret_close(ps->ctx, eng, ps->doc_path, ecmd.close.path);
                break;

              case EDIT_SYNCTEX_FORWARD:
                fprintf(stderr, "[headless] synctex forward: %s:%d\n",
                        ecmd.synctex_forward.path, ecmd.synctex_forward.line);
                command_processor_interpret_synctex_forward(
                    ps->ctx, eng, ps->doc_path,
                    0,  // current_page: use 0 (first page) for now
                    ecmd.synctex_forward.path,
                    ecmd.synctex_forward.line,
                    headless_synctex_callback,
                    (void *)ecmd.synctex_forward.path);
                break;

              case EDIT_RESCAN:
                fprintf(stderr, "[headless] rescan filesystem\n");
                send(detect_changes, eng, ps->ctx);
                break;

              default:
                fprintf(stderr, "[headless] unhandled command type: %d\n", ecmd.tag);
                break;
            }
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

      // Exit the read loop after one chunk in headless mode (stdin is blocking)
      // Unlike GUI which polls and reads multiple chunks
      break;
    }

    if (n == 0)
    {
      stdin_eof = true;
      fprintf(stderr, "[headless] stdin closed\n");
    }

    // Flush buffered changes before ending transaction
    command_processor_flush_changes(ps->ctx, eng, ps->doc_path);

    // End transaction (matches GUI: send(end_changes, ...))
    if (core_loop_end_transaction(eng, ps->ctx))
    {
      fprintf(stderr, "[headless] changes detected, recompiling\n");
      fprintf(stdout, "{\"type\":\"status\",\"state\":\"compiling\"}\n");
      fflush(stdout);

      send(step, eng, ps->ctx, true);

      // Continue stepping until compilation is complete
      step_count = 0;
      while (engine_lifecycle_step_bounded(ps->ctx, eng, 999, false))
      {
        // engine_lifecycle_step_bounded returns true if more work is needed
        step_count++;

        // Output progress every 10 steps to avoid flooding
        if (step_count % 10 == 0)
        {
          int current_pages = engine_lifecycle_get_page_count(eng);
          fprintf(stdout, "{\"type\":\"status\",\"state\":\"compiling\",\"pages\":%d}\n", current_pages);
          fflush(stdout);
        }
      }

      fprintf(stdout, "{\"type\":\"status\",\"state\":\"ready\"}\n");
      fflush(stdout);

      page_count = engine_lifecycle_get_page_count(eng);
      fprintf(stdout, "{\"type\":\"doc.pageCount\",\"count\":%d}\n", page_count);
      fflush(stdout);

      // Output rendering commands for all pages
      fprintf(stderr, "[headless] rendering %d pages to JSON commands\n", page_count);
      for (int i = 0; i < page_count; i++)
      {
        fz_buffer *output = fz_new_buffer(ps->ctx, 4096);
        fz_try(ps->ctx)
        {
          send(render_page_to_json, eng, ps->ctx, i, output);

          // Output the buffer contents to stdout
          unsigned char *data;
          size_t len = fz_buffer_storage(ps->ctx, output, &data);
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
    }
  }

  // Cleanup
  core_loop_free_parser(ps->ctx, parser);
  send(destroy, eng, ps->ctx);

  fprintf(stderr, "[headless] exiting\n");
  return false; // Don't reload
}
