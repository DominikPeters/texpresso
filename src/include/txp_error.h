#ifndef TXP_ERROR_H
#define TXP_ERROR_H
#include <mupdf/fitz.h>
#include <stdio.h>

/* Consume an exception which is being translated to our log/protocol. MuPDF
 * 1.24+ requires this acknowledgement before another exception can be thrown;
 * older distribution versions expose only fz_caught_message(). */
static inline const char *txp_take_error(fz_context *ctx)
{
#if FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR >= 24
  return fz_convert_error(ctx, NULL);
#else
  return fz_caught_message(ctx);
#endif
}

/* Keep error reporting independent of MuPDF allocation. This is also safe
 * when the exception being reported is an allocation failure. */
static inline void txp_write_json_string(FILE *stream, const char *value)
{
  const unsigned char *p = (const unsigned char *)value;
  fputc('"', stream);
  for (; *p; ++p) {
    if (*p == '"' || *p == '\\') { fputc('\\', stream); fputc(*p, stream); }
    else if (*p < 32) fprintf(stream, "\\u%04x", *p);
    else fputc(*p, stream);
  }
  fputc('"', stream);
}
#endif
