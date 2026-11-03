/* Editor commands: protocol parsing (JSON and S-expressions, arbitrarily
 * fragmented across reads), command decoding, and resolution of edits to
 * byte spans of the file being edited.
 *
 * The first input byte selects the protocol and whether the rest is raw
 * protocol text or a list of synthesized change commands with hostile numbers.
 * Changes are applied to an in-memory file, as main.c does.
 *
 * Oracles: resolved spans lie inside the file; on valid UTF-8, change-range
 * agrees with an independent model of UTF-16 (LSP) positions. */
#include "common.h"
#include "editor.h"
#include "prot_parser.h"

#define MAX_FILE 8192

static fz_buffer *file;

static const char initial_text[] =
  "\\documentclass{article}\n"
  "\\begin{document}\n"
  "caf\xc3\xa9 \xf0\x9d\x9b\xbc = \xe2\x88\x91\n" // 2-, 4- and 3-byte sequences
  "\n"
  "\\end{document}"; // no final newline

static bool valid_utf8(const unsigned char *p, size_t len)
{
  for (size_t i = 0; i < len;)
  {
    unsigned char b = p[i];
    size_t n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3
             : (b & 0xF8) == 0xF0 ? 4 : 0;
    if (!n || i + n > len)
      return false;
    for (size_t k = 1; k < n; k++)
      if ((p[i + k] & 0xC0) != 0x80)
        return false;
    i += n;
  }
  return true;
}

/* Byte offset of an LSP position, or -1. Positions may address the end of a
 * line but not past it, and not the middle of a surrogate pair. */
static int model_position(const unsigned char *p, int len, int line, int character)
{
  int i = 0;
  for (; line > 0; i++)
  {
    if (i >= len)
      return -1;
    if (p[i] == '\n')
      line--;
  }
  int units = 0;
  while (units < character)
  {
    if (i >= len || p[i] == '\n')
      return -1;
    unsigned char b = p[i];
    int n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    units += n == 4 ? 2 : 1;
    i += n;
  }
  return units == character ? i : -1;
}

static bool synthesized;

static void apply(fz_context *ctx, struct editor_change *op)
{
  // Synthesized commands only contain valid UTF-8 and escapes, including
  // surrogate pairs and lone surrogates; they must decode to valid UTF-8.
  if (synthesized)
    fuzz_check(valid_utf8((const unsigned char *)op->data, op->length));
  int offset, remove;
  bool ok = editor_change_span(file->data, file->len, op, &offset, &remove);

  if (op->base == BASE_RANGE && valid_utf8(file->data, file->len))
  {
    int start = op->range.start_line < 0 || op->range.start_char < 0 ? -1 :
      model_position(file->data, file->len, op->range.start_line, op->range.start_char);
    int end = op->range.end_line < 0 || op->range.end_char < 0 ? -1 :
      model_position(file->data, file->len, op->range.end_line, op->range.end_char);
    bool expected = start >= 0 && end >= start;
    fuzz_check(ok == expected);
    if (ok)
      fuzz_check(offset == start && remove == end - start);
  }
  if (!ok)
    return;
  fuzz_check(offset >= 0 && remove >= 0 && offset <= (int)file->len &&
             remove <= (int)file->len - offset);
  fuzz_check(op->length >= 0);
  if (file->len - remove + op->length > MAX_FILE)
    return;

  if (file->len - remove + op->length > file->cap)
    fz_resize_buffer(ctx, file, file->len - remove + op->length + 128);
  memmove(file->data + offset + op->length, file->data + offset + remove,
          file->len - offset - remove);
  file->len = file->len - remove + op->length;
  memcpy(file->data + offset, op->data, op->length);
}

static void run_commands(fz_context *ctx, prot_parser *parser, vstack *stack,
                         const char *text, size_t len, fuzz_input *chunking)
{
  const char *ptr = text, *lim = text + len;
  while (ptr < lim)
  {
    size_t step = 1 + fuzz_u8(chunking) % 64;
    const char *chunk_end = lim - ptr < (ptrdiff_t)step ? lim : ptr + step;
    fz_try(ctx)
    {
      const char *p = ptr;
      while ((p = prot_parse(ctx, parser, stack, p, chunk_end)))
      {
        val cmds = vstack_get_values(ctx, stack);
        int n = val_array_length(ctx, stack, cmds);
        for (int i = 0; i < n; i++)
        {
          struct editor_command cmd;
          if (editor_parse(ctx, stack, val_array_get(ctx, stack, cmds, i), &cmd) &&
              cmd.tag == EDIT_CHANGE)
            apply(ctx, &cmd.change);
        }
      }
    }
    fz_catch(ctx)
    {
      // As main.c: drop the partial command and resynchronize.
      (void)txp_take_error(ctx);
      vstack_reset(ctx, stack);
      prot_reinitialize(parser);
    }
    ptr = chunk_end;
  }
}

/* Numbers that editors should never send, but whose handling must stay
 * defined: negatives, past-the-end, fractions, huge and non-integral. */
static void append_number(fz_context *ctx, fz_buffer *out, fuzz_input *in)
{
  static const char *specials[] = {
    "-1", "0.5", "1e10", "-1e10", "2147483648", "-2147483649", "1e400", "4294967296",
  };
  uint8_t kind = fuzz_u8(in);
  if (kind < 200)
    fz_append_printf(ctx, out, "%d", kind % 12);
  else if (kind < 240)
    fz_append_printf(ctx, out, "%d", (int)(int16_t)fuzz_u16(in));
  else
    fz_append_string(ctx, out, specials[kind % 8]);
}

static void append_string(fz_context *ctx, fz_buffer *out, fuzz_input *in, bool json)
{
  static const char *pieces[] = {
    "x", "\\n", "\xc3\xa9", "\xf0\x9f\x98\x80", "}", "\\\\", "\xe2\x82\xac",
    "\\u00e9", "\\ud83d\\ude00", "\\u20ac", "\\ud83d", "\\ude00",
  };
  fz_append_byte(ctx, out, '"');
  int n = fuzz_u8(in) % 6;
  for (int i = 0; i < n; i++)
  {
    const char *piece = pieces[fuzz_u8(in) % 12];
    if (!json && piece[0] == '\\')
      piece = piece[1] == 'n' ? "\n" : "x";
    fz_append_string(ctx, out, piece);
  }
  fz_append_byte(ctx, out, '"');
}

static void synthesize(fz_context *ctx, fz_buffer *out, fuzz_input *in, bool json)
{
  for (int i = 0; i < 16 && fuzz_more(in); i++)
  {
    int kind = fuzz_u8(in) % 3;
    static const char *verbs[] = { "change", "change-lines", "change-range" };
    int numbers = kind == 2 ? 4 : 2;
    fz_append_string(ctx, out, json ? "[\"" : "(");
    fz_append_string(ctx, out, verbs[kind]);
    fz_append_string(ctx, out, json ? "\", \"main.tex\"" : " \"main.tex\"");
    for (int k = 0; k < numbers; k++)
    {
      fz_append_string(ctx, out, json ? ", " : " ");
      append_number(ctx, out, in);
    }
    fz_append_string(ctx, out, json ? ", " : " ");
    append_string(ctx, out, in, json);
    fz_append_string(ctx, out, json ? "]\n" : ")\n");
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  if (size < 1 || size > 16384)
    return 0;
  fuzz_input in = { data + 1, size - 1, 0 };
  bool json = data[0] & 1, raw = data[0] & 2;
  editor_set_protocol(json ? EDITOR_JSON : EDITOR_SEXP);

  fz_context *ctx = fuzz_context();
  file = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)initial_text,
                                        sizeof initial_text - 1);
  vstack *stack = vstack_new(ctx);
  prot_parser parser;
  prot_initialize(&parser, json);

  // Read sizes are taken from the input itself, so that every byte boundary
  // of a command is eventually tried.
  synthesized = !raw;
  if (raw)
  {
    fuzz_input chunking = { data, size, 0 };
    run_commands(ctx, &parser, stack, (const char *)in.data, in.size, &chunking);
  }
  else
  {
    fz_buffer *text = fz_new_buffer(ctx, 1024);
    synthesize(ctx, text, &in, json);
    fuzz_input chunking = { data, size, 0 };
    run_commands(ctx, &parser, stack, (const char *)text->data, text->len, &chunking);
    fz_drop_buffer(ctx, text);
  }

  vstack_free(ctx, stack);
  fz_drop_buffer(ctx, file);
  fuzz_drop_context(ctx);
  return 0;
}
