/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "engine_lifecycle.h"
#include "synctex.h"
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef __APPLE__
# define st_time(a) st_##a##timespec
#else
# define st_time(a) st_##a##tim
#endif

static bool is_more_recent(uint64_t *time, char *candidate)
{
  struct stat st;
  if (stat(candidate, &st) == 0 && st.st_time(c).tv_sec > *time)
  {
    *time = st.st_time(c).tv_sec;
    return 1;
  }
  return 0;
}

void engine_lifecycle_find_tectonic(char tectonic_path[4096], const char *exec_path)
{
  strcpy(tectonic_path, exec_path);
  char *basename = NULL;
  for (int i = 0; i < 4096 && tectonic_path[i]; ++i)
    if (tectonic_path[i] == '/')
      basename = tectonic_path + i + 1;
  uint64_t time = 0;
  if (basename)
  {
    strcpy(basename, "texpresso-tonic");
    if (!is_more_recent(&time, tectonic_path))
      strcpy(tectonic_path, "texpresso-tonic");
  }
}

txp_engine *engine_lifecycle_create_engine(
    fz_context *ctx,
    const char *doc_name,
    const char *doc_path,
    const char *inclusion_path,
    const char *exe_path)
{
  // Determine document type from extension
  const char *doc_ext = NULL;
  for (const char *ptr = doc_name; *ptr; ptr++)
    if (*ptr == '.')
      doc_ext = ptr + 1;

  // Find tectonic
  char tectonic_path[4096];
  engine_lifecycle_find_tectonic(tectonic_path, exe_path);

  // Create appropriate engine
  if (doc_ext && strcmp(doc_ext, "pdf") == 0)
  {
    return txp_create_pdf_engine(ctx, doc_name);
  }
  else if (doc_ext && (strcmp(doc_ext, "dvi") == 0 || strcmp(doc_ext, "xdv") == 0))
  {
    return txp_create_dvi_engine(ctx, tectonic_path, doc_path, doc_name);
  }
  else
  {
    return txp_create_tex_engine(ctx, tectonic_path, inclusion_path, doc_path, doc_name);
  }
}

static bool need_advance(fz_context *ctx, txp_engine *eng, int target_page, bool need_synctex)
{
  int need = send(page_count, eng) <= target_page;

  if (!need && need_synctex)
  {
    fz_buffer *buf;
    synctex_t *stx = send(synctex, eng, &buf);
    need =
      (synctex_page_count(stx) <= target_page) ||
      synctex_has_target(stx);
  }

  return (need && send(get_status, eng) == DOC_RUNNING);
}

bool engine_lifecycle_step_bounded(
    fz_context *ctx,
    txp_engine *eng,
    int target_page,
    bool need_synctex)
{
  bool need = need_advance(ctx, eng, target_page, need_synctex);
  if (!need)
    return false;

  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  int steps = 10;
  while (need)
  {
    if (!send(step, eng, ctx, false))
      break;

    steps -= 1;
    need = need_advance(ctx, eng, target_page, need_synctex);

    if (steps == 0)
    {
      steps = 10;

      struct timespec curr;
      clock_gettime(CLOCK_MONOTONIC, &curr);

      int delta =
        (curr.tv_sec - start.tv_sec) * 1000 * 1000 * 1000 +
        (curr.tv_nsec - start.tv_nsec);

      // Break after ~5ms
      if (delta > 5000000)
        break;
    }
  }
  return need;
}

int engine_lifecycle_get_page_count(txp_engine *eng)
{
  return send(page_count, eng);
}

int engine_lifecycle_get_status(txp_engine *eng)
{
  return send(get_status, eng);
}
