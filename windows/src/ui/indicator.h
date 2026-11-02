#pragma once

// Compact dictation bar. It is a nonactivating, topmost, layered tool window:
// showing it, clicking its buttons and dragging it never take foreground or
// keyboard focus from the app being dictated into. All members must be used
// on the thread that called create(); marshal from workers with
// lemonvox::win::UiThreadExecutor.

#include "adapters.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lemonvox::win::ui {

enum class IndicatorMode : std::uint8_t {
    hidden,
    preparing,
    recording,
    transcribing,
    cleaning,
    inserting,
    success,       // auto-hides after auto_hide_delay
    notice,        // informational message, no buttons; auto-hides after a few seconds
    error,         // message + Dismiss + optional action
    copy_fallback, // insertion failed: message + Copy + Dismiss
};

struct IndicatorState {
    IndicatorMode mode = IndicatorMode::hidden;
    double level = 0.0;                   // recording input level, 0..1
    std::chrono::milliseconds elapsed{0}; // recording duration
    std::wstring message;                 // success / error / copy_fallback text
    std::wstring action_label;            // error: e.g. L"Open microphone settings"
};

enum class IndicatorButton : std::uint8_t { cancel, stop, copy, dismiss, action };

// Invoked on the UI thread after the click completes. Callbacks may change the
// indicator state but must not destroy the IndicatorWindow synchronously.
struct IndicatorCallbacks {
    std::function<void()> cancel;
    std::function<void()> stop;
    std::function<void()> copy;
    std::function<void()> dismiss; // the indicator hides itself first
    std::function<void()> action;
    // Called when a drag ends with the new anchor (see IndicatorWindow::anchor).
    std::function<void(POINT)> position_changed;
};

// Presentation mapping, kept pure for tests.
IndicatorMode indicator_mode_for_phase(lemonvox::WorkflowPhase phase);
std::wstring format_elapsed(std::chrono::milliseconds elapsed);
std::wstring indicator_status_text(const IndicatorState &state);
// Window title exposed to Narrator and other MSAA/UIA clients.
std::wstring indicator_accessible_name(const IndicatorState &state);
std::wstring indicator_button_label(IndicatorButton button, const IndicatorState &state);
std::vector<IndicatorButton> indicator_buttons(const IndicatorState &state);
// Moves `window` the minimum distance to lie inside `work`; a window larger
// than `work` is pinned to its left/top edge.
RECT clamp_rect_to_work_area(const RECT &window, const RECT &work);

class IndicatorWindow {
public:
    explicit IndicatorWindow(IndicatorCallbacks callbacks);
    ~IndicatorWindow();
    IndicatorWindow(const IndicatorWindow &) = delete;
    IndicatorWindow &operator=(const IndicatorWindow &) = delete;

    // Creates the (hidden) window on the calling thread, which must pump messages.
    bool create(HINSTANCE instance);
    void destroy();

    void set_state(IndicatorState state);
    // Cheap per-frame recording update; ignored outside IndicatorMode::recording.
    void set_level(double level, std::chrono::milliseconds elapsed);
    const IndicatorState &state() const;

    // The anchor is the bar's bottom-center in physical virtual-screen pixels;
    // taller message layouts grow upward from it. nullopt restores the default
    // (bottom-center of the primary work area). The visible rect is always
    // clamped to the nearest monitor's work area.
    void set_anchor(std::optional<POINT> anchor);
    POINT anchor() const;
    void set_auto_hide_delay(std::chrono::milliseconds delay);

    HWND hwnd() const;
    bool visible() const;
    // Screen rect of a visible button, for tests and UI automation.
    std::optional<RECT> button_rect(IndicatorButton button) const;
    // WM_ACTIVATE(active) / WM_SETFOCUS messages received; must stay 0.
    unsigned activation_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lemonvox::win::ui
