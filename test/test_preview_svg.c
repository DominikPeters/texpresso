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
    txp_render_features features;
    txp_preview_render_features(ctx, list, &features);
    assert(features.paths == 1 && features.path_bytes > 0 && features.glyphs == 0);
    assert(features.groups == (i != 0));
    assert(features.svg_eligible == (i < 2));
    fz_drop_display_list(ctx, list);
  }
  for (int flag = 0; flag < 4; flag++) {
    fz_pixmap *pix = fz_new_pixmap(ctx, flag == 3 ? fz_device_cmyk(ctx) : fz_device_rgb(ctx), 2, 2, NULL, 0);
    fz_clear_pixmap_with_value(ctx, pix, 255);
    fz_image *image = fz_new_image_from_pixmap(ctx, pix, NULL);
    if (flag == 1) image->use_decode = 1;
    if (flag == 2) image->use_colorkey = 1;
    fz_display_list *list = fz_new_display_list(ctx, fz_make_rect(0, 0, 300, 300));
    fz_device *dev = fz_new_list_device(ctx, list);
    fz_fill_image(ctx, dev, image, fz_scale(100, 100), 1, fz_default_color_params);
    fz_close_device(ctx, dev); fz_drop_device(ctx, dev);
    assert((txp_preview_svg_fallback(ctx, list) != NULL) == (flag != 0));
    txp_render_features features;
    txp_preview_render_features(ctx, list, &features);
    assert(features.images == 1 && features.image_pixels == 4 && !features.svg_eligible);
    fz_drop_display_list(ctx, list); fz_drop_image(ctx, image); fz_drop_pixmap(ctx, pix);
  }
  /* Text fitting ignores figures, preserves translated page coordinates, and
   * does not invent geometry on blank or graphics-only pages. */
  fz_display_list *list = page(ctx, 0, 1, 0, FZ_BLEND_NORMAL);
  assert(fz_is_empty_rect(txp_preview_text_bounds(ctx, list)));
  fz_drop_display_list(ctx, list);
  list = fz_new_display_list(ctx, fz_make_rect(20, 30, 620, 830));
  fz_device *dev = fz_new_list_device(ctx, list);
  fz_font *font = fz_new_base14_font(ctx, "Helvetica");
  fz_text *text = fz_new_text(ctx);
  fz_show_string(ctx, text, font, fz_make_matrix(12, 0, 0, 12, 120, 200), "Text width", 0, 0, FZ_BIDI_NEUTRAL, FZ_LANG_UNSET);
  float black[3] = {0, 0, 0};
  fz_fill_text(ctx, dev, text, fz_identity, fz_device_rgb(ctx), black, 1, fz_default_color_params);
  fz_path *path = fz_new_path(ctx); fz_rectto(ctx, path, 20, 30, 620, 830);
  fz_fill_path(ctx, dev, path, 0, fz_identity, fz_device_rgb(ctx), black, 1, fz_default_color_params);
  fz_close_device(ctx, dev); fz_drop_device(ctx, dev);
  fz_rect bounds = txp_preview_text_bounds(ctx, list);
  assert(bounds.x0 > 115 && bounds.x0 < 125 && bounds.x1 > 150 && bounds.x1 < 220);
  assert(bounds.y0 > 180 && bounds.y1 < 210);
  txp_render_features features;
  txp_preview_render_features(ctx, list, &features);
  assert(features.glyphs == 10 && features.text_spans > 0 && features.fonts == 1);
  assert(features.unique_glyphs > 0 && features.unique_glyphs < features.glyphs && !features.glyph_overflow);
  assert(!features.font_overflow && features.paths == 1 && features.svg_eligible);
  fz_drop_path(ctx, path); fz_drop_text(ctx, text); fz_drop_font(ctx, font); fz_drop_display_list(ctx, list);
  fz_drop_context(ctx);
  puts("PASS: SVG keeps ordinary vectors and isolated opacity; falls back for non-isolation, knockout, blending");
}
