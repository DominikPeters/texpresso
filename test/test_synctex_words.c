/* Text refinement must remain optional, bounded, and independent of the foreground page. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <locale.h>
#include "renderer.h"
#include "mydvi.h"

static fz_display_list *text_page(fz_context *ctx, const char *content)
{
  fz_display_list *list = fz_new_display_list(ctx, fz_make_rect(0, 0, 600, 800));
  fz_device *dev = fz_new_list_device(ctx, list);
  fz_font *font = fz_new_base14_font(ctx, "Helvetica");
  fz_text *text = fz_new_text(ctx);
  fz_show_string(ctx, text, font, fz_make_matrix(12, 0, 0, 12, 100, 200),
                 content, 0, 0, FZ_BIDI_NEUTRAL, FZ_LANG_UNSET);
  float black[3] = {0, 0, 0};
  fz_fill_text(ctx, dev, text, fz_identity, fz_device_rgb(ctx), black, 1, fz_default_color_params);
  fz_close_device(ctx, dev);
  fz_drop_device(ctx, dev); fz_drop_text(ctx, text); fz_drop_font(ctx, font);
  return list;
}
int main(void)
{
  // A GUI/application is not required to call setlocale for Unicode words.
  assert(setlocale(LC_CTYPE, "C"));
  int code[32];
  assert(dvi_glyph_unicode("f_f_i", code, 32) == 3 && code[0] == 'f' && code[2] == 'i');
  assert(dvi_glyph_unicode("uni006600660069.alt", code, 32) == 3 && code[2] == 'i');
  assert(dvi_glyph_unicode("ffi", code, 32) == 3 && code[2] == 'i');
  assert(dvi_glyph_unicode("eacute", code, 32) == 1 && code[0] == 0xe9);
  assert(!dvi_glyph_unicode("f_unknown", code, 32));
  assert(!dvi_glyph_unicode("uniD8000061", code, 32));
  assert(!dvi_glyph_unicode("f_f_i", code, 2));
  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  txp_renderer *renderer = txp_renderer_new(ctx, NULL);
  fz_display_list *first = text_page(ctx, "office café office");
  fz_display_list *second = text_page(ctx, "unrelated");
  txp_renderer_set_contents(ctx, renderer, second);
  fz_rect all = fz_make_rect(0, 0, 600, 800), found;
  assert(txp_renderer_find_source_word(ctx, renderer, first, "café", &all, 1, &found));
  assert(found.x0 > 120 && found.x1 < 190);
  assert(!txp_renderer_find_source_word(ctx, renderer, first, "office", &all, 1, &found));
  assert(!txp_renderer_find_source_word(ctx, renderer, first, "off", &all, 1, &found));
  assert(!txp_renderer_find_source_word(ctx, renderer, NULL, "café", &all, 1, &found));
  fz_rect outside = fz_make_rect(400, 400, 500, 500);
  assert(!txp_renderer_find_source_word(ctx, renderer, first, "café", &outside, 1, &found));
  assert(txp_renderer_find_source_word(ctx, renderer, first, "café", &all, 1, &found));
  char *word = txp_renderer_cached_word(ctx, renderer, first,
    fz_make_point((found.x0 + found.x1)/2, (found.y0 + found.y1)/2));
  assert(word && !strcmp(word, "café")); fz_free(ctx, word);
  assert(txp_renderer_get_contents(ctx, renderer) == second);
  assert(!txp_renderer_cached_word(ctx, renderer, first, fz_make_point(500, 700)));
  /* Logical ligature characters survive zero-width continuation geometry. */
  fz_display_list *ligature = text_page(ctx, "oﬃce");
  assert(txp_renderer_find_source_word(ctx, renderer, ligature, "office", &all, 1, &found));
  txp_renderer_set_contents(ctx, renderer, first);
  assert(txp_renderer_find_source_word(ctx, renderer, first, "café", &all, 1, &found));
  fz_drop_display_list(ctx, ligature); fz_drop_display_list(ctx, first); fz_drop_display_list(ctx, second);
  txp_renderer_free(ctx, renderer); fz_drop_context(ctx);
  assert(!strcmp(setlocale(LC_CTYPE, NULL), "C")); // The private locale was released, not installed globally.
  puts("PASS: cached source words, Unicode, ligatures, and ambiguous matches");
}
