#ifndef TXP_INCLUDE_PATH_H
#define TXP_INCLUDE_PATH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Paths are NUL-separated, with an empty entry at the end. Return the next
 * candidate fitting the caller's buffer; an oversized entry must not hide
 * later directories or write one byte beyond the buffer's terminator. */
static inline const char *txp_expand_path(const char **paths, const char *name, char *buffer, size_t capacity)
{
  if (!*paths || !**paths || name[0] == '/') return NULL;
  if (name[0] == '.' && name[1] == '/') { name += 2; while (*name == '/') ++name; }
  const char *directory = *paths;
  while (*directory) {
    size_t length = strlen(directory), name_length = strlen(name);
    bool slash = directory[length - 1] != '/';
    *paths = directory + length + 1;
    if (length < capacity && slash < capacity - length && name_length < capacity - length - slash) {
      memcpy(buffer, directory, length);
      if (slash) buffer[length++] = '/';
      memcpy(buffer + length, name, name_length + 1);
      return buffer;
    }
    fprintf(stderr, "[warning] Skipping include-path candidate longer than %zu bytes\n", capacity ? capacity - 1 : 0);
    directory = *paths;
  }
  return NULL;
}
#endif
