/* DVI specials (\special{...}): colour stacks, PDF content streams,
 * annotations, transformations and page size declarations.
 *
 * The input is a list of specials separated by NUL bytes. They are placed on
 * one page between rules and movements, so that they interact with drawing
 * state, then the page is indexed and rendered twice. The direct fuzz target
 * is the re2c special parser and the PDF content lexer, which are hard to
 * reach through raw DVI bytes. */
#include "common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  if (size > 16384)
    return 0;
  fz_context *ctx = fuzz_context();
  incdvi_t *d = incdvi_new(ctx, (dvi_reshooks){0});
  fz_buffer *b = fz_new_buffer(ctx, size + 256);
  dvi_preamble(ctx, b);

  // A page header special must also be prescanned for page dimensions.
  dvi_bop(ctx, b);
  int depth = 0;
  for (size_t start = 0, count = 0; start <= size && count < 64; count++)
  {
    const uint8_t *end = memchr(data + start, 0, size - start);
    size_t n = end ? (size_t)(end - (data + start)) : size - start;
    dvi_special(ctx, b, (const char *)data + start, n);
    switch (count % 4)
    {
      case 0: // rule
        fz_append_byte(ctx, b, 132);
        dvi_u32(ctx, b, 65536 * (1 + count));
        dvi_u32(ctx, b, 65536 * 4);
        break;
      case 1: // push and move
        fz_append_byte(ctx, b, 141);
        depth++;
        fz_append_byte(ctx, b, 160);
        dvi_u32(ctx, b, 65536 * 7);
        break;
      case 2:
        if (depth)
        {
          fz_append_byte(ctx, b, 142);
          depth--;
        }
        break;
      case 3: // a second page splits stateful specials across pages
        dvi_eop(ctx, b);
        dvi_bop(ctx, b);
        depth = 0;
        break;
    }
    start += n + 1;
  }
  while (depth--)
    fz_append_byte(ctx, b, 142);
  dvi_eop(ctx, b);

  fz_try(ctx)
  {
    incdvi_update(ctx, d, b);
    int count = incdvi_page_count(d);
    for (int i = 0; i < count; i++)
    {
      float w, h;
      bool landscape;
      fz_try(ctx)
        incdvi_page_dim(ctx, d, b, i, &w, &h, &landscape);
      fz_catch(ctx)
        (void)txp_take_error(ctx);
      probe(ctx, d, b, i);
    }
    for (int i = count - 1; i >= 0; i--) // out of order, cache hits
      probe(ctx, d, b, i);
  }
  fz_catch(ctx)
    (void)txp_take_error(ctx);

  recover(ctx, d);
  fz_drop_buffer(ctx, b);
  incdvi_free(ctx, d);
  fuzz_drop_context(ctx);
  return 0;
}
