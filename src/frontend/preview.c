#include "preview.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void txp_preview_init(txp_preview *p)
{
  memset(p, 0, sizeof *p);
  p->revision = 1;
  p->last_count = p->last_page = p->last_revision = p->last_sequence = p->last_complete = -1;
}

void txp_preview_invalidate(txp_preview *p)
{
  p->revision++;
  p->valid = false;
  printf("[\"preview-invalidated\",%d,%d]\n", p->revision, p->sequence);
}

void txp_preview_state(txp_preview *p, int count, int page, bool complete)
{
  if (count == p->last_count && page == p->last_page &&
      p->revision == p->last_revision && p->sequence == p->last_sequence &&
      complete == p->last_complete)
    return;
  printf("[\"preview-state\",%d,%d,%d,%s,%d]\n",
         p->revision, p->sequence, count, complete ? "true" : "false", page);
  p->last_count = count;
  p->last_page = page;
  p->last_revision = p->revision;
  p->last_sequence = p->sequence;
  p->last_complete = complete;
}

void txp_preview_export(fz_context *ctx, txp_preview *p, fz_display_list *list, int page, txp_renderer *renderer)
{
  if (!p->pending) return;
  if (p->expected_revision != p->revision || p->expected_sequence != p->sequence)
  {
    printf("[\"preview-retry\",%d]\n", p->request);
    p->pending = false;
    return;
  }
  if (!p->valid || !list || page != p->page) return;
  p->pending = false;

  fz_pixmap *pix = NULL;
  fz_buffer *png = NULL;
  fz_var(pix);
  fz_var(png);
  fz_try(ctx)
  {
    fz_rect b = fz_bound_display_list(ctx, list);
    float w = b.x1 - b.x0, h = b.y1 - b.y0;
    if (!isfinite(w) || !isfinite(h) || w < 0.01f || h < 0.01f || w > 1e7f || h > 1e7f ||
        fabsf(b.x0) >= 1e9f || fabsf(b.y0) >= 1e9f || fabsf(b.x1) >= 1e9f || fabsf(b.y1) >= 1e9f)
      fz_throw(ctx, FZ_ERROR_GENERIC, "Invalid page bounds");
    float scale = fminf(p->width / w, sqrtf(4000000.0f / (w * h)));
    scale = fminf(scale, 8192.0f / fmaxf(w, h));
    /* Leave room for outward rounding of the raster bounds. */
    scale *= 0.999f;
    fz_matrix ctm = fz_concat(fz_translate(-b.x0, -b.y0), fz_scale(scale, scale));
    pix = fz_new_pixmap_from_display_list(ctx, list, ctm, fz_device_rgb(ctx), 0);
    txp_renderer_colorize_rgb(ctx, renderer, pix);
    png = fz_new_buffer_from_pixmap_as_png(ctx, pix, fz_default_color_params);
    unsigned char *data;
    size_t len = fz_buffer_storage(ctx, png, &data);
    if (len > 16 * 1024 * 1024)
      fz_throw(ctx, FZ_ERROR_GENERIC, "Preview image exceeds the transfer budget");
    printf("[\"preview-image\",{\"requestId\":%d,\"revision\":%d,\"sequence\":%d,"
           "\"page\":%d,\"bounds\":[%.9g,%.9g,%.9g,%.9g],\"width\":%d,"
           "\"height\":%d,\"mime\":\"image/png\",\"byteLength\":%zu}]\n",
           p->request, p->revision, p->sequence, page,
           b.x0, b.y0, b.x1, b.y1,
           fz_pixmap_width(ctx, pix), fz_pixmap_height(ctx, pix), len);
    if (fwrite(data, 1, len, stdout) != len)
      _Exit(1); /* A partial binary frame cannot be recovered. */
    if (fflush(stdout) != 0) _Exit(1);
  }
  fz_always(ctx)
  {
    fz_drop_buffer(ctx, png);
    fz_drop_pixmap(ctx, pix);
  }
  fz_catch(ctx)
  {
    fprintf(stderr, "[preview] %s\n", fz_caught_message(ctx));
    printf("[\"preview-error\",%d,\"Could not render this page. See the engine log.\"]\n", p->request);
    fflush(stdout);
  }
}
