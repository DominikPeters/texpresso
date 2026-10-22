/* Regression coverage for native inverse SyncTeX geometry. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mupdf/fitz.h>
#include "../src/frontend/synctex.h"

static int selected;
void editor_synctex_word(const char *dir, const char *file, int len,
                        int line, int column, const char *word)
{
  (void)dir; (void)file; (void)len; (void)column; (void)word;
  selected = line;
}
void editor_notify_file_opened(int index, const char *path, int len)
{ (void)index; (void)path; (void)len; }
void myabort_(const char *file, int line, const char *message, unsigned code)
{
  fprintf(stderr, "%s:%d: %s (%u)\n", file, line, message, code);
  abort();
}
static void hit(fz_context *ctx, synctex_t *stx, fz_buffer *buf, int x, int y, int expected)
{
  selected = 0;
  synctex_scan_word(ctx, stx, buf, ".", 0, x, y, NULL);
  if (selected != expected)
  {
    fprintf(stderr, "Click %d,%d selected line %d, expected %d\n", x, y, selected, expected);
    abort();
  }
}
static void forward_stream(fz_context *ctx)
{
  const char *prefix = "Input:1:main.tex\nInput:2:chapter.tex\n{1\n[1,99:0,1000:1000,1000,0\n"
    "(1,99:0,1000:1000,1000,0\n(2,10:100,300:800,10,3\nk2,10:200,300:10\n)\n)\n]\n}1\n";
  const char *closed = "/2\n";
  const char *last = "{2\n[1,99:0,1000:1000,1000,0\n"
    "(1,99:0,1000:1000,1000,0\n(2,30:100,300:800,10,3\nk2,30:200,300:10\n)\n)\n]\n}2\n";
  synctex_t *stx = synctex_new(ctx);
  fz_buffer *buf = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)prefix, strlen(prefix));
  synctex_update(ctx, stx, buf);
  synctex_set_target(stx, 0, "chapter.tex", 30);
  int page = -1, x, y;
  assert(!synctex_find_resolved_target(ctx, stx, buf, false, &page, &x, &y));
  assert(synctex_has_target(stx)); /* Page one is only a provisional candidate. */
  fz_append_string(ctx, buf, closed); synctex_update(ctx, stx, buf);
  assert(!synctex_find_resolved_target(ctx, stx, buf, false, &page, &x, &y));
  assert(synctex_has_target(stx)); /* Closing an input does not ship its last page. */
  fz_append_string(ctx, buf, last); synctex_update(ctx, stx, buf);
  assert(synctex_find_resolved_target(ctx, stx, buf, false, &page, &x, &y));
  assert(page == 1 && !synctex_has_target(stx));

  synctex_set_target(stx, 0, "chapter.tex", 99); /* A non-rendering source line. */
  assert(!synctex_find_resolved_target(ctx, stx, buf, false, &page, &x, &y));
  assert(synctex_find_resolved_target(ctx, stx, buf, true, &page, &x, &y));
  assert(page == 1); /* Final fallback is returned even without new records. */

  synctex_set_target(stx, 0, "chapter.tex", 99);
  assert(!synctex_find_resolved_target(ctx, stx, buf, false, &page, &x, &y));
  buf->len = strlen(prefix) + strlen(closed); synctex_update(ctx, stx, buf);
  assert(synctex_find_resolved_target(ctx, stx, buf, true, &page, &x, &y));
  assert(page == 0); /* candidate_page == retained page count was removed. */
  fz_drop_buffer(ctx, buf); synctex_free(ctx, stx);
}

int main(void)
{
  const char *contents =
    "Input:1:sample.tex\n{1\n[1,99:0,1000:1000,1000,0\n"
    "(1,20:100,300:800,10,3\nk1,18:200,300:10\nk1,19:500,=:10\n)\n"
    "(1,20:100,320:800,10,2\ng1,19:120,320\ng1,19:200,=\nk1,20:900,320:650\n)\n"
    "r1,99:150,330:0,30,10\n"
    "(1,55:100,400:800,12,2\nk1,55:300,400:-50\n"
    "(1,56:600,410:30,3,1\nx1,56:610,410\n)\nx1,55:400,400\n)\n"
    "(1,63:100,500:100,10,2\n)\n]\n}1\n";
  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  assert(ctx);
  synctex_t *stx = synctex_new(ctx);
  fz_buffer *buf = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)contents, strlen(contents));
  synctex_update(ctx, stx, buf);
  hit(ctx, stx, buf, 150, 295, 18); /* Above baseline, not enclosing paragraph. */
  hit(ctx, stx, buf, 600, 295, 19); /* Compressed vertical coordinate. */
  hit(ctx, stx, buf, 150, 325, 19); /* Below line, not enclosing page. */
  hit(ctx, stx, buf, 325, 395, 55); /* Negative kern width. */
  hit(ctx, stx, buf, 400, 392, 55); /* Parent height restored after nested hbox. */
  hit(ctx, stx, buf, 610, 409, 56);
  hit(ctx, stx, buf, 150, 498, 63); /* Glyph-only hbox fallback. */
  fz_drop_buffer(ctx, buf);
  synctex_free(ctx, stx);
  forward_stream(ctx);
  fz_drop_context(ctx);
  puts("PASS: inverse SyncTeX line geometry");
  return 0;
}
