#pragma once

#include "common/trigger_binding.h"
#include "trigger/key_normalizer.h"

#include <cstdint>
#include <string>

namespace lemonvox::win {

// One key message as seen by a focused recorder control.
struct RecorderKey {
    std::uint32_t vk = 0;
    std::uint32_t scan = 0;   // 8-bit scan code from lParam bits 16-23
    bool extended = false;    // lParam bit 24
    bool down = true;
    bool repeat = false;      // lParam bit 30 on a key-down
    std::uint32_t time_ms = 0; // GetMessageTime()
};

// Decodes WM_KEYDOWN / WM_SYSKEYDOWN / WM_KEYUP / WM_SYSKEYUP. Returns false
// for any other message.
bool recorder_key_from_message(std::uint32_t message, std::uintptr_t wparam, std::intptr_t lparam,
    std::uint32_t time_ms, RecorderKey *key);

enum class RecorderStatus {
    idle,      // not recording
    listening, // waiting for more input
    recorded,  // binding() holds the new trigger; recording ended
    cancelled, // Esc pressed; recording ended
    rejected,  // message() explains why; still listening
};

// UI-agnostic shortcut capture. The host control calls begin() when the user
// starts recording (and GlobalTrigger::set_suspended(true)), feeds every key
// message, and stops once the status is recorded or cancelled (or calls
// cancel() on focus loss). A modifier tapped twice records a double-tap
// trigger (either side); a non-modifier key records a combination with the
// modifiers held, validated with the persisted trigger rules.
class ShortcutRecorder {
public:
    void begin(bool allow_double_tap = true);
    void cancel();
    bool active() const { return active_; }
    RecorderStatus feed(const RecorderKey &key);
    const TriggerBinding &binding() const { return binding_; }
    const std::wstring &message() const { return message_; }

private:
    bool active_ = false;
    bool allow_double_tap_ = true;
    KeyNormalizer normalizer_;
    LVDoubleTapDetector detector_{};
    TriggerBinding binding_;
    std::wstring message_;
};

} // namespace lemonvox::win
