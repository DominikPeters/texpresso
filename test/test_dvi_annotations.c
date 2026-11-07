#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "incdvi.h"
#include "mydvi.h"
static void u32(fz_context *ctx, fz_buffer *b, unsigned n)
{ fz_append_byte(ctx,b,n>>24); fz_append_byte(ctx,b,n>>16); fz_append_byte(ctx,b,n>>8); fz_append_byte(ctx,b,n); }
static fz_buffer *page(fz_context *ctx, const char *name)
{
  fz_buffer *b=fz_new_buffer(ctx,128);
  fz_append_byte(ctx,b,247);fz_append_byte(ctx,b,7);u32(ctx,b,25400000);u32(ctx,b,473628672);u32(ctx,b,1000);fz_append_byte(ctx,b,0);
  fz_append_byte(ctx,b,139);for(int i=0;i<11;i++)u32(ctx,b,0);
  char special[128];snprintf(special,sizeof special,"pdf:dest (%s) [@thispage /XYZ @xpos @ypos null]",name);
  fz_append_byte(ctx,b,239);fz_append_byte(ctx,b,strlen(special));fz_append_string(ctx,b,special);fz_append_byte(ctx,b,140);
  return b;
}
static fz_buffer *figure_page(fz_context *ctx, const char *metadata)
{
  fz_buffer *b=page(ctx,"stable"); b->len--; /* replace EOP */
  fz_append_byte(ctx,b,239); fz_append_byte(ctx,b,strlen(metadata)); fz_append_string(ctx,b,metadata);
  fz_append_byte(ctx,b,132); u32(ctx,b,65536); u32(ctx,b,131072);
  fz_append_byte(ctx,b,140);
  return b;
}
static void test_figure_cache(fz_context *ctx)
{
  const char *old="texpresso:figure 0 0 65536 131072 3 7 6d61696e2e746578";
  const char *shifted="texpresso:figure 0 0 65536 131072 30 70 6368696c642d636166c3a92e746578";
  const char *geometry="texpresso:figure 0 0 131072 131072 30 70 6368696c642d636166c3a92e746578";
  incdvi_t *d=incdvi_new(ctx,(dvi_reshooks){0});
  fz_buffer *a=figure_page(ctx,old), *b=figure_page(ctx,shifted), *c=figure_page(ctx,geometry);
  incdvi_update(ctx,d,a);
  assert(!incdvi_cached_display_list(ctx,d,a,0)); /* A miss cannot interpret/render the page. */
  fz_display_list *first=incdvi_display_list(ctx,d,a,0);
  fz_display_list *cached=incdvi_cached_display_list(ctx,d,a,0);
  assert(cached==first); fz_drop_display_list(ctx,cached);
  uint64_t token=incdvi_display_list_token(d,first); assert(token);
  fz_buffer *before=incdvi_annotations(ctx,d); fz_terminate_buffer(ctx,before);
  assert(strstr((char *)before->data,"\"file\":\"main.tex\",\"line\":3,\"endLine\":8"));
  incdvi_reset(d); incdvi_update(ctx,d,b);
  fz_display_list *same=incdvi_display_list(ctx,d,b,0);
  assert(same==first && incdvi_display_list_token(d,same)==token);
  fz_buffer *after=incdvi_annotations(ctx,d); fz_terminate_buffer(ctx,after);
  dvi_figure parsed; assert(dvi_parse_figure(shifted,strlen(shifted),&parsed));
  assert(!strcmp(parsed.file,"child-café.tex") && parsed.line==30 && parsed.end_line==71);
  /* MuPDF may emit valid Unicode escapes rather than literal UTF-8 in JSON. */
  fz_buffer *expected=fz_new_buffer(ctx,64);
  fz_append_printf(ctx,expected,"\"file\":%q,\"line\":30,\"endLine\":71","child-café.tex");
  fz_terminate_buffer(ctx,expected); assert(strstr((char *)after->data,(char *)expected->data));
  fz_drop_buffer(ctx,expected);
  assert(!strstr((char *)after->data,"main.tex"));
  char *old_figures=strstr((char *)before->data,"\"figures\""), *new_figures=strstr((char *)after->data,"\"figures\"");
  assert(old_figures && new_figures);
  size_t bounds_length=(size_t)(strstr(old_figures,"\"file\"")-old_figures);
  assert(!memcmp(old_figures,new_figures,bounds_length));
  assert(strstr((char *)after->data,"\"name\":\"stable\""));
  incdvi_reset(d); incdvi_update(ctx,d,a);
  fz_display_list *again=incdvi_display_list(ctx,d,a,0);
  fz_buffer *restored=incdvi_annotations(ctx,d); fz_terminate_buffer(ctx,restored);
  assert(again==first && strstr((char *)restored->data,"\"file\":\"main.tex\""));
  incdvi_reset(d); incdvi_update(ctx,d,c);
  fz_display_list *changed=incdvi_display_list(ctx,d,c,0);
  assert(incdvi_display_list_token(d,changed)!=token);
  fz_buffer *plain=page(ctx,"stable"); incdvi_reset(d); incdvi_update(ctx,d,plain);
  fz_display_list *without=incdvi_display_list(ctx,d,plain,0);
  fz_buffer *no_figures=incdvi_annotations(ctx,d); fz_terminate_buffer(ctx,no_figures);
  assert(!strstr((char *)no_figures->data,"\"figures\""));
  fz_drop_buffer(ctx,no_figures); fz_drop_buffer(ctx,plain); fz_drop_display_list(ctx,without);
  fz_buffer *bad=figure_page(ctx,"texpresso:figure 0 0 65536 131072 0 7 6d61696e2e746578");
  incdvi_reset(d); incdvi_update(ctx,d,bad);
  fz_display_list *invalid=incdvi_display_list(ctx,d,bad,0);
  assert(incdvi_display_list_token(d,invalid)==0);
  no_figures=incdvi_annotations(ctx,d); fz_terminate_buffer(ctx,no_figures);
  assert(!strstr((char *)no_figures->data,"\"figures\""));
  fz_drop_buffer(ctx,no_figures); fz_drop_buffer(ctx,bad); fz_drop_display_list(ctx,invalid);
  fz_drop_display_list(ctx,changed); fz_drop_display_list(ctx,again);
  fz_drop_display_list(ctx,same); fz_drop_display_list(ctx,first);
  fz_drop_buffer(ctx,before); fz_drop_buffer(ctx,after); fz_drop_buffer(ctx,restored);
  fz_drop_buffer(ctx,a); fz_drop_buffer(ctx,b); fz_drop_buffer(ctx,c); incdvi_free(ctx,d);
}
static void test_json_fractions(fz_context *ctx)
{
  dvi_context *dc=dvi_context_new(ctx,(dvi_reshooks){0});
  dvi_context_begin_frame(ctx,dc,NULL);
  dc->annotation_uri="#fraction";
  fz_rect bounds=fz_make_rect(.25f,-.5f,.75f,.5f);
  dvi_annotation_bounds(ctx,dc,bounds);
  dc->root.gs.ctm=fz_translate(.25f,-.5f);
  const char *dest="pdf:dest (fraction) [@thispage /XYZ @xpos @ypos null]";
  assert(dvi_annotation_special(ctx,dc,&dc->root,dest,strlen(dest)));
  const char *fig="texpresso:figure 0 0 65536 131072 3 7 6d61696e2e746578";
  assert(dvi_annotation_special(ctx,dc,&dc->root,fig,strlen(fig)));
  fz_buffer *json=dvi_annotations(ctx,dc); fz_terminate_buffer(ctx,json);
  assert(strstr((char *)json->data,"\"bounds\":[0.25,-0.5,0.75,0.5]"));
  assert(strstr((char *)json->data,"\"x\":0.25,\"y\":-0.5"));
  assert(strstr((char *)json->data,"\"figures\":[{\"bounds\":[0.25,-0.5,"));
  fz_drop_buffer(ctx,json);
  /* Figure routing refresh must use the same JSON formatting on a cache hit. */
  fz_buffer *base=dvi_annotations_base(ctx,dc), *rects=fz_new_buffer(ctx,32), *sources=fz_new_buffer(ctx,32);
  const int32_t record[]={3,8,8};
  fz_append_data(ctx,rects,&bounds,sizeof bounds);
  fz_append_data(ctx,sources,record,sizeof record); fz_append_string(ctx,sources,"main.tex");
  json=dvi_annotations_refresh_figures(ctx,base,rects,sources); assert(json); fz_terminate_buffer(ctx,json);
  assert(strstr((char *)json->data,"\"figures\":[{\"bounds\":[0.25,-0.5,0.75,0.5]"));
  fz_drop_buffer(ctx,json); fz_drop_buffer(ctx,base); fz_drop_buffer(ctx,rects); fz_drop_buffer(ctx,sources);
  /* Invalid transformed destinations must not put NaN or infinity on the wire. */
  dvi_context_begin_frame(ctx,dc,NULL); dc->root.gs.ctm=fz_translate(NAN,INFINITY);
  assert(dvi_annotation_special(ctx,dc,&dc->root,dest,strlen(dest)));
  assert(dc->annotation_destination_count==0);
  dvi_context_free(ctx,dc);
}
int main(void)
{
  fz_context *ctx=fz_new_context(NULL,NULL,FZ_STORE_DEFAULT);assert(ctx);
  test_json_fractions(ctx);
  test_figure_cache(ctx);
  incdvi_t *d=incdvi_new(ctx,(dvi_reshooks){0});
  assert(incdvi_destination_page(ctx,d,"unread")==-1);
  fz_buffer *empty=incdvi_annotations(ctx,d);
  const char *empty_json="{\"links\":[],\"destinations\":[]}";
  assert(empty->len==strlen(empty_json) && !memcmp(empty->data,empty_json,empty->len));
  fz_drop_buffer(ctx,empty);
  dvi_context *dc=dvi_context_new(ctx,(dvi_reshooks){0});
  dvi_context_begin_frame(ctx,dc,NULL); /* allocated, zero-length buffers */
  empty=dvi_annotations(ctx,dc);
  assert(empty->len==strlen(empty_json) && !memcmp(empty->data,empty_json,empty->len));
  fz_drop_buffer(ctx,empty);
  const char *fig="texpresso:figure 0 0 65536 131072 3 7 6d61696e2e746578";
  assert(dvi_annotation_special(ctx,dc,&dc->root,fig,strlen(fig)));
  assert(dc->annotation_figure_count==1);
  empty=dvi_annotations(ctx,dc);
  fz_terminate_buffer(ctx,empty);
  assert(strstr((char *)empty->data,"\"file\":\"main.tex\",\"line\":3,\"endLine\":8"));
  float x0,y0,x1,y1;
  char *bounds=strstr((char *)empty->data,"\"bounds\":[");
  assert(bounds && sscanf(bounds,"\"bounds\":[%f,%f,%f,%f]",&x0,&y0,&x1,&y1)==4);
  assert(fabsf(x0-72)<.001 && fabsf(y1-72)<.001);
  assert(fabsf((x1-x0)-72.f/72.27f)<.001 && fabsf((y1-y0)-144.f/72.27f)<.001);
  fz_drop_buffer(ctx,empty);
  const char *bad[]={"texpresso:figure nan 0 1 2 1 2 aa", "texpresso:figure 0 0 1 2 -1 2 aa",
    "texpresso:figure 0 0 1 2 1 2 00", "texpresso:figure 0 0 1 2 1 2 xyz",
    "texpresso:figure 0 0 1 2 1 2147483647 aa"};
  for(size_t i=0;i<sizeof bad/sizeof *bad;i++) assert(dvi_annotation_special(ctx,dc,&dc->root,bad[i],strlen(bad[i])));
  assert(dc->annotation_figure_count==1);
  dvi_context_begin_frame(ctx,dc,NULL);
  assert(dc->annotation_figure_count==0);
  dvi_context_free(ctx,dc);
  fz_buffer *full=page(ctx,"old"),*partial=fz_new_buffer(ctx,1);
  for(size_t i=0;i<full->len;i++) {
    fz_append_byte(ctx,partial,full->data[i]);incdvi_update(ctx,d,partial);
    assert(incdvi_page_count(d)==(i+1==full->len ? 1:0));
  }
  assert(incdvi_destination_page(ctx,d,"old")==0);
  partial->len=15;incdvi_update(ctx,d,partial);
  assert(incdvi_destination_page(ctx,d,"old")==-1);
  fz_buffer *next=page(ctx,"new");
  fz_append_data(ctx,partial,next->data+15,next->len-15);incdvi_update(ctx,d,partial);
  assert(incdvi_destination_page(ctx,d,"new")==0);
  assert(incdvi_destination_page(ctx,d,"old")==-1);
  /* Exercise the initial allocation and two capacity increases. */
  incdvi_free(ctx,d); d=incdvi_new(ctx,(dvi_reshooks){0});
  full->len--; /* insert destinations before EOP */
  for (int i=0;i<80;i++) {
    char special[128]; snprintf(special,sizeof special,"pdf:dest (target-%d) [@thispage /XYZ @xpos @ypos null]",i);
    fz_append_byte(ctx,full,239);fz_append_byte(ctx,full,strlen(special));fz_append_string(ctx,full,special);
  }
  fz_append_byte(ctx,full,140);incdvi_update(ctx,d,full);
  for (int i=0;i<80;i++) {
    char name[32];snprintf(name,sizeof name,"target-%d",i);
    assert(incdvi_destination_page(ctx,d,name)==0);
  }
  full->len=15;incdvi_update(ctx,d,full);
  assert(incdvi_destination_page(ctx,d,"target-79")==-1);
  incdvi_free(ctx,d);fz_drop_buffer(ctx,full);fz_drop_buffer(ctx,partial);fz_drop_buffer(ctx,next);fz_drop_context(ctx);
  puts("PASS: partial DVI special bodies and rollback keep destination metadata valid");
}
