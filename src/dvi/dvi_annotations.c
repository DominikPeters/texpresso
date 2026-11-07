/* Dvipdfmx annotation specials are metadata, independent of the output device.
 * Parse PDF strings/dictionaries with MuPDF (escaped and UTF-16 names included).
 * Each flushed text run supplies a tight rectangle, so wrapped links do not
 * swallow the whitespace or unrelated text between lines. */
#include "mydvi.h"
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* MuPDF's locale-independent %g uses PDF syntax (.5 and -.5). Preserve its
 * float precision, but include the integer digit required by JSON. */
static void json_number(fz_context *ctx, fz_buffer *out, float value)
{
  char text[64];
  fz_snprintf(text, sizeof text, "%g", value);
  const char *digits = text;
  if (*digits == '-') { fz_append_byte(ctx, out, '-'); digits++; }
  if (*digits == '.') fz_append_byte(ctx, out, '0');
  fz_append_string(ctx, out, digits);
}
static void json_bounds(fz_context *ctx, fz_buffer *out, fz_rect b)
{
  fz_append_byte(ctx, out, '['); json_number(ctx, out, b.x0);
  fz_append_byte(ctx, out, ','); json_number(ctx, out, b.y0);
  fz_append_byte(ctx, out, ','); json_number(ctx, out, b.x1);
  fz_append_byte(ctx, out, ','); json_number(ctx, out, b.y1);
  fz_append_byte(ctx, out, ']');
}

static int unhex(unsigned char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
/* Shared validation keeps visual-key normalization aligned with rendering. */
bool dvi_parse_figure(const char *data, size_t len, dvi_figure *out)
{
  const char prefix[] = "texpresso:figure ";
  if (len < sizeof(prefix)-1 || len > 16384 ||
      memcmp(data,prefix,sizeof(prefix)-1) || memchr(data,0,len)) return false;
  char text[16385]; memcpy(text,data,len); text[len]=0;
  char *p=text+sizeof(prefix)-1, *end;
  long v[6];
  for (int i=0;i<6;i++) {
    while (*p==' ') p++;
    errno=0; v[i]=strtol(p,&end,10);
    if (p==end || errno || *end!=' ' || v[i]<INT32_MIN || v[i]>INT32_MAX) return false;
    p=end+1;
  }
  while (*p==' ') p++;
  size_t n=strlen(p);
  while (n && p[n-1]==' ') n--;
  if (!n || n>8192 || n%2 || v[4]<1 || v[5]<v[4] || v[5]>=INT32_MAX ||
      v[0]>=v[2] || v[1]>=v[3]) return false;
  for (size_t i=0;i<n/2;i++) {
    int a=unhex(p[2*i]), b=unhex(p[2*i+1]);
    if (a<0 || b<0 || a*16+b<32 || a*16+b==127) return false;
    out->file[i]=(char)(a*16+b);
  }
  out->file[n/2]=0;
  for (int i=0;i<4;i++) out->geometry[i]=(int32_t)v[i];
  out->line=(int32_t)v[4]; out->end_line=(int32_t)v[5]+1;
  return true;
}
static bool figure(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *data, size_t len)
{
  const char prefix[]="texpresso:figure ";
  if (len<sizeof(prefix)-1 || memcmp(data,prefix,sizeof(prefix)-1)) return false;
  /* Malformed or oversized metadata never interferes with drawing. */
  dvi_figure source;
  if (!dc->annotation_figures || dc->annotation_figure_count>=1024 ||
      dc->annotation_figures->len>256*1024-32768 || !dvi_parse_figure(data,len,&source)) return true;
  const float sp=72.f/(72.27f*65536.f);
  int32_t *v=source.geometry;
  fz_rect b=fz_transform_rect(fz_make_rect(v[0]*sp,v[1]*sp,v[2]*sp,v[3]*sp),dvi_get_ctm(dc,st));
  if (fz_is_empty_rect(b) || fz_is_infinite_rect(b) || !isfinite(b.x0) || !isfinite(b.y0) ||
      !isfinite(b.x1) || !isfinite(b.y1) || fabsf(b.x0)>=1e9f || fabsf(b.y0)>=1e9f ||
      fabsf(b.x1)>=1e9f || fabsf(b.y1)>=1e9f) return true;
  fz_append_data(ctx,dc->annotation_figure_bounds,&b,sizeof b);
  fz_append_printf(ctx,dc->annotation_figures,
    "%s{\"bounds\":", dc->annotation_figure_count++ ? ",":"");
  json_bounds(ctx, dc->annotation_figures, b);
  fz_append_printf(ctx, dc->annotation_figures, ",\"file\":%q,\"line\":%d,\"endLine\":%d}",
    source.file, source.line, source.end_line);
  return true;
}

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
      dc->annotation_links->len + 6 * strlen(dc->annotation_uri) + 100 > 384 * 1024 - 128 ||
      !isfinite(b.x0) || !isfinite(b.y0) || !isfinite(b.x1) || !isfinite(b.y1) ||
      fabsf(b.x0) >= 1e9f || fabsf(b.y0) >= 1e9f || fabsf(b.x1) >= 1e9f || fabsf(b.y1) >= 1e9f ||
      fz_is_empty_rect(b) || fz_is_infinite_rect(b)) return;
  fz_append_printf(ctx, dc->annotation_links, "%s{\"bounds\":", dc->annotation_link_count++ ? "," : "");
  json_bounds(ctx, dc->annotation_links, b);
  fz_append_printf(ctx, dc->annotation_links, ",\"uri\":%q}", dc->annotation_uri);
}
bool dvi_annotation_special(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *data, size_t len)
{
  if (figure(ctx, dc, st, data, len)) return true;
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
    if (target && *target && strlen(target) + internal <= 4096) {
      size_t n = strlen(target);
      char *uri = DC_ALLOC(ctx, dc, char, n + 2);
      uri[0] = '#'; memcpy(uri + internal, target, n + 1); dc->annotation_uri = uri;
    }
    pdf_drop_obj(ctx, obj);
    return true;
  }
  char *name = dvi_annotation_destination(ctx, data, len);
  if (name) {
    if (dc->annotation_destinations && dc->annotation_destination_count < 4096 && dc->annotation_destinations->len + 6 * strlen(name) + 100 < 128 * 1024 - 128) {
      fz_point p = fz_transform_point(fz_make_point(0, 0), dvi_get_ctm(dc, st));
      if (isfinite(p.x) && isfinite(p.y) && fabsf(p.x)<1e9f && fabsf(p.y)<1e9f) {
        fz_append_printf(ctx, dc->annotation_destinations, "%s{\"name\":%q,\"x\":",
          dc->annotation_destination_count++ ? "," : "", name);
        json_number(ctx, dc->annotation_destinations, p.x);
        fz_append_string(ctx, dc->annotation_destinations, ",\"y\":");
        json_number(ctx, dc->annotation_destinations, p.y);
        fz_append_byte(ctx, dc->annotation_destinations, '}');
      }
    }
    fz_free(ctx, name); return true;
  }
  return false;
}
static fz_buffer *annotations(fz_context *ctx, dvi_context *dc, bool figures)
{
  fz_buffer *buf = fz_new_buffer(ctx, 256);
  /* MuPDF 1.23's formatter treats zero string precision as unbounded. Empty
   * annotation buffers need length-aware copies, not %.*s over uninitialized data. */
  fz_append_string(ctx, buf, "{\"links\":[");
  if (dc->annotation_links) fz_append_buffer(ctx, buf, dc->annotation_links);
  fz_append_string(ctx, buf, "],\"destinations\":[");
  if (dc->annotation_destinations) fz_append_buffer(ctx, buf, dc->annotation_destinations);
  fz_append_string(ctx, buf, "]");
  if (figures && dc->annotation_figure_count) {
    fz_append_string(ctx, buf, ",\"figures\":[");
    fz_append_buffer(ctx, buf, dc->annotation_figures);
    fz_append_string(ctx, buf, "]");
  }
  fz_append_string(ctx, buf, "}");
  return buf;
}

fz_buffer *dvi_annotations(fz_context *ctx, dvi_context *dc)
{ return annotations(ctx,dc,true); }
fz_buffer *dvi_annotations_base(fz_context *ctx, dvi_context *dc)
{ return annotations(ctx,dc,false); }

/* The visual key retains each figure's geometry and all positioning/transform
 * commands. Its cached page-space bounds therefore remain valid; only routing
 * is replaced. Never reuse metadata if a limit/filter changed the figure count. */
fz_buffer *dvi_annotations_refresh_figures(fz_context *ctx, fz_buffer *base,
                                          fz_buffer *bounds, fz_buffer *sources)
{
  if (!base || !bounds || !sources || !base->len || base->data[base->len-1]!='}' ||
      bounds->len%sizeof(fz_rect) || bounds->len/sizeof(fz_rect)>1024) return NULL;
  size_t count=bounds->len/sizeof(fz_rect), pos=0;
  for (size_t i=0;i<count;i++) {
    int32_t record[3];
    if (sources->len-pos<sizeof record) return NULL;
    memcpy(record,sources->data+pos,sizeof record); pos+=sizeof record;
    if (record[0]<1 || record[1]<=record[0] || record[2]<1 || record[2]>4096 ||
        (size_t)record[2]>sources->len-pos) return NULL;
    pos+=(size_t)record[2];
  }
  if (pos!=sources->len) return NULL;
  if (!count) return fz_keep_buffer(ctx,base);
  fz_buffer *result=NULL;
  fz_var(result);
  fz_try(ctx) {
    result=fz_new_buffer(ctx,base->len+sources->len+256);
    fz_append_data(ctx,result,base->data,base->len-1);
    fz_append_string(ctx,result,",\"figures\":["); pos=0;
    size_t figure_start=result->len;
    for (size_t i=0;i<count;i++) {
      int32_t record[3]; fz_rect b; char file[4097];
      memcpy(record,sources->data+pos,sizeof record); pos+=sizeof record;
      memcpy(file,sources->data+pos,(size_t)record[2]); file[record[2]]=0; pos+=(size_t)record[2];
      memcpy(&b,bounds->data+i*sizeof b,sizeof b);
      fz_append_printf(ctx,result,
        "%s{\"bounds\":", i ? ",":"");
      json_bounds(ctx, result, b);
      fz_append_printf(ctx, result, ",\"file\":%q,\"line\":%d,\"endLine\":%d}",
        file, record[0], record[1]);
      if (result->len-figure_start>256*1024) {
        fz_drop_buffer(ctx,result); result=NULL; break;
      }
    }
    if (result) fz_append_string(ctx,result,"]}");
  }
  fz_catch(ctx) { fz_drop_buffer(ctx,result); fz_rethrow(ctx); }
  return result;
}
