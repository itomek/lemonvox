#include "session.h"

#include <cstddef>
#include <type_traits>

static_assert(std::is_standard_layout<LVNormalizedKeyEvent>::value, "trigger event must remain C-layout");
static_assert(std::is_standard_layout<LVDoubleTapDetector>::value, "trigger state must remain C-layout");
static_assert(sizeof(LVKeyIdentity) == sizeof(int) && sizeof(LVKeySide) == sizeof(int) && sizeof(LVKeyAction) == sizeof(int),
    "trigger enums must retain C int ABI");
static_assert(offsetof(LVNormalizedKeyEvent, timestamp) == 16 && sizeof(LVNormalizedKeyEvent) == 24,
    "trigger event layout must remain stable for native adapters");
static_assert(offsetof(LVDoubleTapDetector, pressed) == 16 && offsetof(LVDoubleTapDetector, first) == 40 &&
    sizeof(LVDoubleTapDetector) == 64, "trigger state layout must remain stable for native adapters");

uint32_t lv_abi_version(void) { return LV_ABI_VERSION; }
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

int lv_model_supports(const LVModelFacts *facts, LVModelPurpose purpose) {
    if (!facts) return 0;
    if (facts->is_cloud_recipe && facts->has_cloud_provider) {
        return purpose == LV_MODEL_CLEANUP && facts->has_chat_label;
    }
    if (!facts->downloaded || facts->has_cloud_provider) return 0;
    if (purpose == LV_MODEL_ASR) {
        return facts->has_transcription_label &&
            (facts->is_whispercpp_recipe || facts->is_moonshine_recipe);
    }
    if (purpose == LV_MODEL_CLEANUP) {
        return facts->has_chat_label &&
            (facts->is_llamacpp_recipe || facts->is_flm_recipe || facts->is_ryzenai_llm_recipe);
    }
    return 0;
}

int lv_can_insert(const LVInsertionFacts *facts) {
    return facts && !facts->secure && facts->frontmost_matches &&
        (facts->current_target || facts->original_matches);
}

void lv_double_tap_init(LVDoubleTapDetector *detector, LVKeyIdentity target_key, LVKeySide target_side) {
    detector->target_key = target_key;
    detector->target_side = target_side;
    lv_double_tap_cancel(detector);
}

void lv_double_tap_cancel(LVDoubleTapDetector *detector) {
    detector->has_pressed = 0;
    detector->has_first = 0;
}

static int lv_matches_target(const LVDoubleTapDetector *detector, LVNormalizedKeyEvent event) {
    return event.key == detector->target_key &&
        (detector->target_side == LV_KEY_SIDE_ANY || event.side == detector->target_side);
}

int lv_double_tap_event(LVDoubleTapDetector *detector, LVNormalizedKeyEvent event) {
    if (event.action == LV_KEY_INTERVENING || !lv_matches_target(detector, event)) {
        lv_double_tap_cancel(detector);
        return 0;
    }

    if (event.action == LV_KEY_PRESS) {
        if (detector->has_pressed) {
            lv_double_tap_cancel(detector);
            return 0;
        }
        if (detector->has_first &&
            (detector->first.key != event.key || detector->first.side != event.side ||
                event.timestamp - detector->first.timestamp > 0.35 || event.timestamp < detector->first.timestamp)) {
            detector->has_first = 0;
        }
        detector->pressed = event;
        detector->has_pressed = 1;
        return 0;
    }

    if (event.action != LV_KEY_RELEASE || !detector->has_pressed ||
        detector->pressed.key != event.key || detector->pressed.side != event.side ||
        event.timestamp < detector->pressed.timestamp || event.timestamp - detector->pressed.timestamp > 0.25) {
        lv_double_tap_cancel(detector);
        return 0;
    }

    detector->has_pressed = 0;
    if (detector->has_first && detector->first.key == event.key && detector->first.side == event.side &&
        event.timestamp - detector->first.timestamp <= 0.60) {
        lv_double_tap_cancel(detector);
        return 1;
    }
    detector->first = event;
    detector->has_first = 1;
    return 0;
}
