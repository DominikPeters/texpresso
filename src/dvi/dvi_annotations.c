/* Dvipdfmx annotation specials are metadata, independent of the output device.
 * Parse PDF strings/dictionaries with MuPDF (escaped and UTF-16 names included).
 * Each flushed text run supplies a tight rectangle, so wrapped links do not
 * swallow the whitespace or unrelated text between lines. */
#include "mydvi.h"
#include <ctype.h>
#include <string.h>

static const char *special(const char *data, size_t len, const char *name)
{
  const char *end = data + len;
  while (data < end && isspace((unsigned char)*data)) data++;
  if (end - data < 4 || memcmp(data, "pdf:", 4)) return NULL;
  data += 4;
  while (data < end && isspace((unsigned char)*data)) data++;
  size_t n = strlen(name);
  if ((size_t)(end - data) < n || memcmp(data, name, n)) return NULL;
  data += n;
  if (data < end && !isspace((unsigned char)*data) && *data != '<' && *data != '(') return NULL;
  while (data < end && isspace((unsigned char)*data)) data++;
  return data;
}
static pdf_obj *object(fz_context *ctx, pdf_document *doc, const char *data, size_t len)
{
  fz_stream *stream = fz_open_memory(ctx, (const unsigned char *)data, len);
  pdf_lexbuf lex;
  pdf_obj *obj = NULL;
  pdf_lexbuf_init(ctx, &lex, PDF_LEXBUF_SMALL);
  fz_var(obj);
  fz_try(ctx) { obj = pdf_parse_stm_obj(ctx, doc, stream, &lex); }
  fz_always(ctx) { pdf_lexbuf_fin(ctx, &lex); fz_drop_stream(ctx, stream); }
  fz_catch(ctx) { /* Malformed annotations must not prevent page rendering. */ }
  return obj;
}
static const char *string(fz_context *ctx, pdf_obj *obj)
{
  return pdf_is_string(ctx, obj) ? pdf_to_text_string(ctx, obj) : pdf_to_name(ctx, obj);
}
char *dvi_annotation_destination(fz_context *ctx, const char *data, size_t len)
{
  const char *start = special(data, len, "dest");
  if (!start || len > 65536) return NULL;
  pdf_obj *obj = object(ctx, NULL, start, data + len - start);
  const char *name = string(ctx, obj);
  char *result = name && *name && strlen(name) <= 4096 ? fz_strdup(ctx, name) : NULL;
  pdf_drop_obj(ctx, obj);
  return result;
}
void dvi_annotation_bounds(fz_context *ctx, dvi_context *dc, fz_rect b)
{
  if (!dc->annotation_uri || !dc->annotation_links || dc->annotation_link_count >= 4096 ||
      dc->annotation_links->len > 384 * 1024 || fz_is_empty_rect(b) || fz_is_infinite_rect(b)) return;
  fz_append_printf(ctx, dc->annotation_links, "%s{\"bounds\":[%g,%g,%g,%g],\"uri\":%q}",
    dc->annotation_link_count++ ? "," : "", b.x0, b.y0, b.x1, b.y1, dc->annotation_uri);
}
bool dvi_annotation_special(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *data, size_t len)
{
  if (special(data, len, "eann")) {
    dvi_context_flush_text(ctx, dc, st); dc->annotation_uri = NULL; return true;
  }
  const char *start = special(data, len, "bann");
  if (start) {
    dvi_context_flush_text(ctx, dc, st); dc->annotation_uri = NULL;
    if (len > 65536) return true;
    if (!dc->annotation_pdf) dc->annotation_pdf = pdf_create_document(ctx);
    pdf_obj *obj = object(ctx, dc->annotation_pdf, start, data + len - start);
    pdf_obj *action = pdf_dict_gets(ctx, obj, "A");
    const char *kind = pdf_to_name(ctx, pdf_dict_gets(ctx, action, "S"));
    const char *target = NULL; bool internal = false;
    if (!strcmp(kind, "URI")) target = string(ctx, pdf_dict_gets(ctx, action, "URI"));
    else if (!strcmp(kind, "GoTo")) { target = string(ctx, pdf_dict_gets(ctx, action, "D")); internal = true; }
    else if (pdf_dict_gets(ctx, obj, "Dest")) { target = string(ctx, pdf_dict_gets(ctx, obj, "Dest")); internal = true; }
    if (target && *target && strlen(target) <= 4096) {
      size_t n = strlen(target);
      char *uri = DC_ALLOC(ctx, dc, char, n + 2);
      uri[0] = '#'; memcpy(uri + internal, target, n + 1); dc->annotation_uri = uri;
    }
    pdf_drop_obj(ctx, obj);
    return true;
  }
  char *name = dvi_annotation_destination(ctx, data, len);
  if (name) {
    if (dc->annotation_destinations && dc->annotation_destination_count < 4096 && dc->annotation_destinations->len < 128 * 1024) {
      fz_point p = fz_transform_point(fz_make_point(0, 0), dvi_get_ctm(dc, st));
      fz_append_printf(ctx, dc->annotation_destinations, "%s{\"name\":%q,\"x\":%g,\"y\":%g}",
        dc->annotation_destination_count++ ? "," : "", name, p.x, p.y);
    }
    fz_free(ctx, name); return true;
  }
  return false;
}
fz_buffer *dvi_annotations(fz_context *ctx, dvi_context *dc)
{
  fz_buffer *buf = fz_new_buffer(ctx, 256);
  fz_append_printf(ctx, buf, "{\"links\":[%.*s],\"destinations\":[%.*s]}",
    dc->annotation_links ? (int)dc->annotation_links->len : 0,
    dc->annotation_links ? dc->annotation_links->data : (unsigned char *)"",
    dc->annotation_destinations ? (int)dc->annotation_destinations->len : 0,
    dc->annotation_destinations ? dc->annotation_destinations->data : (unsigned char *)"");
  return buf;
}
