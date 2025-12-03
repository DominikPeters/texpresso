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

#ifndef ENGINE_LIFECYCLE_H
#define ENGINE_LIFECYCLE_H

#include "engine.h"
#include "mupdf_compat.h"

/**
 * Find the tectonic executable path relative to the TeXpresso executable.
 *
 * @param tectonic_path Output buffer (must be at least 4096 bytes)
 * @param exec_path Path to the TeXpresso executable
 */
void engine_lifecycle_find_tectonic(char tectonic_path[4096], const char *exec_path);

/**
 * Create an appropriate engine based on document type.
 * Detects whether the document is PDF, DVI/XDV, or TeX and creates
 * the corresponding engine.
 *
 * @param ctx MuPDF context
 * @param doc_name Document filename (determines type by extension)
 * @param doc_path Directory containing the document
 * @param inclusion_path Additional search path for TeX files
 * @param exe_path Path to TeXpresso executable (for finding tectonic)
 * @return Created engine
 */
txp_engine *engine_lifecycle_create_engine(
    fz_context *ctx,
    const char *doc_name,
    const char *doc_path,
    const char *inclusion_path,
    const char *exe_path);

/**
 * Step the engine in a time-bounded manner.
 * Runs the engine for up to ~5ms, processing up to the specified page.
 *
 * @param ctx MuPDF context
 * @param eng Engine to step
 * @param target_page Continue stepping until this page is available
 * @param need_synctex Also wait for synctex data
 * @return true if more stepping is needed, false if done
 */
bool engine_lifecycle_step_bounded(
    fz_context *ctx,
    txp_engine *eng,
    int target_page,
    bool need_synctex);

/**
 * Get the current page count from the engine.
 *
 * @param eng Engine to query
 * @return Number of pages currently available
 */
int engine_lifecycle_get_page_count(txp_engine *eng);

/**
 * Get the current status of the engine.
 *
 * @param eng Engine to query
 * @return Engine status (DOC_RUNNING, DOC_TERMINATED, etc.)
 */
int engine_lifecycle_get_status(txp_engine *eng);

#endif /* ENGINE_LIFECYCLE_H */
