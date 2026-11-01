#pragma once

#include "protocol.h"

/*
 * Narrow C ABI for a native serialized workflow executor.
 *
 * All LVProtocolSlice inputs and callback views are borrowed only for the
 * synchronous call/callback in which they appear. Native callers must copy
 * bytes they retain. lv_workflow_start() copies its configuration before it
 * returns. The opaque LVWorkflow handle owns no native context.
 *
 * A native request callback receives both an operation ID and a bridge request
 * ID. Its completion must echo both values. The bridge accepts each request at
 * most once, rejects stale or out-of-order completions, and clears requests
 * before forwarding cancel/shutdown to native code.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct LVWorkflow LVWorkflow;

typedef enum {
    LV_WORKFLOW_ADAPTER_NONE,
    LV_WORKFLOW_ADAPTER_CANCELED,
    LV_WORKFLOW_ADAPTER_UNAVAILABLE,
    LV_WORKFLOW_ADAPTER_INVALID_INPUT,
    LV_WORKFLOW_ADAPTER_NETWORK,
    LV_WORKFLOW_ADAPTER_RESPONSE,
    LV_WORKFLOW_ADAPTER_TARGET_CHANGED,
    LV_WORKFLOW_ADAPTER_INSERTION_UNAVAILABLE,
} LVWorkflowAdapterError;

typedef enum {
    LV_WORKFLOW_METHOD_GET,
    LV_WORKFLOW_METHOD_POST,
} LVWorkflowMethod;

typedef enum {
    LV_WORKFLOW_PHASE_IDLE,
    LV_WORKFLOW_PHASE_PREPARING,
    LV_WORKFLOW_PHASE_RECORDING,
    LV_WORKFLOW_PHASE_TRANSCRIBING,
    LV_WORKFLOW_PHASE_CLEANING,
    LV_WORKFLOW_PHASE_INSERTING,
} LVWorkflowPhase;

typedef enum {
    LV_WORKFLOW_FAILURE_ADAPTER,
    LV_WORKFLOW_FAILURE_PROTOCOL,
} LVWorkflowFailureSource;

typedef enum {
    LV_WORKFLOW_OUTCOME_INSERTED,
    LV_WORKFLOW_OUTCOME_COPY_FALLBACK,
} LVWorkflowOutcomeKind;

typedef enum {
    LV_WORKFLOW_FALLBACK_NONE,
    LV_WORKFLOW_FALLBACK_ACCESSIBILITY_UNAVAILABLE,
    LV_WORKFLOW_FALLBACK_TARGET_UNAVAILABLE,
    LV_WORKFLOW_FALLBACK_TARGET_CHANGED,
    LV_WORKFLOW_FALLBACK_SECURE_FIELD,
    LV_WORKFLOW_FALLBACK_UNSUPPORTED_FIELD,
    LV_WORKFLOW_FALLBACK_PASTE_UNAVAILABLE,
} LVWorkflowFallbackReason;

typedef enum {
    LV_WORKFLOW_DIAGNOSTIC_RECORDING_STARTED,
    LV_WORKFLOW_DIAGNOSTIC_TRANSCRIPTION_STARTED,
    LV_WORKFLOW_DIAGNOSTIC_CLEANUP_STARTED,
    LV_WORKFLOW_DIAGNOSTIC_INSERTION_FALLBACK,
    LV_WORKFLOW_DIAGNOSTIC_TRANSPORT_FAILED,
    LV_WORKFLOW_DIAGNOSTIC_CANCELED,
    LV_WORKFLOW_DIAGNOSTIC_INSERTION_SUCCEEDED,
    LV_WORKFLOW_DIAGNOSTIC_DICTATION_FAILED,
} LVWorkflowDiagnosticEvent;

typedef struct {
    LVEndpointFacts endpoint;
    LVProtocolSlice preference_endpoint;
    LVProtocolSlice asr_model;
    LVProtocolSlice cleanup_model;
    LVProtocolSlice cleanup_prompt;
    uint8_t insert_current_target;
} LVWorkflowConfig;

typedef struct {
    uint64_t token;
    uint8_t editable;
    uint8_t secure;
    uint8_t frontmost;
    uint8_t matches_original;
} LVWorkflowTarget;

typedef struct {
    LVWorkflowMethod method;
    LVProtocolSlice url;
    LVProtocolSlice body;
    LVProtocolSlice content_type;
    uint32_t timeout_ms;
} LVWorkflowTransportRequest;

typedef struct {
    LVWorkflowFailureSource source;
    LVWorkflowAdapterError adapter_error;
    LVProtocolStatus protocol_status;
    int http_status;
    LVProtocolSlice safe_detail;
} LVWorkflowFailure;

typedef struct {
    LVWorkflowOutcomeKind kind;
    LVWorkflowFallbackReason fallback_reason;
} LVWorkflowOutcome;

typedef void (*LVWorkflowCaptureOriginalRequest)(void *context, LVWorkflow *workflow,
    uint64_t operation, uint64_t request_id);
typedef void (*LVWorkflowCaptureStartRequest)(void *context, LVWorkflow *workflow,
    uint64_t operation, uint64_t request_id);
typedef void (*LVWorkflowCaptureStopRequest)(void *context, LVWorkflow *workflow,
    uint64_t operation, uint64_t request_id);
typedef void (*LVWorkflowTransportSendRequest)(void *context, LVWorkflow *workflow,
    uint64_t operation, uint64_t request_id, const LVWorkflowTransportRequest *request);
typedef void (*LVWorkflowInspectRequest)(void *context, LVWorkflow *workflow,
    uint64_t operation, uint64_t request_id, uint64_t original_token);
typedef void (*LVWorkflowInsertRequest)(void *context, LVWorkflow *workflow,
    uint64_t operation, uint64_t request_id, uint64_t target_token, LVProtocolSlice text);
typedef void (*LVWorkflowCancelRequest)(void *context, LVWorkflow *workflow, uint64_t operation);
typedef void (*LVWorkflowShutdownRequest)(void *context, LVWorkflow *workflow);

typedef struct {
    LVWorkflowCaptureOriginalRequest capture_original;
    LVWorkflowCaptureStartRequest capture_start;
    LVWorkflowCaptureStopRequest capture_stop;
    LVWorkflowTransportSendRequest transport_send;
    LVWorkflowInspectRequest inspect_target;
    LVWorkflowInsertRequest insert_text;
    LVWorkflowCancelRequest capture_cancel;
    LVWorkflowCancelRequest transport_cancel;
    LVWorkflowCancelRequest insertion_cancel;
    LVWorkflowShutdownRequest capture_shutdown;
    LVWorkflowShutdownRequest transport_shutdown;
    LVWorkflowShutdownRequest insertion_shutdown;
} LVWorkflowNativeCallbacks;

typedef void (*LVWorkflowPhaseChanged)(void *context, LVWorkflow *workflow,
    uint64_t operation, LVWorkflowPhase phase);
typedef void (*LVWorkflowTextReady)(void *context, LVWorkflow *workflow,
    uint64_t operation, LVProtocolSlice text);
typedef void (*LVWorkflowFailed)(void *context, LVWorkflow *workflow,
    uint64_t operation, LVWorkflowFailure failure);
typedef void (*LVWorkflowCompleted)(void *context, LVWorkflow *workflow,
    uint64_t operation, LVWorkflowOutcome outcome);
typedef void (*LVWorkflowDiagnostic)(void *context, LVWorkflow *workflow,
    LVWorkflowDiagnosticEvent event, LVWorkflowAdapterError error, int http_status);

typedef struct {
    LVWorkflowPhaseChanged phase_changed;
    LVWorkflowTextReady text_ready;
    LVWorkflowFailed failed;
    LVWorkflowCompleted completed;
    LVWorkflowDiagnostic diagnostic;
} LVWorkflowCoreCallbacks;

/* All native request functions are required; core callbacks are optional. */
LVWorkflow *lv_workflow_create(const LVWorkflowNativeCallbacks *native_callbacks,
    const LVWorkflowCoreCallbacks *core_callbacks, void *context);

/* Copies config slices before returning. Returns 0 only when unavailable/invalid. */
uint64_t lv_workflow_start(LVWorkflow *workflow, const LVWorkflowConfig *config);
int lv_workflow_stop(LVWorkflow *workflow, LVProtocolSlice multipart_boundary);
void lv_workflow_cancel(LVWorkflow *workflow);
void lv_workflow_shutdown(LVWorkflow *workflow);

/*
 * Returns 1 only after shutdown and outside every bridge entry/callback. When
 * called after shutdown from an active entry/callback it returns 0, consumes
 * the handle, and deletes it after the stack unwinds. Before shutdown, 0
 * leaves ownership with the caller.
 */
int lv_workflow_destroy(LVWorkflow *workflow);
int lv_workflow_is_shutdown(const LVWorkflow *workflow);

/*
 * Emergency no-callback teardown that consumes the handle. It suppresses all
 * native/core callbacks before stopping the core. If called from a bridge
 * entry or callback, physical deletion waits for that stack to unwind; the
 * caller must never use the handle again.
 */
void lv_workflow_abandon(LVWorkflow *workflow);

/* Completion inputs are copied before the completion call returns. */
int lv_workflow_complete_capture_original(LVWorkflow *workflow, uint64_t operation,
    uint64_t request_id, LVWorkflowAdapterError error, LVWorkflowTarget target);
int lv_workflow_complete_capture_start(LVWorkflow *workflow, uint64_t operation,
    uint64_t request_id, LVWorkflowAdapterError error);
int lv_workflow_complete_capture_stop(LVWorkflow *workflow, uint64_t operation,
    uint64_t request_id, LVWorkflowAdapterError error, LVProtocolSlice wav);
int lv_workflow_complete_transport(LVWorkflow *workflow, uint64_t operation,
    uint64_t request_id, LVWorkflowAdapterError error, int http_status, LVProtocolSlice body);
int lv_workflow_complete_inspect(LVWorkflow *workflow, uint64_t operation,
    uint64_t request_id, LVWorkflowAdapterError error, LVWorkflowTarget target);
int lv_workflow_complete_insert(LVWorkflow *workflow, uint64_t operation,
    uint64_t request_id, LVWorkflowAdapterError error);

#ifdef __cplusplus
}
#endif
