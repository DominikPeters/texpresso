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

#include <stdlib.h>
#include <string.h>
#include "incdvi.h"
#include "mydvi.h"
#include "mydvi_interp.h"
#include "mydvi_opcodes.h"
#include "intcodec.h"
#include "profile.h"

#define PAGE_CACHE_ENTRIES 8
#define PAGE_CACHE_BYTES (32u * 1024u * 1024u)
#define PAGE_PROGRAM_LIMIT (128u * 1024u)

typedef struct {
  fz_buffer *key, *annotations;
  fz_display_list *list;
  size_t charge;
  unsigned long used;
  uint64_t token;
} page_cache_entry;

static uint64_t next_content_token;

struct incdvi_s
{
  int offset;
  int fontdef_offset;
  int page_len, page_cap;
  int *pages;
  dvi_context *dc;
  struct { char *name; int offset, page; } *destinations;
  int destination_count, destination_capacity;
  size_t destination_bytes;
  page_cache_entry cache[PAGE_CACHE_ENTRIES];
  size_t cache_bytes;
  unsigned long cache_clock;
  fz_buffer *cache_annotations;
};

static int add_page(fz_context *ctx, incdvi_t *d)
{
  int result = d->page_len;
  if (result == d->page_cap)
  {
    if (d->page_cap == 0)
    {
      d->pages = fz_malloc_struct_array(ctx, 8, int);
      d->page_cap = 8;
    }
    else
    {
      int *pages = fz_malloc_struct_array(ctx, d->page_cap * 2, int);
      memcpy(pages, d->pages, sizeof(int) * d->page_cap);
      fz_free(ctx, d->pages);
      d->pages = pages;
      d->page_cap = d->page_cap * 2;
    }
  }
  d->page_len += 1;
  return result;
}

incdvi_t *incdvi_new(fz_context *ctx, dvi_reshooks hooks)
{
  incdvi_t *d = fz_malloc_struct(ctx, incdvi_t);
  d->dc = dvi_context_new(ctx, hooks);
  return d;
}

void incdvi_free(fz_context *ctx, incdvi_t *d)
{
  for (int i = 0; i < PAGE_CACHE_ENTRIES; i++) {
    fz_drop_buffer(ctx, d->cache[i].key); fz_drop_buffer(ctx, d->cache[i].annotations);
    fz_drop_display_list(ctx, d->cache[i].list);
  }
  fz_drop_buffer(ctx, d->cache_annotations);
  if (d->pages)
    fz_free(ctx, d->pages);
  for (int i = 0; i < d->destination_count; i++) fz_free(ctx, d->destinations[i].name);
  fz_free(ctx, d->destinations);
  dvi_context_free(ctx, d->dc);
  fz_free(ctx, d);
}

void incdvi_reset(incdvi_t *d)
{
  d->offset = 0;
  d->fontdef_offset = 0;
  d->page_len = 0;
}

void incdvi_update(fz_context *ctx, incdvi_t *d, fz_buffer *buf)
{
  if (buf == NULL)
  {
    incdvi_reset(d);
    return;
  }

  int len = buf->len;

  if (d->offset > len)
  {
    while (d->page_len > 0 && d->pages[d->page_len - 1] >= len)
      d->page_len -= 1;
    if (d->page_len == 0)
      d->offset = 0;
    else
    {
      d->page_len -= 1;
      d->offset = d->pages[d->page_len];
    }
  }

  if (d->offset == 0)
  {
    if (d->page_len != 0) abort();
    int plen = dvi_preamble_size(buf->data, len);
    if (plen > 0)
    {
      if (dvi_preamble_parse(ctx, d->dc, dvi_context_state(d->dc), buf->data))
        d->offset = plen;
    }
  }

  while (d->destination_count && d->destinations[d->destination_count - 1].offset >= d->offset) {
    char *name = d->destinations[--d->destination_count].name;
    d->destination_bytes -= strlen(name) + 1; fz_free(ctx, name);
  }
  if (d->offset > 0)
  {
    enum dvi_version version = dvi_context_state(d->dc)->version;
    while (d->offset < len)
    {
      int ilen = dvi_instr_size(buf->data + d->offset, len - d->offset, version);
      if (ilen <= 0 || ilen > len - d->offset)
        break;
      if (buf->data[d->offset] == BOP || buf->data[d->offset] == EOP)
      {
        int page = add_page(ctx, d);
        if (!(page & 1) != (buf->data[d->offset] == BOP))
          abort();
        d->pages[page] = d->offset;
      }
      int op = buf->data[d->offset];
      if (op >= XXX1 && op <= XXX4 && d->destination_count < 100000) {
        int prefix = 2 + op - XXX1;
        char *name = dvi_annotation_destination(ctx, (char *)buf->data + d->offset + prefix, ilen - prefix);
        if (name && d->destination_bytes + strlen(name) + 1 > 4 * 1024 * 1024) { fz_free(ctx, name); name = NULL; }
        if (name) {
          d->destination_bytes += strlen(name) + 1;
          if (d->destination_count == d->destination_capacity) {
            d->destination_capacity = d->destination_capacity ? d->destination_capacity * 2 : 32;
            // Capacity is bounded above by 131072 entries. Use the public allocator
            // available in both distribution MuPDF 1.23 and our bundled 1.28.
            d->destinations = fz_realloc(ctx, d->destinations,
              (size_t)d->destination_capacity * sizeof *d->destinations);
          }
          int i = d->destination_count++;
          d->destinations[i].name = name; d->destinations[i].offset = d->offset;
          d->destinations[i].page = (d->page_len - 1) / 2;
        }
      }
      d->offset += ilen;
    }
  }

  if (d->fontdef_offset > d->offset)
    d->fontdef_offset = d->offset;
}

bool incdvi_output_started(incdvi_t *d)
{
  return (d->page_len > 0);
}

int incdvi_page_count(incdvi_t *d)
{
  return (d->page_len / 2);
}

static void incdvi_parse_fontdef(fz_context *ctx, incdvi_t *restrict d, fz_buffer *buf, int offset)
{
  if (offset > buf->len) abort();
  enum dvi_version version = dvi_context_state(d->dc)->version;
  while (d->fontdef_offset < offset)
  {
    int ilen = dvi_instr_size(
        buf->data + d->fontdef_offset,
        offset - d->fontdef_offset,
        version
        );
    if (ilen <= 0)
      break;
    if (buf->data[d->fontdef_offset] >= XXX1 && buf->data[d->fontdef_offset] <= XXX4)
      dvi_interp_init(ctx, d->dc, buf->data + d->fontdef_offset, offset - d->fontdef_offset);
    if (dvi_is_fontdef(buf->data[d->fontdef_offset]))
    {
      dvi_interp(ctx, d->dc, buf->data + d->fontdef_offset);
    }
    d->fontdef_offset += ilen;
  }
}

void incdvi_page_dim(incdvi_t *d, fz_buffer *buf, int page, float *width, float *height, bool *landscape)
{
  bool _landscape;
  if (!landscape) landscape = &_landscape;
  *landscape = 0;
  if (page < 0 || page >= incdvi_page_count(d)) abort();
  int bop = d->pages[page * 2];
  if (dvi_interp_bop(buf->data + bop, buf->len - bop, width, height, landscape) <= 0)
    abort();
  if (*landscape)
  {
    float tmp = *width;
    *width = *height;
    *height = tmp;
  }
}

void incdvi_render_page(fz_context *ctx, incdvi_t *d, fz_buffer *buf, int page, fz_device *dev)
{
  if (page < 0 || page >= incdvi_page_count(d)) abort();
  fz_drop_buffer(ctx, d->cache_annotations); d->cache_annotations = NULL;
  int offset = d->pages[page * 2];
  int eop = d->pages[page * 2 + 1];
  incdvi_parse_fontdef(ctx, d, buf, offset);

  dvi_context *dc = d->dc;
  enum dvi_version version = dvi_context_state(dc)->version;
  dvi_context_begin_frame(ctx, d->dc, dev);
  while (offset < eop)
  {
    int ilen = dvi_instr_size(buf->data + offset, eop - offset, version);
    if (ilen <= 0) abort();
    dvi_interp(ctx, dc, buf->data + offset);
    offset += ilen;
  }
  dvi_context_end_frame(ctx, dc);
}

float incdvi_tex_scale_factor(incdvi_t *d)
{
  if (d->page_len == 0)
    return 1;
  return d->dc->scale;
}

fz_buffer *incdvi_annotations(fz_context *ctx, incdvi_t *d) { return d->cache_annotations ? fz_keep_buffer(ctx, d->cache_annotations) : dvi_annotations(ctx, d->dc); }
int incdvi_destination_page(fz_context *ctx, incdvi_t *d, const char *name)
{
  for (int i = 0; i < d->destination_count; i++)
    if (!strcmp(name, d->destinations[i].name) && d->destinations[i].page < incdvi_page_count(d))
      return d->destinations[i].page;
  return -1;
}

/* Cache interpreted pages, not source lines or page numbers. The interpreter's
 * font manager owns immutable loaded font/metric objects for this context's
 * lifetime. We validate their resolved identities and specs in addition to the
 * exact DVI program. Virtual fonts and external/unclassified specials bypass
 * reuse: they can introduce dependencies not represented by that manifest.
 * BOP counters/back pointers do not draw; geometry and the post-BOP program do.
 * Links/destinations are cached only with their exact annotation program; fresh
 * SyncTeX data is owned by the engine and never stored here.
 *
 * MuPDF has no public display-list allocation-size API. The conservative charge
 * (128 bytes per input byte + 4 KiB) accounts conservatively for operation
 * storage; retained key/annotation bytes are charged separately. Entry/program
 * limits remain hard bounds. Huge pages simply render uncached.
 */
static bool cache_token(const unsigned char *s, size_t n, const char *token)
{
  size_t z = strlen(token);
  return n >= z && !memcmp(s, token, z) &&
    (n == z || s[z] == ' ' || s[z] == '\t' || s[z] == '\r' || s[z] == '\n' || s[z] == '(' || s[z] == '<' || s[z] == '=');
}
static bool cache_special(const unsigned char *s, size_t n)
{
  while (n && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) { s++; n--; }
  if (cache_token(s,n,"color") || cache_token(s,n,"pdfcolorstack") || cache_token(s,n,"pdfcolorstackinit") ||
      cache_token(s,n,"landscape") || cache_token(s,n,"papersize")) return true;
  if (n < 4 || memcmp(s,"pdf:",4)) return false;
  s += 4; n -= 4;
  while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
  const char *tokens[] = {"pagesize","bann","eann","dest","bcontent","econtent","code",
    "begintransform","btrans","bt","endtransform","etrans","et",
    "begincolor","bcolor","bc","endcolor","ecolor","ec",
    /* Document dictionaries/resources are currently ignored by dvi_exec_pdf;
     * path code does not resolve them. Audit again if that parser gains support. */
    "docinfo","docview","put","obj","close"};
  for (size_t i=0;i<sizeof tokens/sizeof *tokens;i++) if (cache_token(s,n,tokens[i])) return true;
  return false;
}
static bool cache_font(fz_context *ctx, dvi_context *dc, fz_buffer *key, uint32_t font)
{
  if (font > 9999) return false;
  dvi_fontdef *def = dvi_fonttable_get(ctx, dc->root.fonts, (int)font);
  fz_append_data(ctx,key,&font,sizeof font);
  fz_append_data(ctx,key,&def->kind,sizeof def->kind);
  if (def->kind == TEX_FONT) {
    dvi_font *f = def->tex_font.font;
    if (!f || f->vf || !f->fz || !f->tfm) return false;
    fz_append_data(ctx,key,&def->tex_font.spec,sizeof def->tex_font.spec);
    fz_append_data(ctx,key,&f->fz,sizeof f->fz);
    fz_append_data(ctx,key,&f->tfm,sizeof f->tfm);
    fz_append_data(ctx,key,&f->enc,sizeof f->enc);
  } else if (def->kind == XDV_FONT) {
    if (!def->xdv_font.font) return false;
    /* Append fields separately: compiler padding in this struct is unspecified. */
    const dvi_xdvfontspec *s = &def->xdv_font.spec;
    fz_append_data(ctx,key,&s->size,sizeof s->size); fz_append_data(ctx,key,&s->flags,sizeof s->flags);
    if (s->flags & XDV_FLAG_COLORED) fz_append_data(ctx,key,&s->rgba,sizeof s->rgba);
    if (s->flags & XDV_FLAG_EXTEND) fz_append_data(ctx,key,&s->extend,sizeof s->extend);
    if (s->flags & XDV_FLAG_SLANT) fz_append_data(ctx,key,&s->slant,sizeof s->slant);
    if (s->flags & XDV_FLAG_EMBOLDEN) fz_append_data(ctx,key,&s->bold,sizeof s->bold);
    fz_append_data(ctx,key,&def->xdv_font.font,sizeof def->xdv_font.font);
  } else return false;
  return true;
}
static fz_buffer *page_key(fz_context *ctx, incdvi_t *d, fz_buffer *buf, int page, fz_rect bounds, uint32_t *last_font)
{
  int first=d->pages[page*2]+45, end=d->pages[page*2+1];
  /* An unfinished PDF path can survive a frame in the existing interpreter.
   * Do not reuse or retain pages with that additional cross-page dependency. */
  if (d->dc->path || d->dc->text) return NULL;
  if (first > end || (size_t)(end-first) > PAGE_PROGRAM_LIMIT) return NULL;
  fz_buffer *key=fz_new_buffer(ctx, (size_t)(end-first)+256);
  bool eligible=true, selected=false;
  unsigned char fonts[1250]={0};
  uint32_t font=d->dc->root.f;
  fz_var(key);
  fz_try(ctx) {
    fz_append_data(ctx,key,&bounds,sizeof bounds);
    fz_append_data(ctx,key,&d->dc->scale,sizeof d->dc->scale);
    fz_append_data(ctx,key,&d->dc->root.version,sizeof d->dc->root.version);
    fz_append_data(ctx,key,&d->dc->colorstack.origin,sizeof d->dc->colorstack.origin);
    fz_append_data(ctx,key,&d->dc->pdfcolorstacks.capacity,sizeof d->dc->pdfcolorstacks.capacity);
    for (int i=0;i<d->dc->pdfcolorstacks.capacity;i++)
      fz_append_data(ctx,key,&d->dc->pdfcolorstacks.stacks[i].origin,sizeof(dvi_colorstate));
    int program_size=end-first;
    fz_append_data(ctx,key,&program_size,sizeof program_size);
    fz_append_data(ctx,key,buf->data+first,(size_t)program_size);
    for (int pos=first;pos<end;) {
      int op=buf->data[pos], size=dvi_instr_size(buf->data+pos,end-pos,d->dc->root.version);
      if (size<=0 || size>end-pos) { eligible=false; break; }
      if (op>=FNT_NUM_0 && op<=FNT_NUM_63) { font=(uint32_t)(op-FNT_NUM_0); selected=true; }
      else if (op>=FNT1 && op<=FNT4) { font=decode_uB(buf->data+pos+1,op-FNT1+1); selected=true; }
      else if (dvi_is_fontdef(op)) {
        uint32_t number=op==XDV_NATIVE_FONT_DEF ? decode_u32(buf->data+pos+1) : decode_uB(buf->data+pos+1,op-FNT_DEF1+1);
        /* Do not cache a page that redefines a font after using its old mapping. */
        if (number>9999 || (fonts[number/8] & (1u<<(number%8)))) { eligible=false; break; }
      }
      else if (op>=XXX1 && op<=XXX4) {
        int prefix=2+op-XXX1;
        if (!cache_special(buf->data+pos+prefix,(size_t)(size-prefix))) {
          if (txp_profile_now()) fprintf(stderr,"[cache] page %d bypass special: %.*s\n",page,size-prefix>48?48:size-prefix,buf->data+pos+prefix);
          eligible=false; break;
        }
      }
      bool text=op<=SET4 || (op>=PUT1 && op<=PUT4) || op==XDV_GLYPHS || op==XDV_TEXT_GLYPHS;
      if (text) {
        if (!selected || font>9999) { eligible=false; break; }
        if (!(fonts[font/8] & (1u<<(font%8)))) {
          fonts[font/8] |= 1u<<(font%8);
          if (!cache_font(ctx,d->dc,key,font)) { eligible=false; break; }
        }
      }
      pos+=size;
    }
  }
  fz_catch(ctx) { fz_drop_buffer(ctx,key); fz_rethrow(ctx); }
  *last_font=font;
  if (!eligible) { fz_drop_buffer(ctx,key); return NULL; }
  return key;
}
static void cache_drop(fz_context *ctx, incdvi_t *d, int i)
{
  page_cache_entry *e=&d->cache[i];
  d->cache_bytes-=e->charge;
  fz_drop_buffer(ctx,e->key); fz_drop_buffer(ctx,e->annotations); fz_drop_display_list(ctx,e->list);
  memset(e,0,sizeof *e);
}
fz_display_list *incdvi_display_list(fz_context *ctx, incdvi_t *d, fz_buffer *buf, int page)
{
  double start=txp_profile_now();
  float w,h;
  incdvi_page_dim(d,buf,page,&w,&h,NULL);
  fz_rect bounds=fz_make_rect(0,0,w,h);
  incdvi_parse_fontdef(ctx,d,buf,d->pages[page*2]);
  fz_buffer *key=NULL;
  fz_display_list *list=NULL;
  fz_device *device=NULL;
  uint32_t last_font=d->dc->root.f;
  bool clean_state=!d->dc->path && !d->dc->text;
  fz_var(key); fz_var(list); fz_var(device);
  fz_try(ctx) {
    key=page_key(ctx,d,buf,page,bounds,&last_font);
    if (key) for (int i=0;i<PAGE_CACHE_ENTRIES;i++) {
      page_cache_entry *e=&d->cache[i];
      if (e->key && e->key->len==key->len && !memcmp(e->key->data,key->data,key->len)) {
        /* Preserve font-definition and initializer effects for later pages. These
         * operations load immutable objects already validated by this key. */
        for (int pos=d->pages[page*2]+45,end=d->pages[page*2+1];pos<end;) {
          int op=buf->data[pos], size=dvi_instr_size(buf->data+pos,end-pos,d->dc->root.version);
          if (dvi_is_fontdef(op)) dvi_interp(ctx,d->dc,buf->data+pos);
          else if (op>=XXX1 && op<=XXX4) dvi_interp_init(ctx,d->dc,buf->data+pos,size);
          pos+=size;
        }
        d->dc->root.f=last_font;
        fz_drop_buffer(ctx,d->cache_annotations); d->cache_annotations=fz_keep_buffer(ctx,e->annotations);
        e->used=++d->cache_clock; list=fz_keep_display_list(ctx,e->list);
        txp_profile_record("page-list-cache-hit",start,0,page,d->cache_bytes);
        break;
      }
    }
    if (!list) {
      fz_drop_buffer(ctx,key); key=NULL;
      list=fz_new_display_list(ctx,bounds); device=fz_new_list_device(ctx,list);
      incdvi_render_page(ctx,d,buf,page,device); fz_close_device(ctx,device);
      /* A first render can resolve fonts not loaded during the preflight. */
      key=clean_state?page_key(ctx,d,buf,page,bounds,&last_font):NULL;
      if (key) {
        fz_buffer *annotations=incdvi_annotations(ctx,d);
        size_t charge=4096+128u*(size_t)(d->pages[page*2+1]-d->pages[page*2]-45)+key->len+annotations->len;
        if (charge<=PAGE_CACHE_BYTES/2) {
          int slot=-1;
          while (slot<0 || d->cache_bytes+charge>PAGE_CACHE_BYTES) {
            int oldest=-1;
            for (int i=0;i<PAGE_CACHE_ENTRIES;i++) {
              if (!d->cache[i].list) { slot=i; continue; }
              if (oldest<0 || d->cache[i].used<d->cache[oldest].used) oldest=i;
            }
            if (slot>=0 && d->cache_bytes+charge<=PAGE_CACHE_BYTES) break;
            cache_drop(ctx,d,oldest); slot=oldest;
          }
          d->cache[slot]=(page_cache_entry){fz_keep_buffer(ctx,key),annotations,fz_keep_display_list(ctx,list),charge,++d->cache_clock,++next_content_token};
          d->cache_bytes+=charge; annotations=NULL;
        }
        fz_drop_buffer(ctx,annotations);
      }
      txp_profile_record(key?"page-list-cache-miss":"page-list-cache-bypass",start,0,page,d->cache_bytes);
    }
  }
  fz_always(ctx) { fz_drop_buffer(ctx,key); fz_drop_device(ctx,device); }
  fz_catch(ctx) { fz_drop_display_list(ctx,list); fz_rethrow(ctx); }
  return list;
}

uint64_t incdvi_display_list_token(incdvi_t *d, fz_display_list *list)
{
  for (int i=0;i<PAGE_CACHE_ENTRIES;i++) if (d->cache[i].list==list) return d->cache[i].token;
  return 0;
}
