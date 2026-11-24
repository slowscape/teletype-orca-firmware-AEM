#include "vmio.h"

void oevent_list_init(Oevent_list *olist) {
  olist->count = 0;
}
void oevent_list_deinit(Oevent_list *olist) {
  (void)olist;
  // static buffer, nothing to free
}
void oevent_list_clear(Oevent_list *olist) { olist->count = 0; }
void oevent_list_copy(Oevent_list const *src, Oevent_list *dest) {
  Usz n = src->count;
  if (n > OEVENT_LIST_MAX)
    n = OEVENT_LIST_MAX;
  memcpy(dest->buffer, src->buffer, n * sizeof(Oevent));
  dest->count = n;
}
Oevent *oevent_list_alloc_item(Oevent_list *olist) {
  Usz count = olist->count;
  if (count >= OEVENT_LIST_MAX) {
    // overflow: overwrite the last slot rather than corrupt memory
    return &olist->buffer[OEVENT_LIST_MAX - 1];
  }
  olist->count = count + 1;
  return &olist->buffer[count];
}
