/* Deterministic editing transitions using only prefixes of valid documents. */
#include "fuzz/common.h"

static size_t snapshots;

static fz_buffer *document(fz_context *ctx, unsigned revision)
{
  fz_buffer *b = fz_new_buffer(ctx, 512);
  dvi_preamble(ctx, b);
  for (unsigned page = 0; page < 2; page++) {
    char destination[100];
    dvi_bop(ctx, b);
    dvi_special_str(ctx, b, "color push rgb 1 0 0");
    fz_append_byte(ctx, b, 141); /* push */
    fz_append_byte(ctx, b, 146); /* right4 */
    dvi_u32(ctx, b, 655360 * (revision + page + 1));
    fz_append_byte(ctx, b, 132); /* set_rule */
    dvi_u32(ctx, b, 655360); dvi_u32(ctx, b, 1310720);
    fz_append_byte(ctx, b, 142); /* pop */
    dvi_special_str(ctx, b, "color pop");
    snprintf(destination, sizeof destination,
             "pdf:dest (revision-%u-page-%u) [@thispage /XYZ @xpos @ypos null]",
             revision, page);
    dvi_special_str(ctx, b, destination);
    dvi_eop(ctx, b);
  }
  return b;
}

/* A fresh index is the oracle for page availability, destinations, pixels and
 * annotations. Repeat rendering to cover display-list cache reuse as well. */
static void check(fz_context *ctx, incdvi_t *d, fz_buffer *b)
{
  incdvi_t *fresh = incdvi_new(ctx, (dvi_reshooks){0});
  fz_try(ctx) {
    incdvi_update(ctx, d, b);
    incdvi_update(ctx, fresh, b);
    assert(incdvi_output_started(d) == incdvi_output_started(fresh));
    int pages = incdvi_page_count(fresh);
    assert(incdvi_page_count(d) == pages);
    for (unsigned revision = 0; revision < 2; revision++)
      for (unsigned page = 0; page < 2; page++) {
        char name[40];
        snprintf(name, sizeof name, "revision-%u-page-%u", revision, page);
        assert(incdvi_destination_page(ctx, d, name) ==
               incdvi_destination_page(ctx, fresh, name));
      }
    for (int page = 0; page < pages; page++) {
      uint64_t expected = render(ctx, fresh, b, page);
      assert(render(ctx, d, b, page) == expected);
      assert(render(ctx, d, b, page) == expected);
    }
    snapshots++;
  }
  fz_always(ctx) { incdvi_free(ctx, fresh); }
  fz_catch(ctx) {
    fprintf(stderr, "editing snapshot %zu (length %zu): %s\n",
            snapshots, b->len, txp_take_error(ctx));
    abort();
  }
}

int main(void)
{
  fz_context *ctx = fuzz_context();
  incdvi_t *d = incdvi_new(ctx, (dvi_reshooks){0});
  fz_buffer *first = document(ctx, 0), *second = document(ctx, 1);
  size_t full = first->len;

  /* Output arriving one byte at a time, including split instruction fields. */
  for (size_t len = 0; len <= full; len++) {
    first->len = len;
    check(ctx, d, first);
  }
  assert(incdvi_page_count(d) == 2);
  assert(incdvi_destination_page(ctx, d, "revision-0-page-0") == 0);
  assert(incdvi_destination_page(ctx, d, "revision-0-page-1") == 1);
  uint64_t original = render_output(ctx, d, first, 0, false);

  /* Roll back to each prefix, then append the remaining output again. */
  for (size_t len = full; ; len--) {
    first->len = len;
    check(ctx, d, first);
    first->len = full;
    check(ctx, d, first);
    if (!len) break;
  }

  /* A compiler restart may replace bytes without changing the output length.
   * The caller must reset before replacing that output. Retained caches must
   * still reflect the new program and destination names. */
  assert(first->len == second->len);
  for (unsigned cycle = 0; cycle < 8; cycle++) {
    incdvi_reset(d);
    fz_buffer *b = cycle % 2 ? first : second;
    size_t complete = b->len;
    for (size_t len = 0; len <= complete; len++) {
      b->len = len;
      check(ctx, d, b);
    }
    assert(incdvi_page_count(d) == 2);
    if (b == second) assert(render_output(ctx, d, b, 0, false) != original);
    else assert(render_output(ctx, d, b, 0, false) == original);
    incdvi_update(ctx, d, NULL);
    assert(incdvi_page_count(d) == 0);
    check(ctx, d, b);
  }

  fz_drop_buffer(ctx, first); fz_drop_buffer(ctx, second);
  incdvi_free(ctx, d);
  fuzz_drop_context(ctx);
  printf("PASS: %zu editing snapshots, fresh-index/render equivalence and allocation cleanup\n", snapshots);
  return 0;
}
