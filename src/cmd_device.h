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

#ifndef CMD_DEVICE_H
#define CMD_DEVICE_H

#include <mupdf/fitz.h>
#include "mupdf_compat.h"

/**
 * Opaque command device structure.
 * This device intercepts MuPDF rendering operations and outputs
 * JSON commands suitable for rendering in a web browser.
 */
typedef struct cmd_device cmd_device;

/**
 * Create a new command device that outputs JSON to a buffer.
 *
 * The device will intercept all rendering operations (text, paths, images, etc.)
 * and write them as JSON commands to the output buffer. Each command is a single
 * JSON object on its own line.
 *
 * Example output:
 *   {"cmd":"beginPage","page":1,"width":612,"height":792}
 *   {"cmd":"text","font":1,"matrix":[1,0,0,1,72,720],"glyphs":[...]}
 *   {"cmd":"endPage","page":1}
 *
 * @param ctx MuPDF context
 * @param output Buffer to write JSON commands to
 * @param page_num Page number being rendered
 * @param width Page width in points
 * @param height Page height in points
 * @return Newly created command device
 */
cmd_device *cmd_device_new(
    fz_context *ctx,
    fz_buffer *output,
    int page_num,
    float width,
    float height);

/**
 * Get the underlying fz_device for rendering operations.
 * Pass this device to incdvi_render_page or other rendering functions.
 *
 * @param ctx MuPDF context
 * @param cmd Command device
 * @return The underlying MuPDF device
 */
fz_device *cmd_device_get_device(fz_context *ctx, cmd_device *cmd);

/**
 * Finalize and flush any pending commands.
 * Outputs the endPage command and ensures all data is written to the buffer.
 *
 * @param ctx MuPDF context
 * @param cmd Command device to flush
 */
void cmd_device_flush(fz_context *ctx, cmd_device *cmd);

/**
 * Free the command device.
 * This also drops the underlying MuPDF device.
 *
 * @param ctx MuPDF context
 * @param cmd Command device to free
 */
void cmd_device_drop(fz_context *ctx, cmd_device *cmd);

#endif /* CMD_DEVICE_H */
