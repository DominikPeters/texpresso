#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Move the pointer `start` to an UTF-8 encoded string ending at `end` forward
// by `count` UTF-16 code units.
// Return NULL if a fatal error was encountered (invalid encoding or `count`
// points in the middle of a surrogate pair).
// LSP ranges
static const void *move_utf8_by_utf16_codeunits(const void *start,
                                                const void *end,
                                                int count)
{
  if (start >= end)
  {
    if (count == 0)
      return start;
    return NULL;
  }

  const unsigned char *p = start, *lim = end;
  int warned_continuation = 0;

  while (count > 0)
  {
    // Out of bounds: the position is past the end of the buffer
    if (p >= lim)
      break;

    unsigned char b = *p;

    // 1-byte UTF-8: ASCII char
    if (b < 0x80)
    {

      // Error: reached the end of the line
      if (b == '\n')
      {
        fprintf(stderr,
                "[error] Invalid UTF-16 range: "
                "pointing past end of line\n");
        return NULL;
      }

      // Count as single UTF-16 code unit
      count -= 1;
      p += 1;
      continue;
    }

    // 1-byte: continuation (warn and ignore)
    if (b < 0xC0)
    {
      if (!warned_continuation)
      {
        fprintf(stderr,
                "[warning] UTF-16 range: "
                "unexpected continuation byte\n");
        warned_continuation = 1;
      }
      p += 1;
      continue;
    }

    // 2-byte and 3-byte UTF-8: 1 UTF-16 code unit
    // 4-byte UTF-8: a surrogate pair, needing two UTF-16 code units
    int bytes = b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    if (lim - p < bytes)
    {
      p = lim + 1;
      break;
    }
    for (int i = 1; i < bytes; i++)
      if (p[i] == '\n')
        goto invalid_nl;
    p += bytes;
    count -= bytes == 4 ? 2 : 1;
  }

  // At this point, count is:
  // - exactly 0 if the traversal succeeded
  // - (-1) if the offset given pointed in the middle of a surrogate pair
  // - >0 if the offset is pointing past the end of the buffer
  //
  if (count > 0 || p > lim)
  {
    fprintf(stderr,
            "[error] Invalid UTF-16 range: "
            "pointing past end of buffer\n");
    return NULL;
  }

  if (count < 0)
  {
    fprintf(stderr,
            "[error] Invalid UTF-16 range: "
            "pointing in the middle of a surrogate pair\n");
    return NULL;
  }

  return p;

invalid_nl:
  fprintf(stderr,
          "[error] Broken UTF-8 encoding: "
          "line return in the middle of a codepoint\n");
  return NULL;
}

static int utf16_to_utf8_offset(const void *p, const void *end, size_t utf16_index)
{
  const void *p2 = move_utf8_by_utf16_codeunits(p, end, utf16_index);

  if (p2 && p2 <= end)
    return p2 - p;
  else
    return -1;
}
