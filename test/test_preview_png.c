#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>
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

int main(void)
{
  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  assert(ctx);
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
  fz_drop_context(ctx);
  puts("preview PNG: lossless pixels, bounded multi-chunk streams, CRCs, resolution and grayscale fallback passed");
  return 0;
}
