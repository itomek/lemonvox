#include "session.h"
void lv_init(LVSession *s) { s->state = LV_IDLE; s->generation = 0; }
uint64_t lv_toggle(LVSession *s) {
    if (s->state == LV_IDLE) { ++s->generation; s->state = LV_RECORDING; }
    else if (s->state == LV_RECORDING) s->state = LV_PROCESSING;
    return s->generation;
}
void lv_cancel(LVSession *s) { ++s->generation; s->state = LV_IDLE; }
int lv_finish(LVSession *s, uint64_t generation) {
    if (s->state != LV_PROCESSING || generation != s->generation) return 0;
    s->state = LV_IDLE;
    return 1;
}
