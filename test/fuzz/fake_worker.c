/* A deterministic stand-in for texpresso-xetex, for stress-testing the
 * frontend's process, checkpoint and rollback management without TeX.
 *
 * It speaks the real worker protocol (texpresso_protocol.c and fork.c, so
 * checkpoints fork for real and pass descriptors) and reads its input exactly
 * like the XeTeX worker: byte by byte through a buffered reader that reports
 * SEEN positions and rereads after each fork. What it does with the input is
 * a tiny line-oriented language, so that the document under edit decides
 * the worker's behaviour:
 *
 *   \begin{document}  read main.aux from the previous pass, then rewrite it
 *   \input{name}      process name (an included file)
 *   \label{name}      a PDF destination on the current page, recorded in aux
 *   \ref{name}        page content that depends on the previous pass's aux
 *   \end{document}    finish
 *   \crash, \exit     die like a TeX fatal error (abort) or a plain exit
 *   \hang             spin until killed
 *   \slow             take 3ms for this line
 *   blank line        end the current page, if it has content
 *   anything else     page content (hashed into a rule's dimensions)
 *
 * The output (main.xdv, main.log, main.aux) depends only on the input files,
 * so an edited session must end with the same output as a fresh run. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../src/engine/main/texpresso_protocol.h"

int xetex_tokens; // read by the protocol client for query timestamps

static txp_client *io;
static int next_fid = 3;

enum { FID_XDV = 0, FID_LOG = 1, FID_AUX_OUT = 2 };

/* Input, as ttstub_input_getc reads it. */

typedef struct {
  int fid;
  uint32_t file_pos, generation;
  int buf_pos, buf_len;
  char buffer[64];
} input;

static int input_getc(input *in)
{
  if (in->generation != txp_generation(io))
  {
    // A checkpoint forked: input past the consumed position may have changed.
    in->generation = txp_generation(io);
    in->file_pos += in->buf_pos;
    in->buf_pos = in->buf_len = 0;
  }
  if (in->buf_pos >= in->buf_len)
  {
    in->file_pos += in->buf_len;
    in->buf_pos = 0;
    in->buf_len = txp_read(io, in->fid, in->file_pos, in->buffer, sizeof in->buffer);
    if (in->buf_len == 0)
      return EOF;
  }
  int c = (unsigned char)in->buffer[in->buf_pos++];
  xetex_tokens++;
  txp_seen(io, in->fid, in->file_pos + in->buf_pos);
  return c;
}

static bool input_open(input *in, const char *path, enum txp_file_kind kind)
{
  memset(in, 0, sizeof *in);
  in->fid = next_fid++;
  in->generation = txp_generation(io);
  char *opened = txp_open(io, in->fid, path, kind, TXP_READ);
  if (!opened)
    return false;
  free(opened);
  return true;
}

/* Output. */

static void put_u32(uint8_t *p, uint32_t v)
{
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static bool xdv_open, log_open, aux_open;
static int pages;

static void output(int fid, const void *data, size_t len)
{
  txp_append(io, fid, data, len);
}

static void open_output(int fid, const char *path, bool *flag)
{
  if (*flag)
    return;
  char *opened = txp_open(io, fid, path, TXP_KIND_OTHER, TXP_WRITE);
  free(opened);
  *flag = true;
  if (fid == FID_XDV)
  {
    uint8_t pre[15] = { 247, 7 };
    put_u32(pre + 2, 25400000);
    put_u32(pre + 6, 473628672);
    put_u32(pre + 10, 1000);
    output(FID_XDV, pre, sizeof pre);
  }
}

static void log_line(const char *line)
{
  open_output(FID_LOG, "main.log", &log_open);
  output(FID_LOG, line, strlen(line));
  output(FID_LOG, "\n", 1);
}

/* Page state. */

static uint32_t page_hash;
static int page_lines;
static char page_specials[2048];
static size_t page_specials_len;

static void page_special(const char *s)
{
  size_t n = strlen(s);
  if (n > 255 || page_specials_len + n + 2 > sizeof page_specials)
    return;
  page_specials[page_specials_len++] = (char)239; // xxx1
  page_specials[page_specials_len++] = (char)n;
  memcpy(page_specials + page_specials_len, s, n);
  page_specials_len += n;
}

static void ship_page(void)
{
  if (!page_lines && !page_specials_len)
    return;
  open_output(FID_XDV, "main.xdv", &xdv_open);
  uint8_t bop[45] = { 139 };
  put_u32(bop + 1, pages + 1);
  output(FID_XDV, bop, sizeof bop);
  uint8_t rule[9] = { 132 };
  put_u32(rule + 1, 65536 * (1 + page_lines));
  put_u32(rule + 5, 65536 * (1 + page_hash % 300));
  output(FID_XDV, rule, sizeof rule);
  output(FID_XDV, page_specials, page_specials_len);
  uint8_t eop = 140;
  output(FID_XDV, &eop, 1);
  pages++;
  page_hash = 2166136261u;
  page_lines = 0;
  page_specials_len = 0;
}

/* Labels: those of the previous pass (read from aux) and of this one. */

#define MAX_LABELS 64
static struct { char name[32]; int page; } old_labels[MAX_LABELS];
static int old_label_count;

static void read_aux(void)
{
  input in;
  if (!input_open(&in, "main.aux", TXP_KIND_TEX))
    return;
  char line[128];
  size_t n = 0;
  for (;;)
  {
    int c = input_getc(&in);
    if (c == EOF || c == '\n')
    {
      line[n] = 0;
      char name[32];
      int page;
      if (old_label_count < MAX_LABELS &&
          sscanf(line, "\\newlabel{%31[^}]}{%d}", name, &page) == 2)
      {
        strcpy(old_labels[old_label_count].name, name);
        old_labels[old_label_count++].page = page;
      }
      n = 0;
      if (c == EOF)
        break;
    }
    else if (n < sizeof line - 1)
      line[n++] = c;
  }
  txp_close(io, in.fid);
}

static bool argument(const char *line, const char *command, char *out, size_t size)
{
  size_t n = strlen(command);
  if (strncmp(line, command, n) != 0 || line[n] != '{')
    return false;
  const char *end = strchr(line + n + 1, '}');
  if (!end || (size_t)(end - (line + n + 1)) >= size)
    return false;
  memcpy(out, line + n + 1, end - (line + n + 1));
  out[end - (line + n + 1)] = 0;
  return true;
}

static void finish(void)
{
  ship_page();
  txp_finish(io, true, true);
  exit(0);
}

static void process_file(const char *path, int depth);

static void process_line(const char *line, int depth)
{
  char arg[64];
  log_line(line);
  if (line[0] == 0)
    ship_page();
  else if (strcmp(line, "\\begin{document}") == 0)
  {
    if (!aux_open)
    {
      read_aux();
      open_output(FID_AUX_OUT, "main.aux", &aux_open);
    }
  }
  else if (strcmp(line, "\\end{document}") == 0)
    finish();
  else if (strcmp(line, "\\crash") == 0)
    abort();
  else if (strcmp(line, "\\exit") == 0)
    _exit(3);
  else if (strcmp(line, "\\hang") == 0)
    for (volatile unsigned spin = 0;; spin++) { }
  else if (strcmp(line, "\\slow") == 0)
    usleep(3000);
  else if (argument(line, "\\input", arg, sizeof arg))
  {
    if (depth < 4)
      process_file(arg, depth + 1);
  }
  else if (argument(line, "\\label", arg, sizeof arg))
  {
    char special[128];
    snprintf(special, sizeof special, "pdf:dest (%s) [@thispage /XYZ @xpos @ypos null]", arg);
    page_special(special);
    if (aux_open)
    {
      char record[128];
      int n = snprintf(record, sizeof record, "\\newlabel{%s}{%d}\n", arg, pages + 1);
      output(FID_AUX_OUT, record, n);
    }
    page_lines++;
  }
  else if (argument(line, "\\ref", arg, sizeof arg))
  {
    int page = 0;
    for (int i = 0; i < old_label_count; i++)
      if (strcmp(old_labels[i].name, arg) == 0)
        page = old_labels[i].page;
    for (int i = 0; i <= page; i++)
      page_hash = (page_hash ^ 'r') * 16777619u;
    page_lines++;
  }
  else
  {
    for (const char *p = line; *p; p++)
      page_hash = (page_hash ^ (unsigned char)*p) * 16777619u;
    page_lines++;
  }
}

static void process_file(const char *path, int depth)
{
  input in;
  if (!input_open(&in, path, TXP_KIND_TEX))
    return;
  char line[256];
  size_t n = 0;
  for (;;)
  {
    int c = input_getc(&in);
    if (c == EOF || c == '\n')
    {
      if (c == EOF && n == 0)
        break;
      line[n] = 0;
      process_line(line, depth);
      n = 0;
      if (c == EOF)
        break;
    }
    else if (n < sizeof line - 1)
      line[n++] = c;
  }
  txp_close(io, in.fid);
}

int main(int argc, char **argv)
{
  const char *fd_env = getenv("TEXPRESSO_FD");
  if (argc < 2 || !fd_env)
  {
    fprintf(stderr, "usage: TEXPRESSO_FD=n %s [-texlive|-tectonic] -texpresso file.tex\n", argv[0]);
    return 1;
  }
  io = txp_connect(fdopen(atoi(fd_env), "r+"));
  if (!io)
    return 1;
  page_hash = 2166136261u;
  process_file(argv[argc - 1], 0);
  finish();
}
