#pragma once

#include "common/executor.h"
#include "common/trigger_binding.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace lemonvox::win {

// System-wide trigger. Owns one dedicated listener thread with its own message
// loop; double_tap bindings install WH_KEYBOARD_LL + WH_MOUSE_LL hooks there and
// combination bindings use RegisterHotKey on that thread's message-only window.
// An opt-in cancellation binding keeps the keyboard hook available while a
// session is armed; only its exact Escape key-down/up is then consumed.
//
// on_trigger runs through executor.post(); it never runs while suspended or
// after shutdown() returns (when shutdown() runs on the executor's thread, as
// the app does, that guarantee also covers tasks already queued). `executor`
// must outlive this object. Public methods may be called from any thread
// except the listener thread (i.e. not from inside callback postings).
class GlobalTrigger {
public:
    GlobalTrigger(Executor &executor, std::function<void()> on_trigger,
        std::function<void()> on_cancel = {});
    ~GlobalTrigger();
    GlobalTrigger(const GlobalTrigger &) = delete;
    GlobalTrigger &operator=(const GlobalTrigger &) = delete;

    // Activates `binding`. On failure returns false, fills *error with a
    // user-readable reason (e.g. "Ctrl+Shift+Space is already used by another
    // app") and keeps the previously active binding working.
    bool apply(const TriggerBinding &binding, std::wstring *error);
    // The binding currently live, if any.
    std::optional<TriggerBinding> active() const;
    // Configures an opt-in held-modifier Escape cancellation binding. Passing
    // nullopt removes it. This never uses RegisterHotKey, so ordinary keyboard
    // delivery remains untouched until set_cancellation_armed(true).
    bool apply_cancellation(const std::optional<TriggerBinding> &binding, std::wstring *error);
    std::optional<TriggerBinding> active_cancellation() const;
    // The application arms cancellation only from the start of a session until
    // every terminal/cancel/shutdown path. While false, the hook passes every
    // key through, including a configured cancellation chord.
    void set_cancellation_armed(bool armed);
    bool cancellation_armed() const;
    // While suspended (e.g. the Settings shortcut recorder has focus) nothing
    // activates and the double-tap detector is reset on both edges.
    void set_suspended(bool suspended);
    bool suspended() const;
    // Number of times the health check found the keyboard hook silently
    // removed (LowLevelHooksTimeout) and reinstalled it. Diagnostics only.
    std::uint32_t hook_reinstalls() const;
    // Unhooks, unregisters and joins the listener thread. Idempotent.
    void shutdown();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace lemonvox::win
