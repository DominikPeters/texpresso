#include "engine.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void *count_alloc(void *user, size_t size)
{
  void *ptr = malloc(size);
  if (ptr) ++*(size_t *)user;
  return ptr;
}

static void count_free(void *user, void *ptr)
{
  if (ptr) --*(size_t *)user;
  free(ptr);
}

static void *count_realloc(void *user, void *ptr, size_t size)
{
  if (!ptr) return count_alloc(user, size);
  if (!size)
  {
    count_free(user, ptr);
    return NULL;
  }
  return realloc(ptr, size);
}

static void check_log_lifecycle(fz_context *ctx)
{
  filesystem_t *fs = filesystem_new(ctx);
  fileentry_t *entry = filesystem_lookup_or_create(ctx, fs, "generated.vrb");
  assert(filesystem_lookup_span(fs, ".//generated.vrb\n", 16) == entry);
  log_t *log = log_new(ctx);
  log_fileentry(ctx, log, entry);
  entry->saved.data = fz_new_buffer(ctx, 16);
  fz_append_string(ctx, entry->saved.data, "first");
  log_snapshot(ctx, log);
  log_fileentry(ctx, log, entry);
  log_overwrite(ctx, log, entry->saved.data, 0, 1);
  entry->saved.data->data[0] = 'F';
  fz_drop_buffer(ctx, entry->saved.data);
  entry->saved.data = fz_new_buffer(ctx, 32);
  log_free(ctx, log);
  filesystem_free(ctx, fs);
}

static void check_vf_lifecycle(fz_context *ctx, unsigned char last_opcode, bool invalid)
{
  /* A preamble, one character packet, then a postamble or an invalid opcode. */
  const unsigned char bytes[] = {247, 202, 0, 0, 0, 0, 0, 0, 160, 0, 0,
                                 1, 65, 0, 0, 0, 138, last_opcode};
  fz_stream *stream = fz_open_memory(ctx, bytes, sizeof bytes);
  bool failed = false;
  fz_var(failed);
  fz_try(ctx)
  {
    tex_vf *vf = tex_vf_load(ctx, NULL, stream);
    assert(tex_vf_get(vf, 65));
    tex_vf_free(ctx, vf);
  }
  fz_always(ctx)
    fz_drop_stream(ctx, stream);
  fz_catch(ctx)
    failed = true;
  assert(failed == invalid);
}

int main(int argc, char **argv)
{
  assert(argc == 3);
  size_t live_blocks = 0;
  fz_alloc_context alloc = {&live_blocks, count_alloc, count_realloc, count_free};
  fz_context *ctx = fz_new_context(&alloc, NULL, FZ_STORE_DEFAULT);
  assert(ctx);
  fz_register_document_handlers(ctx);

  fz_document_writer *writer = fz_new_document_writer(ctx, argv[1], "pdf", NULL);
  fz_begin_page(ctx, writer, fz_make_rect(0, 0, 100, 100));
  fz_end_page(ctx, writer);
  fz_close_document_writer(ctx, writer);
  fz_drop_document_writer(ctx, writer);

  /* Standard DVI preamble of an empty, incremental document. */
  const unsigned char preamble[] = {247, DVI_STANDARD, 1, 132, 146, 192,
      28, 59, 0, 0, 0, 0, 3, 232, 0};
  FILE *dvi_file = fopen(argv[2], "wb");
  assert(dvi_file);
  size_t written = fwrite(preamble, 1, sizeof preamble, dvi_file);
  int closed = fclose(dvi_file);
  assert(written == sizeof preamble && closed == 0);

  for (int i = 0; i < 3; ++i)
  {
    txp_engine *tex = txp_create_tex_engine(ctx, "unused", true, true, "", "main.tex", (dvi_reshooks){0});
    fileentry_t *entry = send(find_file, tex, ctx, "chapter.tex");
    entry->fs_data = fz_new_buffer(ctx, 65536);
    send(destroy, tex, ctx);

    txp_engine *pdf = txp_create_pdf_engine(ctx, argv[1]);
    assert(pdf);
    fz_drop_display_list(ctx, send(render_page, pdf, ctx, 0));
    send(destroy, pdf, ctx);

    txp_engine *dvi = txp_create_dvi_engine(ctx, argv[2], (dvi_reshooks){0});
    assert(dvi);
    send(destroy, dvi, ctx);

    check_log_lifecycle(ctx);
    check_vf_lifecycle(ctx, 248, false);
    check_vf_lifecycle(ctx, 249, true);
  }

  remove(argv[1]);
  remove(argv[2]);
  fz_drop_context(ctx);
  assert(live_blocks == 0);
  puts("PASS: engine teardown released all tracked allocations");
  return 0;
}
