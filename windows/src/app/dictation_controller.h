#pragma once

#include "adapters.h"
#include "common/executor.h"
#include "prefs/preferences.h"
#include "workflow.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lemonvox::win {

// Bounded, content-free diagnostics: fixed events, error categories and HTTP
// status codes only. Dictated text, audio, prompts and URLs never reach it.
class DiagnosticsRing final : public DiagnosticsSink {
public:
    static constexpr std::size_t kCapacity = 200;
    void record(DiagnosticEntry entry) override;
    std::vector<DiagnosticEntry> entries() const;
    // One line per entry, e.g. "dictation_failed error=network http=0".
    std::wstring format() const;

private:
    mutable std::mutex mutex_;
    std::deque<DiagnosticEntry> entries_;
};

enum class FailureAction : std::uint8_t { none, open_settings, open_microphone_privacy };

struct UserMessage {
    std::wstring text;
    FailureAction action = FailureAction::none;
};

// Why capture failed, as reported by the native capture adapter at failure time.
enum class CaptureProblem : std::uint8_t {
    none, permission_denied, no_device, device_busy, device_lost, other,
};

UserMessage describe_failure(const WorkflowFailure &failure, CaptureProblem capture);
std::wstring describe_fallback(WorkflowFallbackReason reason);

struct ControllerEvents {
    std::function<void(WorkflowPhase)> phase;
    std::function<void(const UserMessage &)> failed;
    std::function<void()> inserted;
    // Result text is held by the controller for copy_pending(); only the reason is passed.
    std::function<void(const std::wstring &reason)> copy_fallback;
};

// Binds WorkflowCore to the Windows adapters on the serialized executor. All
// methods must run on the executor thread.
class DictationController {
public:
    DictationController(CaptureAdapter &capture, ByteTransport &transport,
        InsertionAdapter &insertion, const PreferencesAdapter &preferences, ControllerEvents events,
        std::function<CaptureProblem()> capture_problem = {});
    ~DictationController();
    DictationController(const DictationController &) = delete;
    DictationController &operator=(const DictationController &) = delete;

    // Starts when idle, stops when recording, ignored while processing.
    void toggle();
    bool start();
    bool stop();
    void cancel();
    // The capture adapter reports an asynchronous device failure; finish the
    // recording so the core surfaces the failure.
    void capture_failed();
    // Shuts down the core, which shuts down every adapter; no callbacks afterwards.
    void shutdown();

    WorkflowPhase phase() const;
    bool busy() const;
    bool has_pending_text() const { return !pending_text_.empty(); }
    // Copies the retained fallback text through `copy`, then clears it on success.
    bool copy_pending(const std::function<bool(const std::string &)> &copy);
    void discard_pending();
    const DiagnosticsRing &diagnostics() const { return diagnostics_; }

private:
    bool accept(OperationId operation);

    const PreferencesAdapter &preferences_;
    ControllerEvents events_;
    std::function<CaptureProblem()> capture_problem_;
    DiagnosticsRing diagnostics_;
    std::unique_ptr<WorkflowCore> core_;
    OperationId active_ = 0;
    bool starting_ = false;
    std::string pending_text_;
    std::string staged_text_;
};

// "LemonVox-" plus a random UUID, matching the Mac multipart boundary.
std::string make_multipart_boundary();

} // namespace lemonvox::win
