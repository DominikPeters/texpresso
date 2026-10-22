#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mydvi.h"
#include "mydvi_interp.h"

typedef struct { fz_device super; int count; } stroke_device;
static void record_stroke(fz_context *ctx, fz_device *dev, const fz_path *path,
                          const fz_stroke_state *stroke, fz_matrix ctm,
                          fz_colorspace *cs, const float *color, float alpha,
                          fz_color_params params)
{
  stroke_device *record = (stroke_device *)dev;
  int variant = record->count++ % 3, len = variant == 0 ? 2 : variant == 1 ? 4 : 0;
  assert(stroke->dash_len == len);
  assert(stroke->dash_phase == variant);
  assert(stroke->linewidth == 1);
  for (int i = 0; i < len; i++) assert(stroke->dash_list[i] == (variant == 0 ? i+2 : i+1));
}
int main(void)
{
  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT); assert(ctx);
  fz_rect box = {0, 0, 300, 300};
  fz_display_list *list = fz_new_display_list(ctx, box);
  fz_device *device = fz_new_list_device(ctx, list);
  dvi_context *dc = dvi_context_new(ctx, (dvi_reshooks){0});
  dvi_context_begin_frame(ctx, dc, device);
  fz_buffer *code = fz_new_buffer(ctx, 512);
  fz_append_string(ctx, code, "pdf:code ");
  const char *operators[] = {"S", "s", "B", "B*", "b", "b*"};
  for (int i = 0; i < 6; i++) {
    const char *dash = i % 3 == 0 ? "[2 3]" : i % 3 == 1 ? "[1 2 3 4]" : "[]";
    fz_append_printf(ctx, code, "%s %d d 1 w 0 0 m 20 0 l 20 20 l %s ", dash, i % 3, operators[i]);
  }
  assert(dvi_exec_special(ctx, dc, dvi_context_state(dc), (char *)code->data, (char *)code->data + code->len));
  dvi_context_end_frame(ctx, dc);
  fz_close_device(ctx, device); fz_drop_device(ctx, device);
  dvi_context_free(ctx, dc); fz_drop_buffer(ctx, code);
  // The interpreter has released its local stroke. Replaying must retain
  // every original pattern even after later operators change its dash length.
  stroke_device *record = fz_new_derived_device(ctx, stroke_device);
  record->super.stroke_path = record_stroke;
  fz_run_display_list(ctx, list, &record->super, fz_identity, box, NULL);
  assert(record->count == 6);
  fz_close_device(ctx, &record->super); fz_drop_device(ctx, &record->super);
  fz_drop_display_list(ctx, list); fz_drop_context(ctx);
  puts("PASS: DVI dashed/solid strokes retain independently owned MuPDF dash arrays");
}
