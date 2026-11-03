# Windows trigger (`lv_trigger`)

Configurable system-wide trigger for the Windows host. The double-tap policy
(timing windows, holds, same-side pairs) lives in the shared C++ core
(`core/session.h`, `lv_double_tap_*`); this module only adapts Windows input to it.

| File | Role |
| --- | --- |
| `key_normalizer.h/.cpp` | Pure, stateful translation of `KBDLLHOOKSTRUCT`-style input (or window key messages) into `LVNormalizedKeyEvent`. Exports `kLemonVoxInjectedTag`. |
| `global_trigger.h/.cpp` | `GlobalTrigger`: listener thread with its own message loop; `WH_KEYBOARD_LL` + `WH_MOUSE_LL` for double taps, `RegisterHotKey` for combinations. |
| `shortcut_recorder.h/.cpp` | UI-agnostic capture logic for the Settings shortcut recorder control. |

## Normalization rules

- `VK_LMENU/VK_RMENU` → `LV_KEY_OPTION` left/right; `L/RCONTROL` → `LV_KEY_CONTROL`;
  `L/RSHIFT` → `LV_KEY_SHIFT`; `L/RWIN` → `LV_KEY_COMMAND`. Generic `VK_MENU`/`VK_CONTROL`
  resolve the side from the extended flag, `VK_SHIFT` from scan code `0x36`.
- Any other key-down is `LV_KEY_INTERVENING`; ordinary key-ups carry nothing (as on macOS,
  which only observes key-downs).
- Auto-repeat key-downs of a held modifier are dropped: a hold is one press, and the core
  rejects it on release when it exceeds the 0.25 s tap limit.
- A modifier pressed or released while another modifier is held is intervening (chord).
- Mouse button presses (`WH_MOUSE_LL`) and foreground-window changes cancel a pending tap.
- Input whose `dwExtraInfo == kLemonVoxInjectedTag` is ignored entirely. Other injected
  input (`LLKHF_INJECTED`: remote desktop, accessibility tools, tests) is treated as hardware.

### AltGr

On AltGr layouts Windows sends a synthetic left Ctrl immediately before right Alt, with
the same timestamp. The low-level hook marks it with scan code `0x21D`; window messages lose
that marker. Both forms are recognised (the scan bit, or a left Ctrl and right Alt with
identical event times), and the whole AltGr press, its repeats and releases are intervening.
AltGr never counts as an Alt tap, so "Alt twice" (any or right side) is never triggered by
typing AltGr characters. In the recorder, AltGr+key records the equivalent Ctrl+Alt+key hotkey,
which is how `RegisterHotKey` treats AltGr anyway.

## Hooks and their limits

- Hook procedures only update in-memory state and post to the executor; every event is passed
  on with `CallNextHookEx`. Nothing is ever swallowed.
- Windows silently removes a low-level hook that exceeds `LowLevelHooksTimeout`. A 2 s timer
  compares `GetLastInputInfo` with the time of our last hook callback; if the system saw input
  more than 1 s newer than anything our hooks received, the hooks are reinstalled
  (`GlobalTrigger::hook_reinstalls()` counts this). Input that never reaches medium-integrity
  hooks (e.g. some elevated or secure-desktop cases) may cause a harmless reinstall.
- Hooks are installed only for double-tap bindings; combinations use only `RegisterHotKey`.
- On a failed `apply` the previous binding stays live: a new hotkey is registered before the old
  one is released, and a hook failure leaves the old hotkey in place.

## Alt and menu mode

A lone Alt press+release is Windows' own menu-bar toggle: `DefWindowProc` turns the
`WM_SYSKEYUP` into `SC_KEYMENU` and the focused classic window enters menu mode (menu bar
highlighted; Office/Explorer show key tips; Firefox shows its menu bar; Chrome focuses its menu
button). The double tap is neutral: tap 1 enters menu mode, tap 2 leaves it, so the target
window is back to normal before dictation starts. `trigger_interactive_test` verifies this with
a real menu bar: after a single tap `GUI_INMENUMODE` is set, after a double tap it is clear,
and typing still reaches the focused EDIT control.

Tools such as AutoHotkey suppress menu activation by injecting a "mask" key on Alt release.
LemonVox deliberately does not: it cannot know on the first tap whether a second will follow,
masking every Alt release would change ordinary Alt behavior system-wide, and masking only the
second release would leave the app *in* menu mode. Users who find the brief menu flash
distracting can choose another modifier or a key combination.

## Integration notes

- Insertion: set `dwExtraInfo = lemonvox::win::kLemonVoxInjectedTag` on every `INPUT` LemonVox
  sends (`key_normalizer.h`), so pasting or typing never interacts with a pending double tap.
- Settings: while the shortcut recorder control has focus call `set_suspended(true)`, feed
  `WM_(SYS)KEYDOWN/UP` through `recorder_key_from_message` + `ShortcutRecorder::feed`, then
  `apply()` the recorded binding and show the returned error if it fails.
- `on_trigger` runs on the executor thread. Call `shutdown()` on that thread to guarantee no
  callback runs afterwards (already-queued deliveries are dropped).
