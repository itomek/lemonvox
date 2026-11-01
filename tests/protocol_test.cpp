#include "protocol.h"
#include "yyjson.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

LVProtocolSlice slice(const char *value) {
    return {reinterpret_cast<const uint8_t *>(value), std::strlen(value)};
}

LVProtocolSlice slice(const std::string &value) {
    return {reinterpret_cast<const uint8_t *>(value.data()), value.size()};
}

LVProtocolConfig validConfig() {
    return {{slice("http://127.0.0.1:13305/v1"), 1, 1, 1, 1, 1},
        slice("asr"), slice("cleanup"), slice("prompt")};
}

void expectBytes(const LVProtocolBytes &bytes, const char *expected) {
    assert(bytes.size == std::strlen(expected));
    assert(std::memcmp(bytes.data, expected, bytes.size) == 0);
}

std::string stringFrom(const LVProtocolBytes &bytes) {
    return std::string(reinterpret_cast<const char *>(bytes.data), bytes.size);
}

void expectResponse(LVProtocolResult result, LVProtocolStatus status, const char *expected) {
    assert(result.status == status);
    if (expected) {
        const auto actual = stringFrom(result.value);
        if (actual != expected) std::fprintf(stderr, "Expected [%s], got [%s]\n", expected, actual.c_str());
        assert(actual == expected);
    }
    lv_protocol_result_free(&result);
}

void expectCleanupPayload(const LVProtocolBytes &bytes, const std::string &model,
    const std::string &prompt, const std::string &transcript) {
    auto *document = yyjson_read_opts(reinterpret_cast<char *>(bytes.data), bytes.size,
        YYJSON_READ_NOFLAG, nullptr, nullptr);
    assert(document);
    const auto *root = yyjson_doc_get_root(document);
    assert(root && yyjson_is_obj(root));

    const auto *modelValue = yyjson_obj_get(root, "model");
    assert(modelValue && yyjson_is_str(modelValue));
    assert(yyjson_get_len(modelValue) == model.size());
    assert(std::memcmp(yyjson_get_str(modelValue), model.data(), model.size()) == 0);
    const auto *stream = yyjson_obj_get(root, "stream");
    assert(stream && yyjson_is_bool(stream) && !yyjson_get_bool(stream));
    const auto *temperature = yyjson_obj_get(root, "temperature");
    assert(temperature && yyjson_is_int(temperature) && yyjson_get_sint(temperature) == 0);
    assert(!yyjson_obj_get(root, "tools"));

    const auto *messages = yyjson_obj_get(root, "messages");
    assert(messages && yyjson_is_arr(messages) && yyjson_arr_size(messages) == 2);
    const auto *system = yyjson_arr_get_first(messages);
    const auto *user = yyjson_arr_get(messages, 1);
    assert(system && user && yyjson_is_obj(system) && yyjson_is_obj(user));
    const auto *systemRole = yyjson_obj_get(system, "role");
    const auto *systemContent = yyjson_obj_get(system, "content");
    const auto *userRole = yyjson_obj_get(user, "role");
    const auto *userContent = yyjson_obj_get(user, "content");
    assert(systemRole && systemContent && userRole && userContent);
    assert(std::string(yyjson_get_str(systemRole), yyjson_get_len(systemRole)) == "system");
    assert(std::string(yyjson_get_str(systemContent), yyjson_get_len(systemContent)) == prompt);
    assert(std::string(yyjson_get_str(userRole), yyjson_get_len(userRole)) == "user");
    assert(std::string(yyjson_get_str(userContent), yyjson_get_len(userContent)) == transcript);
    yyjson_doc_free(document);
}

} // namespace

int main() {
    auto config = validConfig();
    assert(lv_protocol_validate_config(&config) == LV_PROTOCOL_OK);

    lemonvox::ProtocolConfiguration directConfig{{"http://127.0.0.1:13305/v1", true, true, true, true, true},
        "asr", "cleanup", "prompt"};
    lemonvox::TransportRequest directModels;
    assert(lemonvox::Protocol::make_models_request(directConfig, directModels) == LV_PROTOCOL_OK);
    assert(directModels.method == lemonvox::HttpMethod::get && directModels.url == "http://127.0.0.1:13305/v1/models");

    auto models = lv_protocol_make_models_request(&config);
    assert(models.status == LV_PROTOCOL_OK && models.method == LV_PROTOCOL_GET);
    expectBytes(models.url, "http://127.0.0.1:13305/v1/models");
    assert(models.body.size == 0 && models.content_type.size == 0);
    lv_protocol_request_free(&models);

    const uint8_t wav[] = {'R', 'I', 'F', 'F'};
    const auto boundary = slice("fixture-boundary");
    auto transcription = lv_protocol_make_transcription_request(&config, {wav, sizeof(wav)}, boundary);
    assert(transcription.status == LV_PROTOCOL_OK && transcription.method == LV_PROTOCOL_POST);
    expectBytes(transcription.url, "http://127.0.0.1:13305/v1/audio/transcriptions");
    expectBytes(transcription.content_type, "multipart/form-data; boundary=fixture-boundary");
    const auto multipart = std::string(reinterpret_cast<const char *>(transcription.body.data), transcription.body.size);
    assert(multipart.find("name=\"model\"\r\n\r\nasr") != std::string::npos);
    assert(multipart.find("name=\"response_format\"\r\n\r\njson") != std::string::npos);
    assert(multipart.find("filename=\"dictation.wav\"") != std::string::npos);
    assert(multipart.find("RIFF") != std::string::npos);
    lv_protocol_request_free(&transcription);

    const auto transcript = slice("um send Łukasz 14 invoices");
    auto cleanup = lv_protocol_make_cleanup_request(&config, transcript);
    assert(cleanup.status == LV_PROTOCOL_OK && cleanup.method == LV_PROTOCOL_POST);
    expectBytes(cleanup.url, "http://127.0.0.1:13305/v1/chat/completions");
    expectBytes(cleanup.content_type, "application/json");
    expectCleanupPayload(cleanup.body, "cleanup", "prompt", "um send Łukasz 14 invoices");
    lv_protocol_request_free(&cleanup);

    const std::string embeddedNulModel("clean\0up", 8);
    auto nulConfig = validConfig();
    nulConfig.cleanup_model = slice(embeddedNulModel);
    assert(lv_protocol_validate_config(&nulConfig) == LV_PROTOCOL_OK);
    auto nulCleanup = lv_protocol_make_cleanup_request(&nulConfig, transcript);
    assert(nulCleanup.status == LV_PROTOCOL_OK);
    expectCleanupPayload(nulCleanup.body, embeddedNulModel, "prompt", "um send Łukasz 14 invoices");
    lv_protocol_request_free(&nulCleanup);

    const std::string inventory = R"({"data":[{"id":"remote.chat","recipe":"cloud","cloud_provider":"remote","labels":["chat"]},{"id":"chat","downloaded":true,"recipe":"llamacpp","labels":["chat"]},{"id":"asr","downloaded":true,"recipe":"whispercpp","labels":["transcription"]},{"id":"embed","downloaded":true,"recipe":"llamacpp","labels":["embeddings"]}]})";
    auto parsed = lv_protocol_parse_inventory(slice(inventory));
    assert(parsed.status == LV_PROTOCOL_OK && parsed.count == 3);
    assert(stringFrom(parsed.models[0].id) == "asr" && parsed.models[0].asr && !parsed.models[0].cleanup);
    assert(stringFrom(parsed.models[1].id) == "chat" && !parsed.models[1].asr && parsed.models[1].cleanup);
    assert(stringFrom(parsed.models[2].id) == "remote.chat" && parsed.models[2].cleanup && parsed.models[2].remote);
    lv_protocol_inventory_free(&parsed);
    lemonvox::ProtocolInventory directInventory;
    assert(lemonvox::Protocol::parse_inventory(std::vector<std::uint8_t>(inventory.begin(), inventory.end()), directInventory) == LV_PROTOCOL_OK);
    assert(directInventory.models.size() == 3 && directInventory.models[0].id == "asr" && directInventory.models[2].remote);
    auto selected = validConfig();
    selected.cleanup_model = slice("chat");
    assert(lv_protocol_validate_models(slice(inventory), &selected) == LV_PROTOCOL_OK);
    const auto inventoryWithWrongMetadata = inventory.substr(0, inventory.size() - 1) + R"(,"reason":"ok","message":123})";
    auto discoveryMetadata = lv_protocol_parse_inventory(slice(inventoryWithWrongMetadata));
    assert(discoveryMetadata.status == LV_PROTOCOL_RESPONSE);
    lv_protocol_inventory_free(&discoveryMetadata);
    assert(lv_protocol_validate_models(slice(inventoryWithWrongMetadata), &selected) == LV_PROTOCOL_OK);

    expectResponse(lv_protocol_parse_transcription(slice(R"({"text":"Żółć 😀"})")), LV_PROTOCOL_OK, "Żółć 😀");
    expectResponse(lv_protocol_parse_transcription(slice(R"({"text":"\uD83D\uDE00"})")), LV_PROTOCOL_OK, "😀");
    expectResponse(lv_protocol_parse_transcription(slice(R"({"text":"  dictated text \n"})")), LV_PROTOCOL_OK, "  dictated text \n");
    expectResponse(lv_protocol_parse_transcription(slice(R"({"text":"first","text":"second"})")), LV_PROTOCOL_OK, "first");
    expectResponse(lv_protocol_parse_transcription(slice(R"({"text":" "})")), LV_PROTOCOL_EMPTY_TRANSCRIPT, nullptr);
    expectResponse(lv_protocol_parse_cleanup(slice(R"({"choices":[{"message":{"content":"Send Alex 14 invoices."},"finish_reason":"stop"}]})")), LV_PROTOCOL_OK, "Send Alex 14 invoices.");
    expectResponse(lv_protocol_parse_cleanup(slice(R"({"choices":[{"message":{"content":"Partial"},"finish_reason":"length"}]})")), LV_PROTOCOL_RESPONSE, nullptr);
    expectResponse(lv_protocol_parse_cleanup(slice(R"({"choices":[{"message":{"content":"First"},"finish_reason":"stop"},{"message":{"content":7},"finish_reason":"stop"}]})")), LV_PROTOCOL_RESPONSE, nullptr);
    expectResponse(lv_protocol_parse_version(slice(R"({"version":"2026.44.1"})")), LV_PROTOCOL_OK, "2026.44.1");
    expectResponse(lv_protocol_classify_http(401, slice(R"({"error":{"message":"Authorization: Bearer sk-secretvalue api_key=hidden"}})")), LV_PROTOCOL_UPSTREAM, "[redacted] [redacted]");
    expectResponse(lv_protocol_classify_http(401, slice(R"({"error":{"message":7},"message":"fallback"})")), LV_PROTOCOL_UPSTREAM, "fallback");
    expectResponse(lv_protocol_classify_http(401, slice(R"({"error":"fallback error"})")), LV_PROTOCOL_UPSTREAM, "fallback error");
    expectResponse(lv_protocol_classify_http(500, slice("private server failure")), LV_PROTOCOL_HTTP, nullptr);
    const std::string privateReason = std::string(1990, 'x') + " api_key=hidden-value";
    const std::string inventoryWithReason = std::string("{\"data\":[],\"reason\":\"") + privateReason + "\"}";
    auto bounded = lv_protocol_parse_inventory(slice(inventoryWithReason));
    assert(bounded.status == LV_PROTOCOL_OK && bounded.reason.size <= 2000);
    assert(stringFrom(bounded.reason).find("hidden-value") == std::string::npos);
    lv_protocol_inventory_free(&bounded);
    std::string unicodeVersion;
    for (size_t index = 0; index < 1999; ++index) unicodeVersion += "é";
    unicodeVersion += "😀";
    const auto unicodeVersionJSON = std::string("{\"version\":\"") + unicodeVersion + "\"}";
    auto boundedUnicode = lv_protocol_parse_version(slice(unicodeVersionJSON));
    assert(boundedUnicode.status == LV_PROTOCOL_OK && stringFrom(boundedUnicode.value) == unicodeVersion);
    lv_protocol_result_free(&boundedUnicode);

    for (const auto &malformed : std::vector<std::string>{"{", "{\"text\":NaN}", "{\"text\":\"x\"} trailing", "{\"text\":\"\\uD83D\"}"}) {
        expectResponse(lv_protocol_parse_transcription(slice(malformed)), LV_PROTOCOL_RESPONSE, nullptr);
    }
    std::string rawNul = R"({"text":"x"})";
    rawNul.push_back('\0');
    rawNul += "trailing";
    expectResponse(lv_protocol_parse_transcription(slice(rawNul)), LV_PROTOCOL_RESPONSE, nullptr);
    std::string invalidUTF8 = "{\"text\":\"";
    invalidUTF8.push_back(static_cast<char>(0xFF));
    invalidUTF8 += "\"}";
    expectResponse(lv_protocol_parse_transcription(slice(invalidUTF8)), LV_PROTOCOL_RESPONSE, nullptr);
    const std::string bomJSON("\xEF\xBB\xBF{\"text\":\"x\"}");
    expectResponse(lv_protocol_parse_transcription(slice(bomJSON)), LV_PROTOCOL_RESPONSE, nullptr);
    const auto nested = [](size_t depth) { return std::string(depth, '[') + "0" + std::string(depth, ']'); };
    assert(lv_protocol_json_valid(slice(nested(512))));
    assert(!lv_protocol_json_valid(slice(nested(513))));

    std::puts("PASS: protocol configuration, wire mapping, Foundation-parity JSON and safe errors");
}
