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
    uint8_t host_loopback; /* Informational; retained for bridge compatibility. */
    uint8_t no_userinfo;
    uint8_t no_query_fragment;
    uint8_t path_ends_v1;
} LVEndpointFacts;

typedef enum { LV_CLEANUP_BACKEND_GENERIC = 0, LV_CLEANUP_BACKEND_LEMONADE = 1 } LVCleanupBackend;
typedef enum { LV_CLEANUP_THINKING_DISABLED = 0, LV_CLEANUP_THINKING_SERVER_DEFAULT = 1 } LVCleanupThinking;

/* Static storage; native defaults use the same preservation requirements. */
const char *lv_cleanup_default_prompt(void);

typedef struct LVProtocolConfig {
    LVEndpointFacts endpoint;
    LVProtocolSlice asr_model;
    LVProtocolSlice cleanup_model;
    LVProtocolSlice cleanup_prompt;
#ifdef __cplusplus
    LVCleanupBackend cleanup_backend = LV_CLEANUP_BACKEND_GENERIC;
    LVCleanupThinking cleanup_thinking = LV_CLEANUP_THINKING_DISABLED;
    uint32_t cleanup_context_tokens = 0; /* 0: backend context unknown. */
#else
    LVCleanupBackend cleanup_backend;
    LVCleanupThinking cleanup_thinking;
    uint32_t cleanup_context_tokens;
#endif
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
    uint8_t downloaded;
    uint8_t loaded;
    uint8_t loaded_known;
    double size_gb;
} LVProtocolModel;

typedef struct {
    LVProtocolStatus status;
    LVProtocolModel *models;
    size_t count;
    LVProtocolBytes reason;
    uint8_t has_reason;
} LVProtocolInventory;

/* Serialized by the native UI executor. Times are monotonic seconds. The
 * platform owns GET /models, a 3-second transport timeout and cancellation.
 * Connected means valid inventory API response, not available inference models.
 * begin invalidates a timed-out active_request and returns 0; the native owner
 * must cancel that transport before asking begin again. Poll every second. */
typedef enum {
    LV_SERVER_HEALTH_CHECKING,
    LV_SERVER_HEALTH_CONNECTED,
    LV_SERVER_HEALTH_DISCONNECTED,
} LVServerHealthStatus;

typedef struct {
    LVServerHealthStatus status;
    uint64_t next_request;
    uint64_t active_request;
    double request_started_at;
    double next_check_at;
} LVServerHealth;

void lv_server_health_init(LVServerHealth *health);
/* Call when the saved endpoint changes; cancel native work first. */
void lv_server_health_reset(LVServerHealth *health);
uint64_t lv_server_health_begin(LVServerHealth *health, double monotonic_seconds);
/* Returns 1 if this completion belonged to the current request, including
 * failure. Returns 0 for stale/duplicate completions. Borrows body synchronously. */
uint8_t lv_server_health_complete(LVServerHealth *health, uint64_t request,
    double monotonic_seconds, uint8_t transport_ok, int http_status, LVProtocolSlice body);
/* Shared setup policy. Catalog parsing includes uninstalled and incompatible entries;
 * normal inventory parsing remains restricted to inference-ready entries. */
typedef enum {
    LV_SETUP_NO_SELECTION, LV_SETUP_NOT_INSTALLED, LV_SETUP_INSTALLED_NOT_LOADED,
    LV_SETUP_READY, LV_SETUP_SERVER_UNAVAILABLE, LV_SETUP_INCOMPATIBLE,
    LV_SETUP_EXTERNAL_SERVER, LV_SETUP_INSTALLING, LV_SETUP_CANCELED, LV_SETUP_FAILED
} LVModelSetupState;

typedef struct {
    LVModelSetupState state;
    uint8_t can_install;
    LVProtocolBytes message;
} LVModelSetupInfo;

typedef struct {
    LVProtocolStatus status;
    double percent; /* -1 when server supplies no numerical progress. */
    uint8_t complete;
    uint8_t failed;
    uint8_t canceled;
    uint8_t running;
    LVProtocolBytes message;
} LVModelInstallProgress;

LVProtocolRequest lv_protocol_make_catalog_request(const LVProtocolConfig *config);
LVProtocolRequest lv_protocol_make_downloads_request(const LVProtocolConfig *config);
LVProtocolRequest lv_protocol_make_cancel_install_request(const LVProtocolConfig *config,
    const LVProtocolInventory *catalog, LVProtocolSlice model_id, LVModelPurpose purpose,
    uint8_t management_supported);
LVProtocolInventory lv_protocol_parse_catalog(LVProtocolSlice json);
LVProtocolStatus lv_protocol_catalog_apply_health(LVProtocolInventory *catalog, LVProtocolSlice health_json);
LVModelSetupInfo lv_protocol_model_setup(const LVProtocolInventory *catalog,
    LVProtocolSlice model_id, LVModelPurpose purpose, uint8_t server_available,
    uint8_t management_supported);
LVProtocolRequest lv_protocol_make_install_request(const LVProtocolConfig *config,
    const LVProtocolInventory *catalog, LVProtocolSlice model_id, LVModelPurpose purpose,
    uint8_t management_supported, uint8_t explicit_consent);
/* A supported Lemonade version is required in addition to a compatible catalog. */
int lv_protocol_supports_model_management(LVProtocolSlice version_json);
LVModelInstallProgress lv_protocol_parse_install_progress(LVProtocolSlice bytes);
LVModelInstallProgress lv_protocol_parse_install_progress_for_model(LVProtocolSlice bytes,
    LVProtocolSlice model_id);
void lv_protocol_model_setup_free(LVModelSetupInfo *info);
void lv_protocol_install_progress_free(LVModelInstallProgress *progress);

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

/* Privacy-safe, fixed-shape telemetry. Unknown values are marked absent;
 * finish_reason never contains arbitrary upstream text. No returned storage. */
typedef enum {
    LV_CLEANUP_FINISH_UNKNOWN, LV_CLEANUP_FINISH_STOP, LV_CLEANUP_FINISH_LENGTH,
    LV_CLEANUP_FINISH_OTHER,
} LVCleanupFinish;
typedef struct {
    LVProtocolStatus status;
    LVCleanupFinish finish_reason;
    uint64_t prompt_tokens;
    uint64_t completion_tokens;
    uint64_t reasoning_tokens;
    uint64_t reasoning_characters;
    uint8_t has_prompt_tokens;
    uint8_t has_completion_tokens;
    uint8_t has_reasoning_tokens;
    uint8_t has_reasoning_characters;
} LVCleanupMetrics;
LVCleanupMetrics lv_protocol_cleanup_metrics(LVProtocolSlice json);
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
    bool host_loopback = false; // Informational; remote endpoints are supported.
    bool no_userinfo = false;
    bool no_query_fragment = false;
    bool path_ends_v1 = false;
};

struct ProtocolConfiguration {
    ProtocolEndpointFacts endpoint;
    std::string asr_model;
    std::string cleanup_model;
    std::string cleanup_prompt;
    LVCleanupBackend cleanup_backend = LV_CLEANUP_BACKEND_GENERIC;
    LVCleanupThinking cleanup_thinking = LV_CLEANUP_THINKING_DISABLED;
    uint32_t cleanup_context_tokens = 0;
};

struct ProtocolModel {
    std::string id;
    bool asr = false;
    bool cleanup = false;
    bool remote = false;
    bool downloaded = false;
    bool loaded = false;
    bool loaded_known = false;
    double size_gb = 0;
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

struct ModelSetupInfo {
    LVModelSetupState state = LV_SETUP_SERVER_UNAVAILABLE;
    bool can_install = false;
    std::string message;
};
struct ModelInstallProgress {
    LVProtocolStatus status = LV_PROTOCOL_RESPONSE;
    double percent = -1;
    bool complete = false;
    bool failed = false;
    bool canceled = false;
    bool running = false;
    std::string message;
};

class Protocol {
public:
    static LVProtocolStatus make_catalog_request(const ProtocolConfiguration &config, TransportRequest &request);
    static LVProtocolStatus make_downloads_request(const ProtocolConfiguration &config, TransportRequest &request);
    static LVProtocolStatus make_cancel_install_request(const ProtocolConfiguration &config,
        const ProtocolInventory &catalog, const std::string &model_id, LVModelPurpose purpose,
        bool management_supported, TransportRequest &request);
    static LVProtocolStatus parse_catalog(const std::vector<std::uint8_t> &json, ProtocolInventory &inventory);
    static LVProtocolStatus catalog_apply_health(ProtocolInventory &catalog, const std::vector<std::uint8_t> &health_json);
    static ModelSetupInfo model_setup(const ProtocolInventory &catalog, const std::string &model_id,
        LVModelPurpose purpose, bool server_available, bool management_supported);
    static LVProtocolStatus make_install_request(const ProtocolConfiguration &config,
        const ProtocolInventory &catalog, const std::string &model_id, LVModelPurpose purpose,
        bool management_supported, bool explicit_consent, TransportRequest &request);
    static bool supports_model_management(const std::vector<std::uint8_t> &version_json);
    static ModelInstallProgress parse_install_progress(const std::vector<std::uint8_t> &bytes);
    static ModelInstallProgress parse_install_progress_for_model(const std::vector<std::uint8_t> &bytes,
        const std::string &model_id);
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
