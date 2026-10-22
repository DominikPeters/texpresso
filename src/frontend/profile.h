#ifndef TXP_PROFILE_H
#define TXP_PROFILE_H
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
/* Opt-in diagnostics stay on stderr, outside the binary preview protocol. */
static double txp_profile_now(void)
{
  static int enabled = -1;
  if (enabled < 0) { const char *v = getenv("TEXPRESSO_PROFILE"); enabled = v && v[0] == '1'; }
  if (!enabled) return 0;
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
static void txp_profile_record(const char *stage, double start, int request, int page, size_t bytes)
{
  if (start) fprintf(stderr, "[profile] {\"stage\":\"%s\",\"ms\":%.4f,\"request\":%d,\"page\":%d,\"bytes\":%zu}\n",
                     stage, txp_profile_now() - start, request, page, bytes);
}
#endif
