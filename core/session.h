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

// The native adapter owns these scalar facts and their source storage. This
// boundary borrows no strings, allocates no cross-language memory, and has no callbacks.
typedef enum { LV_MODEL_ASR, LV_MODEL_CLEANUP } LVModelPurpose;
typedef struct {
    uint8_t downloaded;
    uint8_t has_cloud_provider;
    uint8_t is_cloud_recipe;
    uint8_t is_whispercpp_recipe;
    uint8_t is_moonshine_recipe;
    uint8_t is_llamacpp_recipe;
    uint8_t is_flm_recipe;
    uint8_t is_ryzenai_llm_recipe;
    uint8_t has_transcription_label;
    uint8_t has_chat_label;
} LVModelFacts;
typedef struct {
    uint8_t current_target;
    uint8_t original_matches;
    uint8_t frontmost_matches;
    uint8_t secure;
} LVInsertionFacts;

void lv_init(LVSession *session);
uint64_t lv_toggle(LVSession *session);
void lv_cancel(LVSession *session);
int lv_finish(LVSession *session, uint64_t generation);
int lv_model_supports(const LVModelFacts *facts, LVModelPurpose purpose);
int lv_can_insert(const LVInsertionFacts *facts);
#ifdef __cplusplus
}
static_assert(std::is_standard_layout<LVSession>::value, "LVSession must remain C-layout");
static_assert(std::is_trivially_copyable<LVSession>::value, "LVSession must remain value-safe");
static_assert(offsetof(LVSession, state) == 0, "LVSession state must remain first");
static_assert(alignof(LVSession) >= alignof(uint64_t), "LVSession generation alignment changed");
static_assert(std::is_standard_layout<LVModelFacts>::value, "LVModelFacts must remain C-layout");
static_assert(std::is_trivially_copyable<LVModelFacts>::value, "LVModelFacts must remain value-safe");
static_assert(sizeof(LVModelFacts) == 10, "LVModelFacts ABI changed");
static_assert(offsetof(LVModelFacts, has_chat_label) == 9, "LVModelFacts order changed");
static_assert(std::is_standard_layout<LVInsertionFacts>::value, "LVInsertionFacts must remain C-layout");
static_assert(std::is_trivially_copyable<LVInsertionFacts>::value, "LVInsertionFacts must remain value-safe");
static_assert(sizeof(LVInsertionFacts) == 4, "LVInsertionFacts ABI changed");
static_assert(offsetof(LVInsertionFacts, secure) == 3, "LVInsertionFacts order changed");
#endif
