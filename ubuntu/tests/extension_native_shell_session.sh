#!/usr/bin/env bash
# Runs inside extension_native_shell_test.sh's private D-Bus/XDG environment.
# Do not invoke this script against a live desktop session.
set -euo pipefail

readonly test_root="$1"
readonly uuid="$2"
readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly backend_script="$script_dir/extension_native_shell_backend.mjs"
readonly fixture_script="$script_dir/extension_native_shell_fixture.mjs"
readonly shell_log="$test_root/gnome-shell.log"
readonly introspection_log="$test_root/gnome-shell-introspection.log"
shell_pid=''
backend_pid=''
fixture_pid=''
phase='startup'

print_diagnostics() {
    printf '\n--- GNOME Shell log (last 100 lines) ---\n' >&2
    tail -100 "$shell_log" >&2 || true
    printf '\n--- Shell D-Bus probe ---\n' >&2
    cat "$introspection_log" >&2 || true
}

cleanup() {
    if [[ -n "$fixture_pid" ]] && kill -0 "$fixture_pid" 2>/dev/null; then
        kill -TERM "$fixture_pid" 2>/dev/null || true
        wait "$fixture_pid" 2>/dev/null || true
    fi
    if [[ -n "$backend_pid" ]] && kill -0 "$backend_pid" 2>/dev/null; then
        kill -TERM "$backend_pid" 2>/dev/null || true
        wait "$backend_pid" 2>/dev/null || true
    fi
    if [[ -n "$shell_pid" ]] && kill -0 "$shell_pid" 2>/dev/null; then
        kill -TERM "$shell_pid" 2>/dev/null || true
        wait "$shell_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

fail() {
    printf 'FAIL [%s]: %s\n' "$phase" "$1" >&2
    print_diagnostics
    exit 1
}

shell_eval() {
    gdbus call --session \
        --dest org.gnome.Shell \
        --object-path /org/gnome/Shell \
        --method org.gnome.Shell.Eval "$1"
}

backend_state() {
    gdbus call --session \
        --dest com.itomek.LemonVox \
        --object-path /com/itomek/LemonVox \
        --method org.freedesktop.DBus.Properties.Get \
        com.itomek.LemonVox State
}

backend_result() {
    gdbus call --session \
        --dest com.itomek.LemonVox \
        --object-path /com/itomek/LemonVox \
        --method org.freedesktop.DBus.Properties.Get \
        com.itomek.LemonVox Result
}

wait_for_backend_state() {
    local expected="$1"
    local observed=''
    for _attempt in $(seq 1 100); do
        observed="$(backend_state 2>/dev/null || true)"
        if [[ "$observed" == *"'$expected'"* ]]; then
            return
        fi
        sleep 0.05
    done
    printf '%s\n' "$observed" >"$introspection_log"
    fail "synthetic backend did not reach $expected"
}

assert_backend_state() {
    local expected="$1"
    local observed=''
    observed="$(backend_state 2>/dev/null || true)"
    [[ "$observed" == *"'$expected'"* ]] || {
        printf '%s\n' "$observed" >"$introspection_log"
        fail "synthetic backend was not still $expected"
    }
}

wait_for_backend_result() {
    local expected="$1"
    local observed=''
    for _attempt in $(seq 1 100); do
        observed="$(backend_result 2>/dev/null || true)"
        if [[ "$observed" == *"'$expected'"* ]]; then
            return
        fi
        sleep 0.05
    done
    printf '%s\n' "$observed" >"$introspection_log"
    fail 'synthetic backend result did not reach the expected value'
}

assert_fixture_event() {
    local expected="$1"
    local observed=''
    if [[ -f "$fixture_events" ]]; then
        observed="$(<"$fixture_events")"
    fi
    [[ "$observed" == "$expected" ]] || {
        printf '%s\n' "$observed" >"$introspection_log"
        fail "GTK fixture received an unexpected key event (wanted $expected)"
    }
}

wait_for_fixture_focus() {
    local expected="$1"
    local observed=''
    for _attempt in $(seq 1 100); do
        if [[ -f "$fixture_focus" ]]; then
            observed="$(<"$fixture_focus")"
        else
            observed=''
        fi
        if [[ "$observed" == "$expected" ]]; then
            return
        fi
        sleep 0.05
    done
    printf '%s\n' "$observed" >"$introspection_log"
    fail "GTK target field did not become $expected"
}

assert_fixture_focus() {
    local expected="$1"
    local observed=''
    if [[ -f "$fixture_focus" ]]; then
        observed="$(<"$fixture_focus")"
    fi
    [[ "$observed" == "$expected" ]] || {
        printf '%s\n' "$observed" >"$introspection_log"
        fail "GTK target field was not still $expected"
    }
}

wait_for_fixture_text() {
    local expected="$1"
    local observed=''
    for _attempt in $(seq 1 100); do
        if [[ -f "$fixture_text" ]]; then
            observed="$(<"$fixture_text")"
        else
            observed=''
        fi
        if [[ "$observed" == "$expected" ]]; then
            return
        fi
        sleep 0.05
    done
    printf '%s\n' "$observed" >"$introspection_log"
    fail 'GTK target field did not receive the explicit copied text'
}

wait_for_eval() {
    local script="$1"
    local expected="$2"
    local observed=''
    for _attempt in $(seq 1 100); do
        observed="$(shell_eval "$script" 2>/dev/null || true)"
        if [[ "$observed" == *"$expected"* ]]; then
            return
        fi
        sleep 0.05
    done
    printf '%s\n' "$observed" >"$introspection_log"
    fail "Shell Eval did not report $expected"
}

# Backend property reads can complete before the Shell proxy processes its
# PropertiesChanged notification. Wait for the consumer, including successful
# native key registration, before sending another key or disabling controls.
wait_for_extension_state() {
    local state="$1"
    local active="$2"
    wait_for_eval \
        "import('resource:///org/gnome/shell/ui/main.js').then(Main => { const e = Main.extensionManager.lookup('$uuid').stateObj; const b = e?._bindings; return e?._client?.snapshot.state === '$state' && b?._active === $active && b._toggleInstalled && b._cancelInstalled === $active && !b._rebindSource ? 'extension-state-ready' : 'extension-state-pending'; })" \
        extension-state-ready
}

wait_for_fixture_event() {
    local expected="$1"
    local observed=''
    for _attempt in $(seq 1 100); do
        if [[ -f "$fixture_events" ]]; then
            observed="$(<"$fixture_events")"
        else
            observed=''
        fi
        if [[ "$observed" == "$expected" ]]; then
            return
        fi
        sleep 0.05
    done
    printf '%s\n' "$observed" >"$introspection_log"
    fail "GTK fixture did not receive $expected"
}

send_shortcut() {
    local modifiers="$1"
    local key="$2"
    local injected=''
    injected="$(shell_eval \
        "Promise.all([import('gi://Clutter'), import('gi://GLib')]).then(([{default: Clutter}, {default: GLib}]) => { const device = global.__lemonVoxTestVirtualKeyboard ??= Clutter.get_default_backend().get_default_seat().create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE); return new Promise(resolve => GLib.idle_add(GLib.PRIORITY_DEFAULT, () => { let time = GLib.get_monotonic_time(); for (const keyval of [$modifiers]) device.notify_keyval(time++, keyval, Clutter.KeyState.PRESSED); device.notify_keyval(time++, $key, Clutter.KeyState.PRESSED); device.notify_keyval(time++, $key, Clutter.KeyState.RELEASED); for (const keyval of [$modifiers].reverse()) device.notify_keyval(time++, keyval, Clutter.KeyState.RELEASED); resolve('shortcut-injected'); return GLib.SOURCE_REMOVE; })); })")" || fail 'could not inject a virtual keyboard shortcut into private Shell'
    [[ "$injected" == *shortcut-injected* ]] || {
        printf '%s\n' "$injected" >"$introspection_log"
        fail 'private Shell did not accept virtual keyboard shortcut injection'
    }
}

prepare_virtual_keyboard() {
    local prepared=''
    prepared="$(shell_eval \
        "import('gi://Clutter').then(({default: Clutter}) => { global.__lemonVoxTestVirtualKeyboard ??= Clutter.get_default_backend().get_default_seat().create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE); return global.__lemonVoxTestVirtualKeyboard ? 'virtual-keyboard-ready' : 'virtual-keyboard-missing'; })")" || fail 'could not create a private virtual keyboard'
    [[ "$prepared" == *virtual-keyboard-ready* ]] || {
        printf '%s\n' "$prepared" >"$introspection_log"
        fail 'private Shell did not create a virtual keyboard'
    }
}

capture_private_shell() {
    local name="$1"
    local target="$test_root/$name.png"
    local result=''
    result="$(gdbus call --session \
        --dest org.gnome.Shell.Screenshot \
        --object-path /org/gnome/Shell/Screenshot \
        --method org.gnome.Shell.Screenshot.Screenshot \
        false false "$target" 2>/dev/null || true)"
    if [[ "$result" != *true* || ! -s "$target" ]]; then
        printf 'NOTE: private Shell screenshot unavailable for %s\n' "$name" >&2
        return
    fi
    if [[ -n "${LEMONVOX_EVIDENCE_DIR:-}" ]]; then
        [[ -d "$LEMONVOX_EVIDENCE_DIR" ]] || fail 'configured private screenshot evidence directory is absent'
        cp -- "$target" "$LEMONVOX_EVIDENCE_DIR/$name.png"
    fi
}

# --unsafe-mode applies to this fresh, headless process only. GNOME 46's
# DebugControl exposes HDR but not an UnsafeMode property, so this documented
# command-line gate is the safe way to inspect the private test compositor.
gnome-shell --wayland --wayland-display=wayland-0 --headless --no-x11 --virtual-monitor=1280x800 --unsafe-mode >"$shell_log" 2>&1 &
shell_pid="$!"

shell_ready=0
for _attempt in $(seq 1 100); do
    if gdbus call --session \
        --dest org.gnome.Shell \
        --object-path /org/gnome/Shell \
        --method org.gnome.Shell.Extensions.ListExtensions >"$introspection_log" 2>&1; then
        shell_ready=1
        break
    fi
    if ! kill -0 "$shell_pid" 2>/dev/null; then
        fail 'headless GNOME Shell exited before its extension interface was ready'
    fi
    sleep 0.1
done

[[ "$shell_ready" == 1 ]] || fail 'timed out waiting for the private headless Shell extension interface'
prepare_virtual_keyboard

backend_ready_file="$test_root/synthetic-backend-ready.txt"
gjs -m "$backend_script" "$backend_ready_file" >"$test_root/synthetic-backend.log" 2>&1 &
backend_pid="$!"
backend_ready=0
for _attempt in $(seq 1 100); do
    if [[ -f "$backend_ready_file" ]] && [[ "$(<"$backend_ready_file")" == 'ready' ]]; then
        backend_ready=1
        break
    fi
    if ! kill -0 "$backend_pid" 2>/dev/null; then
        cat "$test_root/synthetic-backend.log" >"$introspection_log" || true
        fail 'synthetic backend exited before claiming its private D-Bus name'
    fi
    sleep 0.05
done
[[ "$backend_ready" == 1 ]] || fail 'timed out waiting for synthetic backend D-Bus ownership'
wait_for_backend_state idle

info_before="$(gdbus call --session \
    --dest org.gnome.Shell \
    --object-path /org/gnome/Shell \
    --method org.gnome.Shell.Extensions.GetExtensionInfo "$uuid")" || fail 'GNOME Shell did not discover the staged extension'
[[ "$info_before" == *"'state': <2.0>"* || "$info_before" == *"'state': <6.0>"* ]] || {
    printf '%s\n' "$info_before" >"$introspection_log"
    fail 'staged extension was not inactive or initialized before enable'
}

enable_result="$(gdbus call --session \
    --dest org.gnome.Shell \
    --object-path /org/gnome/Shell \
    --method org.gnome.Shell.Extensions.EnableExtension "$uuid")" || fail 'GNOME Shell rejected extension enable'
[[ "$enable_result" == *true* ]] || {
    printf '%s\n' "$enable_result" >"$introspection_log"
    fail 'GNOME Shell reported extension enable failure'
}

active=0
for _attempt in $(seq 1 100); do
    info_active="$(gdbus call --session \
        --dest org.gnome.Shell \
        --object-path /org/gnome/Shell \
        --method org.gnome.Shell.Extensions.GetExtensionInfo "$uuid")" || true
    if [[ "$info_active" == *"'state': <1.0>"* ]]; then
        active=1
        break
    fi
    sleep 0.1
done
[[ "$active" == 1 ]] || {
    printf '%s\n' "$info_active" >"$introspection_log"
    fail 'extension did not become active'
}

wait_for_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => Main.extensionManager.lookup('$uuid').stateObj._client.snapshot.available ? 'backend-ready' : 'backend-pending')" \
    backend-ready

fixture_events="$test_root/fixture-events.txt"
fixture_focus="$test_root/fixture-focus.txt"
fixture_text="$test_root/fixture-text.txt"
fixture_activation="$test_root/fixture-activation.txt"
GDK_BACKEND=wayland WAYLAND_DISPLAY=wayland-0 \
    gjs -m "$fixture_script" "$fixture_events" "$fixture_focus" "$fixture_text" "$fixture_activation" >"$test_root/fixture.log" 2>&1 &
fixture_pid="$!"
wait_for_eval \
    "JSON.stringify(global.get_window_actors().map(actor => actor.meta_window?.get_title()))" \
    'LemonVox Native Fixture'
fixture_activation="$(shell_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => { const window = global.get_window_actors().map(actor => actor.meta_window).find(candidate => candidate?.get_title() === 'LemonVox Native Fixture'); if (!window) return 'fixture-missing'; Main.activateWindow(window, global.get_current_time()); return 'fixture-activation-requested'; })")" || fail 'could not request focus for disposable GTK fixture'
[[ "$fixture_activation" == *fixture-activation-requested* ]] || {
    printf '%s\n' "$fixture_activation" >"$introspection_log"
    fail 'Shell did not find the disposable GTK fixture window'
}
wait_for_eval \
    "global.display.focus_window?.get_title() === 'LemonVox Native Fixture' ? 'fixture-focused' : 'fixture-not-focused'" \
    fixture-focused
wait_for_fixture_focus focused
wait_for_fixture_event ready

# Eval is enabled by --unsafe-mode on this disposable compositor only. It is
# never requested from the logged-in Shell, and makes native actor/binding
# assertions possible without an X11 session or privileged input access.
eval_smoke="$(shell_eval '1 + 1')" || fail 'Shell Eval was unavailable on private headless Shell'
[[ "$eval_smoke" == *"2"* ]] || {
    printf '%s\n' "$eval_smoke" >"$introspection_log"
    fail 'private Shell Eval did not return the expected result'
}
eval_extension="$(shell_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => Main.extensionManager.lookup('$uuid').state)")" || fail 'private Shell Eval could not inspect the extension manager'
[[ "$eval_extension" == *"1"* ]] || {
    printf '%s\n' "$eval_extension" >"$introspection_log"
    fail 'private Shell Eval did not observe the extension as active'
}

phase='idle-key-passthrough'
wait_for_extension_state idle false

# First prove the virtual keyboard reaches the focused fixture, then prove an
# idle LemonVox instance does not claim its configured cancellation chord.
send_shortcut '' 'Clutter.KEY_Escape'
wait_for_fixture_event 'escape:1'
send_shortcut 'Clutter.KEY_Control_L, Clutter.KEY_Super_L' 'Clutter.KEY_Escape'
wait_for_fixture_event 'escape:2'

phase='default-toggle'
# The default toggle is a compositor-native binding. It routes to the service,
# reveals the non-focusable native bar and does not take focus from the client.
send_shortcut 'Clutter.KEY_Control_L, Clutter.KEY_Alt_L' 'Clutter.KEY_space'
wait_for_backend_state recording
wait_for_extension_state recording true
wait_for_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => { const bar = Main.extensionManager.lookup('$uuid').stateObj._bar; return bar.actor.visible && !bar.actor.can_focus && bar._stop.visible && bar._cancel.visible && !bar._copy.visible && !bar._settings.visible && global.display.focus_window?.get_title() === 'LemonVox Native Fixture' ? 'recording-bar-focus-preserved' : 'recording-bar-check-failed'; })" \
    recording-bar-focus-preserved
assert_fixture_focus focused
capture_private_shell gnome-shell-recording

# A bare Escape is still ordinary application input even while recording.
send_shortcut '' 'Clutter.KEY_Escape'
wait_for_fixture_event 'escape:3'
sleep 0.2
assert_backend_state recording

phase='active-cancel'
# The configured cancellation chord is active only during work: it cancels the
# synthetic backend and is withheld from the focused client.
send_shortcut 'Clutter.KEY_Control_L, Clutter.KEY_Super_L' 'Clutter.KEY_Escape'
wait_for_backend_state idle
wait_for_extension_state idle false
assert_fixture_event 'escape:3'
wait_for_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => { const bar = Main.extensionManager.lookup('$uuid').stateObj._bar; return !bar.actor.visible && global.display.focus_window?.get_title() === 'LemonVox Native Fixture' ? 'idle-bar-hidden-focus-preserved' : 'idle-bar-check-failed'; })" \
    idle-bar-hidden-focus-preserved
assert_fixture_focus focused

phase='error-recovery'
# A backend error stays actionable at idle: it displays Settings plus an X but
# does not reactivate the cancellation binding. SIGUSR1 is test-only plumbing
# on the disposable synthetic backend, not a production extension hook.
kill -USR1 "$backend_pid"
wait_for_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => { const extension = Main.extensionManager.lookup('$uuid').stateObj; const bar = extension._bar; return bar.actor.visible && bar._settings.visible && bar._cancel.visible && !bar._stop.visible && !bar._copy.visible && !extension._bindings._active && global.display.focus_window?.get_title() === 'LemonVox Native Fixture' ? 'idle-error-recovery-visible' : 'idle-error-recovery-check-failed'; })" \
    idle-error-recovery-visible
assert_fixture_focus focused
capture_private_shell gnome-shell-error
error_dismiss="$(shell_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => { Main.extensionManager.lookup('$uuid').stateObj._bar._cancel.emit('clicked', null); return 'error-dismiss-clicked'; })")" || fail 'could not click private error dismiss control'
[[ "$error_dismiss" == *error-dismiss-clicked* ]] || {
    printf '%s\n' "$error_dismiss" >"$introspection_log"
    fail 'private error dismiss control did not emit clicked'
}
wait_for_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => Main.extensionManager.lookup('$uuid').stateObj._bar.actor.visible ? 'error-still-visible' : 'error-dismissed')" \
    error-dismissed

phase='manual-copy-paste'
# Copy is the sole supported result path. A test-only synthetic result makes
# the real bar Copy handler populate the Shell clipboard; Ctrl+V then pastes
# into the already-focused empty GTK entry. No Return key is injected.
readonly synthetic_copy_text='Synthetic manual copy text'
kill -USR2 "$backend_pid"
wait_for_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => { const extension = Main.extensionManager.lookup('$uuid').stateObj; const bar = extension._bar; return bar.actor.visible && bar._copy.visible && !bar._stop.visible && !bar._cancel.visible && !bar._settings.visible && global.display.focus_window?.get_title() === 'LemonVox Native Fixture' ? 'explicit-copy-visible' : 'explicit-copy-check-failed'; })" \
    explicit-copy-visible
assert_fixture_focus focused
copy_click="$(shell_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => { Main.extensionManager.lookup('$uuid').stateObj._bar._copy.emit('clicked', null); return 'copy-clicked'; })")" || fail 'could not click private Copy control'
[[ "$copy_click" == *copy-clicked* ]] || {
    printf '%s\n' "$copy_click" >"$introspection_log"
    fail 'private Copy control did not emit clicked'
}
sleep 0.2
send_shortcut 'Clutter.KEY_Control_L' 'Clutter.KEY_v'
wait_for_fixture_text "$synthetic_copy_text"
wait_for_backend_result ''
assert_fixture_focus focused
[[ ! -s "$fixture_activation" ]] || fail 'explicit Copy unexpectedly activated the target field'
wait_for_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => Main.extensionManager.lookup('$uuid').stateObj._bar.actor.visible ? 'copy-bar-still-visible' : 'copy-bar-hidden')" \
    copy-bar-hidden

phase='rebind-shortcuts'
# Settings updates rebind both compositor shortcuts without accepting the old
# toggle. The private schema directory is the staged extension, never a host
# settings path.
readonly schema_dir="$test_root/data/gnome-shell/extensions/$uuid/schemas"
GSETTINGS_SCHEMA_DIR="$schema_dir" gsettings set org.gnome.shell.extensions.lemonvox toggle-shortcut "['<Control><Super>F10']"
GSETTINGS_SCHEMA_DIR="$schema_dir" gsettings set org.gnome.shell.extensions.lemonvox cancel-shortcut "['<Control><Super>F11']"
wait_for_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => { const b = Main.extensionManager.lookup('$uuid').stateObj._bindings; return !b._rebindSource && b._toggleInstalled && b._lastGood.toggle[0] === '<Control><Super>F10' && b._lastGood.cancel[0] === '<Control><Super>F11' ? 'shortcuts-rebound' : 'shortcuts-pending'; })" \
    shortcuts-rebound
send_shortcut 'Clutter.KEY_Control_L, Clutter.KEY_Alt_L' 'Clutter.KEY_space'
sleep 0.2
assert_backend_state idle
send_shortcut 'Clutter.KEY_Control_L, Clutter.KEY_Super_L' 'Clutter.KEY_F10'
wait_for_backend_state recording
wait_for_extension_state recording true
phase='rebound-cancel'
send_shortcut 'Clutter.KEY_Control_L, Clutter.KEY_Super_L' 'Clutter.KEY_F11'
wait_for_backend_state idle
wait_for_extension_state idle false

phase='disable-cancel'
# Put the service back into an active operation so disable must issue its
# best-effort Cancel before removing the only visible controls.
send_shortcut 'Clutter.KEY_Control_L, Clutter.KEY_Super_L' 'Clutter.KEY_F10'
wait_for_backend_state recording
wait_for_extension_state recording true

disable_result="$(gdbus call --session \
    --dest org.gnome.Shell \
    --object-path /org/gnome/Shell \
    --method org.gnome.Shell.Extensions.DisableExtension "$uuid")" || fail 'GNOME Shell rejected extension disable'
[[ "$disable_result" == *true* ]] || {
    printf '%s\n' "$disable_result" >"$introspection_log"
    fail 'GNOME Shell reported extension disable failure'
}

inactive=0
for _attempt in $(seq 1 100); do
    info_inactive="$(gdbus call --session \
        --dest org.gnome.Shell \
        --object-path /org/gnome/Shell \
        --method org.gnome.Shell.Extensions.GetExtensionInfo "$uuid")" || true
    if [[ "$info_inactive" == *"'state': <2.0>"* || "$info_inactive" == *"'state': <6.0>"* ]]; then
        inactive=1
        break
    fi
    sleep 0.1
done
[[ "$inactive" == 1 ]] || {
    printf '%s\n' "$info_inactive" >"$introspection_log"
    fail 'extension did not return to inactive state'
}
wait_for_backend_state idle

# The inactive extension no longer owns the rebound toggle.
send_shortcut 'Clutter.KEY_Control_L, Clutter.KEY_Super_L' 'Clutter.KEY_F10'
sleep 0.2
assert_backend_state idle

if rg -q '(JS ERROR|ERROR.*lemonvox@itomek\.com|SyntaxError)' "$shell_log"; then
    fail 'GNOME Shell logged an extension JavaScript error'
fi

printf 'PASS: Shell discovered, enabled, activated, disabled, and deactivated %s\n' "$uuid"
