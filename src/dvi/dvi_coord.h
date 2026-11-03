#ifndef DVI_COORD_H
#define DVI_COORD_H
#include <limits.h>
#include <stdint.h>
#include <mupdf/fitz.h>
/* Malformed movements must not wrap signed DVI registers or invoke C UB. */
static inline int32_t dvi_coord(fz_context *ctx, int64_t value)
{
  if (value < INT32_MIN || value > INT32_MAX)
    fz_throw(ctx, FZ_ERROR_GENERIC, "DVI coordinate overflow");
  return (int32_t)value;
}
#endif
