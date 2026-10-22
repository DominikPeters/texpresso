#ifndef TXP_PREVIEW_H
#define TXP_PREVIEW_H

#include <mupdf/fitz.h>
#include <stdbool.h>
#include "renderer.h"

/* One current display-list snapshot and one replaceable render request. */
typedef struct {
  int revision, sequence;
  bool valid, pending, region, svg, automatic;
  fz_buffer *annotations;
  char destination[4097];
  fz_rect bounds;
  int vector_limit;
  int request, page, width, expected_revision, expected_sequence;
  int last_count, last_page, last_revision, last_sequence, last_complete;
} txp_preview;

fz_buffer *txp_preview_png(fz_context *ctx, fz_pixmap *pix);
void txp_preview_init(txp_preview *p);
void txp_preview_invalidate(txp_preview *p);
void txp_preview_state(txp_preview *p, int count, int page, bool complete);
const char *txp_preview_svg_fallback(fz_context *ctx, fz_display_list *list);
void txp_preview_export(fz_context *ctx, txp_preview *p, fz_display_list *list, int page, txp_renderer *renderer);

#endif
