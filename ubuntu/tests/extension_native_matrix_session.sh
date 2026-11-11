#!/usr/bin/env bash
# Runs inside extension_native_matrix_test.sh's disposable D-Bus/XDG session.
# It proves manual Copy and Ctrl+V against real Chrome and Electron Wayland
# clients. Never invoke it against a logged-in desktop session.
set -euo pipefail

readonly test_root="$1"
readonly uuid="$2"
readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly backend_script="$script_dir/extension_native_shell_backend.mjs"
readonly server_script="$script_dir/extension_native_matrix_server.mjs"
readonly electron_fixture="$script_dir/extension_native_matrix_electron.cjs"
readonly chrome_binary="${LEMONVOX_MATRIX_CHROME:?matrix Chrome binary is required}"
readonly electron_binary="${LEMONVOX_MATRIX_ELECTRON:?matrix Electron binary is required}"
readonly electron_version="${LEMONVOX_MATRIX_ELECTRON_VERSION:?matrix Electron version is required}"
readonly shell_log="$test_root/gnome-shell.log"
readonly introspection_log="$test_root/matrix-introspection.log"
readonly events_file="$test_root/browser-events.ndjson"
readonly synthetic_copy_text='Synthetic manual copy text'

shell_pid=''
backend_pid=''
server_pid=''
chrome_pid=''
electron_pid=''

terminate_group() {
    local pid="${1:-}"
    [[ -n "$pid" ]] || return
    kill -TERM -- "-$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

cleanup() {
    terminate_group "$electron_pid"
    terminate_group "$chrome_pid"
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
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

print_diagnostics() {
    printf '\n--- matrix event collector ---\n' >&2
    cat "$events_file" >&2 || true
    printf '\n--- Chrome log (last 80 lines) ---\n' >&2
    tail -80 "$test_root/chrome.log" >&2 || true
    printf '\n--- Electron log (last 80 lines) ---\n' >&2
    tail -80 "$test_root/electron.log" >&2 || true
    printf '\n--- GNOME Shell log (last 80 lines) ---\n' >&2
    tail -80 "$shell_log" >&2 || true
    printf '\n--- Shell D-Bus probe ---\n' >&2
    cat "$introspection_log" >&2 || true
}

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    print_diagnostics
    exit 1
}

[[ -z "${DISPLAY-}" && -z "${XAUTHORITY-}" ]] || fail 'matrix inherited host X11 environment'
[[ "${WAYLAND_DISPLAY-}" == 'wayland-0' ]] || fail 'matrix did not receive its private Wayland display'
[[ "${GDK_BACKEND-}" == 'wayland' ]] || fail 'matrix did not force native Wayland clients'

shell_eval() {
    gdbus call --session \
        --dest org.gnome.Shell \
        --object-path /org/gnome/Shell \
        --method org.gnome.Shell.Eval "$1"
}

backend_result() {
    gdbus call --session \
        --dest com.itomek.LemonVox \
        --object-path /com/itomek/LemonVox \
        --method org.freedesktop.DBus.Properties.Get \
        com.itomek.LemonVox Result
}

wait_for_eval() {
    local script="$1"
    local expected="$2"
    local observed=''
    for _attempt in $(seq 1 120); do
        observed="$(shell_eval "$script" 2>/dev/null || true)"
        if [[ "$observed" == *"$expected"* ]]; then
            return
        fi
        sleep 0.05
    done
    printf '%s\n' "$observed" >"$introspection_log"
    fail "Shell Eval did not report $expected"
}

wait_for_backend_result() {
    local expected="$1"
    local observed=''
    for _attempt in $(seq 1 120); do
        observed="$(backend_result 2>/dev/null || true)"
        if [[ "$observed" == *"'$expected'"* ]]; then
            return
        fi
        sleep 0.05
    done
    printf '%s\n' "$observed" >"$introspection_log"
    fail 'synthetic backend result did not reach the expected value'
}

event_matches() {
    local client="$1"
    local event_type="$2"
    local value="${3-}"
    [[ -f "$events_file" ]] || return 1
    rg -Fq -- "\"client\":\"$client\",\"type\":\"$event_type\",\"value\":\"$value\"" "$events_file"
}

wait_for_event() {
    local client="$1"
    local event_type="$2"
    local value="${3-}"
    for _attempt in $(seq 1 120); do
        if event_matches "$client" "$event_type" "$value"; then
            return
        fi
        sleep 0.05
    done
    fail "${client} fixture did not report ${event_type}"
}

event_offset() {
    if [[ -f "$events_file" ]]; then
        wc -l <"$events_file" | tr -d ' '
    else
        printf '0'
    fi
}

assert_no_event_since() {
    local start="$1"
    local client="$2"
    local event_type="$3"
    local current='0'
    current="$(event_offset)"
    (( current > start )) || return 0
    if tail -n "+$((start + 1))" "$events_file" | rg -Fq -- "\"client\":\"$client\",\"type\":\"$event_type\""; then
        fail "${client} fixture reported unexpected ${event_type} after explicit Copy began"
    fi
}

prepare_virtual_keyboard() {
    local prepared=''
    prepared="$(shell_eval \
        "import('gi://Clutter').then(({default: Clutter}) => { global.__lemonVoxMatrixVirtualKeyboard ??= Clutter.get_default_backend().get_default_seat().create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE); return global.__lemonVoxMatrixVirtualKeyboard ? 'virtual-keyboard-ready' : 'virtual-keyboard-missing'; })")" || fail 'could not create the private virtual keyboard'
    [[ "$prepared" == *virtual-keyboard-ready* ]] || fail 'private Shell did not create the virtual keyboard'
}

send_shortcut() {
    local modifiers="$1"
    local key="$2"
    local injected=''
    injected="$(shell_eval \
        "Promise.all([import('gi://Clutter'), import('gi://GLib')]).then(([{default: Clutter}, {default: GLib}]) => { const device = global.__lemonVoxMatrixVirtualKeyboard ??= Clutter.get_default_backend().get_default_seat().create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE); return new Promise(resolve => GLib.idle_add(GLib.PRIORITY_DEFAULT, () => { let time = GLib.get_monotonic_time(); for (const keyval of [$modifiers]) device.notify_keyval(time++, keyval, Clutter.KeyState.PRESSED); device.notify_keyval(time++, $key, Clutter.KeyState.PRESSED); device.notify_keyval(time++, $key, Clutter.KeyState.RELEASED); for (const keyval of [$modifiers].reverse()) device.notify_keyval(time++, keyval, Clutter.KeyState.RELEASED); resolve('shortcut-injected'); return GLib.SOURCE_REMOVE; })); })")" || fail 'could not inject a private virtual keyboard shortcut'
    [[ "$injected" == *shortcut-injected* ]] || fail 'private Shell did not accept virtual keyboard injection'
}

wait_for_clipboard() {
    local expected="$1"
    wait_for_eval \
        "import('gi://St').then(({default: St}) => new Promise(resolve => St.Clipboard.get_default().get_text(St.ClipboardType.CLIPBOARD, (_clipboard, text) => resolve(text === '$expected' ? 'clipboard-ready' : 'clipboard-pending'))))" \
        clipboard-ready
}

clear_clipboard() {
    local cleared=''
    cleared="$(shell_eval \
        "import('gi://St').then(({default: St}) => { St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, ''); return 'clipboard-cleared'; })")" || fail 'could not clear the private Shell clipboard'
    [[ "$cleared" == *clipboard-cleared* ]] || fail 'private Shell did not clear the clipboard'
    wait_for_eval \
        "import('gi://St').then(({default: St}) => new Promise(resolve => St.Clipboard.get_default().get_text(St.ClipboardType.CLIPBOARD, (_clipboard, text) => resolve(text === '' ? 'clipboard-empty' : 'clipboard-not-empty'))))" \
        clipboard-empty
}

wait_for_wayland_window() {
    local client="$1"
    local title="$2"
    wait_for_eval \
        "import('gi://Meta').then(({default: Meta}) => { const window = global.get_window_actors().map(actor => actor.meta_window).find(candidate => candidate?.get_title()?.includes('$title')); if (!window) return '$client-missing'; return window.get_client_type() === Meta.WindowClientType.WAYLAND ? '$client-wayland' : '$client-not-wayland'; })" \
        "$client-wayland"
}

focus_client_window() {
    local client="$1"
    local title="$2"
    local focused=''
    focused="$(shell_eval \
        "import('resource:///org/gnome/shell/ui/main.js').then(Main => { const window = global.get_window_actors().map(actor => actor.meta_window).find(candidate => candidate?.get_title()?.includes('$title')); if (!window) return '$client-missing'; Main.activateWindow(window, global.get_current_time()); return '$client-activation-requested'; })")" || fail "could not activate ${client} fixture"
    [[ "$focused" == *"$client-activation-requested"* ]] || fail "private Shell could not find ${client} fixture"
    wait_for_eval \
        "global.display.focus_window?.get_title()?.includes('$title') ? '$client-focused' : '$client-not-focused'" \
        "$client-focused"
}

assert_client_focus() {
    local client="$1"
    local title="$2"
    wait_for_eval \
        "global.display.focus_window?.get_title()?.includes('$title') ? '$client-focused' : '$client-not-focused'" \
        "$client-focused"
}

wait_for_copy_bar() {
    local client="$1"
    local title="$2"
    wait_for_eval \
        "import('resource:///org/gnome/shell/ui/main.js').then(Main => { const extension = Main.extensionManager.lookup('$uuid').stateObj; const bar = extension?._bar; return bar?.actor.visible && bar._copy.visible && !bar.actor.can_focus && global.display.focus_window?.get_title()?.includes('$title') ? '$client-copy-bar-focus-preserved' : '$client-copy-bar-check-failed'; })" \
        "$client-copy-bar-focus-preserved"
}

start_server() {
    local ready_file="$test_root/collector-ready.txt"
    node "$server_script" "$ready_file" "$events_file" >"$test_root/collector.log" 2>&1 &
    server_pid="$!"
    for _attempt in $(seq 1 120); do
        if [[ -s "$ready_file" ]]; then
            fixture_url="$(<"$ready_file")"
            return
        fi
        if ! kill -0 "$server_pid" 2>/dev/null; then
            cat "$test_root/collector.log" >"$introspection_log" || true
            fail 'local browser event collector exited before becoming ready'
        fi
        sleep 0.05
    done
    fail 'timed out waiting for local browser event collector'
}

start_chrome() {
    readonly chrome_title='LemonVox Chrome Fixture'
    setsid "$chrome_binary" \
        --ozone-platform=wayland \
        --enable-features=UseOzonePlatform \
        --user-data-dir="$test_root/chrome-profile" \
        --no-first-run \
        --no-default-browser-check \
        --disable-background-networking \
        --disable-component-update \
        --disable-sync \
        --disable-extensions \
        --disable-background-mode \
        --password-store=basic \
        --use-mock-keychain \
        "${fixture_url}?client=chrome" >"$test_root/chrome.log" 2>&1 &
    chrome_pid="$!"
    wait_for_event chrome ready
    wait_for_wayland_window chrome "$chrome_title"
    focus_client_window chrome "$chrome_title"
    wait_for_event chrome focus
}

start_electron() {
    readonly electron_title='LemonVox Electron Fixture'
    readonly electron_ready_file="$test_root/electron-ready.txt"
    setsid "$electron_binary" \
        --ozone-platform=wayland \
        --enable-features=UseOzonePlatform \
        --disable-gpu \
        "$electron_fixture" \
        "${fixture_url}?client=electron" \
        "$electron_ready_file" \
        "$test_root/electron-profile" >"$test_root/electron.log" 2>&1 &
    electron_pid="$!"
    for _attempt in $(seq 1 120); do
        if [[ -s "$electron_ready_file" ]]; then
            break
        fi
        if ! kill -0 "$electron_pid" 2>/dev/null; then
            fail "Electron ${electron_version} exited before loading its private fixture"
        fi
        sleep 0.05
    done
    [[ -s "$electron_ready_file" ]] || fail 'timed out waiting for Electron to load its private fixture'
    wait_for_event electron ready
    wait_for_wayland_window electron "$electron_title"
    focus_client_window electron "$electron_title"
    wait_for_event electron focus
}

exercise_automatic_paste() {
    local client="$1"
    local title="$2"
    local start=''
    local input_count=''

    assert_client_focus "$client" "$title"
    clear_clipboard
    start="$(event_offset)"
    # Arm a fresh operation even when two recordings produce identical text.
    gdbus call --session --dest com.itomek.LemonVox \
        --object-path /com/itomek/LemonVox \
        --method com.itomek.LemonVox.Toggle >/dev/null
    wait_for_eval \
        "import('resource:///org/gnome/shell/ui/main.js').then(Main => Main.extensionManager.lookup('$uuid').stateObj._client.snapshot.state === 'recording' ? 'recording-ready' : 'recording-pending')" \
        recording-ready
    assert_no_event_since "$start" "$client" input
    kill -USR2 "$backend_pid"
    wait_for_event "$client" input "$synthetic_copy_text"
    wait_for_backend_result ''
    assert_no_event_since "$start" "$client" submit
    assert_no_event_since "$start" "$client" blur
    assert_client_focus "$client" "$title"
    sleep 0.2
    input_count="$(tail -n "+$((start + 1))" "$events_file" | rg -Fc -- "\"client\":\"$client\",\"type\":\"input\"")"
    [[ "$input_count" == 1 ]] || fail "$client received more than one automatic paste"
    wait_for_eval \
        "import('resource:///org/gnome/shell/ui/main.js').then(Main => Main.extensionManager.lookup('$uuid').stateObj._bar.actor.visible ? '$client-result-bar-still-visible' : '$client-result-bar-hidden')" \
        "$client-result-bar-hidden"
    printf 'PASS: %s Wayland field received one automatic paste without submit or focus change\n' "$client"
}

# --unsafe-mode applies only to this fresh, headless test compositor; it
# exposes Eval for controlled actor and virtual-keyboard assertions.
gnome-shell --wayland --wayland-display=wayland-0 --headless --no-x11 --virtual-monitor=1280x800 --unsafe-mode >"$shell_log" 2>&1 &
shell_pid="$!"

shell_ready=0
for _attempt in $(seq 1 120); do
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
    sleep 0.05
done
[[ "$shell_ready" == 1 ]] || fail 'timed out waiting for private Shell extension interface'
prepare_virtual_keyboard

backend_ready_file="$test_root/synthetic-backend-ready.txt"
gjs -m "$backend_script" "$backend_ready_file" >"$test_root/synthetic-backend.log" 2>&1 &
backend_pid="$!"
for _attempt in $(seq 1 120); do
    if [[ -s "$backend_ready_file" && "$(<"$backend_ready_file")" == 'ready' ]]; then
        break
    fi
    if ! kill -0 "$backend_pid" 2>/dev/null; then
        cat "$test_root/synthetic-backend.log" >"$introspection_log" || true
        fail 'synthetic backend exited before owning its D-Bus name'
    fi
    sleep 0.05
done
[[ -s "$backend_ready_file" ]] || fail 'timed out waiting for synthetic backend ownership'

enable_result="$(gdbus call --session \
    --dest org.gnome.Shell \
    --object-path /org/gnome/Shell \
    --method org.gnome.Shell.Extensions.EnableExtension "$uuid")" || fail 'GNOME Shell rejected staged extension enable'
[[ "$enable_result" == *true* ]] || fail 'GNOME Shell reported staged extension enable failure'
wait_for_eval \
    "import('resource:///org/gnome/shell/ui/main.js').then(Main => Main.extensionManager.lookup('$uuid').stateObj._client.snapshot.available ? 'extension-backend-ready' : 'extension-backend-pending')" \
    extension-backend-ready

start_server
start_chrome
exercise_automatic_paste chrome "$chrome_title"
terminate_group "$chrome_pid"
chrome_pid=''

start_electron
exercise_automatic_paste electron "$electron_title"

if rg -q '(JS ERROR|ERROR.*lemonvox@itomek\.com|SyntaxError)' "$shell_log"; then
    fail 'GNOME Shell logged an extension JavaScript error during the matrix'
fi

printf 'PASS: native Chrome %s and Electron %s Wayland Copy matrix\n' \
    "$("$chrome_binary" --version)" "$electron_version"
