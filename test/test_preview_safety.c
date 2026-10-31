#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "incdvi.h"
#include "mydvi.h"
#include "mydvi_interp.h"
#include "mydvi_opcodes.h"
#include "txp_error.h"
#include "include_path.h"
#include "intcodec.h"

#define EXPECT_ERROR(statement, fragment) do { \
  volatile int caught = 0; \
  fz_try(ctx) { statement; } \
  fz_catch(ctx) { caught = 1; assert(strstr(txp_take_error(ctx), fragment)); } \
  assert(caught); \
} while (0)

static void u32(fz_context *ctx, fz_buffer *buffer, unsigned n)
{ fz_append_byte(ctx,buffer,n>>24);fz_append_byte(ctx,buffer,n>>16);fz_append_byte(ctx,buffer,n>>8);fz_append_byte(ctx,buffer,n); }

int main(void)
{
  fz_context *ctx = fz_new_context(NULL,NULL,FZ_STORE_DEFAULT); assert(ctx);
  const uint8_t negative[]={255,255,255,255};
  assert(decode_u32(negative)==UINT32_MAX && decode_s32(negative)==-1);
  assert(decode_s16(negative)==-1 && decode_s24(negative)==-1);
  char directories[1110], output[1024]; memset(directories,'x',1100);
  memcpy(directories+1100,"\0good\0\0",7);
  const char *paths=directories;
  assert(txp_expand_path(&paths,"file.tex",output,sizeof output)==output);
  assert(!strcmp(output,"good/file.tex"));
  assert(txp_expand_path(&paths,"file.tex",output,sizeof output)==NULL);
  paths="short\0";
  assert(txp_expand_path(&paths,"/absolute",output,sizeof output)==NULL);
  paths="dir/\0";char exact[6];
  assert(txp_expand_path(&paths,"x",exact,sizeof exact)==exact && !strcmp(exact,"dir/x"));
  paths="dir/\0";
  assert(txp_expand_path(&paths,"x",exact,5)==NULL);
  // Every opcode with short, exactly allocated input: sanitizer builds catch
  // accidental reads into a missing length field or the next instruction.
  for (int op=0;op<256;op++) for (int len=0;len<64;len++) {
    uint8_t *input = malloc(len ? len : 1); assert(input);
    memset(input,0,len); if (len) input[0]=op;
    (void)dvi_instr_size(input,len,DVI_XDV7);
    free(input);
  }
  uint8_t huge[] = {XXX4,255,255,255,255};
  assert(dvi_instr_size(huge,sizeof huge,DVI_XDV7)==0);
  uint8_t font[19] = {FNT_DEF1}; font[14]=1; font[15]=2;
  assert(dvi_instr_size(font,sizeof font,DVI_XDV7)==19); // area + name, not a u16
  for (int len=0;len<48;len++) {
    uint8_t *input = malloc(len ? len : 1); assert(input); memset(input,0,len);
    if (len) input[0]=BOP;
    float width,height; bool landscape;
    (void)dvi_interp_bop(input,len,&width,&height,&landscape);
    free(input);
  }
  const char *special="pdf: pagesize width 100pt height 200pt";
  for (int n=0;n<=(int)strlen(special);n++) {
    int len=47+n; uint8_t *input=calloc(len,1); assert(input);
    input[0]=BOP;input[45]=XXX1;input[46]=n;memcpy(input+47,special,n);
    float width,height;bool landscape;
    (void)dvi_interp_bop(input,len,&width,&height,&landscape);
    free(input);
  }
  incdvi_t *dvi = incdvi_new(ctx,(dvi_reshooks){0});
  float width,height;
  EXPECT_ERROR(incdvi_page_dim(ctx,dvi,NULL,0,&width,&height,NULL),"not available");
  EXPECT_ERROR(incdvi_display_list(ctx,dvi,NULL,-1),"not available");
  fz_buffer *buffer = fz_new_buffer(ctx,128);
  fz_append_byte(ctx,buffer,PRE);fz_append_byte(ctx,buffer,7);
  u32(ctx,buffer,25400000);u32(ctx,buffer,473628672);u32(ctx,buffer,1000);fz_append_byte(ctx,buffer,0);
  fz_append_byte(ctx,buffer,BOP);for(int i=0;i<11;i++)u32(ctx,buffer,0);
  fz_append_byte(ctx,buffer,EOP);incdvi_update(ctx,dvi,buffer);
  assert(incdvi_page_count(dvi)==1);
  EXPECT_ERROR(incdvi_page_dim(ctx,dvi,buffer,1,&width,&height,NULL),"not available");
  size_t complete=buffer->len; buffer->len=16;
  EXPECT_ERROR(incdvi_display_list(ctx,dvi,buffer,0),"Invalid DVI page header");
  buffer->len=complete;
  fz_display_list *list=incdvi_display_list(ctx,dvi,buffer,0);assert(list);fz_drop_display_list(ctx,list);
  incdvi_reset(dvi);buffer->data[buffer->len-1]=BOP;
  for(int i=0;i<11;i++)u32(ctx,buffer,0);
  EXPECT_ERROR(incdvi_update(ctx,dvi,buffer),"Unexpected DVI page boundary");
  incdvi_free(ctx,dvi);fz_drop_buffer(ctx,buffer);
  dvi_context *dc=dvi_context_new(ctx,(dvi_reshooks){0});
  EXPECT_ERROR(dvi_fonttable_get(ctx,dvi_context_state(dc)->fonts,-1),"font number");
  EXPECT_ERROR(dvi_fonttable_get(ctx,dvi_context_state(dc)->fonts,10000),"font number");
  dvi_context_begin_frame(ctx,dc,NULL);
  EXPECT_ERROR(dvi_exec_char(ctx,dc,dvi_context_state(dc),65,false),"font");
  dvi_context_abort_frame(ctx,dc);
  // The same context can start a clean frame after a rendering exception.
  dvi_context_begin_frame(ctx,dc,NULL);dvi_context_end_frame(ctx,dc);
  dvi_context_free(ctx,dc);fz_drop_context(ctx);
  puts("PASS: 16384 short instructions, DVI bounds, malformed boundaries and font failures");
}
