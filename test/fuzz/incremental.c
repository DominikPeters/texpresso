/* The DVI output of a compiler as it is edited.
 *
 * One input is a sequence of operations on a growing output buffer: raw and
 * structured appends (often cut inside an instruction), rollbacks, compiler
 * restarts that rewrite output of the same length, and page renders. These
 * follow the producer contract: bytes are only replaced after truncation or
 * incdvi_reset.
 *
 * Oracle: after every operation, the incrementally maintained index must
 * agree with a fresh index of the same bytes on page count and destinations,
 * for arbitrary bytes. In strict mode (first input bit), the output also
 * follows TeX's rule that fonts are defined and selected before use; then
 * rendered pages (or failures to render) must agree too. Without that rule a
 * page that draws before defining its font legitimately depends on whether
 * later pages, whose definitions are global, were interpreted first. */
#include "common.h"

#define MAX_OUTPUT 65536
#define MAX_STEPS 48
#define CHECKED_PAGES 3
#define DESTINATIONS 4

static const char *special_dictionary[] = {
  "pdf:dest (d0) [@thispage /XYZ @xpos @ypos null]",
  "pdf:dest (d1) [@thispage /XYZ @xpos @ypos null]",
  "pdf:dest (d2) [@thispage /Fit]",
  "pdf:dest (d3)",
  "pdf:bann << /Subtype /Link /A << /S /URI /URI (https://example.test) >> >>",
  "pdf:bann << /Subtype /Link /A << /S /GoTo /D (d1) >> >>",
  "pdf:eann",
  "pdf:code 0 0 m 100 100 l S",
  "pdf:code q 1 0 0 1 10 10 cm 0 0 5 5 re f Q",
  "pdf:code [1 2] 0 d 3 w 0 0 m 50 0 l S",
  "pdf:btrans matrix 1 0 0 1 2 3",
  "pdf:etrans",
  "pdf:bcolor [1 0 0]",
  "pdf:ecolor",
  "pdfcolorstackinit 3 page direct (0 g)",
  "pdfcolorstack 3 push (1 0 0 rg)",
  "pdfcolorstack 3 pop",
  "pdfcolorstack 3 current",
  "color push rgb 1 0 0",
  "color push cmyk 0 1 1 0",
  "color push gray 0.5",
  "color pop",
  "x:rotate 45",
  "pdf:pagesize width 100pt height 200pt",
  "landscape",
  "pdf:image width 10pt (missing.png)",
  "pdf:epdf (missing.pdf)",
};
#define SPECIALS (sizeof special_dictionary / sizeof *special_dictionary)

static bool strict;

static void append_signed(fz_context *ctx, fz_buffer *b, fuzz_input *in, int base_op)
{
  int n = fuzz_u8(in) % 4;
  fz_append_byte(ctx, b, base_op + n);
  uint32_t v = fuzz_u32(in);
  for (int i = n; i >= 0; i--)
    fz_append_byte(ctx, b, v >> (8 * i));
}

/* One instruction, mostly well formed so that interpretation gets deep. */
static void append_instruction(fz_context *ctx, fz_buffer *b, fuzz_input *in)
{
  switch (fuzz_u8(in) % (strict ? 9 : 13))
  {
    case 0: // rule: signed extents, possibly huge or negative
      fz_append_byte(ctx, b, fuzz_u8(in) & 1 ? 132 : 137);
      dvi_u32(ctx, b, fuzz_u32(in));
      dvi_u32(ctx, b, fuzz_u32(in));
      break;
    case 1: append_signed(ctx, b, in, 143); break; // right
    case 2: append_signed(ctx, b, in, 157); break; // down
    case 3: append_signed(ctx, b, in, 148 + 5 * (fuzz_u8(in) % 2)); break; // w, x
    case 4: append_signed(ctx, b, in, 162 + 5 * (fuzz_u8(in) % 2)); break; // y, z
    case 5: fz_append_byte(ctx, b, 141); break; // push
    case 6: fz_append_byte(ctx, b, 142); break; // pop, possibly unbalanced
    case 7:
    {
      const char *s = special_dictionary[fuzz_u8(in) % SPECIALS];
      if (fuzz_u8(in) % 4)
        dvi_special_str(ctx, b, s);
      else // truncated or extended variant
      {
        char buf[256];
        size_t n = strlen(s);
        size_t keep = fuzz_u8(in) % (n + 1);
        memcpy(buf, s, keep);
        const uint8_t *tail;
        size_t extra = fuzz_bytes(in, fuzz_u8(in) % 16, &tail);
        memcpy(buf + keep, tail, extra);
        dvi_special(ctx, b, buf, keep + extra);
      }
      break;
    }
    case 8:
    {
      const uint8_t *raw;
      size_t n = fuzz_bytes(in, fuzz_u8(in), &raw);
      dvi_special(ctx, b, (const char *)raw, n);
      break;
    }
    case 9: // characters without a usable font
      fz_append_byte(ctx, b, fuzz_u8(in) & 0x7f);
      break;
    case 10: // font selection and definitions of fonts that cannot load
      if (fuzz_u8(in) & 1)
        fz_append_byte(ctx, b, 171 + fuzz_u8(in) % 64);
      else
      {
        fz_append_byte(ctx, b, 243);
        fz_append_byte(ctx, b, fuzz_u8(in));
        dvi_u32(ctx, b, 0);
        dvi_u32(ctx, b, 655360);
        dvi_u32(ctx, b, 655360);
        fz_append_byte(ctx, b, 0);
        fz_append_byte(ctx, b, 4);
        fz_append_data(ctx, b, "cmr9", 4);
      }
      break;
    case 11: // XDV glyph run
    {
      int n = fuzz_u8(in) % 4;
      fz_append_byte(ctx, b, 253);
      dvi_u32(ctx, b, fuzz_u32(in));
      fz_append_byte(ctx, b, 0);
      fz_append_byte(ctx, b, n);
      for (int i = 0; i < n * 10; i++)
        fz_append_byte(ctx, b, fuzz_u8(in));
      break;
    }
    case 12: // anything
      fz_append_byte(ctx, b, fuzz_u8(in));
      break;
  }
}

static void append_page(fz_context *ctx, fz_buffer *b, fuzz_input *in)
{
  if (b->len == 0)
    dvi_preamble(ctx, b);
  dvi_bop(ctx, b);
  int n = fuzz_u8(in) % 24;
  for (int i = 0; i < n && b->len < MAX_OUTPUT - 512; i++)
    append_instruction(ctx, b, in);
  dvi_eop(ctx, b);
}

/* Update, and report failure instead of propagating it. */
static bool try_update(fz_context *ctx, incdvi_t *d, fz_buffer *b)
{
  bool ok = true;
  fz_try(ctx)
    incdvi_update(ctx, d, b);
  fz_catch(ctx)
  {
    (void)txp_take_error(ctx);
    ok = false;
  }
  return ok;
}

static bool try_render(fz_context *ctx, incdvi_t *d, fz_buffer *b, int page, uint64_t *hash)
{
  bool ok = true;
  fz_try(ctx)
    *hash = render(ctx, d, b, page);
  fz_catch(ctx)
  {
    const char *error = txp_take_error(ctx);
    if (getenv("FUZZ_VERBOSE"))
      fprintf(stderr, "render of page %d failed: %s\n", page, error);
    ok = false;
  }
  return ok;
}

static void compare_with_fresh(fz_context *ctx, incdvi_t *d, fz_buffer *b)
{
  incdvi_t *fresh = incdvi_new(ctx, (dvi_reshooks){0});
  bool ok_d = try_update(ctx, d, b);
  bool ok_fresh = try_update(ctx, fresh, b);
  fuzz_check(ok_d == ok_fresh);
  if (ok_d)
  {
    int pages = incdvi_page_count(fresh);
    fuzz_check(incdvi_page_count(d) == pages);
    fuzz_check(incdvi_output_started(d) == incdvi_output_started(fresh));
    for (int i = 0; i < DESTINATIONS; i++)
    {
      char name[8];
      snprintf(name, sizeof name, "d%d", i);
      fuzz_check(incdvi_destination_page(ctx, d, name) ==
                 incdvi_destination_page(ctx, fresh, name));
    }
    for (int page = 0; strict && page < pages && page < CHECKED_PAGES; page++)
    {
      uint64_t expected = 0, actual = 0, again = 0;
      bool ok = try_render(ctx, fresh, b, page, &expected);
      fuzz_check(try_render(ctx, d, b, page, &actual) == ok);
      fuzz_check(try_render(ctx, d, b, page, &again) == ok); // cached
      if (ok)
        fuzz_check(actual == expected && again == expected);
    }
  }
  incdvi_free(ctx, fresh);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  if (size > 65536)
    return 0;
  if (size < 1)
    return 0;
  strict = data[0] & 1;
  fuzz_input in = { data + 1, size - 1, 0 };
  fz_context *ctx = fuzz_context();
  incdvi_t *d = incdvi_new(ctx, (dvi_reshooks){0});
  fz_buffer *b = dvi_baseline(ctx);
  incdvi_update(ctx, d, b);

  for (int step = 0; step < MAX_STEPS && fuzz_more(&in); step++)
  {
    fz_try(ctx)
    {
      switch (fuzz_u8(&in) % 7)
      {
        case 0: // raw bytes
        {
          const uint8_t *raw;
          size_t n = fuzz_bytes(&in, fuzz_u8(&in), &raw);
          if (!strict && b->len + n <= MAX_OUTPUT)
            fz_append_data(ctx, b, raw, n);
          break;
        }
        case 1: // rollback
        {
          size_t n = fuzz_u16(&in);
          b->len = n < b->len ? b->len - n : 0;
          break;
        }
        case 2: // compiler restart with no output yet
          incdvi_reset(d);
          b->len = 0;
          break;
        case 3: // structured page, arriving in chunks
        {
          size_t start = b->len;
          append_page(ctx, b, &in);
          size_t full = b->len;
          unsigned chunk = 1 + fuzz_u8(&in) % 64;
          for (b->len = start; b->len < full;)
          {
            b->len += chunk < full - b->len ? chunk : full - b->len;
            incdvi_update(ctx, d, b);
          }
          break;
        }
        case 4: // a page cut inside an instruction
        {
          size_t start = b->len;
          append_page(ctx, b, &in);
          size_t cut = start + fuzz_u16(&in) % (b->len - start + 1);
          b->len = cut;
          break;
        }
        case 5: // restart producing different output of the same length
        {
          size_t len = b->len;
          incdvi_reset(d);
          b->len = 0;
          while (b->len < len)
            append_page(ctx, b, &in);
          b->len = len;
          break;
        }
        case 6: // render, including pages that do not exist
          probe(ctx, d, b, (int)(fuzz_u8(&in) % 8) - 1);
          break;
      }
    }
    fz_catch(ctx)
      (void)txp_take_error(ctx);
    compare_with_fresh(ctx, d, b);
  }

  recover(ctx, d);
  fz_drop_buffer(ctx, b);
  incdvi_free(ctx, d);
  fuzz_drop_context(ctx);
  return 0;
}
