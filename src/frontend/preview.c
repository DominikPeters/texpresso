#include "preview.h"
#include "profile.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

/* Audit against MuPDF 1.28.5 source/fitz/svg-device.c. Keep this cheap pass
 * independent of export: no glyph outlining, image decoding, or rasterization.
 * Conservative fallbacks preserve the draw device as the fidelity reference. */
typedef struct { fz_device super; const char *reason; size_t cost; bool images; } svg_check;
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
  ((svg_check *)dev)->cost += 100;
  ((svg_check *)dev)->images = true;
  if (image->colorspace && image->colorspace != fz_device_rgb(ctx) && image->colorspace != fz_device_gray(ctx))
    ((svg_check *)dev)->reason = "PDF image color spaces";
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
static const char *svg_analysis(fz_context *ctx, fz_display_list *list, size_t *cost, bool *images)
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
    if (cost) *cost = check->cost;
    if (images) *images = check->images;
    reason = check->cost > 2000000 ? "Vector complexity budget" : check->reason;
    fz_close_device(ctx, &check->super);
  }
  fz_always(ctx) { fz_drop_device(ctx, &check->super); }
  fz_catch(ctx) { fz_rethrow(ctx); }
  return reason;
}

const char *txp_preview_svg_fallback(fz_context *ctx, fz_display_list *list)
{ return svg_analysis(ctx, list, NULL, NULL); }

/* Interactive RGB PNGs: one scanline and a bounded deflate buffer instead of
 * MuPDF's whole-image filtered/compressed scratch allocations. Unfiltered
 * scanlines and level 1 won the local speed/size comparison with Sub/Up/RLE.
 * Exact grayscale pages use one PNG channel after checking every RGB pixel.
 * Rendering/color conversion is unchanged, and the format remains lossless. */
static void png_chunk(fz_context *ctx, fz_buffer *buffer, const char *tag,
                      const unsigned char *data, size_t size)
{
  fz_append_int32_be(ctx, buffer, (int32_t)size);
  fz_append_data(ctx, buffer, tag, 4);
  if (size) fz_append_data(ctx, buffer, data, size);
  uLong checksum = crc32(0, (const Bytef *)tag, 4);
  if (size) checksum = crc32(checksum, data, (uInt)size);
  fz_append_int32_be(ctx, buffer, (int32_t)checksum);
}

fz_buffer *txp_preview_png(fz_context *ctx, fz_pixmap *pix)
{
  if (fz_pixmap_colorspace(ctx, pix) != fz_device_rgb(ctx) || fz_pixmap_components(ctx, pix) != 3 || fz_pixmap_alpha(ctx, pix))
    return fz_new_buffer_from_pixmap_as_png(ctx, pix, fz_default_color_params);
  int w = fz_pixmap_width(ctx, pix), h = fz_pixmap_height(ctx, pix);
  int stride = fz_pixmap_stride(ctx, pix);
  size_t source_row_size = (size_t)w * 3;
  fz_buffer *buffer = NULL;
  unsigned char *row = NULL;
  unsigned char compressed[65536];
  z_stream stream = {0};
  int started = 0;
  fz_var(buffer); fz_var(row); fz_var(started); fz_var(stream);
  fz_try(ctx) {
    if (w < 1 || h < 1 || w > 8192 || h > 8192 || (double)w * h > 4000000 || stride < 0 || (size_t)stride < source_row_size)
      fz_throw(ctx, FZ_ERROR_GENERIC, "Invalid preview PNG dimensions");
    const unsigned char *samples = fz_pixmap_samples(ctx, pix);
    int grayscale = 1;
    for (int y = 0; y < h && grayscale; y++) {
      const unsigned char *line = samples + (size_t)y * stride;
      for (int x = 0; x < w; x++) {
        if (line[3*x] != line[3*x+1] || line[3*x] != line[3*x+2]) { grayscale = 0; break; }
      }
    }
    size_t row_size = (size_t)w * (grayscale ? 1 : 3);
    buffer = fz_new_buffer(ctx, 65536);
    row = fz_malloc(ctx, row_size + 1);
    fz_append_data(ctx, buffer, "\x89PNG\r\n\x1a\n", 8);
    unsigned char header[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, grayscale ? 0 : 2, 0, 0, 0};
    png_chunk(ctx, buffer, "IHDR", header, 13);
    int xres = pix->xres, yres = pix->yres;
    xres = (int)(xres * 100 / 2.54f + 0.5f); yres = (int)(yres * 100 / 2.54f + 0.5f);
    unsigned char resolution[9] = {xres >> 24, xres >> 16, xres >> 8, xres, yres >> 24, yres >> 16, yres >> 8, yres, 1};
    png_chunk(ctx, buffer, "pHYs", resolution, 9);
    if (deflateInit(&stream, Z_BEST_SPEED) != Z_OK)
      fz_throw(ctx, FZ_ERROR_GENERIC, "Could not initialize preview PNG compression");
    started = 1; stream.next_out = compressed; stream.avail_out = sizeof compressed;
    for (int y = 0; y < h; y++) {
      const unsigned char *current = samples + (size_t)y * stride;
      row[0] = 0; /* Same unfiltered RGB scanlines as MuPDF's writer. */
      if (grayscale) { for (int x = 0; x < w; x++) row[x+1] = current[3*x]; }
      else memcpy(row + 1, current, row_size);
      stream.next_in = row; stream.avail_in = (uInt)(row_size + 1);
      int final = y == h - 1;
      int result;
      do {
        result = deflate(&stream, final ? Z_FINISH : Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END)
          fz_throw(ctx, FZ_ERROR_GENERIC, "Could not compress preview PNG");
        if (!stream.avail_out || result == Z_STREAM_END) {
          png_chunk(ctx, buffer, "IDAT", compressed, sizeof compressed - stream.avail_out);
          stream.next_out = compressed; stream.avail_out = sizeof compressed;
        }
      } while (stream.avail_in || (final && result != Z_STREAM_END));
    }
    png_chunk(ctx, buffer, "IEND", NULL, 0);
  }
  fz_always(ctx) { if (started) deflateEnd(&stream); fz_free(ctx, row); }
  fz_catch(ctx) { fz_drop_buffer(ctx, buffer); fz_rethrow(ctx); }
  return buffer;
}

/* Exact RGB comparison, including white pixels that erase old text. One
 * retained raster is bounded by the existing four-megapixel page budget. */
bool txp_preview_dirty_rect(fz_context *ctx, fz_pixmap *before, fz_pixmap *after, fz_irect *rect)
{
  if (!before || !after || before->w != after->w || before->h != after->h ||
      before->x != after->x || before->y != after->y || before->n != 3 || after->n != 3 ||
      before->alpha || after->alpha || before->colorspace != fz_device_rgb(ctx) ||
      after->colorspace != fz_device_rgb(ctx)) return false;
  int w = after->w, h = after->h;
  fz_irect b = {w, h, 0, 0};
  for (int y = 0; y < h; y++) {
    const unsigned char *a = before->samples + (size_t)y * before->stride;
    const unsigned char *c = after->samples + (size_t)y * after->stride;
    if (!memcmp(a, c, (size_t)w * 3)) continue;
    int left = 0, right = w;
    while (left < right && !memcmp(a + left * 3, c + left * 3, 3)) left++;
    while (right > left && !memcmp(a + (right-1) * 3, c + (right-1) * 3, 3)) right--;
    if (left < b.x0) b.x0 = left;
    if (right > b.x1) b.x1 = right;
    if (y < b.y0) b.y0 = y;
    b.y1 = y + 1;
  }
  *rect = b.x1 ? b : (fz_irect){0,0,0,0};
  return true;
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

  /* Measure export only: compilation and display-list construction have already
   * finished. Two monotonic reads let the extension compare rendering costs
   * without enabling verbose profiling or confusing compiler rollback latency. */
  struct timespec export_start;
  clock_gettime(CLOCK_MONOTONIC, &export_start);
  fz_pixmap *pix = NULL, *crop = NULL;
  fz_buffer *png = NULL;
  fz_device *dev = NULL;
  fz_output *out = NULL;
  const char *fallback = NULL;
  bool svg = false, svg_eligible = false, images = false;
  size_t vector_cost = 0;
  bool patch = false;
  fz_irect dirty = {0};
  int image_width = 0, image_height = 0;
  fz_var(pix); fz_var(crop); fz_var(patch); fz_var(dirty);
  fz_var(png);
  fz_var(dev); fz_var(out); fz_var(fallback); fz_var(svg); fz_var(svg_eligible); fz_var(vector_cost);
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
    if ((p->svg || p->automatic) && !p->region)
    {
      txp_renderer_config *config = txp_renderer_get_config(ctx, renderer);
      double preflight_start = txp_profile_now();
      fallback = ceilf(w) > 8192 || ceilf(h) > 8192 || ceilf(w) * ceilf(h) > 4000000 ? "Oversized SVG page" : config->invert_color || (config->themed_color &&
        ((config->foreground_color & 0xFFFFFF) != 0 || (config->background_color & 0xFFFFFF) != 0xFFFFFF))
        ? "Preview color theme" : svg_analysis(ctx, list, &vector_cost, &images);
      txp_profile_record("svg-preflight", preflight_start, p->request, page, 0);
      /* Count existing display-list operations; do not export both codecs on
       * the interactive path. Raster images already have a good PNG representation.
       * Small pages start as SVG, larger eligible pages can be upgraded on idle. */
      svg_eligible = !fallback && !images && vector_cost <= 200000;
      if (!fallback && (!p->automatic || (svg_eligible && vector_cost <= (size_t)p->vector_limit)))
      {
        /* Paths preserve exact fonts; the SVG stays isolated in a blob <img>. */
        fz_try(ctx) {
          double svg_start = txp_profile_now();
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
          txp_profile_record("svg-export", svg_start, p->request, page, png->len);
          svg = true;
          image_width = (int)ceilf(w); image_height = (int)ceilf(h);
        }
        fz_catch(ctx) {
          fz_drop_device(ctx, dev); dev = NULL; fz_drop_output(ctx, out); out = NULL;
          fz_drop_buffer(ctx, png); png = NULL;
          fallback = "SVG export failed or exceeded the transfer budget";
          svg_eligible = false;
        }
      }
    }
    if (p->automatic) fallback = NULL; /* PNG is an ordinary Auto choice. */
    if (!svg)
    {
      double draw_start = txp_profile_now();
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
      txp_profile_record("png-draw", draw_start, p->request, page, 0);
      double color_start = txp_profile_now();
      txp_renderer_colorize_rgb(ctx, renderer, pix);
      txp_profile_record("png-color", color_start, p->request, page, 0);
      image_width = fz_pixmap_width(ctx, pix); image_height = fz_pixmap_height(ctx, pix);
      double compare_start = txp_profile_now();
      patch = p->patches && !p->region && p->patch_base > 0 && p->patch_base == p->previous_id &&
        page == p->previous_page && b.x0 == p->previous_bounds.x0 && b.y0 == p->previous_bounds.y0 &&
        b.x1 == p->previous_bounds.x1 && b.y1 == p->previous_bounds.y1 &&
        txp_preview_dirty_rect(ctx, p->previous_png, pix, &dirty) &&
        (double)(dirty.x1 - dirty.x0) * (dirty.y1 - dirty.y0) <= (double)image_width * image_height * 0.4;
      txp_profile_record("png-compare", compare_start, p->request, page, patch ? (dirty.x1-dirty.x0)*(dirty.y1-dirty.y0) : 0);
      double encode_start = txp_profile_now();
      if (patch && !dirty.x1) png = fz_new_buffer(ctx, 0); /* same pixels, fresh metadata */
      else {
        fz_pixmap *source = pix;
        if (patch) {
          crop = fz_new_pixmap_with_data(ctx, fz_device_rgb(ctx), dirty.x1-dirty.x0, dirty.y1-dirty.y0,
            NULL, 0, pix->stride, pix->samples + (size_t)dirty.y0 * pix->stride + dirty.x0 * 3);
          crop->xres = pix->xres; crop->yres = pix->yres;
          source = crop;
        }
        png = txp_preview_png(ctx, source);
      }
      txp_profile_record("png-encode", encode_start, p->request, page, png->len);
    }
    unsigned char *data;
    size_t len = fz_buffer_storage(ctx, png, &data);
    if (len > 16 * 1024 * 1024)
      fz_throw(ctx, FZ_ERROR_GENERIC, "Preview image exceeds the transfer budget");
    struct timespec export_end;
    clock_gettime(CLOCK_MONOTONIC, &export_end);
    double export_ms = (export_end.tv_sec - export_start.tv_sec) * 1000.0 +
      (export_end.tv_nsec - export_start.tv_nsec) / 1000000.0;
    double transfer_start = txp_profile_now();
    printf("[\"preview-image\",{\"requestId\":%d,\"revision\":%d,\"sequence\":%d,"
           "\"page\":%d,\"bounds\":[%.9g,%.9g,%.9g,%.9g],\"width\":%d,"
           "\"height\":%d,\"mime\":\"%s\",\"byteLength\":%zu",
           p->request, p->revision, p->sequence, page,
           b.x0, b.y0, b.x1, b.y1,
           image_width, image_height, svg ? "image/svg+xml" : "image/png", len);
    printf(",\"exportMs\":%.4f", export_ms);
    if ((p->automatic || p->svg) && !p->region)
      printf(",\"svgEligible\":%s,\"vectorCost\":%zu", svg_eligible ? "true" : "false", vector_cost);
    if (!svg && !p->region && p->patches) printf(",\"rasterId\":%d", p->request);
    if (patch) printf(",\"patch\":{\"base\":%d,\"rect\":[%d,%d,%d,%d]}",
      p->patch_base, dirty.x0, dirty.y0, dirty.x1-dirty.x0, dirty.y1-dirty.y0);
    if (fallback) printf(",\"fallback\":\"%s\"", fallback);
    if (!p->region && p->annotations && p->annotations->len) {
      printf(",\"annotations\":%.*s", (int)p->annotations->len, p->annotations->data);
    }
    printf("}]\n");
    if (len && fwrite(data, 1, len, stdout) != len)
      _Exit(1); /* A partial binary frame cannot be recovered. */
    if (fflush(stdout) != 0) _Exit(1);
    txp_profile_record("pipe-write", transfer_start, p->request, page, len);
    if (!p->region) {
      fz_drop_pixmap(ctx, p->previous_png); p->previous_png = NULL; p->previous_id = 0;
      if (!svg && p->patches) {
        p->previous_png = fz_keep_pixmap(ctx, pix); p->previous_bounds = b;
        p->previous_page = page; p->previous_id = p->request;
      }
    }
  }
  fz_always(ctx)
  {
    fz_drop_device(ctx, dev);
    fz_drop_output(ctx, out);
    fz_drop_buffer(ctx, png);
    fz_drop_pixmap(ctx, crop); fz_drop_pixmap(ctx, pix);
  }
  fz_catch(ctx)
  {
    fprintf(stderr, "[preview] %s\n", fz_caught_message(ctx));
    printf("[\"preview-error\",%d,\"Could not render this page. See the engine log.\"]\n", p->request);
    fflush(stdout);
  }
}
