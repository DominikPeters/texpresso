#ifndef TXP_CHECKPOINT_SELECTION_H
#define TXP_CHECKPOINT_SELECTION_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/* Reserve existing edit checkpoints; never create additional workers here. */
enum { TXP_CHECKPOINT_CAPACITY = 32, TXP_EDIT_ANCHORS = 2 };
typedef struct {
  int trace;
  const void *entry;
  int offset;
  uint64_t anchor;
  bool live;
} txp_checkpoint;

static int txp_select_checkpoints(const txp_checkpoint *points, int count, bool *keep, int *reserved_count)
{
  assert(count > 0 && count <= TXP_CHECKPOINT_CAPACITY);
  bool sites[TXP_CHECKPOINT_CAPACITY] = {0}, protected[TXP_CHECKPOINT_CAPACITY] = {0};
  for (int i = 0; i < count; ++i) keep[i] = false;
  int64_t target = 32;
  for (int i = 0; i < count; ++i)
    if (points[i].trace >= target) { keep[i] = true; target *= 2; }
  target = points[count-1].trace;
  int64_t delta = 32;
  for (int i = count-1; i >= 0; --i)
    if (points[i].trace <= target) {
      keep[i] = true; delta *= 2; target -= delta;
    } else if (keep[i]) { delta *= 2; target = points[i].trace-delta; }

  int reserved = 0;
  for (int slot = 0; slot < TXP_EDIT_ANCHORS; ++slot) {
    int newest = -1;
    for (int i = 0; i < count; ++i) {
      if (!points[i].live || !points[i].entry || !points[i].anchor) continue;
      bool duplicate = false;
      for (int j = 0; j < count; ++j)
        if (sites[j] && points[j].entry == points[i].entry && points[j].offset == points[i].offset)
          duplicate = true;
      if (!duplicate && (newest < 0 || points[i].anchor > points[newest].anchor)) newest = i;
    }
    if (newest < 0) break;
    // Recency belongs to the site, but the earliest live ancestor at that
    // site has the fewest observed bytes and also remains valid for undo.
    // A newer duplicate can contain a later SEEN trace at the same READ offset.
    int ancestor = newest;
    for (int i = 0; i < newest; ++i)
      if (points[i].live && points[i].anchor && points[i].entry == points[newest].entry && points[i].offset == points[newest].offset) {
        ancestor = i; break;
      }
    sites[ancestor] = protected[ancestor] = keep[ancestor] = true;
    // Resuming an exact edit checkpoint consumes its process state. Keep one
    // already-existing broader prefix for a later undo that invalidates that
    // resumed state. Shared predecessors occupy only one reserved slot.
    for (int i = ancestor-1; i >= 0; --i)
      if (points[i].live && points[i].trace < points[ancestor].trace) {
        protected[i] = keep[i] = true; break;
      }
  }
  for (int i = 0; i < count; ++i) reserved += protected[i];
  // The last worker is Q_CHLD's parent; retain a live oldest fallback.
  // Normally all suspended ancestors are live, but do not pin a closed root.
  keep[0] = points[0].live;
  keep[count-1] = true;
  int retained = 0;
  for (int i = 0; i < count; ++i) retained += keep[i];
  // Exponential coverage is normally sparse. Bound even a pathological union
  // without sacrificing either reserved anchor or the current parent worker.
  for (int i = 1; retained >= TXP_CHECKPOINT_CAPACITY && i < count-1; ++i)
    if (keep[i] && !protected[i]) { keep[i] = false; retained--; }
  assert(retained < TXP_CHECKPOINT_CAPACITY);
  if (reserved_count) *reserved_count = reserved;
  return retained;
}
#endif
