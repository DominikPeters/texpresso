#ifndef TXP_PREVIEW_H
#define TXP_PREVIEW_H

#include <mupdf/fitz.h>
#include <stdbool.h>
#include "renderer.h"

#define TXP_PREVIEW_DEFAULT_PIXELS 4000000
#define TXP_PREVIEW_MAX_PIXELS 16000000
#define TXP_PREVIEW_PNG_BYTES (64u * 1024u * 1024u)

typedef struct txp_preview_cache txp_preview_cache;

/* Opt-in, content-only profiling counters. No strings or document content. */
typedef struct {
  size_t glyphs, unique_glyphs, text_spans, fonts, paths, path_bytes, images, clips, groups, shades, vector_cost;
  uint64_t image_pixels;
  bool font_overflow, glyph_overflow, svg_eligible;
} txp_render_features;
void txp_preview_render_features(fz_context *ctx, fz_display_list *list, txp_render_features *features);

/* One current display-list snapshot and one replaceable render request. */
typedef struct {
  int revision, sequence;
  uint64_t content_token;
  txp_preview_cache *export_cache;
  bool valid, pending, region, svg, automatic, patches, background;
  bool foreground_valid;
  int foreground_page, saved_foreground_page, foreground_revision, foreground_sequence;
  fz_display_list *background_list;
  fz_buffer *foreground_annotations;
  fz_buffer *annotations;
  char destination[4097];
  fz_rect bounds;
  fz_pixmap *previous_png;
  fz_rect previous_bounds;
  int previous_page, previous_id, patch_base;
  int vector_limit;
  int request, page, width, max_pixels, expected_revision, expected_sequence;
  int last_count, last_page, last_revision, last_sequence, last_complete;
  int stopped_revision, stopped_sequence;
} txp_preview;

bool txp_preview_dirty_rect(fz_context *ctx, fz_pixmap *before, fz_pixmap *after, fz_irect *rect);
fz_buffer *txp_preview_png(fz_context *ctx, fz_pixmap *pix);
fz_irect txp_preview_raster_size(float width, float height, int requested_width, int max_pixels);
/* Full-page text geometry, excluding paths/images; empty if there is no text. */
fz_rect txp_preview_text_bounds(fz_context *ctx, fz_display_list *list);
void txp_preview_init(txp_preview *p);
void txp_preview_drop_cache(fz_context *ctx, txp_preview *p);
void txp_preview_invalidate(txp_preview *p);
void txp_preview_state(txp_preview *p, int count, int page, bool complete);
void txp_preview_stopped(txp_preview *p, int count);
const char *txp_preview_svg_fallback(fz_context *ctx, fz_display_list *list);
void txp_preview_export(fz_context *ctx, txp_preview *p, fz_display_list *list, int page, txp_renderer *renderer);

#endif
