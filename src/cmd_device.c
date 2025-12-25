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
#include "dvi/mydvi.h"
#include <mupdf/fitz.h>
#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Glyph cache for avoiding repeated path transmission
 * ============================================================================ */

#define GLYPH_CACHE_SIZE 4096

typedef struct {
  fz_font *font;      // Font pointer (used as identifier)
  int gid;            // Glyph ID within font
  int cache_id;       // Assigned cache ID for client reference
} glyph_cache_entry;

static glyph_cache_entry glyph_cache[GLYPH_CACHE_SIZE];
static int glyph_cache_next_id = 1;  // Start from 1, 0 means not cached

/* Simple hash function for font+glyph lookup */
static unsigned int glyph_hash(fz_font *font, int gid)
{
  return ((unsigned int)(uintptr_t)font * 31 + (unsigned int)gid) % GLYPH_CACHE_SIZE;
}

/* Look up glyph in cache, returns cache_id or 0 if not found */
static int glyph_cache_lookup(fz_font *font, int gid)
{
  unsigned int hash = glyph_hash(font, gid);
  int probes = 0;

  while (probes < GLYPH_CACHE_SIZE)
  {
    glyph_cache_entry *e = &glyph_cache[hash];
    if (e->font == NULL)
      return 0;  // Empty slot, not found
    if (e->font == font && e->gid == gid)
      return e->cache_id;  // Found
    hash = (hash + 1) % GLYPH_CACHE_SIZE;
    probes++;
  }
  return 0;  // Cache full, not found
}

/* Insert glyph into cache, returns assigned cache_id */
static int glyph_cache_insert(fz_font *font, int gid)
{
  unsigned int hash = glyph_hash(font, gid);
  int probes = 0;

  while (probes < GLYPH_CACHE_SIZE)
  {
    glyph_cache_entry *e = &glyph_cache[hash];
    if (e->font == NULL)
    {
      // Empty slot, insert here
      e->font = font;
      e->gid = gid;
      e->cache_id = glyph_cache_next_id++;
      return e->cache_id;
    }
    if (e->font == font && e->gid == gid)
      return e->cache_id;  // Already exists
    hash = (hash + 1) % GLYPH_CACHE_SIZE;
    probes++;
  }
  return 0;  // Cache full, couldn't insert
}

/* Reset glyph cache (call when client disconnects) */
void cmd_device_reset_glyph_cache(void)
{
  memset(glyph_cache, 0, sizeof(glyph_cache));
  glyph_cache_next_id = 1;
}

/* ============================================================================
 * Image store for separate fetch
 * ============================================================================ */

#define IMAGE_STORE_SIZE 256

typedef struct {
  int id;
  fz_image *image;
  fz_context *ctx;  // Context for dropping image
} image_store_entry;

static image_store_entry image_store[IMAGE_STORE_SIZE];
static int image_store_next_id = 1;

/* Store an image and return its ID */
static int image_store_add(fz_context *ctx, fz_image *image)
{
  // Check if image is already stored (by pointer)
  for (int i = 0; i < IMAGE_STORE_SIZE; i++)
  {
    if (image_store[i].image == image)
      return image_store[i].id;
  }

  // Find empty slot
  for (int i = 0; i < IMAGE_STORE_SIZE; i++)
  {
    if (image_store[i].image == NULL)
    {
      image_store[i].id = image_store_next_id++;
      image_store[i].image = fz_keep_image(ctx, image);
      image_store[i].ctx = ctx;
      return image_store[i].id;
    }
  }

  // Store full, evict oldest (simple FIFO - slot 0)
  if (image_store[0].image)
    fz_drop_image(image_store[0].ctx, image_store[0].image);
  memmove(&image_store[0], &image_store[1], sizeof(image_store_entry) * (IMAGE_STORE_SIZE - 1));
  image_store[IMAGE_STORE_SIZE - 1].id = image_store_next_id++;
  image_store[IMAGE_STORE_SIZE - 1].image = fz_keep_image(ctx, image);
  image_store[IMAGE_STORE_SIZE - 1].ctx = ctx;
  return image_store[IMAGE_STORE_SIZE - 1].id;
}

/* Get image by ID */
fz_image *cmd_device_get_image(int id)
{
  for (int i = 0; i < IMAGE_STORE_SIZE; i++)
  {
    if (image_store[i].id == id)
      return image_store[i].image;
  }
  return NULL;
}

/* Reset image store */
void cmd_device_reset_image_store(void)
{
  for (int i = 0; i < IMAGE_STORE_SIZE; i++)
  {
    if (image_store[i].image)
    {
      fz_drop_image(image_store[i].ctx, image_store[i].image);
      image_store[i].image = NULL;
    }
    image_store[i].id = 0;
  }
  image_store_next_id = 1;
}

/* Get image as PNG data (base64 encoded) */
fz_buffer *cmd_device_get_image_as_png(fz_context *ctx, int id)
{
  fz_image *image = cmd_device_get_image(id);
  if (!image)
    return NULL;

  fz_buffer *buf = NULL;
  fz_pixmap *pix = NULL;

  fz_try(ctx)
  {
    pix = fz_get_pixmap_from_image(ctx, image, NULL, NULL, NULL, NULL);
    buf = fz_new_buffer_from_pixmap_as_png(ctx, pix, fz_default_color_params);
  }
  fz_always(ctx)
  {
    fz_drop_pixmap(ctx, pix);
  }
  fz_catch(ctx)
  {
    fz_rethrow(ctx);
  }

  return buf;
}

/* ============================================================================
 * Device structure
 * ============================================================================ */

struct cmd_device
{
  fz_device super;
  fz_context *ctx;
  fz_buffer *output;
  int page_num;
  float width;
  float height;
  int clip_depth;
  int mask_depth;
  int group_depth;
  int tile_depth;
};

/* ============================================================================
 * Helper functions
 * ============================================================================ */

/* Convert colorspace to RGB */
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

/* Get blend mode name */
static const char *blend_mode_name(int blendmode)
{
  switch (blendmode & FZ_BLEND_MODEMASK)
  {
    case FZ_BLEND_NORMAL: return "normal";
    case FZ_BLEND_MULTIPLY: return "multiply";
    case FZ_BLEND_SCREEN: return "screen";
    case FZ_BLEND_OVERLAY: return "overlay";
    case FZ_BLEND_DARKEN: return "darken";
    case FZ_BLEND_LIGHTEN: return "lighten";
    case FZ_BLEND_COLOR_DODGE: return "color-dodge";
    case FZ_BLEND_COLOR_BURN: return "color-burn";
    case FZ_BLEND_HARD_LIGHT: return "hard-light";
    case FZ_BLEND_SOFT_LIGHT: return "soft-light";
    case FZ_BLEND_DIFFERENCE: return "difference";
    case FZ_BLEND_EXCLUSION: return "exclusion";
    case FZ_BLEND_HUE: return "hue";
    case FZ_BLEND_SATURATION: return "saturation";
    case FZ_BLEND_COLOR: return "color";
    case FZ_BLEND_LUMINOSITY: return "luminosity";
    default: return "normal";
  }
}

/* ============================================================================
 * Path walking callbacks for JSON output
 * ============================================================================ */

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

/* Helper to write path to JSON */
static void write_path(fz_context *ctx, json_writer *jw, const fz_path *path)
{
  json_write_key(ctx, jw, "path");
  json_write_array_start(ctx, jw);
  path_walk_ctx pw = { ctx, jw };
  fz_walk_path(ctx, path, &path_walker, &pw);
  json_write_array_end(ctx, jw);
}

/* Helper to write stroke state to JSON */
static void write_stroke_state(fz_context *ctx, json_writer *jw, const fz_stroke_state *stroke)
{
  json_write_key(ctx, jw, "lineWidth");
  json_write_float(ctx, jw, stroke->linewidth);

  if (stroke->start_cap != 0)
  {
    json_write_key(ctx, jw, "lineCap");
    json_write_int(ctx, jw, stroke->start_cap);
  }

  if (stroke->linejoin != 0)
  {
    json_write_key(ctx, jw, "lineJoin");
    json_write_int(ctx, jw, stroke->linejoin);
  }

  if (stroke->miterlimit != 10.0f)
  {
    json_write_key(ctx, jw, "miterLimit");
    json_write_float(ctx, jw, stroke->miterlimit);
  }

  if (stroke->dash_len > 0)
  {
    json_write_key(ctx, jw, "dashArray");
    json_write_array_start(ctx, jw);
    for (int i = 0; i < stroke->dash_len; i++)
      json_write_float(ctx, jw, stroke->dash_list[i]);
    json_write_array_end(ctx, jw);

    json_write_key(ctx, jw, "dashPhase");
    json_write_float(ctx, jw, stroke->dash_phase);
  }
}

/* Helper to write text spans (shared by fill_text and stroke_text) */
static void write_text_spans(fz_context *ctx, json_writer *jw, const fz_text *text, fz_matrix ctm)
{
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

      // For non-OpenType fonts or missing/invalid Unicode, output glyph path or cache reference
      if (!has_opentype || item->ucs <= 0)
      {
        int cache_id = glyph_cache_lookup(span->font, item->gid);
        if (cache_id > 0)
        {
          json_write_key(ctx, jw, "cid");
          json_write_int(ctx, jw, cache_id);
        }
        else
        {
          fz_path *glyph_path = fz_outline_glyph(ctx, span->font, item->gid, fz_identity);
          if (glyph_path)
          {
            cache_id = glyph_cache_insert(span->font, item->gid);
            if (cache_id > 0)
            {
              json_write_key(ctx, jw, "cid");
              json_write_int(ctx, jw, cache_id);
            }
            json_write_key(ctx, jw, "path");
            json_write_array_start(ctx, jw);
            path_walk_ctx pw = { ctx, jw };
            fz_walk_path(ctx, glyph_path, &path_walker, &pw);
            json_write_array_end(ctx, jw);
            fz_drop_path(ctx, glyph_path);
          }
        }
      }

      json_write_object_end(ctx, jw);
    }

    json_write_array_end(ctx, jw);
    json_write_object_end(ctx, jw);
  }

  json_write_array_end(ctx, jw);
}

/* ============================================================================
 * Device callbacks - Text
 * ============================================================================ */

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

  write_text_spans(ctx, jw, text, ctm);

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

  write_stroke_state(ctx, jw, stroke);

  if (alpha < 1.0f)
  {
    json_write_key(ctx, jw, "alpha");
    json_write_float(ctx, jw, alpha);
  }

  write_text_spans(ctx, jw, text, ctm);

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_clip_text(fz_context *ctx, fz_device *dev_, const fz_text *text,
              fz_matrix ctm, fz_rect scissor)
{
  cmd_device *dev = (cmd_device *)dev_;
  dev->clip_depth++;

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "clipText");

  write_text_spans(ctx, jw, text, ctm);

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_clip_stroke_text(fz_context *ctx, fz_device *dev_, const fz_text *text,
                     const fz_stroke_state *stroke, fz_matrix ctm, fz_rect scissor)
{
  cmd_device *dev = (cmd_device *)dev_;
  dev->clip_depth++;

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "clipStrokeText");

  write_stroke_state(ctx, jw, stroke);
  write_text_spans(ctx, jw, text, ctm);

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_ignore_text(fz_context *ctx, fz_device *dev_, const fz_text *text, fz_matrix ctm)
{
  // Ignore text is used for invisible text (e.g., for searching in scanned PDFs)
  // We don't need to output anything for this
}

/* ============================================================================
 * Device callbacks - Paths
 * ============================================================================ */

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

  write_path(ctx, jw, path);

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

  write_stroke_state(ctx, jw, stroke);
  write_path(ctx, jw, path);

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

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "clipPath");

  json_write_key(ctx, jw, "matrix");
  json_write_matrix(ctx, jw, ctm);

  if (even_odd)
  {
    json_write_key(ctx, jw, "fillRule");
    json_write_string(ctx, jw, "evenodd");
  }

  write_path(ctx, jw, path);

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_clip_stroke_path(fz_context *ctx, fz_device *dev_, const fz_path *path,
                     const fz_stroke_state *stroke, fz_matrix ctm, fz_rect scissor)
{
  cmd_device *dev = (cmd_device *)dev_;
  dev->clip_depth++;

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "clipStrokePath");

  json_write_key(ctx, jw, "matrix");
  json_write_matrix(ctx, jw, ctm);

  write_stroke_state(ctx, jw, stroke);
  write_path(ctx, jw, path);

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_pop_clip(fz_context *ctx, fz_device *dev_)
{
  cmd_device *dev = (cmd_device *)dev_;
  if (dev->clip_depth > 0)
    dev->clip_depth--;

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "popClip");
  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

/* ============================================================================
 * Device callbacks - Images
 * ============================================================================ */

static void
cmd_fill_image(fz_context *ctx, fz_device *dev_, fz_image *image,
               fz_matrix ctm, float alpha, fz_color_params color_params)
{
  cmd_device *dev = (cmd_device *)dev_;

  // Store image and get ID for separate fetch
  int image_id = image_store_add(ctx, image);

  // Look up the source filename if available
  const char *filename = dvi_resmanager_lookup_img_filename(image);

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "fillImage");

  json_write_key(ctx, jw, "imageId");
  json_write_int(ctx, jw, image_id);

  if (filename)
  {
    json_write_key(ctx, jw, "filename");
    json_write_string(ctx, jw, filename);
  }

  json_write_key(ctx, jw, "width");
  json_write_int(ctx, jw, image->w);

  json_write_key(ctx, jw, "height");
  json_write_int(ctx, jw, image->h);

  json_write_key(ctx, jw, "matrix");
  json_write_matrix(ctx, jw, ctm);

  if (alpha < 1.0f)
  {
    json_write_key(ctx, jw, "alpha");
    json_write_float(ctx, jw, alpha);
  }

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_fill_image_mask(fz_context *ctx, fz_device *dev_, fz_image *image,
                    fz_matrix ctm, fz_colorspace *colorspace, const float *color,
                    float alpha, fz_color_params color_params)
{
  cmd_device *dev = (cmd_device *)dev_;

  // Store image and get ID
  int image_id = image_store_add(ctx, image);

  // Look up the source filename if available
  const char *filename = dvi_resmanager_lookup_img_filename(image);

  float r, g, b;
  colorspace_to_rgb(ctx, colorspace, color, &r, &g, &b);

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "fillImageMask");

  json_write_key(ctx, jw, "imageId");
  json_write_int(ctx, jw, image_id);

  if (filename)
  {
    json_write_key(ctx, jw, "filename");
    json_write_string(ctx, jw, filename);
  }

  json_write_key(ctx, jw, "width");
  json_write_int(ctx, jw, image->w);

  json_write_key(ctx, jw, "height");
  json_write_int(ctx, jw, image->h);

  json_write_key(ctx, jw, "matrix");
  json_write_matrix(ctx, jw, ctm);

  json_write_key(ctx, jw, "color");
  json_write_color_rgb(ctx, jw, r, g, b);

  if (alpha < 1.0f)
  {
    json_write_key(ctx, jw, "alpha");
    json_write_float(ctx, jw, alpha);
  }

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_clip_image_mask(fz_context *ctx, fz_device *dev_, fz_image *image,
                    fz_matrix ctm, fz_rect scissor)
{
  cmd_device *dev = (cmd_device *)dev_;
  dev->clip_depth++;

  // Store image and get ID
  int image_id = image_store_add(ctx, image);

  // Look up the source filename if available
  const char *filename = dvi_resmanager_lookup_img_filename(image);

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "clipImageMask");

  json_write_key(ctx, jw, "imageId");
  json_write_int(ctx, jw, image_id);

  if (filename)
  {
    json_write_key(ctx, jw, "filename");
    json_write_string(ctx, jw, filename);
  }

  json_write_key(ctx, jw, "width");
  json_write_int(ctx, jw, image->w);

  json_write_key(ctx, jw, "height");
  json_write_int(ctx, jw, image->h);

  json_write_key(ctx, jw, "matrix");
  json_write_matrix(ctx, jw, ctm);

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

/* ============================================================================
 * Device callbacks - Shading (gradients)
 * ============================================================================ */

static void
cmd_fill_shade(fz_context *ctx, fz_device *dev_, fz_shade *shade,
               fz_matrix ctm, float alpha, fz_color_params color_params)
{
  cmd_device *dev = (cmd_device *)dev_;
  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "fillShade");

  json_write_key(ctx, jw, "matrix");
  json_write_matrix(ctx, jw, ctm);

  if (alpha < 1.0f)
  {
    json_write_key(ctx, jw, "alpha");
    json_write_float(ctx, jw, alpha);
  }

  // Shade type
  json_write_key(ctx, jw, "type");
  switch (shade->type)
  {
    case FZ_LINEAR:
      json_write_string(ctx, jw, "linear");

      json_write_key(ctx, jw, "coords");
      json_write_array_start(ctx, jw);
      json_write_float(ctx, jw, shade->u.l_or_r.coords[0][0]); // x0
      json_write_float(ctx, jw, shade->u.l_or_r.coords[0][1]); // y0
      json_write_float(ctx, jw, shade->u.l_or_r.coords[1][0]); // x1
      json_write_float(ctx, jw, shade->u.l_or_r.coords[1][1]); // y1
      json_write_array_end(ctx, jw);

      json_write_key(ctx, jw, "extend");
      json_write_array_start(ctx, jw);
      json_write_bool(ctx, jw, shade->u.l_or_r.extend[0]);
      json_write_bool(ctx, jw, shade->u.l_or_r.extend[1]);
      json_write_array_end(ctx, jw);
      break;

    case FZ_RADIAL:
      json_write_string(ctx, jw, "radial");

      json_write_key(ctx, jw, "coords");
      json_write_array_start(ctx, jw);
      json_write_float(ctx, jw, shade->u.l_or_r.coords[0][0]); // x0
      json_write_float(ctx, jw, shade->u.l_or_r.coords[0][1]); // y0
      json_write_float(ctx, jw, shade->u.l_or_r.coords[0][2]); // r0
      json_write_float(ctx, jw, shade->u.l_or_r.coords[1][0]); // x1
      json_write_float(ctx, jw, shade->u.l_or_r.coords[1][1]); // y1
      json_write_float(ctx, jw, shade->u.l_or_r.coords[1][2]); // r1
      json_write_array_end(ctx, jw);

      json_write_key(ctx, jw, "extend");
      json_write_array_start(ctx, jw);
      json_write_bool(ctx, jw, shade->u.l_or_r.extend[0]);
      json_write_bool(ctx, jw, shade->u.l_or_r.extend[1]);
      json_write_array_end(ctx, jw);
      break;

    default:
      // For mesh shadings (types 4-7) and function-based (type 1),
      // we output a simplified representation with bounding box
      json_write_string(ctx, jw, "mesh");

      json_write_key(ctx, jw, "bbox");
      json_write_array_start(ctx, jw);
      json_write_float(ctx, jw, shade->bbox.x0);
      json_write_float(ctx, jw, shade->bbox.y0);
      json_write_float(ctx, jw, shade->bbox.x1);
      json_write_float(ctx, jw, shade->bbox.y1);
      json_write_array_end(ctx, jw);
      break;
  }

  // Output color stops if function is available
  if (shade->function && shade->function_stride > 0)
  {
    json_write_key(ctx, jw, "stops");
    json_write_array_start(ctx, jw);

    // Sample the function at regular intervals
    int n = fz_colorspace_n(ctx, shade->colorspace);
    for (int i = 0; i < 256; i += 32)  // Sample at 8 points
    {
      float t = i / 255.0f;
      float *colors = &shade->function[i * shade->function_stride];

      json_write_object_start(ctx, jw);
      json_write_key(ctx, jw, "t");
      json_write_float(ctx, jw, t);

      float r, g, b;
      colorspace_to_rgb(ctx, shade->colorspace, colors, &r, &g, &b);
      json_write_key(ctx, jw, "color");
      json_write_color_rgb(ctx, jw, r, g, b);

      json_write_object_end(ctx, jw);
    }

    json_write_array_end(ctx, jw);
  }

  // Background color if available
  if (shade->use_background)
  {
    float r, g, b;
    colorspace_to_rgb(ctx, shade->colorspace, shade->background, &r, &g, &b);
    json_write_key(ctx, jw, "background");
    json_write_color_rgb(ctx, jw, r, g, b);
  }

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

/* ============================================================================
 * Device callbacks - Masks and Groups
 * ============================================================================ */

static void
cmd_begin_mask(fz_context *ctx, fz_device *dev_, fz_rect area, int luminosity,
               fz_colorspace *colorspace, const float *bc, fz_color_params color_params)
{
  cmd_device *dev = (cmd_device *)dev_;
  dev->mask_depth++;

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "beginMask");

  json_write_key(ctx, jw, "bounds");
  json_write_array_start(ctx, jw);
  json_write_float(ctx, jw, area.x0);
  json_write_float(ctx, jw, area.y0);
  json_write_float(ctx, jw, area.x1);
  json_write_float(ctx, jw, area.y1);
  json_write_array_end(ctx, jw);

  json_write_key(ctx, jw, "luminosity");
  json_write_bool(ctx, jw, luminosity);

  if (bc && colorspace)
  {
    float r, g, b;
    colorspace_to_rgb(ctx, colorspace, bc, &r, &g, &b);
    json_write_key(ctx, jw, "backdrop");
    json_write_color_rgb(ctx, jw, r, g, b);
  }

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_end_mask(fz_context *ctx, fz_device *dev_, fz_function *fn)
{
  cmd_device *dev = (cmd_device *)dev_;
  if (dev->mask_depth > 0)
    dev->mask_depth--;

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "endMask");
  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_begin_group(fz_context *ctx, fz_device *dev_, fz_rect area,
                fz_colorspace *cs, int isolated, int knockout, int blendmode, float alpha)
{
  cmd_device *dev = (cmd_device *)dev_;
  dev->group_depth++;

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "beginGroup");

  json_write_key(ctx, jw, "bounds");
  json_write_array_start(ctx, jw);
  json_write_float(ctx, jw, area.x0);
  json_write_float(ctx, jw, area.y0);
  json_write_float(ctx, jw, area.x1);
  json_write_float(ctx, jw, area.y1);
  json_write_array_end(ctx, jw);

  if (isolated)
  {
    json_write_key(ctx, jw, "isolated");
    json_write_bool(ctx, jw, 1);
  }

  if (knockout)
  {
    json_write_key(ctx, jw, "knockout");
    json_write_bool(ctx, jw, 1);
  }

  if (blendmode != FZ_BLEND_NORMAL)
  {
    json_write_key(ctx, jw, "blendMode");
    json_write_string(ctx, jw, blend_mode_name(blendmode));
  }

  if (alpha < 1.0f)
  {
    json_write_key(ctx, jw, "alpha");
    json_write_float(ctx, jw, alpha);
  }

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_end_group(fz_context *ctx, fz_device *dev_)
{
  cmd_device *dev = (cmd_device *)dev_;
  if (dev->group_depth > 0)
    dev->group_depth--;

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "endGroup");
  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

/* ============================================================================
 * Device callbacks - Tiles (patterns)
 * ============================================================================ */

static int
cmd_begin_tile(fz_context *ctx, fz_device *dev_, fz_rect area, fz_rect view,
               float xstep, float ystep, fz_matrix ctm, int id, int doc_id)
{
  cmd_device *dev = (cmd_device *)dev_;
  dev->tile_depth++;

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "beginTile");

  json_write_key(ctx, jw, "id");
  json_write_int(ctx, jw, id);

  json_write_key(ctx, jw, "area");
  json_write_array_start(ctx, jw);
  json_write_float(ctx, jw, area.x0);
  json_write_float(ctx, jw, area.y0);
  json_write_float(ctx, jw, area.x1);
  json_write_float(ctx, jw, area.y1);
  json_write_array_end(ctx, jw);

  json_write_key(ctx, jw, "view");
  json_write_array_start(ctx, jw);
  json_write_float(ctx, jw, view.x0);
  json_write_float(ctx, jw, view.y0);
  json_write_float(ctx, jw, view.x1);
  json_write_float(ctx, jw, view.y1);
  json_write_array_end(ctx, jw);

  json_write_key(ctx, jw, "xstep");
  json_write_float(ctx, jw, xstep);

  json_write_key(ctx, jw, "ystep");
  json_write_float(ctx, jw, ystep);

  json_write_key(ctx, jw, "matrix");
  json_write_matrix(ctx, jw, ctm);

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);

  // Return 0 to indicate we want the tile content
  return 0;
}

static void
cmd_end_tile(fz_context *ctx, fz_device *dev_)
{
  cmd_device *dev = (cmd_device *)dev_;
  if (dev->tile_depth > 0)
    dev->tile_depth--;

  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "endTile");
  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

/* ============================================================================
 * Device callbacks - Layers (optional content)
 * ============================================================================ */

static void
cmd_begin_layer(fz_context *ctx, fz_device *dev_, const char *layer_name)
{
  cmd_device *dev = (cmd_device *)dev_;
  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "beginLayer");

  json_write_key(ctx, jw, "name");
  json_write_string(ctx, jw, layer_name ? layer_name : "");

  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

static void
cmd_end_layer(fz_context *ctx, fz_device *dev_)
{
  cmd_device *dev = (cmd_device *)dev_;
  json_writer *jw = json_writer_new(ctx, dev->output);

  json_write_object_start(ctx, jw);
  json_write_key(ctx, jw, "cmd");
  json_write_string(ctx, jw, "endLayer");
  json_write_object_end(ctx, jw);
  json_writer_flush_line(ctx, jw);
  json_writer_drop(ctx, jw);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

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
  dev->mask_depth = 0;
  dev->group_depth = 0;
  dev->tile_depth = 0;

  // Set up all device callbacks
  dev->super.fill_path = cmd_fill_path;
  dev->super.stroke_path = cmd_stroke_path;
  dev->super.clip_path = cmd_clip_path;
  dev->super.clip_stroke_path = cmd_clip_stroke_path;

  dev->super.fill_text = cmd_fill_text;
  dev->super.stroke_text = cmd_stroke_text;
  dev->super.clip_text = cmd_clip_text;
  dev->super.clip_stroke_text = cmd_clip_stroke_text;
  dev->super.ignore_text = cmd_ignore_text;

  dev->super.fill_shade = cmd_fill_shade;
  dev->super.fill_image = cmd_fill_image;
  dev->super.fill_image_mask = cmd_fill_image_mask;
  dev->super.clip_image_mask = cmd_clip_image_mask;

  dev->super.pop_clip = cmd_pop_clip;

  dev->super.begin_mask = cmd_begin_mask;
  dev->super.end_mask = cmd_end_mask;
  dev->super.begin_group = cmd_begin_group;
  dev->super.end_group = cmd_end_group;

  dev->super.begin_tile = cmd_begin_tile;
  dev->super.end_tile = cmd_end_tile;

  dev->super.begin_layer = cmd_begin_layer;
  dev->super.end_layer = cmd_end_layer;

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
