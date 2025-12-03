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

#include "core_loop.h"
#include "driver.h"
#include <stdlib.h>

core_loop_state *core_loop_create_parser(fz_context *ctx, int protocol)
{
  core_loop_state *state = malloc(sizeof(core_loop_state));
  if (!state)
    return NULL;

  state->cmd_stack = vstack_new(ctx);
  prot_initialize(&state->cmd_parser, (protocol == EDITOR_JSON));

  return state;
}

void core_loop_free_parser(fz_context *ctx, core_loop_state *state)
{
  if (!state)
    return;

  vstack_free(ctx, state->cmd_stack);
  free(state);
}

void core_loop_begin_transaction(txp_engine *eng, fz_context *ctx)
{
  send(begin_changes, eng, ctx);
}

bool core_loop_end_transaction(txp_engine *eng, fz_context *ctx)
{
  return send(end_changes, eng, ctx);
}
