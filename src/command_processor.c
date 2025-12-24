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

#include "command_processor.h"
#include "synctex.h"
#include "utf_mapping.h"
#include <string.h>
#include <stdio.h>

/* Change buffering for performance */
#define BUFFERED_OPS 64
#define BUFFERED_CHARS 4096

static struct {
  char buffer[BUFFERED_CHARS];
  int cursor;
  struct editor_change op[BUFFERED_OPS];
  int count;
} delayed_changes = {0,};

/* Helper functions */

static const char *relative_path(const char *path, const char *dir, int *go_up)
{
  const char *rel_path = path, *dir_path = dir;

  // Skip common parts
  while (*rel_path && *rel_path == *dir_path)
  {
    if (*rel_path == '/')
    {
      while (*rel_path == '/') rel_path += 1;
      while (*dir_path == '/') dir_path += 1;
    }
    else
    {
      rel_path += 1;
      dir_path += 1;
    }
  }

  // Go back to last directory separator
  if (*rel_path && *dir_path)
  {
    rel_path -= 1;
    dir_path -= 1;
    while (path < rel_path && *rel_path != '/')
    {
      rel_path -= 1;
      dir_path -= 1;
    }
    if (*rel_path == '/')
    {
      if (*dir_path != '/') abort();
      rel_path += 1;
      dir_path += 1;
    }
  }

  // Count number of '../'
  *go_up = 0;
  if (*dir_path)
  {
    *go_up = 1;
    while (*dir_path)
    {
      if (*dir_path == '/')
      {
        *go_up += 1;
        while (*dir_path == '/')
          dir_path += 1;
      }
      else
        dir_path += 1;
    }
  }

  while (*rel_path == '/')
    rel_path += 1;

  return rel_path;
}

static int find_diff(const fz_buffer *buf, const void *data, int size)
{
  const unsigned char *ptr = data;
  int i, len = fz_mini(buf->len, size);
  for (i = 0; i < len && buf->data[i] == ptr[i]; ++i);
  fprintf(stderr, "i:%d len:%d size:%d\n", i, (int)buf->len, size);
  return i;
}

static void realize_change(fz_context *ctx,
                           txp_engine *eng,
                           const char *doc_path,
                           struct editor_change *op)
{
  int go_up = 0;
  const char *path = relative_path(op->path, doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr, "[command] change %s: file has a different root, skipping\n", path);
    return;
  }

  fileentry_t *e = send(find_file, eng, ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] change %s: file not found, skipping\n", path);
    return;
  }

  fz_buffer *b = e->edit_data;
  if (!b)
  {
    fprintf(stderr, "[command] change %s: file not opened, skipping\n", path);
    return;
  }

  int offset = op->span.offset, remove = op->span.remove, length = op->length;

  if (op->base == BASE_LINE)
  {
    // Compute byte offsets from line offsets
    int line = offset, count = remove;

    offset = remove = 0;

    uint8_t *p = b->data;
    size_t len = b->len;

    while (line > 0 && offset < len)
    {
      if (p[offset] == '\n')
        line -= 1;
      offset++;
    }

    if (line > 0)
    {
      fprintf(stderr, "[command] change line %s: invalid line number, skipping\n", path);
      return;
    }

    remove = offset;
    while (count > 0 && remove < len)
    {
      if (p[remove] == '\n')
        count -= 1;
      remove++;
    }

    if (count > 1)
    {
      fprintf(stderr, "[command] change line %s: invalid line count, skipping\n", path);
      return;
    }

    remove -= offset;
  }
  else if (op->base == BASE_RANGE)
  {
    // Compute byte offsets from line offsets
    int line = op->range.start_line;
    offset = remove = 0;

    uint8_t *p = b->data;
    size_t len = b->len;

    while (line > 0 && offset < len)
    {
      if (p[offset] == '\n')
        line -= 1;
      offset++;
    }

    if (line > 0)
    {
      fprintf(stderr, "[command] change range %s: invalid start line, skipping\n", path);
      return;
    }

    int start_char_offset = utf16_to_utf8_offset(p + offset, p + len, op->range.start_char);
    if (start_char_offset == -1)
    {
      fprintf(stderr, "[command] change range %s: invalid start char, skipping\n", path);
      return;
    }

    remove = offset;
    offset += start_char_offset;

    line = op->range.end_line - op->range.start_line;
    if (line < 0)
    {
      fprintf(stderr, "[command] change range %s: invalid end line, skipping\n", path);
      return;
    }

    while (line > 0 && remove < len)
    {
      if (p[remove] == '\n')
        line -= 1;
      remove++;
    }

    if (line > 0)
    {
      fprintf(stderr, "[command] change range %s: invalid end line, skipping\n", path);
      return;
    }

    int end_char_offset = utf16_to_utf8_offset(p + remove, p + len, op->range.end_char);
    if (end_char_offset == -1)
    {
      fprintf(stderr, "[command] change range %s: invalid end char, skipping\n", path);
      return;
    }

    remove += end_char_offset;
    remove -= offset;
  }

  if (remove < 0 || offset < 0 || offset + remove > b->len)
  {
    fprintf(stderr, "[command] change %s: invalid range, skipping\n", path);
    return;
  }

  if (b->len - remove + length > b->cap)
    fz_resize_buffer(ctx, b, b->len - remove + length + 128);

  memmove(b->data + offset + length, b->data + offset + remove,
          b->len - offset - remove);

  b->len = b->len - remove + length;

  memmove(b->data + offset, op->data, length);

  fprintf(stderr, "[command] change %s: changed offset %d\n", path, offset);
  send(notify_file_changes, eng, ctx, e, offset);
}

/* Public API */

const char *command_processor_relative_path(
    const char *path,
    const char *doc_path,
    int *go_up)
{
  return relative_path(path, doc_path, go_up);
}

void command_processor_flush_changes(
    fz_context *ctx,
    txp_engine *eng,
    const char *doc_path)
{
  int count = delayed_changes.count;
  if (count)
  {
    delayed_changes.count = 0;
    delayed_changes.cursor = 0;
    for (int i = 0; i < count; ++i)
    {
      struct editor_change *op = &delayed_changes.op[i];
      realize_change(ctx, eng, doc_path, op);
    }
  }
}

void command_processor_interpret_open(
    fz_context *ctx,
    txp_engine *eng,
    const char *doc_path,
    const char *path,
    const void *data,
    int size)
{
  int go_up = 0;
  path = relative_path(path, doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr, "[command] open %s: file has a different root, skipping\n", path);
    return;
  }

  fileentry_t *e = send(find_file, eng, ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] open %s: file not found, skipping\n", path);
    return;
  }

  command_processor_flush_changes(ctx, eng, doc_path);

  int changed = -1;

  if (e->edit_data)
  {
    fprintf(stderr, "[command] open %s: known file, updating\n", path);
    changed = find_diff(e->edit_data, data, size);
    if (e->edit_data->cap < size)
      fz_resize_buffer(ctx, e->edit_data, size + 128);
    e->edit_data->len = size;
    memcpy(e->edit_data->data, data, size);
  }
  else
  {
    fprintf(stderr, "[command] open %s: new file\n", path);
    e->edit_data = fz_new_buffer_from_copied_data(ctx, data, size);
    if (e->fs_data)
      changed = find_diff(e->fs_data, data, size);
  }

  if (changed >= 0)
  {
    fprintf(stderr, "[command] open %s: changed offset is %d\n", path, changed);
    send(notify_file_changes, eng, ctx, e, changed);
  }
}

void command_processor_interpret_close(
    fz_context *ctx,
    txp_engine *eng,
    const char *doc_path,
    const char *path)
{
  int go_up = 0;
  path = relative_path(path, doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr, "[command] close %s: file has a different root, skipping\n", path);
    return;
  }

  fileentry_t *e = send(find_file, eng, ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] close %s: file not found, skipping\n", path);
    return;
  }

  if (!e->edit_data)
  {
    fprintf(stderr, "[command] close %s: file not opened, skipping\n", path);
    return;
  }

  command_processor_flush_changes(ctx, eng, doc_path);

  int changed = 0;

  if (e->fs_data)
    changed = find_diff(e->fs_data, e->edit_data->data, e->edit_data->len);

  fz_drop_buffer(ctx, e->edit_data);
  e->edit_data = NULL;

  fprintf(stderr, "[command] close %s: closing, changed offset %d\n", path,
          changed);

  send(notify_file_changes, eng, ctx, e, changed);
}

void command_processor_interpret_change(
    fz_context *ctx,
    txp_engine *eng,
    const char *doc_path,
    int current_page,
    struct editor_change *op)
{
  int plen = strlen(op->path);
  int page_count = send(page_count, eng);
  int cursor = delayed_changes.cursor;

  // Smart buffering logic matching GUI behavior (main.c:765-768):
  // Buffer changes only when engine is 1-2 pages behind current view.
  // This provides responsive feedback when caught up, but batches during
  // rapid typing when the engine is falling behind.
  if ((page_count == current_page - 2 || page_count == current_page - 1) &&
      send(get_status, eng) == DOC_RUNNING &&
      delayed_changes.count < BUFFERED_OPS &&
      cursor + plen + 1 + op->length <= BUFFERED_CHARS)
  {
    char *op_path = delayed_changes.buffer + cursor;
    memcpy(op_path, op->path, plen + 1);
    cursor += plen + 1;
    char *op_data = delayed_changes.buffer + cursor;
    memcpy(op_data, op->data, op->length);
    cursor += op->length;
    delayed_changes.cursor = cursor;

    delayed_changes.op[delayed_changes.count] = *op;
    delayed_changes.op[delayed_changes.count].path = op_path;
    delayed_changes.op[delayed_changes.count].data = op_data;
    delayed_changes.count += 1;
  }
  else
  {
    command_processor_flush_changes(ctx, eng, doc_path);
    realize_change(ctx, eng, doc_path, op);
  }
}

void command_processor_interpret_synctex_forward(
    fz_context *ctx,
    txp_engine *eng,
    const char *doc_path,
    int current_page,
    const char *path,
    int line,
    synctex_result_callback callback,
    void *user_data)
{
  fz_buffer *buf;
  synctex_t *stx = send(synctex, eng, &buf);
  int go_up = 0;
  const char *rel_path = relative_path(path, doc_path, &go_up);

  if (go_up > 0)
  {
    fprintf(stderr,
            "[command] synctex-forward %s: file has a different root, skipping\n",
            rel_path);
    return;
  }

  synctex_set_target(stx, current_page, rel_path, line);

  // Try to find the target immediately
  int page = -1, x = -1, y = -1;
  if (synctex_find_target(ctx, stx, buf, &page, &x, &y))
  {
    fprintf(stderr, "[synctex forward] sync: hit page %d, coordinates (%d, %d)\n",
            page, x, y);
    if (callback)
      callback(user_data, page, x, y);
  }
}
