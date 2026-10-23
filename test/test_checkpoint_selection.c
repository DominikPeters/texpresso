#include <stdio.h>
#include <string.h>
#include "../src/frontend/checkpoint_selection.h"

static void select(txp_checkpoint *p, bool *keep)
{
  int reserved = 0, n = txp_select_checkpoints(p, TXP_CHECKPOINT_CAPACITY, keep, &reserved), actual = 0;
  assert(reserved >= 0 && reserved <= 2*TXP_EDIT_ANCHORS);
  for (int i = 0; i < TXP_CHECKPOINT_CAPACITY; ++i) actual += keep[i];
  assert(n == actual && n < TXP_CHECKPOINT_CAPACITY);
  assert(keep[0] == p[0].live && keep[TXP_CHECKPOINT_CAPACITY-1]);
}
int main(void)
{
  int file_a, file_b;
  txp_checkpoint p[TXP_CHECKPOINT_CAPACITY]; bool keep[TXP_CHECKPOINT_CAPACITY];
  for (int i = 0; i < TXP_CHECKPOINT_CAPACITY; ++i)
    p[i] = (txp_checkpoint){ .trace=500+i, .entry=&file_a, .offset=i*64, .live=true };
  p[5].anchor=1; p[10].anchor=2; p[15].anchor=3;
  select(p,keep);
  assert(keep[10] && keep[15] && !keep[5]);
  assert(keep[9] && keep[14]); // Existing strict predecessors survive consumption.
  // Refreshing an actively reused older edit site replaces the colder site.
  p[5].anchor=4; select(p,keep);
  assert(keep[5] && keep[15] && !keep[10]);
  // Duplicate exact positions use one reserved slot, including old generations.
  p[15].offset=p[5].offset; p[15].anchor=5; select(p,keep);
  assert(keep[5] && keep[10] && !keep[15]);
  // Equal offsets in different input files are independent anchors.
  p[15].entry=&file_b; p[15].anchor=6; select(p,keep);
  assert(keep[5] && keep[15] && !keep[10]);
  // Dead/invalidated snapshots cannot use reserved anchor slots.
  p[15].live=false; select(p,keep);
  assert(keep[5] && keep[10] && !keep[15]);
  p[5].entry=NULL; select(p,keep);
  assert(keep[10] && !keep[5]);
  p[0].live=false;select(p,keep);assert(!keep[0]);
  // Stress exponential coverage and all possible anchor locations at the cap.
  for (int a=1;a<31;++a) for(int b=a+1;b<31;++b) {
    for (int i=0;i<32;++i) {
      p[i]=(txp_checkpoint){.trace=i<30 ? 1<<i : INT32_MAX-(31-i), .entry=&file_a, .offset=i, .live=true};
    }
    p[a].anchor=1;p[b].anchor=2;select(p,keep);
    assert(keep[a] && keep[b]);
  }
  puts("checkpoint selection: bounded recent edit anchors, refresh, deduplication and invalidation passed");
}
