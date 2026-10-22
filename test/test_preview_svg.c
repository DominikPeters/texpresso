#include <assert.h>
#include <stdio.h>
#include "preview.h"
static fz_display_list *page(fz_context *ctx, int group, int isolated, int knockout, int blend)
{
  fz_rect box = {0, 0, 300, 300};
  fz_display_list *list = fz_new_display_list(ctx, box);
  fz_device *dev = fz_new_list_device(ctx, list);
  fz_path *path = fz_new_path(ctx);
  fz_rectto(ctx, path, 10, 10, 100, 100);
  float rgb[3] = {1, 0, 0};
  if (group) fz_begin_group(ctx, dev, box, fz_device_rgb(ctx), isolated, knockout, blend, 0.5f);
  fz_fill_path(ctx, dev, path, 0, fz_identity, fz_device_rgb(ctx), rgb, 1, fz_default_color_params);
  if (group) fz_end_group(ctx, dev);
  fz_close_device(ctx, dev); fz_drop_device(ctx, dev); fz_drop_path(ctx, path);
  return list;
}
int main(void)
{
  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  assert(ctx);
  for (int i = 0; i < 5; i++) {
    fz_display_list *list = page(ctx, i != 0, i != 2, i == 3, i == 4 ? FZ_BLEND_MULTIPLY : FZ_BLEND_NORMAL);
    const char *reason = txp_preview_svg_fallback(ctx, list);
    assert((reason != NULL) == (i >= 2));
    fz_drop_display_list(ctx, list);
  }
  for (int flag = 0; flag < 3; flag++) {
    fz_pixmap *pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), 2, 2, NULL, 0);
    fz_clear_pixmap_with_value(ctx, pix, 255);
    fz_image *image = fz_new_image_from_pixmap(ctx, pix, NULL);
    if (flag == 1) image->use_decode = 1;
    if (flag == 2) image->use_colorkey = 1;
    fz_display_list *list = fz_new_display_list(ctx, fz_make_rect(0, 0, 300, 300));
    fz_device *dev = fz_new_list_device(ctx, list);
    fz_fill_image(ctx, dev, image, fz_scale(100, 100), 1, fz_default_color_params);
    fz_close_device(ctx, dev); fz_drop_device(ctx, dev);
    assert((txp_preview_svg_fallback(ctx, list) != NULL) == (flag != 0));
    fz_drop_display_list(ctx, list); fz_drop_image(ctx, image); fz_drop_pixmap(ctx, pix);
  }
  fz_drop_context(ctx);
  puts("PASS: SVG keeps ordinary vectors and isolated opacity; falls back for non-isolation, knockout, blending");
}
