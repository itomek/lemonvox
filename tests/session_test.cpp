#include "session.h"
#include <cassert>
#include <cstdio>

static LVModelFacts modelFacts(uint8_t downloaded, uint8_t cloudProvider, uint8_t cloudRecipe,
    uint8_t whisper, uint8_t moonshine, uint8_t llama, uint8_t flm, uint8_t ryzen,
    uint8_t transcription, uint8_t chat) {
    return {downloaded, cloudProvider, cloudRecipe, whisper, moonshine, llama, flm, ryzen, transcription, chat};
}

static LVInsertionFacts insertionFacts(uint8_t currentTarget, uint8_t originalMatches,
    uint8_t frontmostMatches, uint8_t secure) {
    return {currentTarget, originalMatches, frontmostMatches, secure};
}

// This is the pre-extraction Swift rule written as a test-only parity oracle.
static bool swiftModelSupports(const LVModelFacts &facts, LVModelPurpose purpose) {
    if (facts.is_cloud_recipe && facts.has_cloud_provider) {
        return purpose == LV_MODEL_CLEANUP && facts.has_chat_label;
    }
    if (!facts.downloaded || facts.has_cloud_provider) return false;
    if (purpose == LV_MODEL_ASR) {
        return facts.has_transcription_label &&
            (facts.is_whispercpp_recipe || facts.is_moonshine_recipe);
    }
    return purpose == LV_MODEL_CLEANUP && facts.has_chat_label &&
        (facts.is_llamacpp_recipe || facts.is_flm_recipe || facts.is_ryzenai_llm_recipe);
}

static bool swiftCanInsert(const LVInsertionFacts &facts) {
    return !facts.secure && facts.frontmost_matches &&
        (facts.current_target || facts.original_matches);
}

int main() {
    assert(lv_abi_version() == 1);
    LVSession s;
    lv_init(&s);
    assert(s.state == LV_IDLE);
    auto first = lv_toggle(&s);
    assert(s.state == LV_RECORDING);
    assert(!lv_finish(&s, first));
    assert(lv_toggle(&s) == first && s.state == LV_PROCESSING);
    assert(lv_toggle(&s) == first && s.state == LV_PROCESSING);
    lv_cancel(&s);
    assert(s.state == LV_IDLE && !lv_finish(&s, first));
    auto second = lv_toggle(&s);
    assert(second != first);
    lv_toggle(&s);
    assert(!lv_finish(&s, first) && s.state == LV_PROCESSING);
    assert(lv_finish(&s, second) && s.state == LV_IDLE);
    assert(!lv_finish(&s, second));

    auto localAsr = modelFacts(1, 0, 0, 1, 0, 0, 0, 0, 1, 0);
    auto localCleanup = modelFacts(1, 0, 0, 0, 0, 1, 0, 0, 0, 1);
    auto remoteCleanup = modelFacts(0, 1, 1, 0, 0, 0, 0, 0, 0, 1);
    auto remoteAsr = modelFacts(0, 1, 1, 0, 0, 0, 0, 0, 1, 0);
    auto missingMetadata = modelFacts(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    auto moonshineAsr = modelFacts(1, 0, 0, 0, 1, 0, 0, 0, 1, 0);
    auto flmCleanup = modelFacts(1, 0, 0, 0, 0, 0, 1, 0, 0, 1);
    auto ryzenCleanup = modelFacts(1, 0, 0, 0, 0, 0, 0, 1, 0, 1);
    auto routedLocal = modelFacts(1, 1, 0, 1, 0, 0, 0, 0, 1, 0);
    const LVModelFacts modelFixtures[] = {
        localAsr, localCleanup, remoteCleanup, remoteAsr, missingMetadata,
        moonshineAsr, flmCleanup, ryzenCleanup, routedLocal
    };
    for (const auto &facts : modelFixtures) {
        assert(lv_model_supports(&facts, LV_MODEL_ASR) == swiftModelSupports(facts, LV_MODEL_ASR));
        assert(lv_model_supports(&facts, LV_MODEL_CLEANUP) == swiftModelSupports(facts, LV_MODEL_CLEANUP));
    }
    assert(lv_model_supports(&localAsr, LV_MODEL_ASR));
    assert(!lv_model_supports(&localAsr, LV_MODEL_CLEANUP));
    assert(lv_model_supports(&localCleanup, LV_MODEL_CLEANUP));
    assert(!lv_model_supports(&localCleanup, LV_MODEL_ASR));
    assert(lv_model_supports(&remoteCleanup, LV_MODEL_CLEANUP));
    assert(!lv_model_supports(&remoteCleanup, LV_MODEL_ASR));
    assert(!lv_model_supports(&remoteAsr, LV_MODEL_ASR));
    assert(!lv_model_supports(&missingMetadata, LV_MODEL_ASR));
    assert(!lv_model_supports(&missingMetadata, LV_MODEL_CLEANUP));

    for (uint8_t currentTarget = 0; currentTarget < 2; ++currentTarget) {
        for (uint8_t originalMatches = 0; originalMatches < 2; ++originalMatches) {
            for (uint8_t frontmostMatches = 0; frontmostMatches < 2; ++frontmostMatches) {
                for (uint8_t secure = 0; secure < 2; ++secure) {
                    auto facts = insertionFacts(currentTarget, originalMatches, frontmostMatches, secure);
                    const auto expected = swiftCanInsert(facts);
                    assert(lv_can_insert(&facts) == expected);
                }
            }
        }
    }
    std::puts("PASS: transitions, stale completion, model eligibility and safe insertion policy");
}
