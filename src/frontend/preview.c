#include "preview.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Audit against MuPDF 1.28.5 source/fitz/svg-device.c. Keep this cheap pass
 * independent of export: no glyph outlining, image decoding, or rasterization.
 * Conservative fallbacks preserve the draw device as the fidelity reference. */
typedef struct { fz_device super; const char *reason; size_t cost; } svg_check;
static void check_group(fz_context *ctx, fz_device *dev, fz_rect b, fz_colorspace *cs,
                        int isolated, int knockout, int blend, float alpha)
{
  if (knockout || !isolated || blend != FZ_BLEND_NORMAL ||
      (cs && cs != fz_device_rgb(ctx) && cs != fz_device_gray(ctx)))
    ((svg_check *)dev)->reason = "PDF transparency groups or blending";
}
static void check_mask(fz_context *ctx, fz_device *dev, fz_rect b, int luminosity,
                       fz_colorspace *cs, const float *color, fz_color_params cp)
{ ((svg_check *)dev)->reason = "PDF soft masks"; }
static void check_shade(fz_context *ctx, fz_device *dev, fz_shade *s, fz_matrix m, float alpha, fz_color_params cp)
{ ((svg_check *)dev)->reason = "PDF shadings (SVG rasterizes these at a fixed resolution)"; }
static int check_tile(fz_context *ctx, fz_device *dev, fz_rect a, fz_rect v, float x, float y, fz_matrix m, int id
#if FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR >= 27
                      , int doc
#endif
                      )
{ ((svg_check *)dev)->reason = "PDF tiling patterns"; return 0; }
static void check_text(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix m,
                       fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{
  if (cp.op || cp.opm) ((svg_check *)dev)->reason = "PDF overprint";
  for (const fz_text_span *span = text->head; span; span = span->next) {
    ((svg_check *)dev)->cost += (size_t)span->len * 20;
    if (fz_font_t3_procs(ctx, span->font)) ((svg_check *)dev)->reason = "Type 3 fonts";
  }
}
static void check_path(fz_context *ctx, fz_device *dev, const fz_path *path, int eo, fz_matrix m,
                       fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{ ((svg_check *)dev)->cost += fz_packed_path_size(path);
  if (cp.op || cp.opm) ((svg_check *)dev)->reason = "PDF overprint"; }
static void check_stroke(fz_context *ctx, fz_device *dev, const fz_path *path, const fz_stroke_state *stroke, fz_matrix m,
                         fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{ ((svg_check *)dev)->cost += fz_packed_path_size(path);
  if (cp.op || cp.opm) ((svg_check *)dev)->reason = "PDF overprint"; }
static void check_image(fz_context *ctx, fz_device *dev, fz_image *image, fz_matrix m, float alpha, fz_color_params cp)
{
  if (cp.op || cp.opm) ((svg_check *)dev)->reason = "PDF overprint";
  if (image->mask || image->use_decode || image->use_colorkey)
    ((svg_check *)dev)->reason = "PDF image masks or decode arrays";
  if ((double)image->w * image->h > 16000000) ((svg_check *)dev)->reason = "Large embedded image";
}
static void check_stroke_text(fz_context *ctx, fz_device *dev, const fz_text *text, const fz_stroke_state *stroke,
                              fz_matrix m, fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{ check_text(ctx, dev, text, m, cs, color, alpha, cp); }
static void check_clip_text(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix m, fz_rect scissor)
{ check_text(ctx, dev, text, m, NULL, NULL, 1, fz_default_color_params); }
static void check_clip_stroke_text(fz_context *ctx, fz_device *dev, const fz_text *text, const fz_stroke_state *stroke, fz_matrix m, fz_rect scissor)
{ check_clip_text(ctx, dev, text, m, scissor); }
static void check_image_mask(fz_context *ctx, fz_device *dev, fz_image *image, fz_matrix m,
                             fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{ check_image(ctx, dev, image, m, alpha, cp); }
static void check_clip_image(fz_context *ctx, fz_device *dev, fz_image *image, fz_matrix m, fz_rect scissor)
{ check_image(ctx, dev, image, m, 1, fz_default_color_params); }
static void bounded_svg_write(fz_context *ctx, void *state, const void *data, size_t size)
{
  fz_buffer *buffer = state;
  if (size > 16 * 1024 * 1024 - buffer->len) fz_throw(ctx, FZ_ERROR_GENERIC, "SVG transfer budget");
  fz_append_data(ctx, buffer, data, size);
}
const char *txp_preview_svg_fallback(fz_context *ctx, fz_display_list *list)
{
  svg_check *check = fz_new_derived_device(ctx, svg_check);
  check->super.begin_group = check_group; check->super.begin_mask = check_mask;
  check->super.fill_shade = check_shade; check->super.begin_tile = check_tile;
  check->super.fill_text = check_text; check->super.fill_path = check_path;
  check->super.stroke_text = check_stroke_text; check->super.clip_text = check_clip_text;
  check->super.clip_stroke_text = check_clip_stroke_text; check->super.fill_image_mask = check_image_mask;
  check->super.stroke_path = check_stroke; check->super.fill_image = check_image;
  check->super.clip_image_mask = check_clip_image;
  const char *reason = NULL;
  fz_var(reason);
  fz_try(ctx) {
    fz_run_display_list(ctx, list, &check->super, fz_identity, fz_infinite_rect, NULL);
    reason = check->cost > 2000000 ? "Vector complexity budget" : check->reason;
    fz_close_device(ctx, &check->super);
  }
  fz_always(ctx) { fz_drop_device(ctx, &check->super); }
  fz_catch(ctx) { fz_rethrow(ctx); }
  return reason;
}

void txp_preview_init(txp_preview *p)
{
  memset(p, 0, sizeof *p);
  p->revision = 1;
  p->last_count = p->last_page = p->last_revision = p->last_sequence = p->last_complete = -1;
}

void txp_preview_invalidate(txp_preview *p)
{
  p->revision++;
  p->valid = false; p->destination[0] = 0;
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
  fz_device *dev = NULL;
  fz_output *out = NULL;
  const char *fallback = NULL;
  bool svg = false;
  int image_width = 0, image_height = 0;
  fz_var(pix);
  fz_var(png);
  fz_var(dev); fz_var(out); fz_var(fallback); fz_var(svg);
  fz_try(ctx)
  {
    fz_rect b = fz_bound_display_list(ctx, list);
    if (p->region)
    {
      b = fz_intersect_rect(b, p->bounds);
      if (fz_is_empty_rect(b)) fz_throw(ctx, FZ_ERROR_GENERIC, "Region is outside the page");
    }
    float w = b.x1 - b.x0, h = b.y1 - b.y0;
    if (!isfinite(w) || !isfinite(h) || w < 0.01f || h < 0.01f || w > 1e7f || h > 1e7f ||
        fabsf(b.x0) >= 1e9f || fabsf(b.y0) >= 1e9f || fabsf(b.x1) >= 1e9f || fabsf(b.y1) >= 1e9f)
      fz_throw(ctx, FZ_ERROR_GENERIC, "Invalid page bounds");
    if (p->svg && !p->region)
    {
      txp_renderer_config *config = txp_renderer_get_config(ctx, renderer);
      fallback = w > 8192 || h > 8192 || w * h > 4000000 ? "Oversized SVG page" : config->invert_color || (config->themed_color &&
        ((config->foreground_color & 0xFFFFFF) != 0 || (config->background_color & 0xFFFFFF) != 0xFFFFFF))
        ? "Preview color theme" : txp_preview_svg_fallback(ctx, list);
      if (!fallback)
      {
        /* Paths preserve exact fonts; the SVG stays isolated in a blob <img>. */
        fz_try(ctx) {
          png = fz_new_buffer(ctx, 4096);
          out = fz_new_output(ctx, 0, png, bounded_svg_write, NULL, NULL);
          dev = fz_new_svg_device(ctx, out, w, h, FZ_SVG_TEXT_AS_PATH, 1);
          fz_path *background = fz_new_path(ctx);
          fz_rectto(ctx, background, 0, 0, w, h);
          float white[3] = {1, 1, 1};
          fz_fill_path(ctx, dev, background, 0, fz_identity, fz_device_rgb(ctx), white, 1, fz_default_color_params);
          fz_drop_path(ctx, background);
          fz_run_display_list(ctx, list, dev, fz_translate(-b.x0, -b.y0), fz_infinite_rect, NULL);
          fz_close_device(ctx, dev); fz_drop_device(ctx, dev); dev = NULL;
          fz_close_output(ctx, out); fz_drop_output(ctx, out); out = NULL;
          if (png->len > 16 * 1024 * 1024) fz_throw(ctx, FZ_ERROR_GENERIC, "SVG transfer budget");
          svg = true;
          image_width = (int)ceilf(w); image_height = (int)ceilf(h);
        }
        fz_catch(ctx) {
          fz_drop_device(ctx, dev); dev = NULL; fz_drop_output(ctx, out); out = NULL;
          fz_drop_buffer(ctx, png); png = NULL;
          fallback = "SVG export failed or exceeded the transfer budget";
        }
      }
    }
    if (!svg)
    {
      float scale = fminf(p->width / w, sqrtf(4000000.0f / (w * h)));
      scale = fminf(scale, 8192.0f / fmaxf(w, h));
      /* Leave room for outward rounding of the raster bounds. */
      scale *= 0.999f;
      fz_matrix ctm = fz_concat(fz_translate(-b.x0, -b.y0), fz_scale(scale, scale));
      /* Allocate only the clipped region, never a full page at loupe resolution. */
      fz_irect bbox = fz_round_rect(fz_transform_rect(b, ctm));
      pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, NULL, 0);
      fz_clear_pixmap_with_value(ctx, pix, 255);
      dev = fz_new_draw_device(ctx, fz_identity, pix);
      fz_run_display_list(ctx, list, dev, ctm, fz_rect_from_irect(bbox), NULL);
      fz_close_device(ctx, dev);
      txp_renderer_colorize_rgb(ctx, renderer, pix);
      png = fz_new_buffer_from_pixmap_as_png(ctx, pix, fz_default_color_params);
      image_width = fz_pixmap_width(ctx, pix); image_height = fz_pixmap_height(ctx, pix);
    }
    unsigned char *data;
    size_t len = fz_buffer_storage(ctx, png, &data);
    if (len > 16 * 1024 * 1024)
      fz_throw(ctx, FZ_ERROR_GENERIC, "Preview image exceeds the transfer budget");
    printf("[\"preview-image\",{\"requestId\":%d,\"revision\":%d,\"sequence\":%d,"
           "\"page\":%d,\"bounds\":[%.9g,%.9g,%.9g,%.9g],\"width\":%d,"
           "\"height\":%d,\"mime\":\"%s\",\"byteLength\":%zu",
           p->request, p->revision, p->sequence, page,
           b.x0, b.y0, b.x1, b.y1,
           image_width, image_height, svg ? "image/svg+xml" : "image/png", len);
    if (fallback) printf(",\"fallback\":\"%s\"", fallback);
    if (!p->region && p->annotations && p->annotations->len) {
      printf(",\"annotations\":%.*s", (int)p->annotations->len, p->annotations->data);
    }
    printf("}]\n");
    if (fwrite(data, 1, len, stdout) != len)
      _Exit(1); /* A partial binary frame cannot be recovered. */
    if (fflush(stdout) != 0) _Exit(1);
  }
  fz_always(ctx)
  {
    fz_drop_device(ctx, dev);
    fz_drop_output(ctx, out);
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
