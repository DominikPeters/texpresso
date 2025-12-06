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

#include "cmd_device.h"
#include "json_writer.h"
#include <mupdf/fitz.h>
#include <stdio.h>

struct cmd_device
{
  fz_device super;
  fz_context *ctx;
  fz_buffer *output;
  int page_num;
  float width;
  float height;
  int clip_depth;
};

/* Helper to convert colorspace to RGB */
static void colorspace_to_rgb(fz_context *ctx, fz_colorspace *cs, const float *color,
                              float *r, float *g, float *b)
{
  if (!cs || fz_colorspace_is_gray(ctx, cs))
  {
    *r = *g = *b = color ? color[0] : 0;
  }
  else if (fz_colorspace_is_rgb(ctx, cs))
  {
    *r = color[0];
    *g = color[1];
    *b = color[2];
  }
  else if (fz_colorspace_is_cmyk(ctx, cs))
  {
    float c = color[0], m = color[1], y = color[2], k = color[3];
    *r = (1.0f - c) * (1.0f - k);
    *g = (1.0f - m) * (1.0f - k);
    *b = (1.0f - y) * (1.0f - k);
  }
  else
  {
    *r = *g = *b = 0;
  }
}

/* Path walking callbacks for JSON output */
typedef struct {
  fz_context *ctx;
  json_writer *jw;
} path_walk_ctx;

static void path_moveto(fz_context *ctx, void *arg, float x, float y)
{
  path_walk_ctx *pw = arg;
  json_write_object_start(pw->ctx, pw->jw);
  json_write_key(pw->ctx, pw->jw, "op");
  json_write_string(pw->ctx, pw->jw, "M");
  json_write_key(pw->ctx, pw->jw, "x");
  json_write_float(pw->ctx, pw->jw, x);
  json_write_key(pw->ctx, pw->jw, "y");
  json_write_float(pw->ctx, pw->jw, y);
  json_write_object_end(pw->ctx, pw->jw);
}

static void path_lineto(fz_context *ctx, void *arg, float x, float y)
{
  path_walk_ctx *pw = arg;
  json_write_object_start(pw->ctx, pw->jw);
  json_write_key(pw->ctx, pw->jw, "op");
  json_write_string(pw->ctx, pw->jw, "L");
  json_write_key(pw->ctx, pw->jw, "x");
  json_write_float(pw->ctx, pw->jw, x);
  json_write_key(pw->ctx, pw->jw, "y");
  json_write_float(pw->ctx, pw->jw, y);
  json_write_object_end(pw->ctx, pw->jw);
}

static void path_curveto(fz_context *ctx, void *arg,
                         float x1, float y1, float x2, float y2, float x3, float y3)
{
  path_walk_ctx *pw = arg;
  json_write_object_start(pw->ctx, pw->jw);
  json_write_key(pw->ctx, pw->jw, "op");
  json_write_string(pw->ctx, pw->jw, "C");
  json_write_key(pw->ctx, pw->jw, "x1");
  json_write_float(pw->ctx, pw->jw, x1);
  json_write_key(pw->ctx, pw->jw, "y1");
  json_write_float(pw->ctx, pw->jw, y1);
  json_write_key(pw->ctx, pw->jw, "x2");
  json_write_float(pw->ctx, pw->jw, x2);
  json_write_key(pw->ctx, pw->jw, "y2");
  json_write_float(pw->ctx, pw->jw, y2);
  json_write_key(pw->ctx, pw->jw, "x3");
  json_write_float(pw->ctx, pw->jw, x3);
  json_write_key(pw->ctx, pw->jw, "y3");
  json_write_float(pw->ctx, pw->jw, y3);
  json_write_object_end(pw->ctx, pw->jw);
}

static void path_closepath(fz_context *ctx, void *arg)
{
  path_walk_ctx *pw = arg;
  json_write_object_start(pw->ctx, pw->jw);
  json_write_key(pw->ctx, pw->jw, "op");
  json_write_string(pw->ctx, pw->jw, "Z");
  json_write_object_end(pw->ctx, pw->jw);
}

static const fz_path_walker path_walker = {
  path_moveto,
  path_lineto,
  path_curveto,
  path_closepath,
  NULL, // quadto
  NULL, // curvetov
  NULL, // curvetoy
  NULL  // rectto
};

/* Device callbacks */

static void
cmd_fill_text(fz_context *ctx, fz_device *dev_, const fz_text *text, fz_matrix ctm,
              fz_colorspace *colorspace, const float *color, float alpha,
              fz_color_params color_params)
{
  cmd_device *dev = (cmd_device *)dev_;
  json_writer *jw = json_writer_new(ctx, dev->output);

  float r, g, b;
  colorspace_to_rgb(ctx, colorspace, color, &r, &g, &b);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "fillText");

  json_write_key(ctx, jw, "color");
  json_write_color_rgb(ctx, jw, r, g, b);

  if (alpha < 1.0f)
  {
    json_write_key(ctx, jw, "alpha");
    json_write_float(ctx, jw, alpha);
  }

  // Output text spans with full glyph information
  json_write_key(ctx, jw, "spans");
  json_write_array_start(ctx, jw);

  for (fz_text_span *span = text->head; span; span = span->next)
  {
    json_write_object_start(ctx, jw);

    // Font information
    json_write_key(ctx, jw, "font");
    json_write_string(ctx, jw, fz_font_name(ctx, span->font));

    // Check if font has OpenType tables (for Unicode rendering)
    fz_font_flags_t *flags = fz_font_flags(span->font);
    int has_opentype = flags ? flags->has_opentype : 0;

    json_write_key(ctx, jw, "hasOpentype");
    json_write_bool(ctx, jw, has_opentype);

    // Transform matrix (text matrix concatenated with CTM)
    json_write_key(ctx, jw, "matrix");
    json_write_matrix(ctx, jw, fz_concat(span->trm, ctm));

    // Writing mode (horizontal or vertical)
    json_write_key(ctx, jw, "wmode");
    json_write_int(ctx, jw, span->wmode);

    // Glyphs array
    json_write_key(ctx, jw, "glyphs");
    json_write_array_start(ctx, jw);

    for (int i = 0; i < span->len; i++)
    {
      fz_text_item *item = &span->items[i];
      json_write_object_start(ctx, jw);

      json_write_key(ctx, jw, "gid");
      json_write_int(ctx, jw, item->gid);

      json_write_key(ctx, jw, "x");
      json_write_float(ctx, jw, item->x);

      json_write_key(ctx, jw, "y");
      json_write_float(ctx, jw, item->y);

      if (item->ucs >= 0)
      {
        json_write_key(ctx, jw, "ucs");
        json_write_int(ctx, jw, item->ucs);
      }

      // For non-OpenType fonts or missing/invalid Unicode, output glyph path
      // ucs <= 0 means no valid Unicode mapping (0 = NUL, -1 = undefined)
      if (!has_opentype || item->ucs <= 0)
      {
        fz_path *glyph_path = fz_outline_glyph(ctx, span->font, item->gid, fz_identity);
        if (glyph_path)
        {
          json_write_key(ctx, jw, "path");
          json_write_array_start(ctx, jw);
          path_walk_ctx pw = { ctx, jw };
          fz_walk_path(ctx, glyph_path, &path_walker, &pw);
          json_write_array_end(ctx, jw);
          fz_drop_path(ctx, glyph_path);
        }
      }

      json_write_object_end(ctx, jw);
    }

    json_write_array_end(ctx, jw);
    json_write_object_end(ctx, jw);
  }

  json_write_array_end(ctx, jw);
  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_stroke_text(fz_context *ctx, fz_device *dev_, const fz_text *text,
                const fz_stroke_state *stroke, fz_matrix ctm,
                fz_colorspace *colorspace, const float *color, float alpha,
                fz_color_params color_params)
{
  cmd_device *dev = (cmd_device *)dev_;
  json_writer *jw = json_writer_new(ctx, dev->output);

  float r, g, b;
  colorspace_to_rgb(ctx, colorspace, color, &r, &g, &b);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "strokeText");

  json_write_key(ctx, jw, "color");
  json_write_color_rgb(ctx, jw, r, g, b);

  json_write_key(ctx, jw, "lineWidth");
  json_write_float(ctx, jw, stroke->linewidth);

  if (alpha < 1.0f)
  {
    json_write_key(ctx, jw, "alpha");
    json_write_float(ctx, jw, alpha);
  }

  // Output text spans with full glyph information
  json_write_key(ctx, jw, "spans");
  json_write_array_start(ctx, jw);

  for (fz_text_span *span = text->head; span; span = span->next)
  {
    json_write_object_start(ctx, jw);

    // Font information
    json_write_key(ctx, jw, "font");
    json_write_string(ctx, jw, fz_font_name(ctx, span->font));

    // Check if font has OpenType tables (for Unicode rendering)
    fz_font_flags_t *flags = fz_font_flags(span->font);
    int has_opentype = flags ? flags->has_opentype : 0;

    json_write_key(ctx, jw, "hasOpentype");
    json_write_bool(ctx, jw, has_opentype);

    // Transform matrix
    json_write_key(ctx, jw, "matrix");
    json_write_matrix(ctx, jw, fz_concat(span->trm, ctm));

    // Writing mode
    json_write_key(ctx, jw, "wmode");
    json_write_int(ctx, jw, span->wmode);

    // Glyphs array
    json_write_key(ctx, jw, "glyphs");
    json_write_array_start(ctx, jw);

    for (int i = 0; i < span->len; i++)
    {
      fz_text_item *item = &span->items[i];
      json_write_object_start(ctx, jw);

      json_write_key(ctx, jw, "gid");
      json_write_int(ctx, jw, item->gid);

      json_write_key(ctx, jw, "x");
      json_write_float(ctx, jw, item->x);

      json_write_key(ctx, jw, "y");
      json_write_float(ctx, jw, item->y);

      if (item->ucs >= 0)
      {
        json_write_key(ctx, jw, "ucs");
        json_write_int(ctx, jw, item->ucs);
      }

      // For non-OpenType fonts or missing/invalid Unicode, output glyph path
      // ucs <= 0 means no valid Unicode mapping (0 = NUL, -1 = undefined)
      if (!has_opentype || item->ucs <= 0)
      {
        fz_path *glyph_path = fz_outline_glyph(ctx, span->font, item->gid, fz_identity);
        if (glyph_path)
        {
          json_write_key(ctx, jw, "path");
          json_write_array_start(ctx, jw);
          path_walk_ctx pw = { ctx, jw };
          fz_walk_path(ctx, glyph_path, &path_walker, &pw);
          json_write_array_end(ctx, jw);
          fz_drop_path(ctx, glyph_path);
        }
      }

      json_write_object_end(ctx, jw);
    }

    json_write_array_end(ctx, jw);
    json_write_object_end(ctx, jw);
  }

  json_write_array_end(ctx, jw);
  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_fill_path(fz_context *ctx, fz_device *dev_, const fz_path *path, int even_odd,
              fz_matrix ctm, fz_colorspace *colorspace, const float *color, float alpha,
              fz_color_params color_params)
{
  cmd_device *dev = (cmd_device *)dev_;
  json_writer *jw = json_writer_new(ctx, dev->output);

  float r, g, b;
  colorspace_to_rgb(ctx, colorspace, color, &r, &g, &b);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "fillPath");

  json_write_key(ctx, jw, "color");
  json_write_color_rgb(ctx, jw, r, g, b);

  if (alpha < 1.0f)
  {
    json_write_key(ctx, jw, "alpha");
    json_write_float(ctx, jw, alpha);
  }

  json_write_key(ctx, jw, "matrix");
  json_write_matrix(ctx, jw, ctm);

  if (even_odd)
  {
    json_write_key(ctx, jw, "fillRule");
    json_write_string(ctx, jw, "evenodd");
  }

  // Output complete path with all segments
  json_write_key(ctx, jw, "path");
  json_write_array_start(ctx, jw);

  path_walk_ctx pw = { ctx, jw };
  fz_walk_path(ctx, path, &path_walker, &pw);

  json_write_array_end(ctx, jw);

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_stroke_path(fz_context *ctx, fz_device *dev_, const fz_path *path,
                const fz_stroke_state *stroke, fz_matrix ctm,
                fz_colorspace *colorspace, const float *color, float alpha,
                fz_color_params color_params)
{
  cmd_device *dev = (cmd_device *)dev_;
  json_writer *jw = json_writer_new(ctx, dev->output);

  float r, g, b;
  colorspace_to_rgb(ctx, colorspace, color, &r, &g, &b);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "strokePath");

  json_write_key(ctx, jw, "color");
  json_write_color_rgb(ctx, jw, r, g, b);

  if (alpha < 1.0f)
  {
    json_write_key(ctx, jw, "alpha");
    json_write_float(ctx, jw, alpha);
  }

  json_write_key(ctx, jw, "matrix");
  json_write_matrix(ctx, jw, ctm);

  json_write_key(ctx, jw, "lineWidth");
  json_write_float(ctx, jw, stroke->linewidth);

  // Output complete path with all segments
  json_write_key(ctx, jw, "path");
  json_write_array_start(ctx, jw);

  path_walk_ctx pw = { ctx, jw };
  fz_walk_path(ctx, path, &path_walker, &pw);

  json_write_array_end(ctx, jw);

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_clip_path(fz_context *ctx, fz_device *dev_, const fz_path *path, int even_odd,
              fz_matrix ctm, fz_rect scissor)
{
  cmd_device *dev = (cmd_device *)dev_;
  dev->clip_depth++;

  // For now, just track clipping without outputting commands
  // A complete implementation would output clip regions
}

static void
cmd_pop_clip(fz_context *ctx, fz_device *dev_)
{
  cmd_device *dev = (cmd_device *)dev_;
  if (dev->clip_depth > 0)
    dev->clip_depth--;
}

/* Public API */

cmd_device *cmd_device_new(fz_context *ctx, fz_buffer *output, int page_num,
                          float width, float height)
{
  cmd_device *dev = fz_new_derived_device(ctx, cmd_device);

  dev->ctx = ctx;
  dev->output = fz_keep_buffer(ctx, output);
  dev->page_num = page_num;
  dev->width = width;
  dev->height = height;
  dev->clip_depth = 0;

  // Set up device callbacks
  dev->super.fill_path = cmd_fill_path;
  dev->super.stroke_path = cmd_stroke_path;
  dev->super.fill_text = cmd_fill_text;
  dev->super.stroke_text = cmd_stroke_text;
  dev->super.clip_path = cmd_clip_path;
  dev->super.pop_clip = cmd_pop_clip;

  // Output beginPage command
  json_writer *jw = json_writer_new(ctx, output);
  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "beginPage");
  json_write_key(ctx, jw, "page");
  json_write_int(ctx, jw, page_num);
  json_write_key(ctx, jw, "width");
  json_write_float(ctx, jw, width);
  json_write_key(ctx, jw, "height");
  json_write_float(ctx, jw, height);
  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);

  return dev;
}

fz_device *cmd_device_get_device(fz_context *ctx, cmd_device *cmd)
{
  return &cmd->super;
}

void cmd_device_flush(fz_context *ctx, cmd_device *dev)
{
  // Output endPage command
  json_writer *jw = json_writer_new(ctx, dev->output);
  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "endPage");
  json_write_key(ctx, jw, "page");
  json_write_int(ctx, jw, dev->page_num);
  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

void cmd_device_drop(fz_context *ctx, cmd_device *dev)
{
  if (dev)
  {
    fz_drop_buffer(ctx, dev->output);
    fz_drop_device(ctx, &dev->super);
  }
}
