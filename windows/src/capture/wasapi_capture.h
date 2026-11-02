#pragma once

#include "adapters.h"
#include "capture/capture_backend.h"
#include "common/executor.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace lemonvox::win {

// Microphone capture for the shared core. Audio is captured in WASAPI shared
// mode on a dedicated worker thread (COM MTA), kept in memory only, and
// converted to a 16 kHz mono 16-bit WAV at stop(). Nothing is written to disk
// or logged.
//
// Every completion and callback is delivered through the Executor. Completions
// for an operation passed to cancel() are suppressed, and nothing is delivered
// after shutdown().
class WasapiCapture final : public lemonvox::CaptureAdapter {
public:
    using LevelCallback = std::function<void(float level)>;
    using FailureCallback = std::function<void(OperationId, CaptureFailureReason)>;

    // Matches the macOS recorder: capture ends after 120 s; the audio so far is kept.
    static constexpr double kMaxSeconds = 120.0;
    // Recordings shorter than this return empty bytes, which the core reports
    // as a recording failure.
    static constexpr double kMinSeconds = 0.1;

    // An empty device_id follows the default capture endpoint. backend is a
    // seam for tests; nullptr selects WASAPI.
    explicit WasapiCapture(Executor &executor, std::wstring device_id = {},
        std::shared_ptr<CaptureBackend> backend = nullptr);
    ~WasapiCapture() override;
    WasapiCapture(const WasapiCapture &) = delete;
    WasapiCapture &operator=(const WasapiCapture &) = delete;

    // Thread-safe; applies to the next start().
    void set_device_id(std::wstring device_id);
    std::wstring device_id() const;

    // Thread-safe. The meter callback runs on the executor at most ~20 Hz while
    // recording, with a value in [0, 1].
    void set_level_callback(LevelCallback callback);
    // Thread-safe. Runs on the executor when an active recording fails on its own
    // (device removed, disabled or default endpoint switched). The following
    // stop() completes with AdapterError::unavailable.
    void set_failure_callback(FailureCallback callback);

    // Thread-safe. Reason for the most recent start/recording failure; reset to
    // none by each start().
    CaptureFailureReason last_failure_reason() const;

    // start: none, or unavailable (see last_failure_reason()); invalid_input
    // never occurs for start. stop: WAV bytes; empty bytes with none when the
    // recording is too short; unavailable after a device failure; invalid_input
    // when the operation is not the one recording.
    void start(OperationId operation, VoidCompletion completion) override;
    void stop(OperationId operation, BytesCompletion completion) override;
    void cancel(OperationId operation) override;
    // Barrier: stops capture, releases the device, joins the worker. No
    // completion or callback runs afterwards. Idempotent.
    void shutdown() override;

private:
    struct Shared;
    struct Worker;

    Executor &executor_;
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<Worker> worker_;
    std::mutex shutdown_mutex_;
};

} // namespace lemonvox::win
