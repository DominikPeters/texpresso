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

#ifndef CORE_LOOP_H
#define CORE_LOOP_H

#include "engine.h"
#include "prot_parser.h"
#include "vstack.h"
#include "sprotocol.h"
#include "mupdf_compat.h"

/**
 * Opaque state for protocol parsing.
 */
typedef struct core_loop_state {
  vstack *cmd_stack;
  prot_parser cmd_parser;
} core_loop_state;

/**
 * Create a new protocol parser state.
 *
 * @param ctx MuPDF context
 * @param protocol Protocol type (EDITOR_SEXP or EDITOR_JSON)
 * @return Newly allocated parser state
 */
core_loop_state *core_loop_create_parser(fz_context *ctx, int protocol);

/**
 * Free the protocol parser state.
 *
 * @param ctx MuPDF context
 * @param state Parser state to free
 */
void core_loop_free_parser(fz_context *ctx, core_loop_state *state);

/**
 * Begin a change transaction with the engine.
 * Should be called before processing a batch of commands.
 *
 * @param eng Engine to notify
 * @param ctx MuPDF context
 */
void core_loop_begin_transaction(txp_engine *eng, fz_context *ctx);

/**
 * End a change transaction with the engine.
 * Returns true if changes were detected and engine should be stepped.
 *
 * @param eng Engine to notify
 * @param ctx MuPDF context
 * @return true if changes were made, false otherwise
 */
bool core_loop_end_transaction(txp_engine *eng, fz_context *ctx);

#endif /* CORE_LOOP_H */
