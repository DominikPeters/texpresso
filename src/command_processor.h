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

#ifndef COMMAND_PROCESSOR_H
#define COMMAND_PROCESSOR_H

#include "engine.h"
#include "editor.h"
#include "mupdf_compat.h"

/**
 * Flush any buffered file changes and apply them to the engine.
 *
 * @param ctx MuPDF context
 * @param eng Engine to apply changes to
 * @param doc_path Base document path for resolving relative paths
 */
void command_processor_flush_changes(
    fz_context *ctx,
    txp_engine *eng,
    const char *doc_path);

/**
 * Process an OPEN command - open a file in the virtual file system.
 *
 * @param ctx MuPDF context
 * @param eng Engine to apply changes to
 * @param doc_path Base document path for resolving relative paths
 * @param path Path to the file to open
 * @param data File contents
 * @param size Size of file contents
 */
void command_processor_interpret_open(
    fz_context *ctx,
    txp_engine *eng,
    const char *doc_path,
    const char *path,
    const void *data,
    int size);

/**
 * Process a CLOSE command - close a file in the virtual file system.
 *
 * @param ctx MuPDF context
 * @param eng Engine to apply changes to
 * @param doc_path Base document path for resolving relative paths
 * @param path Path to the file to close
 */
void command_processor_interpret_close(
    fz_context *ctx,
    txp_engine *eng,
    const char *doc_path,
    const char *path);

/**
 * Process a CHANGE command - modify a file in the virtual file system.
 * Changes may be buffered for performance.
 *
 * @param ctx MuPDF context
 * @param eng Engine to apply changes to
 * @param doc_path Base document path for resolving relative paths
 * @param op Change operation to apply
 */
void command_processor_interpret_change(
    fz_context *ctx,
    txp_engine *eng,
    const char *doc_path,
    struct editor_change *op);

/**
 * Callback type for SyncTeX results.
 * Called with the page number and coordinates when SyncTeX forward search finds a target.
 *
 * @param user_data User-provided data
 * @param page Page number
 * @param x X coordinate
 * @param y Y coordinate
 */
typedef void (*synctex_result_callback)(void *user_data, int page, int x, int y);

/**
 * Process a SYNCTEX_FORWARD command - find position in PDF from source location.
 *
 * @param ctx MuPDF context
 * @param eng Engine to query
 * @param doc_path Base document path for resolving relative paths
 * @param current_page Current page being viewed
 * @param path Source file path
 * @param line Line number in source
 * @param callback Callback to receive results
 * @param user_data User data passed to callback
 */
void command_processor_interpret_synctex_forward(
    fz_context *ctx,
    txp_engine *eng,
    const char *doc_path,
    int current_page,
    const char *path,
    int line,
    synctex_result_callback callback,
    void *user_data);

#endif /* COMMAND_PROCESSOR_H */
