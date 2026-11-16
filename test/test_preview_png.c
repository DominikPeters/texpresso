#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>
#include <math.h>
#include <stdlib.h>
#include "preview.h"

static uint32_t be32(const unsigned char *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }

static void check_chunks(fz_buffer *buffer)
{
  assert(buffer->len > 8 && !memcmp(buffer->data, "\x89PNG\r\n\x1a\n", 8));
  size_t offset = 8;
  int ended = 0, data = 0;
  while (offset < buffer->len) {
    assert(buffer->len - offset >= 12);
    size_t size = be32(buffer->data + offset);
    assert(size <= buffer->len - offset - 12);
    const unsigned char *tag = buffer->data + offset + 4;
    assert(crc32(0, tag, (uInt)(size + 4)) == be32(tag + 4 + size));
    if (!memcmp(tag, "IDAT", 4)) data++;
    if (!memcmp(tag, "IEND", 4)) { assert(!size); ended++; }
    offset += size + 12;
  }
  assert(ended == 1 && data > 0 && offset == buffer->len);
}

/* Text, antialiased curves, hairlines and translucent overlap: content whose
 * pixels depend on subpixel phase, so a clip with its own scale or a
 * fractional origin cannot reproduce the page. */
static fz_display_list *sample_page(fz_context *ctx, fz_rect page)
{
  fz_display_list *list = fz_new_display_list(ctx, page);
  fz_device *dev = fz_new_list_device(ctx, list);
  fz_font *font = fz_new_base14_font(ctx, "Times-Roman");
  float black[3] = {0, 0, 0}, red[3] = {.8f, .1f, .1f}, blue[3] = {.1f, .2f, .9f};
  for (int line = 0; line < 14; line++) {
    fz_text *text = fz_new_text(ctx);
    float size = 5.5f + line * .73f, x = page.x0 + 7.31f + line * .417f, y = page.y0 + 14.2f + line * 12.63f;
    const char *words = "Sharp pixels, fixed grid: efficiency fjord 0123456789";
    for (const char *c = words; *c; c++) {
      int glyph = fz_encode_character(ctx, font, *c);
      fz_show_glyph(ctx, text, font, fz_make_matrix(size, 0, 0, -size, x, y), glyph, *c, 0, 0, FZ_BIDI_LTR, FZ_LANG_UNSET);
      x += fz_advance_glyph(ctx, font, glyph, 0) * size;
    }
    fz_fill_text(ctx, dev, text, fz_identity, fz_device_rgb(ctx), black, 1, fz_default_color_params);
    fz_drop_text(ctx, text);
  }
  fz_path *curve = fz_new_path(ctx);
  fz_moveto(ctx, curve, page.x0 + 3.3f, page.y1 - 4.7f);
  for (int i = 0; i < 40; i++)
    fz_curveto(ctx, curve, page.x0 + 9.1f * i, page.y0 + 31.7f, page.x0 + 5.3f * i + 17.9f, page.y1 - 13.3f,
      page.x0 + 7.77f * i + 2.2f, page.y0 + 97.31f + 2.9f * i);
  fz_stroke_state *stroke = fz_new_stroke_state(ctx);
  stroke->linewidth = .37f;
  fz_stroke_path(ctx, dev, curve, stroke, fz_identity, fz_device_rgb(ctx), blue, .6f, fz_default_color_params);
  fz_path *disc = fz_new_path(ctx);
  fz_moveto(ctx, disc, page.x0 + 150.3f, page.y0 + 40.9f);
  fz_curveto(ctx, disc, page.x0 + 260.1f, page.y0 + 20.2f, page.x0 + 280.7f, page.y0 + 170.4f, page.x0 + 140.6f, page.y0 + 150.8f);
  fz_closepath(ctx, disc);
  fz_fill_path(ctx, dev, disc, 0, fz_identity, fz_device_rgb(ctx), red, .35f, fz_default_color_params);
  fz_fill_path(ctx, dev, disc, 1, fz_translate(31.4f, 17.7f), fz_device_rgb(ctx), blue, .35f, fz_default_color_params);
  fz_drop_path(ctx, disc); fz_drop_stroke_state(ctx, stroke); fz_drop_path(ctx, curve);
  fz_drop_font(ctx, font);
  fz_close_device(ctx, dev); fz_drop_device(ctx, dev);
  return list;
}

static void test_grid(fz_context *ctx)
{
  txp_preview_grid grid;
  /* Float noise in the page size must not add a row: 841.89/595.276 is a hair
   * above 99/70 in single precision, so widths that are multiples of 70 land a
   * fraction of a row past an integer. */
  const fz_rect a4 = {0, 0, 595.276f, 841.89f};
  for (int width = 70; width <= 7000; width += 70) {
    assert(txp_preview_page_grid(a4, width, &grid));
    assert(grid.width == width && grid.height == width / 70 * 99);
    fz_irect legacy = txp_preview_raster_size(595.276f, 841.89f, width, 16000000);
    if (legacy.x1 == width && grid.height <= 8192)
      assert(legacy.y1 == (int)ceil((double)841.89f * width / 595.276f));
  }
  assert(!txp_preview_page_grid(a4, 0, &grid) && !txp_preview_page_grid(a4, TXP_PREVIEW_MAX_GRID + 1, &grid));
  assert(!txp_preview_page_grid((fz_rect){0, 0, 1, 9999999}, 4096, &grid));
  assert(!txp_preview_page_grid((fz_rect){0, 0, 0, 10}, 100, &grid));

  /* Rectangles are clamped to the page and trimmed, never rescaled. */
  assert(txp_preview_page_grid(a4, 4000, &grid));
  fz_irect clip = txp_preview_surface_clip(&grid, (fz_irect){-5, 10, 5000, 300}, 4000000);
  assert(clip.x0 == 0 && clip.y0 == 10 && clip.x1 == 4000 && clip.y1 == 300);
  clip = txp_preview_surface_clip(&grid, (fz_irect){0, 0, 4000, 6000}, 4000000);
  assert(clip.x1 == 4000 && clip.y1 == 1000);
  clip = txp_preview_surface_clip(&grid, (fz_irect){256, 512, 2256, 6000}, 16000000);
  assert(clip.x0 == 256 && clip.y0 == 512 && clip.x1 == 2256 && clip.y1 == grid.height);
  clip = txp_preview_surface_clip(&grid, (fz_irect){4000, 0, 4100, 10}, 4000000);
  assert(fz_is_empty_irect(clip));
  assert(txp_preview_page_grid((fz_rect){0, 0, 100, 100}, 20000, &grid));
  clip = txp_preview_surface_clip(&grid, (fz_irect){100, 100, 20000, 20000}, 16000000);
  assert(clip.x0 == 100 && clip.x1 == 8292 && clip.y0 == 100 && clip.y1 == 100 + 16000000 / 8192);

  /* Any rectangle of a grid equals the same pixels of the complete page,
   * including pages whose origin is not at zero and non-integer scales. */
  const fz_rect pages[] = {{0, 0, 311.3f, 207.9f}, {-72.27f, -72.27f, 239.03f, 135.63f}, {13.37f, 5.11f, 324.67f, 213.01f}};
  const int widths[] = {311, 1000, 1401, 2000, 3404};
  uint32_t random = 7;
  size_t compared = 0;
  for (int p = 0; p < 3; p++) {
    fz_display_list *list = sample_page(ctx, pages[p]);
    for (int w = 0; w < 5; w++) {
      assert(txp_preview_page_grid(pages[p], widths[w], &grid));
      fz_irect whole = {0, 0, grid.width, grid.height};
      fz_pixmap *full = txp_preview_draw(ctx, list, pages[p], &grid, whole);
      assert(full->w == grid.width && full->h == grid.height && full->n == 3 && !full->alpha);
      int ink = 0;
      for (size_t i = 0; i < (size_t)full->stride * full->h && !ink; i++) ink = full->samples[i] != 255;
      assert(ink);
      /* Lattice seams stay within antialiasing noise of a single pass, and
       * the lattice size itself is not observable in a request's geometry. */
      fz_pixmap *single = txp_preview_draw_cells(ctx, list, pages[p], &grid, whole, 1 << 20);
      size_t different = 0;
      for (size_t i = 0; i < (size_t)full->stride * full->h; i++) {
        int delta = abs(full->samples[i] - single->samples[i]);
        assert(delta <= 24);
        different += delta != 0;
      }
      assert(different * 100 < (size_t)full->stride * full->h);
      fz_drop_pixmap(ctx, single);
      for (int trial = 0; trial < 12; trial++) {
        int box[4];
        for (int i = 0; i < 4; i++) {
          random ^= random << 13; random ^= random >> 17; random ^= random << 5;
          box[i] = random % (i % 2 ? grid.height : grid.width);
        }
        fz_irect r = {fz_mini(box[0], box[2]), fz_mini(box[1], box[3]), fz_maxi(box[0], box[2]) + 1, fz_maxi(box[1], box[3]) + 1};
        if (trial == 0) r = (fz_irect){0, 0, grid.width, 1};
        if (trial == 1) r = (fz_irect){grid.width - 1, grid.height - 1, grid.width, grid.height};
        if (trial == 2) r = (fz_irect){256, 0, fz_mini(768, grid.width), grid.height};
        r = txp_preview_surface_clip(&grid, r, 16000000);
        if (fz_is_empty_irect(r)) continue;
        fz_pixmap *part = txp_preview_draw(ctx, list, pages[p], &grid, r);
        assert(part->x == r.x0 && part->y == r.y0 && part->w == r.x1 - r.x0 && part->h == r.y1 - r.y0);
        for (int y = 0; y < part->h; y++)
          assert(!memcmp(part->samples + (size_t)y * part->stride,
            full->samples + (size_t)(r.y0 + y) * full->stride + (size_t)r.x0 * 3, (size_t)part->w * 3));
        compared += (size_t)part->w * part->h;
        fz_drop_pixmap(ctx, part);
      }
      fz_drop_pixmap(ctx, full);
    }
    fz_drop_display_list(ctx, list);
  }
  assert(compared > 1000000);
}

static void test_retained(fz_context *ctx)
{
  txp_preview p;
  txp_preview_init(&p);
  p.max_pixels = p.surface_pixels = 1000; /* 3000 sample bytes */
  const fz_rect page = {0, 0, 10, 10};
  fz_pixmap *small = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), (fz_irect){5, 7, 25, 17}, NULL, 0); /* 600 bytes */
  fz_pixmap *large = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), (fz_irect){0, 0, 40, 20}, NULL, 0); /* 2400 bytes */
  fz_pixmap *excess = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), (fz_irect){0, 0, 40, 26}, NULL, 0); /* 3120 bytes */
  assert(!txp_preview_retained(&p, 1) && !txp_preview_retained(&p, 0));
  txp_preview_retain(ctx, &p, 1, 3, page, 100, small);
  txp_preview_raster *r = txp_preview_retained(&p, 1);
  assert(r && r->page == 3 && r->grid_width == 100 && r->clip.x0 == 5 && r->clip.y0 == 7 && r->clip.x1 == 25 && r->clip.y1 == 17);
  txp_preview_retain(ctx, &p, 2, 3, page, 100, small);
  txp_preview_retain(ctx, &p, 3, 4, page, 100, small);
  txp_preview_retain(ctx, &p, 4, 4, page, 100, small);
  assert(txp_preview_retained(&p, 1) && txp_preview_retained(&p, 2) && txp_preview_retained(&p, 3) && txp_preview_retained(&p, 4));
  /* A fifth raster takes the least recently named slot. */
  assert(txp_preview_retained(&p, 1));
  txp_preview_retain(ctx, &p, 5, 5, page, 100, small);
  assert(txp_preview_retained(&p, 1) && !txp_preview_retained(&p, 2) && txp_preview_retained(&p, 5));
  /* The byte budget, not the slot count, bounds a large surface. */
  assert(txp_preview_retained(&p, 5));
  txp_preview_retain(ctx, &p, 6, 6, page, 200, large);
  assert(txp_preview_retained(&p, 6) && txp_preview_retained(&p, 5));
  assert(!txp_preview_retained(&p, 1) && !txp_preview_retained(&p, 3) && !txp_preview_retained(&p, 4));
  txp_preview_retain(ctx, &p, 7, 6, page, 200, excess);
  assert(!txp_preview_retained(&p, 7) && txp_preview_retained(&p, 6));
  txp_preview_retain(ctx, &p, 6, 6, page, 200, small); /* same identity replaces */
  assert(txp_preview_retained(&p, 6)->clip.x0 == 5);
  txp_preview_release(ctx, &p, 6);
  assert(!txp_preview_retained(&p, 6) && txp_preview_retained(&p, 5));
  txp_preview_release(ctx, &p, 0);
  assert(!txp_preview_retained(&p, 5));
  txp_preview_drop_cache(ctx, &p);
  fz_drop_pixmap(ctx, small); fz_drop_pixmap(ctx, large); fz_drop_pixmap(ctx, excess);
}

int main(void)
{
  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  assert(ctx);
  /* Standard pages preserve the requested physical-pixel width; large zooms
   * and extreme page aspect ratios always respect both hard bounds. */
  for (int budget = 4000000; budget <= 16000000; budget *= 2) {
    const float pages[][2] = {{595.276f,841.89f}, {612,792}, {841.89f,595.276f}, {.01f,9999999}, {9999999,.01f}};
    for (int p = 0; p < 5; p++) for (int width = 1; width <= 4096; width += 37) {
      fz_irect size = txp_preview_raster_size(pages[p][0], pages[p][1], width, budget);
      assert(size.x0 == 0 && size.y0 == 0 && size.x1 > 0 && size.y1 > 0);
      assert(size.x1 <= 8192 && size.y1 <= 8192 && (double)size.x1 * size.y1 <= budget);
      if (p < 3 && (double)width * ceil((double)pages[p][1] * width / pages[p][0]) <= budget)
        assert(size.x1 == width);
    }
  }
  fz_irect retina = txp_preview_raster_size(595.276f, 841.89f, 2240, 16000000);
  assert(retina.x1 == 2240 && retina.y1 == 3168);
  uint32_t random = 42;
  const int sizes[][2] = {{1,1}, {17,31}, {251,263}, {1600,2262}};
  for (int mode = 0; mode < 4; mode++) for (int i = 0; i < 4; i++) {
    int w = sizes[i][0], h = sizes[i][1];
    fz_pixmap *original = fz_new_pixmap(ctx, fz_device_rgb(ctx), w, h, NULL, 0);
    fz_set_pixmap_resolution(ctx, original, 144, 144);
    unsigned char *p = fz_pixmap_samples(ctx, original);
    for (int y = 0; y < h; y++) for (int x = 0; x < w * 3; x++) {
      random ^= random << 13; random ^= random >> 17; random ^= random << 5;
      p[y * original->stride + x] = mode == 0 ? 255 : mode == 1 ? ((x/3+y)%17 ? 255 : 0) : mode == 2 ? random : 255;
    }
    if (mode == 3) { p[(h-1)*original->stride+(w-1)*3] = 127; }
    fz_buffer *png = txp_preview_png(ctx, original);
    check_chunks(png);
    fz_image *image = fz_new_image_from_buffer(ctx, png);
    fz_pixmap *decoded = fz_get_unscaled_pixmap_from_image(ctx, image);
    assert(decoded->w == w && decoded->h == h && decoded->n == (mode < 2 ? 1 : 3));
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 3; c++)
      assert(original->samples[y * original->stride + 3*x + c] == decoded->samples[y * decoded->stride + decoded->n*x + (decoded->n == 1 ? 0 : c)]);
    assert(image->xres == 144 && image->yres == 144);
    fz_drop_pixmap(ctx, decoded); fz_drop_image(ctx, image);
    fz_drop_buffer(ctx, png); fz_drop_pixmap(ctx, original);
  }
  /* A high-entropy Retina raster can exceed the old 16 MiB wire limit. */
  fz_pixmap *large = fz_new_pixmap(ctx, fz_device_rgb(ctx), 2240, 3168, NULL, 0);
  for (size_t i = 0; i < (size_t)large->stride * large->h; i++) {
    random ^= random << 13; random ^= random >> 17; random ^= random << 5;
    large->samples[i] = random;
  }
  fz_buffer *large_png = txp_preview_png(ctx, large);
  assert(large_png->len > 16u * 1024u * 1024u && large_png->len < TXP_PREVIEW_PNG_BYTES);
  check_chunks(large_png);
  fz_image *large_image = fz_new_image_from_buffer(ctx, large_png);
  fz_pixmap *large_decoded = fz_get_unscaled_pixmap_from_image(ctx, large_image);
  assert(large_decoded->w == large->w && large_decoded->h == large->h && large_decoded->n == 3);
  for (int y = 0; y < large->h; y++)
    assert(!memcmp(large->samples + y * large->stride, large_decoded->samples + y * large_decoded->stride, large->w * 3));
  fz_drop_pixmap(ctx, large_decoded); fz_drop_image(ctx, large_image);
  fz_drop_buffer(ctx, large_png); fz_drop_pixmap(ctx, large);
  /* Padded rows occur for pixmap views; padding must not enter the stream. */
  unsigned char padded[23 * 128];
  memset(padded, 238, sizeof padded);
  for (int y = 0; y < 23; y++) for (int x = 0; x < 17; x++) for (int c = 0; c < 3; c++) padded[y*128+3*x+c] = x+y;
  fz_pixmap *view = fz_new_pixmap_with_data(ctx, fz_device_rgb(ctx), 17, 23, NULL, 0, 128, padded);
  fz_buffer *view_png = txp_preview_png(ctx, view);
  check_chunks(view_png);
  fz_image *view_image = fz_new_image_from_buffer(ctx, view_png);
  fz_pixmap *view_decoded = fz_get_unscaled_pixmap_from_image(ctx, view_image);
  assert(view_decoded->n == 1);
  for (int y = 0; y < 23; y++) for (int x = 0; x < 17; x++) assert(view_decoded->samples[y*view_decoded->stride+x] == x+y);
  fz_drop_pixmap(ctx, view_decoded); fz_drop_image(ctx, view_image);
  fz_drop_buffer(ctx, view_png); fz_drop_pixmap(ctx, view);
  fz_pixmap *gray = fz_new_pixmap(ctx, fz_device_gray(ctx), 17, 23, NULL, 0);
  fz_clear_pixmap_with_value(ctx, gray, 123);
  fz_buffer *png = txp_preview_png(ctx, gray);
  fz_buffer *reference = fz_new_buffer_from_pixmap_as_png(ctx, gray, fz_default_color_params);
  assert(png->len == reference->len && !memcmp(png->data, reference->data, png->len));
  fz_drop_buffer(ctx, png); fz_drop_buffer(ctx, reference); fz_drop_pixmap(ctx, gray);
  /* Sparse edits, erasure, color changes and row padding use exact RGB bytes. */
  unsigned char old_data[23*128], new_data[23*97];
  memset(old_data, 55, sizeof old_data); memset(new_data, 77, sizeof new_data);
  for (int y=0;y<23;y++) { memset(old_data+y*128,255,17*3); memset(new_data+y*97,255,17*3); }
  fz_pixmap *before=fz_new_pixmap_with_data(ctx,fz_device_rgb(ctx),17,23,NULL,0,128,old_data);
  fz_pixmap *after=fz_new_pixmap_with_data(ctx,fz_device_rgb(ctx),17,23,NULL,0,97,new_data);
  fz_irect rect;
  assert(txp_preview_dirty_rect(ctx,before,after,&rect) && rect.x1==0 && rect.y1==0);
  new_data[22*97+16*3]=0;
  assert(txp_preview_dirty_rect(ctx,before,after,&rect));
  assert(rect.x0==16 && rect.x1==17 && rect.y0==22 && rect.y1==23);
  new_data[0]=0;
  assert(txp_preview_dirty_rect(ctx,before,after,&rect));
  assert(rect.x0==0 && rect.x1==17 && rect.y0==0 && rect.y1==23);
  assert(txp_preview_dirty_rect(ctx,after,before,&rect)); /* white erases color */
  assert(rect.x0==0 && rect.x1==17 && rect.y0==0 && rect.y1==23);
  assert(!txp_preview_dirty_rect(ctx,NULL,after,&rect));
  fz_drop_pixmap(ctx,before);fz_drop_pixmap(ctx,after);
  test_grid(ctx);
  test_retained(ctx);
  fz_drop_context(ctx);
  puts("preview PNG: lossless pixels, bounded multi-chunk streams, CRCs, resolution and grayscale fallback passed");
  return 0;
}
