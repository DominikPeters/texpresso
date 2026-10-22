#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "incdvi.h"
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
int main(void)
{
  fz_context *ctx=fz_new_context(NULL,NULL,FZ_STORE_DEFAULT);assert(ctx);
  incdvi_t *d=incdvi_new(ctx,(dvi_reshooks){0});
  fz_buffer *full=page(ctx,"old"),*partial=fz_new_buffer(ctx,1);
  for(size_t i=0;i<full->len;i++) {
    fz_append_byte(ctx,partial,full->data[i]);incdvi_update(ctx,d,partial);
    assert(incdvi_page_count(d)==(i+1==full->len ? 1:0));
  }
  assert(incdvi_destination_page(ctx,d,partial,"old")==0);
  partial->len=15;incdvi_update(ctx,d,partial);
  assert(incdvi_destination_page(ctx,d,partial,"old")==-1);
  fz_buffer *next=page(ctx,"new");
  fz_append_data(ctx,partial,next->data+15,next->len-15);incdvi_update(ctx,d,partial);
  assert(incdvi_destination_page(ctx,d,partial,"new")==0);
  assert(incdvi_destination_page(ctx,d,partial,"old")==-1);
  incdvi_free(ctx,d);fz_drop_buffer(ctx,full);fz_drop_buffer(ctx,partial);fz_drop_buffer(ctx,next);fz_drop_context(ctx);
  puts("PASS: partial DVI special bodies and rollback keep destination metadata valid");
}
