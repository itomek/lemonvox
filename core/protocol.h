#pragma once

#include <stddef.h>
#include <stdint.h>

#include "session.h"

/*
 * Swift bridge ownership:
 * - LVProtocolSlice is borrowed only for the synchronous call. A null data
 *   pointer is valid only with size == 0.
 * - LVProtocolBytes, LVProtocolRequest, LVProtocolResult, and
 *   LVProtocolInventory are core-owned return values. Release them with the
 *   matching lv_protocol_*_free function once Swift has copied the bytes.
 * - This bridge neither retains input storage nor accepts callbacks.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LV_PROTOCOL_OK,
    LV_PROTOCOL_CONFIGURATION,
    LV_PROTOCOL_MODEL_UNAVAILABLE,
    LV_PROTOCOL_RESPONSE,
    LV_PROTOCOL_EMPTY_TRANSCRIPT,
    LV_PROTOCOL_EMPTY_CLEANUP,
    LV_PROTOCOL_HTTP,
    LV_PROTOCOL_UPSTREAM,
    LV_PROTOCOL_RECORDING,
} LVProtocolStatus;

typedef struct {
    const uint8_t *data;
    size_t size;
} LVProtocolSlice;

typedef struct {
    /* URLComponents supplies this canonical URL and all normalized facts. */
    LVProtocolSlice canonical_base_url;
    uint8_t scheme_allowed;
    uint8_t host_loopback;
    uint8_t no_userinfo;
    uint8_t no_query_fragment;
    uint8_t path_ends_v1;
} LVEndpointFacts;

typedef struct {
    LVEndpointFacts endpoint;
    LVProtocolSlice asr_model;
    LVProtocolSlice cleanup_model;
    LVProtocolSlice cleanup_prompt;
} LVProtocolConfig;

typedef struct {
    uint8_t *data;
    size_t size;
} LVProtocolBytes;

typedef enum { LV_PROTOCOL_GET, LV_PROTOCOL_POST } LVProtocolMethod;

typedef struct {
    LVProtocolStatus status;
    LVProtocolMethod method;
    LVProtocolBytes url;
    LVProtocolBytes body;
    LVProtocolBytes content_type;
} LVProtocolRequest;

typedef struct {
    LVProtocolStatus status;
    int http_status;
    LVProtocolBytes value;
} LVProtocolResult;

typedef struct {
    LVProtocolBytes id;
    uint8_t asr;
    uint8_t cleanup;
    uint8_t remote;
} LVProtocolModel;

typedef struct {
    LVProtocolStatus status;
    LVProtocolModel *models;
    size_t count;
    LVProtocolBytes reason;
    uint8_t has_reason;
} LVProtocolInventory;

LVProtocolStatus lv_protocol_validate_endpoint(const LVEndpointFacts *endpoint);
LVProtocolStatus lv_protocol_validate_config(const LVProtocolConfig *config);
/* Uses endpoint facts only so Settings can discover models before selections exist. */
LVProtocolRequest lv_protocol_make_models_request(const LVProtocolConfig *config);
LVProtocolRequest lv_protocol_make_transcription_request(const LVProtocolConfig *config,
    LVProtocolSlice wav, LVProtocolSlice boundary);
LVProtocolRequest lv_protocol_make_cleanup_request(const LVProtocolConfig *config,
    LVProtocolSlice transcript);
LVProtocolInventory lv_protocol_parse_inventory(LVProtocolSlice json);
LVProtocolStatus lv_protocol_validate_models(LVProtocolSlice json, const LVProtocolConfig *config);
LVProtocolResult lv_protocol_parse_transcription(LVProtocolSlice json);
LVProtocolResult lv_protocol_parse_cleanup(LVProtocolSlice json);
LVProtocolResult lv_protocol_parse_version(LVProtocolSlice json);
LVProtocolResult lv_protocol_classify_http(int status, LVProtocolSlice body);
int lv_protocol_json_valid(LVProtocolSlice json);
void lv_protocol_request_free(LVProtocolRequest *request);
void lv_protocol_bytes_free(LVProtocolBytes *bytes);
void lv_protocol_result_free(LVProtocolResult *result);
void lv_protocol_inventory_free(LVProtocolInventory *inventory);

#ifdef __cplusplus
}

#include "adapters.h"

#include <string>
#include <vector>

namespace lemonvox {

/*
 * Direct C++ facade for workflow cores. It converts the narrow C bridge into
 * the owned values used by ByteTransport; it contains no platform URL or HTTP
 * implementation. Native shells provide normalized endpoint facts, then own
 * dispatch, timeouts, cancellation, and byte delivery through ByteTransport.
 */
struct ProtocolEndpointFacts {
    std::string canonical_base_url;
    bool scheme_allowed = false;
    bool host_loopback = false;
    bool no_userinfo = false;
    bool no_query_fragment = false;
    bool path_ends_v1 = false;
};

struct ProtocolConfiguration {
    ProtocolEndpointFacts endpoint;
    std::string asr_model;
    std::string cleanup_model;
    std::string cleanup_prompt;
};

struct ProtocolModel {
    std::string id;
    bool asr = false;
    bool cleanup = false;
    bool remote = false;
};

struct ProtocolInventory {
    std::vector<ProtocolModel> models;
    std::string reason;
    bool has_reason = false;
};

struct ProtocolValue {
    LVProtocolStatus status = LV_PROTOCOL_RESPONSE;
    int http_status = 0;
    std::string value;
};

class Protocol {
public:
    static LVProtocolStatus validate_endpoint(const ProtocolEndpointFacts &endpoint);
    static LVProtocolStatus validate_config(const ProtocolConfiguration &config);
    static LVProtocolStatus make_models_request(const ProtocolConfiguration &config, TransportRequest &request);
    static LVProtocolStatus make_transcription_request(const ProtocolConfiguration &config,
        const std::vector<std::uint8_t> &wav, const std::string &boundary, TransportRequest &request);
    static LVProtocolStatus make_cleanup_request(const ProtocolConfiguration &config,
        const std::string &transcript, TransportRequest &request);
    static ProtocolValue parse_transcription(const std::vector<std::uint8_t> &json);
    static ProtocolValue parse_cleanup(const std::vector<std::uint8_t> &json);
    static ProtocolValue parse_version(const std::vector<std::uint8_t> &json);
    static ProtocolValue classify_http(int status, const std::vector<std::uint8_t> &body);
    static LVProtocolStatus parse_inventory(const std::vector<std::uint8_t> &json, ProtocolInventory &inventory);
    static LVProtocolStatus validate_models(const std::vector<std::uint8_t> &json,
        const ProtocolConfiguration &config);
};

} // namespace lemonvox
#endif
