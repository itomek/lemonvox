#include "session.h"
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
