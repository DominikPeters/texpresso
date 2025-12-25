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
 *
 * Supported commands (all output as JSON lines):
 *
 * Page structure:
 *   - beginPage: Start of page with dimensions
 *   - endPage: End of page
 *
 * Path operations:
 *   - fillPath: Fill a path with color
 *   - strokePath: Stroke a path with color
 *   - clipPath: Set clip region from path
 *   - clipStrokePath: Set clip region from stroked path
 *   - popClip: Restore previous clip
 *
 * Text operations:
 *   - fillText: Fill text with color
 *   - strokeText: Stroke text with color
 *   - clipText: Set clip region from text
 *   - clipStrokeText: Set clip region from stroked text
 *
 * Image operations (include filename when available for client-side caching):
 *   - fillImage: Draw an image (includes "filename" field if known)
 *   - fillImageMask: Use image as mask to paint color (includes "filename" field if known)
 *   - clipImageMask: Set clip region from image mask (includes "filename" field if known)
 *   Note: The "filename" field contains the source file path. Clients can use
 *   this to cache images by filename rather than fetching via imageId.
 *
 * Shading/gradient operations:
 *   - fillShade: Fill with gradient (linear, radial, or mesh)
 *
 * Transparency:
 *   - beginGroup: Start transparency group
 *   - endGroup: End transparency group
 *   - beginMask: Start soft mask definition
 *   - endMask: End soft mask definition
 *
 * Patterns:
 *   - beginTile: Start tile pattern definition
 *   - endTile: End tile pattern definition
 *
 * Layers:
 *   - beginLayer: Start optional content layer
 *   - endLayer: End optional content layer
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
 *   {"cmd":"fillText","color":"#000000","spans":[...]}
 *   {"cmd":"fillImage","imageId":1,"width":100,"height":100,"matrix":[...]}
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

/**
 * Reset the glyph path cache.
 * Call this when a new client connects or when the cache should be invalidated.
 * The client is responsible for discarding its local glyph cache when this is called.
 */
void cmd_device_reset_glyph_cache(void);

/* ============================================================================
 * Image Store API - for separate fetch of image data
 *
 * When the device encounters an image, it stores it and outputs only the image ID.
 * The client can then fetch the actual image data using these functions.
 * This approach keeps the command stream small and allows image caching.
 * ============================================================================ */

/**
 * Get an image by its ID.
 * Returns the fz_image pointer, or NULL if not found.
 * The returned image is owned by the store - do not drop it.
 *
 * @param id Image ID from a fillImage/fillImageMask/clipImageMask command
 * @return The image, or NULL if not found
 */
fz_image *cmd_device_get_image(int id);

/**
 * Get image data as PNG.
 * The returned buffer contains PNG-encoded image data.
 * Caller is responsible for dropping the buffer with fz_drop_buffer().
 *
 * @param ctx MuPDF context
 * @param id Image ID
 * @return Buffer containing PNG data, or NULL if image not found
 */
fz_buffer *cmd_device_get_image_as_png(fz_context *ctx, int id);

/**
 * Reset the image store.
 * Call this when a new client connects or when images should be invalidated.
 * All stored images are dropped.
 */
void cmd_device_reset_image_store(void);

#endif /* CMD_DEVICE_H */
