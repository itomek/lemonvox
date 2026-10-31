#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { LV_IDLE, LV_RECORDING, LV_PROCESSING } LVState;
typedef struct { LVState state; uint64_t generation; } LVSession;
void lv_init(LVSession *session);
uint64_t lv_toggle(LVSession *session);
void lv_cancel(LVSession *session);
int lv_finish(LVSession *session, uint64_t generation);
#ifdef __cplusplus
}
#endif
