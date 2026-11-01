#pragma once
#include <stdint.h>
#ifdef __cplusplus
#include <cstddef>
#include <type_traits>
extern "C" {
#endif
#define LV_ABI_VERSION 1
typedef enum { LV_IDLE, LV_RECORDING, LV_PROCESSING } LVState;
typedef struct { LVState state; uint64_t generation; } LVSession;
uint32_t lv_abi_version(void);
void lv_init(LVSession *session);
uint64_t lv_toggle(LVSession *session);
void lv_cancel(LVSession *session);
int lv_finish(LVSession *session, uint64_t generation);
#ifdef __cplusplus
}
static_assert(std::is_standard_layout<LVSession>::value, "LVSession must remain C-layout");
static_assert(std::is_trivially_copyable<LVSession>::value, "LVSession must remain value-safe");
static_assert(offsetof(LVSession, state) == 0, "LVSession state must remain first");
static_assert(alignof(LVSession) >= alignof(uint64_t), "LVSession generation alignment changed");
#endif
