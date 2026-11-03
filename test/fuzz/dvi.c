/* One DVI/XDV output, indexed and rendered in one go.
 *
 * Covers instruction sizing for every DVI flavour, page header parsing, the
 * incremental indexer and the interpreter. Input is copied into an exactly
 * sized allocation: libFuzzer's own buffer may have spare capacity that would
 * hide overreads from ASan. */
#include "common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  if (size > 65536)
    return 0;
  uint8_t *input = malloc(size ? size : 1);
  fuzz_check(input);
  memcpy(input, data, size);

  static const enum dvi_version versions[] = {
    DVI_STANDARD, DVI_PTEX, DVI_XDV5, DVI_XDV6, DVI_XDV7, DVI_VF
  };
  for (size_t v = 0; v < sizeof versions / sizeof *versions; v++)
    for (size_t pos = 0; pos < size;)
    {
      int n = dvi_instr_size(input + pos, (int)(size - pos), versions[v]);
      if (n <= 0 || (size_t)n > size - pos)
        break;
      pos += (size_t)n;
    }

  float w, h;
  bool landscape;
  (void)dvi_interp_bop(input, (int)size, &w, &h, &landscape);

  fz_context *ctx = fuzz_context();
  incdvi_t *d = incdvi_new(ctx, (dvi_reshooks){0});
  fz_buffer *b = fz_new_buffer_from_shared_data(ctx, input, size);
  fz_try(ctx)
  {
    incdvi_update(ctx, d, b);
    int count = incdvi_page_count(d);
    for (int i = 0; i < count && i < 4; i++)
    {
      probe(ctx, d, b, i);
      probe(ctx, d, b, i); // second render may hit the display-list cache
    }
    if (count > 4)
      probe(ctx, d, b, count - 1);
  }
  fz_catch(ctx)
    (void)txp_take_error(ctx);
  recover(ctx, d);
  fz_drop_buffer(ctx, b);
  incdvi_free(ctx, d);
  fuzz_drop_context(ctx);
  free(input);
  return 0;
}
