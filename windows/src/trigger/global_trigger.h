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
// The hooks only observe: every event is passed on with CallNextHookEx.
//
// on_trigger runs through executor.post(); it never runs while suspended or
// after shutdown() returns (when shutdown() runs on the executor's thread, as
// the app does, that guarantee also covers tasks already queued). `executor`
// must outlive this object. Public methods may be called from any thread
// except the listener thread (i.e. not from inside on_trigger's own posting).
class GlobalTrigger {
public:
    GlobalTrigger(Executor &executor, std::function<void()> on_trigger);
    ~GlobalTrigger();
    GlobalTrigger(const GlobalTrigger &) = delete;
    GlobalTrigger &operator=(const GlobalTrigger &) = delete;

    // Activates `binding`. On failure returns false, fills *error with a
    // user-readable reason (e.g. "Ctrl+Shift+Space is already used by another
    // app") and keeps the previously active binding working.
    bool apply(const TriggerBinding &binding, std::wstring *error);
    // The binding currently live, if any.
    std::optional<TriggerBinding> active() const;
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
