#include "protocol.h"

#include "yyjson.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <regex>
#include <string>
#include <type_traits>
#include <vector>

namespace {

bool hasNul(LVProtocolSlice value) {
    return value.data && std::memchr(value.data, 0, value.size) != nullptr;
}

bool readCodePoint(LVProtocolSlice value, size_t &index, uint32_t &codePoint) {
    if (!value.data || index >= value.size) return false;
    const auto first = value.data[index++];
    if (first < 0x80) {
        codePoint = first;
        return true;
    }
    size_t length = 0;
    uint32_t minimum = 0;
    if ((first & 0xE0) == 0xC0) { length = 1; codePoint = first & 0x1F; minimum = 0x80; }
    else if ((first & 0xF0) == 0xE0) { length = 2; codePoint = first & 0x0F; minimum = 0x800; }
    else if ((first & 0xF8) == 0xF0) { length = 3; codePoint = first & 0x07; minimum = 0x10000; }
    else return false;
    if (value.size - index < length) return false;
    for (size_t offset = 0; offset < length; ++offset) {
        const auto next = value.data[index++];
        if ((next & 0xC0) != 0x80) return false;
        codePoint = (codePoint << 6) | (next & 0x3F);
    }
    return codePoint >= minimum && codePoint <= 0x10FFFF &&
        !(codePoint >= 0xD800 && codePoint <= 0xDFFF);
}

bool unicodeWhitespace(uint32_t codePoint) {
    return codePoint == 0x0009 || codePoint == 0x000A || codePoint == 0x000B ||
        codePoint == 0x000C || codePoint == 0x000D || codePoint == 0x0020 ||
        codePoint == 0x0085 || codePoint == 0x00A0 || codePoint == 0x1680 ||
        (codePoint >= 0x2000 && codePoint <= 0x200A) || codePoint == 0x2028 ||
        codePoint == 0x2029 || codePoint == 0x202F || codePoint == 0x205F ||
        codePoint == 0x3000;
}

bool unicodeNewline(uint32_t codePoint) {
    return codePoint == 0x000A || codePoint == 0x000B || codePoint == 0x000C ||
        codePoint == 0x000D || codePoint == 0x0085 || codePoint == 0x2028 ||
        codePoint == 0x2029;
}

bool trim(LVProtocolSlice value, std::string &result) {
    if (!value.data) return value.size == 0;
    size_t index = 0;
    size_t start = value.size;
    size_t end = 0;
    while (index < value.size) {
        const auto position = index;
        uint32_t codePoint = 0;
        if (!readCodePoint(value, index, codePoint)) return false;
        if (!unicodeWhitespace(codePoint)) {
            if (start == value.size) start = position;
            end = index;
        }
    }
    if (start == value.size) result.clear();
    else result.assign(reinterpret_cast<const char *>(value.data + start), end - start);
    return true;
}

bool hasNewline(LVProtocolSlice value) {
    if (!value.data) return value.size != 0;
    size_t index = 0;
    while (index < value.size) {
        uint32_t codePoint = 0;
        if (!readCodePoint(value, index, codePoint)) return true;
        if (unicodeNewline(codePoint)) return true;
    }
    return false;
}

std::string stringFrom(LVProtocolSlice value) {
    if (!value.data || value.size == 0) return {};
    return std::string(reinterpret_cast<const char *>(value.data), value.size);
}

LVProtocolBytes bytesFrom(const std::string &value) {
    LVProtocolBytes bytes{};
    if (value.empty()) return bytes;
    bytes.data = static_cast<uint8_t *>(std::malloc(value.size()));
    if (!bytes.data) return bytes;
    bytes.size = value.size();
    std::memcpy(bytes.data, value.data(), value.size());
    return bytes;
}

LVProtocolRequest failedRequest(LVProtocolStatus status) {
    LVProtocolRequest request{};
    request.status = status;
    return request;
}

LVProtocolResult failedResult(LVProtocolStatus status, int httpStatus = 0) {
    LVProtocolResult result{};
    result.status = status;
    result.http_status = httpStatus;
    return result;
}

LVProtocolResult resultFrom(LVProtocolStatus status, const std::string &value, int httpStatus = 0) {
    auto result = failedResult(status, httpStatus);
    result.value = bytesFrom(value);
    if (!value.empty() && !result.value.data) return failedResult(LV_PROTOCOL_RESPONSE, httpStatus);
    return result;
}

std::string appendPath(LVProtocolSlice base, const char *path) {
    std::string value = stringFrom(base);
    while (!value.empty() && value.back() == '/') value.pop_back();
    value += path;
    return value;
}

bool withinFoundationDepth(const yyjson_val *root) {
    struct PendingValue {
        const yyjson_val *value;
        size_t depth;
    };
    std::vector<PendingValue> pending;
    if (root && (yyjson_is_arr(root) || yyjson_is_obj(root))) pending.push_back({root, 1});
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (current.depth > 512) return false;
        if (yyjson_is_arr(current.value)) {
            size_t index = 0;
            size_t maximum = 0;
            yyjson_val *item = nullptr;
            yyjson_arr_foreach(current.value, index, maximum, item) {
                if (yyjson_is_arr(item) || yyjson_is_obj(item)) pending.push_back({item, current.depth + 1});
            }
        } else if (yyjson_is_obj(current.value)) {
            size_t index = 0;
            size_t maximum = 0;
            yyjson_val *key = nullptr;
            yyjson_val *item = nullptr;
            yyjson_obj_foreach(current.value, index, maximum, key, item) {
                if (yyjson_is_arr(item) || yyjson_is_obj(item)) pending.push_back({item, current.depth + 1});
            }
        }
    }
    return true;
}

yyjson_doc *readJSON(LVProtocolSlice json) {
    if ((!json.data && json.size != 0) || hasNul(json)) return nullptr;
    auto *document = yyjson_read_opts(reinterpret_cast<char *>(const_cast<uint8_t *>(json.data)), json.size,
        YYJSON_READ_NOFLAG, nullptr, nullptr);
    if (!document || withinFoundationDepth(yyjson_doc_get_root(document))) return document;
    yyjson_doc_free(document);
    return nullptr;
}

bool readString(const yyjson_val *value, std::string &result) {
    if (!value || !yyjson_is_str(value)) return false;
    result.assign(yyjson_get_str(value), yyjson_get_len(value));
    return true;
}

bool readOptionalString(const yyjson_val *object, const char *key, std::optional<std::string> &result) {
    const auto value = yyjson_obj_get(object, key);
    if (!value || yyjson_is_null(value)) {
        result.reset();
        return true;
    }
    std::string text;
    if (!readString(value, text)) return false;
    result = std::move(text);
    return true;
}

bool readOptionalBool(const yyjson_val *object, const char *key, bool &result) {
    const auto value = yyjson_obj_get(object, key);
    if (!value || yyjson_is_null(value)) {
        result = false;
        return true;
    }
    if (!yyjson_is_bool(value)) return false;
    result = yyjson_get_bool(value);
    return true;
}

bool readOptionalLabels(const yyjson_val *object, bool &transcription, bool &chat) {
    transcription = false;
    chat = false;
    const auto value = yyjson_obj_get(object, "labels");
    if (!value || yyjson_is_null(value)) return true;
    if (!yyjson_is_arr(value)) return false;
    size_t index = 0;
    size_t maximum = 0;
    yyjson_val *item = nullptr;
    yyjson_arr_foreach(value, index, maximum, item) {
        std::string label;
        if (!readString(item, label)) return false;
        transcription = transcription || label == "transcription";
        chat = chat || label == "chat";
    }
    return true;
}

std::string redact(std::string value) {
    LVProtocolSlice input{reinterpret_cast<const uint8_t *>(value.data()), value.size()};
    size_t index = 0;
    size_t end = 0;
    size_t count = 0;
    while (index < input.size && count < 2000) {
        uint32_t codePoint = 0;
        if (!readCodePoint(input, index, codePoint)) {
            value.resize(end);
            break;
        }
        end = index;
        ++count;
    }
    if (index < input.size) value.resize(end);
    const auto insensitive = std::regex_constants::icase;
    value = std::regex_replace(value,
        std::regex(R"((authorization\s*[:=]\s*|bearer\s+)[^\s,;]+(?:\s+[^\s,;]+)?)", insensitive),
        "[redacted]");
    value = std::regex_replace(value,
        std::regex(R"(([\"']?(?:api[\s_-]*key|access[\s_-]*token|refresh[\s_-]*token|password|secret)[\"']?\s*[:=]\s*)[\"']?[^\s,;\"'}]+)", insensitive),
        "[redacted]");
    value = std::regex_replace(value, std::regex(R"(\b(?:sk-|fw-)[A-Za-z0-9_-]+)"), "[redacted]");
    value = std::regex_replace(value, std::regex(R"([A-Za-z0-9_+/=-]{32,})"), "[redacted]");
    value = std::regex_replace(value, std::regex(R"(https?://[^\s<>\"']+)"), "[redacted]");
    value.erase(std::remove(value.begin(), value.end(), '\0'), value.end());
    return value;
}

LVProtocolInventory failedInventory(LVProtocolStatus status) {
    LVProtocolInventory inventory{};
    inventory.status = status;
    return inventory;
}

struct ParsedModel {
    std::string id;
    uint8_t asr = 0;
    uint8_t cleanup = 0;
    uint8_t remote = 0;
};

LVProtocolSlice sliceFrom(const std::string &value) {
    return {value.empty() ? nullptr : reinterpret_cast<const uint8_t *>(value.data()), value.size()};
}

LVProtocolSlice sliceFrom(const std::vector<std::uint8_t> &value) {
    return {value.empty() ? nullptr : value.data(), value.size()};
}

std::string copiedString(const LVProtocolBytes &value) {
    if (!value.data || value.size == 0) return {};
    return {reinterpret_cast<const char *>(value.data), value.size};
}

std::vector<std::uint8_t> copiedBytes(const LVProtocolBytes &value) {
    if (!value.data || value.size == 0) return {};
    return {value.data, value.data + value.size};
}

LVEndpointFacts bridgeEndpoint(const lemonvox::ProtocolEndpointFacts &endpoint) {
    return {sliceFrom(endpoint.canonical_base_url), static_cast<uint8_t>(endpoint.scheme_allowed),
        static_cast<uint8_t>(endpoint.host_loopback), static_cast<uint8_t>(endpoint.no_userinfo),
        static_cast<uint8_t>(endpoint.no_query_fragment), static_cast<uint8_t>(endpoint.path_ends_v1)};
}

LVProtocolConfig bridgeConfig(const lemonvox::ProtocolConfiguration &config) {
    return {bridgeEndpoint(config.endpoint), sliceFrom(config.asr_model), sliceFrom(config.cleanup_model),
        sliceFrom(config.cleanup_prompt)};
}

LVProtocolStatus bridgeRequest(LVProtocolRequest raw, lemonvox::TransportRequest &request) {
    const auto status = raw.status;
    if (status == LV_PROTOCOL_OK) {
        lemonvox::TransportRequest next;
        next.method = raw.method == LV_PROTOCOL_POST ? lemonvox::HttpMethod::post : lemonvox::HttpMethod::get;
        next.url = copiedString(raw.url);
        next.body = copiedBytes(raw.body);
        if (raw.content_type.size != 0) next.headers.push_back({"Content-Type", copiedString(raw.content_type)});
        request = std::move(next);
    }
    lv_protocol_request_free(&raw);
    return status;
}

lemonvox::ProtocolValue bridgeValue(LVProtocolResult raw) {
    lemonvox::ProtocolValue value;
    value.status = raw.status;
    value.http_status = raw.http_status;
    value.value = copiedString(raw.value);
    lv_protocol_result_free(&raw);
    return value;
}

} // namespace

static_assert(std::is_standard_layout<LVProtocolSlice>::value, "LVProtocolSlice must remain C-layout");
static_assert(std::is_trivially_copyable<LVProtocolSlice>::value, "LVProtocolSlice must remain value-safe");
static_assert(std::is_standard_layout<LVEndpointFacts>::value, "LVEndpointFacts must remain C-layout");
static_assert(std::is_trivially_copyable<LVEndpointFacts>::value, "LVEndpointFacts must remain value-safe");
static_assert(std::is_standard_layout<LVProtocolConfig>::value, "LVProtocolConfig must remain C-layout");
static_assert(std::is_trivially_copyable<LVProtocolConfig>::value, "LVProtocolConfig must remain value-safe");

LVProtocolStatus lv_protocol_validate_endpoint(const LVEndpointFacts *endpoint) {
    if (!endpoint || !endpoint->canonical_base_url.data || endpoint->canonical_base_url.size == 0 ||
        hasNul(endpoint->canonical_base_url) || !endpoint->scheme_allowed || !endpoint->host_loopback ||
        !endpoint->no_userinfo || !endpoint->no_query_fragment || !endpoint->path_ends_v1) {
        return LV_PROTOCOL_CONFIGURATION;
    }
    return LV_PROTOCOL_OK;
}

LVProtocolStatus lv_protocol_validate_config(const LVProtocolConfig *config) {
    std::string prompt;
    if (!config || lv_protocol_validate_endpoint(&config->endpoint) != LV_PROTOCOL_OK ||
        !config->asr_model.data || !config->cleanup_model.data || hasNewline(config->asr_model) ||
        hasNewline(config->cleanup_model) ||
        config->asr_model.size == 0 || config->cleanup_model.size == 0 || !trim(config->cleanup_prompt, prompt) || prompt.empty()) {
        return LV_PROTOCOL_CONFIGURATION;
    }
    return LV_PROTOCOL_OK;
}

LVProtocolRequest lv_protocol_make_models_request(const LVProtocolConfig *config) {
    if (!config || lv_protocol_validate_endpoint(&config->endpoint) != LV_PROTOCOL_OK) {
        return failedRequest(LV_PROTOCOL_CONFIGURATION);
    }
    LVProtocolRequest request{};
    request.status = LV_PROTOCOL_OK;
    request.method = LV_PROTOCOL_GET;
    request.url = bytesFrom(appendPath(config->endpoint.canonical_base_url, "/models"));
    if (!request.url.data) return failedRequest(LV_PROTOCOL_RESPONSE);
    return request;
}

LVProtocolRequest lv_protocol_make_transcription_request(const LVProtocolConfig *config,
    LVProtocolSlice wav, LVProtocolSlice boundary) {
    if (lv_protocol_validate_config(config) != LV_PROTOCOL_OK) return failedRequest(LV_PROTOCOL_CONFIGURATION);
    if (!wav.data || wav.size == 0) return failedRequest(LV_PROTOCOL_RECORDING);
    if (!boundary.data || boundary.size == 0 || hasNul(boundary) || hasNewline(boundary)) {
        return failedRequest(LV_PROTOCOL_RESPONSE);
    }

    const auto boundaryText = stringFrom(boundary);
    std::string body;
    body += "--" + boundaryText + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n";
    body += stringFrom(config->asr_model);
    body += "\r\n--" + boundaryText + "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n";
    body += "--" + boundaryText + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"dictation.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    body.append(reinterpret_cast<const char *>(wav.data), wav.size);
    body += "\r\n--" + boundaryText + "--\r\n";

    LVProtocolRequest request{};
    request.status = LV_PROTOCOL_OK;
    request.method = LV_PROTOCOL_POST;
    request.url = bytesFrom(appendPath(config->endpoint.canonical_base_url, "/audio/transcriptions"));
    request.body = bytesFrom(body);
    request.content_type = bytesFrom("multipart/form-data; boundary=" + boundaryText);
    if (!request.url.data || !request.body.data || !request.content_type.data) {
        lv_protocol_request_free(&request);
        return failedRequest(LV_PROTOCOL_RESPONSE);
    }
    return request;
}

LVProtocolRequest lv_protocol_make_cleanup_request(const LVProtocolConfig *config, LVProtocolSlice transcript) {
    if (lv_protocol_validate_config(config) != LV_PROTOCOL_OK) return failedRequest(LV_PROTOCOL_CONFIGURATION);
    std::string ignored;
    if (!trim(transcript, ignored) || ignored.empty()) return failedRequest(LV_PROTOCOL_EMPTY_TRANSCRIPT);

    auto *document = yyjson_mut_doc_new(nullptr);
    if (!document) return failedRequest(LV_PROTOCOL_RESPONSE);
    auto *root = yyjson_mut_obj(document);
    auto *messages = yyjson_mut_obj_add_arr(document, root, "messages");
    auto *system = messages ? yyjson_mut_arr_add_obj(document, messages) : nullptr;
    auto *user = messages ? yyjson_mut_arr_add_obj(document, messages) : nullptr;
    const bool valid = root && messages && system && user &&
        yyjson_mut_obj_add_strncpy(document, root, "model",
            reinterpret_cast<const char *>(config->cleanup_model.data), config->cleanup_model.size) &&
        yyjson_mut_obj_add_bool(document, root, "stream", false) &&
        yyjson_mut_obj_add_int(document, root, "temperature", 0) &&
        yyjson_mut_obj_add_strncpy(document, system, "role", "system", 6) &&
        yyjson_mut_obj_add_strncpy(document, system, "content",
            reinterpret_cast<const char *>(config->cleanup_prompt.data), config->cleanup_prompt.size) &&
        yyjson_mut_obj_add_strncpy(document, user, "role", "user", 4) &&
        yyjson_mut_obj_add_strncpy(document, user, "content",
            reinterpret_cast<const char *>(transcript.data), transcript.size);
    yyjson_mut_doc_set_root(document, root);
    size_t size = 0;
    char *serialized = valid ? yyjson_mut_write(document, YYJSON_WRITE_NOFLAG, &size) : nullptr;
    LVProtocolRequest request{};
    request.status = LV_PROTOCOL_OK;
    request.method = LV_PROTOCOL_POST;
    request.url = bytesFrom(appendPath(config->endpoint.canonical_base_url, "/chat/completions"));
    request.content_type = bytesFrom("application/json");
    if (serialized) request.body = bytesFrom(std::string(serialized, size));
    std::free(serialized);
    yyjson_mut_doc_free(document);
    if (!request.url.data || !request.content_type.data || !request.body.data) {
        lv_protocol_request_free(&request);
        return failedRequest(LV_PROTOCOL_RESPONSE);
    }
    return request;
}

static LVProtocolInventory parseInventory(LVProtocolSlice json, bool includeMetadata) {
    auto *document = readJSON(json);
    if (!document) return failedInventory(LV_PROTOCOL_RESPONSE);
    const auto *root = yyjson_doc_get_root(document);
    const auto *data = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "data") : nullptr;
    if (!data || !yyjson_is_arr(data)) {
        yyjson_doc_free(document);
        return failedInventory(LV_PROTOCOL_RESPONSE);
    }

    std::optional<std::string> reason;
    std::optional<std::string> message;
    if (includeMetadata && (!readOptionalString(root, "reason", reason) ||
        !readOptionalString(root, "message", message))) {
        yyjson_doc_free(document);
        return failedInventory(LV_PROTOCOL_RESPONSE);
    }
    std::vector<ParsedModel> models;
    size_t index = 0;
    size_t maximum = 0;
    yyjson_val *item = nullptr;
    yyjson_arr_foreach(data, index, maximum, item) {
        if (!yyjson_is_obj(item)) {
            yyjson_doc_free(document);
            return failedInventory(LV_PROTOCOL_RESPONSE);
        }
        std::string id;
        if (!readString(yyjson_obj_get(item, "id"), id)) {
            yyjson_doc_free(document);
            return failedInventory(LV_PROTOCOL_RESPONSE);
        }
        bool downloaded = false;
        bool hasTranscription = false;
        bool hasChat = false;
        std::optional<std::string> recipe;
        std::optional<std::string> cloudProvider;
        if (!readOptionalBool(item, "downloaded", downloaded) || !readOptionalString(item, "recipe", recipe) ||
            !readOptionalString(item, "cloud_provider", cloudProvider) ||
            !readOptionalLabels(item, hasTranscription, hasChat)) {
            yyjson_doc_free(document);
            return failedInventory(LV_PROTOCOL_RESPONSE);
        }
        LVModelFacts facts{};
        facts.downloaded = downloaded;
        facts.has_cloud_provider = cloudProvider.has_value();
        facts.is_cloud_recipe = recipe && *recipe == "cloud";
        facts.is_whispercpp_recipe = recipe && *recipe == "whispercpp";
        facts.is_moonshine_recipe = recipe && *recipe == "moonshine";
        facts.is_llamacpp_recipe = recipe && *recipe == "llamacpp";
        facts.is_flm_recipe = recipe && *recipe == "flm";
        facts.is_ryzenai_llm_recipe = recipe && *recipe == "ryzenai-llm";
        facts.has_transcription_label = hasTranscription;
        facts.has_chat_label = hasChat;
        const auto asr = static_cast<uint8_t>(lv_model_supports(&facts, LV_MODEL_ASR));
        const auto cleanup = static_cast<uint8_t>(lv_model_supports(&facts, LV_MODEL_CLEANUP));
        if (asr || cleanup) models.push_back({std::move(id), asr, cleanup, static_cast<uint8_t>(cloudProvider.has_value())});
    }
    std::sort(models.begin(), models.end(), [](const ParsedModel &left, const ParsedModel &right) {
        return left.id < right.id;
    });
    LVProtocolInventory inventory{};
    inventory.status = LV_PROTOCOL_OK;
    inventory.count = models.size();
    if (reason || message) {
        const auto &selectedReason = reason ? *reason : *message;
        inventory.reason = bytesFrom(redact(selectedReason));
        inventory.has_reason = 1;
    }
    if (inventory.count) inventory.models = static_cast<LVProtocolModel *>(std::calloc(inventory.count, sizeof(LVProtocolModel)));
    if ((inventory.count && !inventory.models) || (inventory.has_reason && !inventory.reason.data && !(reason ? reason->empty() : message->empty()))) {
        lv_protocol_inventory_free(&inventory);
        yyjson_doc_free(document);
        return failedInventory(LV_PROTOCOL_RESPONSE);
    }
    for (size_t modelIndex = 0; modelIndex < inventory.count; ++modelIndex) {
        inventory.models[modelIndex].id = bytesFrom(models[modelIndex].id);
        inventory.models[modelIndex].asr = models[modelIndex].asr;
        inventory.models[modelIndex].cleanup = models[modelIndex].cleanup;
        inventory.models[modelIndex].remote = models[modelIndex].remote;
        if (!inventory.models[modelIndex].id.data && !models[modelIndex].id.empty()) {
            lv_protocol_inventory_free(&inventory);
            yyjson_doc_free(document);
            return failedInventory(LV_PROTOCOL_RESPONSE);
        }
    }
    yyjson_doc_free(document);
    return inventory;
}

LVProtocolInventory lv_protocol_parse_inventory(LVProtocolSlice json) {
    return parseInventory(json, true);
}

LVProtocolStatus lv_protocol_validate_models(LVProtocolSlice json, const LVProtocolConfig *config) {
    if (lv_protocol_validate_config(config) != LV_PROTOCOL_OK) return LV_PROTOCOL_CONFIGURATION;
    auto inventory = parseInventory(json, false);
    if (inventory.status != LV_PROTOCOL_OK) return inventory.status;
    const auto asr = stringFrom(config->asr_model);
    const auto cleanup = stringFrom(config->cleanup_model);
    bool hasASR = false;
    bool hasCleanup = false;
    for (size_t index = 0; index < inventory.count; ++index) {
        const auto id = stringFrom({inventory.models[index].id.data, inventory.models[index].id.size});
        hasASR = hasASR || (id == asr && inventory.models[index].asr);
        hasCleanup = hasCleanup || (id == cleanup && inventory.models[index].cleanup);
    }
    lv_protocol_inventory_free(&inventory);
    return hasASR && hasCleanup ? LV_PROTOCOL_OK : LV_PROTOCOL_MODEL_UNAVAILABLE;
}

LVProtocolResult lv_protocol_parse_transcription(LVProtocolSlice json) {
    auto *document = readJSON(json);
    if (!document) return failedResult(LV_PROTOCOL_RESPONSE);
    const auto *root = yyjson_doc_get_root(document);
    std::string text;
    const bool valid = root && yyjson_is_obj(root) && readString(yyjson_obj_get(root, "text"), text);
    yyjson_doc_free(document);
    if (!valid) return failedResult(LV_PROTOCOL_RESPONSE);
    std::string trimmed;
    if (!trim({reinterpret_cast<const uint8_t *>(text.data()), text.size()}, trimmed)) return failedResult(LV_PROTOCOL_RESPONSE);
    if (trimmed.empty()) return failedResult(LV_PROTOCOL_EMPTY_TRANSCRIPT);
    return resultFrom(LV_PROTOCOL_OK, text);
}

LVProtocolResult lv_protocol_parse_cleanup(LVProtocolSlice json) {
    auto *document = readJSON(json);
    if (!document) return failedResult(LV_PROTOCOL_RESPONSE);
    const auto *root = yyjson_doc_get_root(document);
    const auto *choices = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "choices") : nullptr;
    bool valid = choices && yyjson_is_arr(choices) && yyjson_arr_size(choices) != 0;
    std::string finish;
    std::string content;
    if (valid) {
        size_t index = 0;
        size_t maximum = 0;
        yyjson_val *choice = nullptr;
        yyjson_arr_foreach(choices, index, maximum, choice) {
            const auto *message = yyjson_is_obj(choice) ? yyjson_obj_get(choice, "message") : nullptr;
            std::string choiceContent;
            std::optional<std::string> choiceFinish;
            if (!message || !yyjson_is_obj(message) || !readString(yyjson_obj_get(message, "content"), choiceContent) ||
                !readOptionalString(choice, "finish_reason", choiceFinish)) {
                valid = false;
                break;
            }
            if (index == 0) {
                content = std::move(choiceContent);
                if (choiceFinish) finish = std::move(*choiceFinish);
            }
        }
    }
    yyjson_doc_free(document);
    if (!valid || finish != "stop") return failedResult(LV_PROTOCOL_RESPONSE);
    std::string trimmed;
    if (!trim({reinterpret_cast<const uint8_t *>(content.data()), content.size()}, trimmed)) return failedResult(LV_PROTOCOL_RESPONSE);
    if (trimmed.empty()) return failedResult(LV_PROTOCOL_EMPTY_CLEANUP);
    return resultFrom(LV_PROTOCOL_OK, trimmed);
}

LVProtocolResult lv_protocol_parse_version(LVProtocolSlice json) {
    auto *document = readJSON(json);
    if (!document) return failedResult(LV_PROTOCOL_RESPONSE);
    const auto *root = yyjson_doc_get_root(document);
    std::string version;
    const bool valid = root && yyjson_is_obj(root) && readString(yyjson_obj_get(root, "version"), version);
    yyjson_doc_free(document);
    if (!valid) return failedResult(LV_PROTOCOL_RESPONSE);
    return resultFrom(LV_PROTOCOL_OK, redact(version));
}

LVProtocolResult lv_protocol_classify_http(int status, LVProtocolSlice body) {
    if (status >= 200 && status < 300) return failedResult(LV_PROTOCOL_OK, status);
    auto *document = readJSON(body);
    if (!document) return failedResult(LV_PROTOCOL_HTTP, status);
    const auto *root = yyjson_doc_get_root(document);
    const yyjson_val *detail = nullptr;
    if (root && yyjson_is_obj(root)) {
        const auto *error = yyjson_obj_get(root, "error");
        if (error && yyjson_is_obj(error)) {
            const auto *message = yyjson_obj_get(error, "message");
            if (message && yyjson_is_str(message)) detail = message;
        }
        if (!detail) {
            const auto *message = yyjson_obj_get(root, "message");
            if (message && yyjson_is_str(message)) detail = message;
        }
        if (!detail) {
            const auto *topError = yyjson_obj_get(root, "error");
            if (topError && yyjson_is_str(topError)) detail = topError;
        }
    }
    std::string message;
    const bool valid = readString(detail, message);
    yyjson_doc_free(document);
    if (!valid) return failedResult(LV_PROTOCOL_HTTP, status);
    return resultFrom(LV_PROTOCOL_UPSTREAM, redact(message), status);
}

int lv_protocol_json_valid(LVProtocolSlice json) {
    auto *document = readJSON(json);
    if (!document) return 0;
    yyjson_doc_free(document);
    return 1;
}

void lv_protocol_bytes_free(LVProtocolBytes *bytes) {
    if (!bytes) return;
    std::free(bytes->data);
    bytes->data = nullptr;
    bytes->size = 0;
}

void lv_protocol_request_free(LVProtocolRequest *request) {
    if (!request) return;
    lv_protocol_bytes_free(&request->url);
    lv_protocol_bytes_free(&request->body);
    lv_protocol_bytes_free(&request->content_type);
    request->status = LV_PROTOCOL_RESPONSE;
    request->method = LV_PROTOCOL_GET;
}

void lv_protocol_result_free(LVProtocolResult *result) {
    if (!result) return;
    lv_protocol_bytes_free(&result->value);
    result->status = LV_PROTOCOL_RESPONSE;
    result->http_status = 0;
}

void lv_protocol_inventory_free(LVProtocolInventory *inventory) {
    if (!inventory) return;
    for (size_t index = 0; index < inventory->count; ++index) lv_protocol_bytes_free(&inventory->models[index].id);
    std::free(inventory->models);
    inventory->models = nullptr;
    inventory->count = 0;
    lv_protocol_bytes_free(&inventory->reason);
    inventory->has_reason = 0;
    inventory->status = LV_PROTOCOL_RESPONSE;
}

namespace lemonvox {

LVProtocolStatus Protocol::validate_endpoint(const ProtocolEndpointFacts &endpoint) {
    const auto bridge = bridgeEndpoint(endpoint);
    return lv_protocol_validate_endpoint(&bridge);
}

LVProtocolStatus Protocol::validate_config(const ProtocolConfiguration &config) {
    const auto bridge = bridgeConfig(config);
    return lv_protocol_validate_config(&bridge);
}

LVProtocolStatus Protocol::make_models_request(const ProtocolConfiguration &config, TransportRequest &request) {
    const auto bridge = bridgeConfig(config);
    return bridgeRequest(lv_protocol_make_models_request(&bridge), request);
}

LVProtocolStatus Protocol::make_transcription_request(const ProtocolConfiguration &config,
    const std::vector<std::uint8_t> &wav, const std::string &boundary, TransportRequest &request) {
    const auto bridge = bridgeConfig(config);
    return bridgeRequest(lv_protocol_make_transcription_request(&bridge, sliceFrom(wav), sliceFrom(boundary)), request);
}

LVProtocolStatus Protocol::make_cleanup_request(const ProtocolConfiguration &config,
    const std::string &transcript, TransportRequest &request) {
    const auto bridge = bridgeConfig(config);
    return bridgeRequest(lv_protocol_make_cleanup_request(&bridge, sliceFrom(transcript)), request);
}

ProtocolValue Protocol::parse_transcription(const std::vector<std::uint8_t> &json) {
    return bridgeValue(lv_protocol_parse_transcription(sliceFrom(json)));
}

ProtocolValue Protocol::parse_cleanup(const std::vector<std::uint8_t> &json) {
    return bridgeValue(lv_protocol_parse_cleanup(sliceFrom(json)));
}

ProtocolValue Protocol::parse_version(const std::vector<std::uint8_t> &json) {
    return bridgeValue(lv_protocol_parse_version(sliceFrom(json)));
}

ProtocolValue Protocol::classify_http(int status, const std::vector<std::uint8_t> &body) {
    return bridgeValue(lv_protocol_classify_http(status, sliceFrom(body)));
}

LVProtocolStatus Protocol::parse_inventory(const std::vector<std::uint8_t> &json, ProtocolInventory &inventory) {
    inventory = {};
    auto raw = lv_protocol_parse_inventory(sliceFrom(json));
    const auto status = raw.status;
    if (status == LV_PROTOCOL_OK) {
        inventory.has_reason = raw.has_reason != 0;
        inventory.reason = copiedString(raw.reason);
        inventory.models.reserve(raw.count);
        for (size_t index = 0; index < raw.count; ++index) {
            inventory.models.push_back({copiedString(raw.models[index].id), raw.models[index].asr != 0,
                raw.models[index].cleanup != 0, raw.models[index].remote != 0});
        }
    }
    lv_protocol_inventory_free(&raw);
    return status;
}

LVProtocolStatus Protocol::validate_models(const std::vector<std::uint8_t> &json,
    const ProtocolConfiguration &config) {
    const auto bridge = bridgeConfig(config);
    return lv_protocol_validate_models(sliceFrom(json), &bridge);
}

} // namespace lemonvox
