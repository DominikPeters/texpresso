#ifndef TXP_PREVIEW_H
#define TXP_PREVIEW_H

#include <mupdf/fitz.h>
#include <stdbool.h>
#include "renderer.h"

#define TXP_PREVIEW_DEFAULT_PIXELS 4000000
#define TXP_PREVIEW_MAX_PIXELS 16000000
#define TXP_PREVIEW_PNG_BYTES (64u * 1024u * 1024u)

/* A page's device grid is named by one integer: the width of the complete
 * page in device pixels. Every raster of that page at that zoom, whole or
 * clipped, uses the same transform and integer device rectangles, so
 * independently rendered rectangles agree pixel for pixel where they overlap. */
#define TXP_PREVIEW_MAX_GRID 65536
#define TXP_PREVIEW_RETAINED 4
/* Side of the canonical rasterization cells, in device pixels. Clients align
 * rectangles to it, so it is part of the protocol; the override exists only to
 * measure other sizes (docs/profiling in the extension repository). */
#ifndef TXP_PREVIEW_CELL
#define TXP_PREVIEW_CELL 512
#endif
typedef struct { float scale; int width, height; } txp_preview_grid;
typedef struct {
  int id, page, grid_width;
  fz_rect bounds;
  fz_irect clip;
  fz_pixmap *pixels;
  unsigned long used;
} txp_preview_raster;

typedef struct txp_preview_cache txp_preview_cache;

/* One current display-list snapshot and one replaceable render request. */
typedef struct {
  int revision, sequence;
  uint64_t content_token;
  txp_preview_cache *export_cache;
  bool valid, pending, region, svg, automatic, patches, background;
  /* A surface is an integer device rectangle of the page grid `width`. */
  bool surface;
  fz_irect clip;
  bool foreground_valid;
  int foreground_page, saved_foreground_page, foreground_revision, foreground_sequence;
  fz_display_list *background_list;
  fz_buffer *foreground_annotations;
  fz_buffer *annotations;
  char destination[4097];
  fz_rect bounds;
  /* Rasters the client may name as a patch base, bounded by the pixel budget. */
  txp_preview_raster retained[TXP_PREVIEW_RETAINED];
  unsigned long retained_clock;
  int patch_base;
  int vector_limit;
  /* max_pixels bounds whole pages and regions; surface_pixels bounds a surface. */
  int request, page, width, max_pixels, surface_pixels, expected_revision, expected_sequence;
  int last_count, last_page, last_revision, last_sequence, last_complete;
  int stopped_revision, stopped_sequence;
} txp_preview;

bool txp_preview_dirty_rect(fz_context *ctx, fz_pixmap *before, fz_pixmap *after, fz_irect *rect);
fz_buffer *txp_preview_png(fz_context *ctx, fz_pixmap *pix);
fz_irect txp_preview_raster_size(float width, float height, int requested_width, int max_pixels);
/* False when the page or grid cannot be rasterized. */
bool txp_preview_page_grid(fz_rect page, int grid_width, txp_preview_grid *grid);
/* Clamp a requested rectangle to the page and to the dimension/area limits,
 * trimming rows, then columns, from the far edges. Empty if nothing remains. */
fz_irect txp_preview_surface_clip(const txp_preview_grid *grid, fz_irect requested, int max_pixels);
/* Opaque RGB pixels of `clip`; the pixmap keeps the clip's device origin. */
fz_pixmap *txp_preview_draw(fz_context *ctx, fz_display_list *list, fz_rect page,
                            const txp_preview_grid *grid, fz_irect clip);
fz_pixmap *txp_preview_draw_cells(fz_context *ctx, fz_display_list *list, fz_rect page,
                                  const txp_preview_grid *grid, fz_irect clip, int cell);
txp_preview_raster *txp_preview_retained(txp_preview *p, int id);
void txp_preview_retain(fz_context *ctx, txp_preview *p, int id, int page, fz_rect bounds,
                        int grid_width, fz_pixmap *pixels);
void txp_preview_release(fz_context *ctx, txp_preview *p, int id);
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
