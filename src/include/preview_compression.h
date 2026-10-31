#ifndef TXP_PREVIEW_COMPRESSION_H
#define TXP_PREVIEW_COMPRESSION_H

/* Use separate native symbols so MuPDF and other users of system zlib retain
 * their own ABI. Standalone builds need no additional dependency. */
#ifdef TXP_ZLIB_NG
#include <zlib-ng.h>
typedef zng_stream txp_zstream;
#define txp_deflate_init(s) zng_deflateInit(s, 2)
#define txp_deflate zng_deflate
#define txp_deflate_end zng_deflateEnd
#define txp_crc32 zng_crc32
#define TXP_PNG_COMPRESSION "{\"library\":\"zlib-ng\",\"version\":\"" ZLIBNG_VERSION "\",\"level\":2}"
#else
#include <zlib.h>
typedef z_stream txp_zstream;
#define txp_deflate_init(s) deflateInit(s, Z_BEST_SPEED)
#define txp_deflate deflate
#define txp_deflate_end deflateEnd
#define txp_crc32 crc32
#define TXP_PNG_COMPRESSION "{\"library\":\"zlib\",\"version\":\"" ZLIB_VERSION "\",\"level\":1}"
#endif
#endif
