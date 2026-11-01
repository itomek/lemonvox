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

/*
 * Portable modifier input. Native adapters translate platform key codes before
 * calling this boundary. The caller owns detector storage and serializes access;
 * calls copy their input synchronously and allocate no memory or callbacks.
 */
typedef enum { LV_KEY_UNKNOWN, LV_KEY_COMMAND, LV_KEY_OPTION, LV_KEY_CONTROL, LV_KEY_SHIFT } LVKeyIdentity;
typedef enum { LV_KEY_SIDE_ANY, LV_KEY_SIDE_LEFT, LV_KEY_SIDE_RIGHT, LV_KEY_SIDE_NONE } LVKeySide;
typedef enum { LV_KEY_PRESS, LV_KEY_RELEASE, LV_KEY_INTERVENING } LVKeyAction;
typedef struct {
    LVKeyIdentity key;
    LVKeySide side;
    LVKeyAction action;
    double timestamp;
} LVNormalizedKeyEvent;
typedef struct {
    LVKeyIdentity target_key;
    LVKeySide target_side;
    int has_pressed;
    int has_first;
    LVNormalizedKeyEvent pressed;
    LVNormalizedKeyEvent first;
} LVDoubleTapDetector;
void lv_double_tap_init(LVDoubleTapDetector *detector, LVKeyIdentity target_key, LVKeySide target_side);
void lv_double_tap_cancel(LVDoubleTapDetector *detector);
int lv_double_tap_event(LVDoubleTapDetector *detector, LVNormalizedKeyEvent event);
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
