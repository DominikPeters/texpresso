#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mydvi.h"
#include "mydvi_interp.h"

typedef struct { fz_device super; int count; } stroke_device;
static void record_stroke(fz_context *ctx, fz_device *dev, const fz_path *path,
                          const fz_stroke_state *stroke, fz_matrix ctm,
                          fz_colorspace *cs, const float *color, float alpha,
                          fz_color_params params)
{
  stroke_device *record = (stroke_device *)dev;
  int variant = record->count++ % 3, len = variant == 0 ? 2 : variant == 1 ? 4 : 0;
  assert(stroke->dash_len == len);
  assert(stroke->dash_phase == variant);
  assert(stroke->linewidth == 1);
  for (int i = 0; i < len; i++) assert(stroke->dash_list[i] == (variant == 0 ? i+2 : i+1));
}

static void special(fz_context *ctx, dvi_context *dc, const char *code)
{
  bool handled = dvi_exec_special(ctx, dc, dvi_context_state(dc), code, code + strlen(code));
  if (!handled) fprintf(stderr, "Special failed: %s\n", code);
  assert(handled);
}

static void test_graphics_clipping(fz_context *ctx)
{
  fz_pixmap *pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx),
      fz_make_irect(0, 0, 128, 128), NULL, 0);
  fz_clear_pixmap_with_value(ctx, pix, 255);
  fz_device *device = fz_new_draw_device(ctx, fz_identity, pix);
  dvi_context *dc = dvi_context_new(ctx, (dvi_reshooks){0});
  dvi_context_begin_frame(ctx, dc, device);

  // XeTeX's adjustbox sequence clips the red rectangle to its left half.
  special(ctx, dc, "pdf:content q");
  special(ctx, dc, "pdf:code 0 0 10 20 re W n");
  special(ctx, dc, "pdf:content q");
  special(ctx, dc, "pdf:code 1 0 0 rg 0 0 20 20 re f");
  special(ctx, dc, "pdf:literal direct Q");
  special(ctx, dc, "pdf:literal direct Q");
  // XeTeX's PDF import scopes must also release their clip before more ink.
  special(ctx, dc, "x:gsave");
  special(ctx, dc, "pdf:code 0 0 10 20 re W n");
  special(ctx, dc, "x:grestore");
  special(ctx, dc, "pdf:code 0 0 1 rg 20 0 10 20 re f");
  // An unmatched clip must still allow the draw device to close.
  special(ctx, dc, "pdf:code 0 0 10 20 re W n");
  dvi_context_end_frame(ctx, dc);
  fz_close_device(ctx, device);

  const unsigned char *row = pix->samples + 62 * pix->stride;
  assert(!memcmp(row + 77 * 3, "\xff\0\0", 3));
  assert(!memcmp(row + 87 * 3, "\xff\xff\xff", 3));
  assert(!memcmp(row + 97 * 3, "\0\0\xff", 3));
  fz_drop_device(ctx, device);
  dvi_context_free(ctx, dc);
  fz_drop_pixmap(ctx, pix);
  puts("PASS: XeTeX clip scopes preserve the expected pixels and close cleanly");
}

int main(void)
{
  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT); assert(ctx);
  fz_rect box = {0, 0, 300, 300};
  fz_display_list *list = fz_new_display_list(ctx, box);
  fz_device *device = fz_new_list_device(ctx, list);
  dvi_context *dc = dvi_context_new(ctx, (dvi_reshooks){0});
  dvi_context_begin_frame(ctx, dc, device);
  fz_buffer *code = fz_new_buffer(ctx, 512);
  fz_append_string(ctx, code, "pdf:code ");
  const char *operators[] = {"S", "s", "B", "B*", "b", "b*"};
  for (int i = 0; i < 6; i++) {
    const char *dash = i % 3 == 0 ? "[2 3]" : i % 3 == 1 ? "[1 2 3 4]" : "[]";
    fz_append_printf(ctx, code, "%s %d d 1 w 0 0 m 20 0 l 20 20 l %s ", dash, i % 3, operators[i]);
  }
  assert(dvi_exec_special(ctx, dc, dvi_context_state(dc), (char *)code->data, (char *)code->data + code->len));
  dvi_context_end_frame(ctx, dc);
  fz_close_device(ctx, device); fz_drop_device(ctx, device);
  dvi_context_free(ctx, dc); fz_drop_buffer(ctx, code);
  // The interpreter has released its local stroke. Replaying must retain
  // every original pattern even after later operators change its dash length.
  stroke_device *record = fz_new_derived_device(ctx, stroke_device);
  record->super.stroke_path = record_stroke;
  fz_run_display_list(ctx, list, &record->super, fz_identity, box, NULL);
  assert(record->count == 6);
  fz_close_device(ctx, &record->super); fz_drop_device(ctx, &record->super);
  fz_drop_display_list(ctx, list);
  test_graphics_clipping(ctx);
  fz_drop_context(ctx);
  puts("PASS: DVI dashed/solid strokes retain independently owned MuPDF dash arrays");
}
