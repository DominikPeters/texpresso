#ifndef TXP_PROFILE_H
#define TXP_PROFILE_H
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
/* stderr is shared with compiler workers. A bounded write keeps a complete
 * JSON record atomic on a pipe, including POSIX's minimum 512-byte PIPE_BUF. */
static inline void txp_profile_printf(const char *format, ...)
  __attribute__((format(printf, 1, 2)));
static inline void txp_profile_printf(const char *format, ...)
{
  char line[512];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (length > 0 && (size_t)length < sizeof(line)) {
    ssize_t written = write(STDERR_FILENO, line, (size_t)length);
    (void)written; /* Diagnostic output must never affect compilation. */
  }
}
/* Monotonic wall time also drives cooperative budgets when profiling is off. */
static double txp_monotonic_ms(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
/* Opt-in diagnostics stay on stderr, outside the binary preview protocol. */
static double txp_profile_now(void)
{
  static int enabled = -1;
  if (enabled < 0) { const char *v = getenv("TEXPRESSO_PROFILE"); enabled = v && v[0] == '1'; }
  if (!enabled) return 0;
  return txp_monotonic_ms();
}
static void txp_profile_record(const char *stage, double start, int request, int page, size_t bytes)
{
  if (!start) return;
  double now = txp_profile_now();
  txp_profile_printf("[profile] {\"stage\":\"%s\",\"ms\":%.4f,\"nativeAt\":%.4f,\"request\":%d,\"page\":%d,\"bytes\":%zu}\n",
          stage, now - start, now, request, page, bytes);
}
#endif
