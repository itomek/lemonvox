#!/usr/bin/env bash
# Isolated native Settings acceptance: no real microphone, HTTP or user profile.
set -euo pipefail
if [[ ${1:-} == --inside ]]; then
    gnome-shell --wayland --wayland-display=wayland-0 --headless --no-x11 --virtual-monitor=1280x800 --unsafe-mode > "$LEMONVOX_SETTINGS_TEST_ROOT/shell.log" 2>&1 &
    shell_pid=$!
    trap 'kill "$shell_pid" 2>/dev/null || true' EXIT
    for _ in $(seq 1 100); do [[ ! -S "$XDG_RUNTIME_DIR/wayland-0" ]] || break; sleep .1; done
    GDK_BACKEND=wayland WAYLAND_DISPLAY=wayland-0 GTK_A11Y=none GSETTINGS_BACKEND=memory GDK_DEBUG=no-portals "$2"
    exit
fi
binary=$(realpath "${1:?usage: settings_session_test.sh path/to/lemonvox_settings_session_test}")
root=$(mktemp -d /tmp/lemonvox-settings-acceptance.XXXXXX)
cleanup() {
    status=$?
    # Session helpers may still be releasing files after dbus-run-session exits.
    # Cleanup must preserve the executable's status rather than report a race.
    for _ in $(seq 1 20); do
        rm -rf -- "$root" 2>/dev/null || true
        [[ -e "$root" ]] || return "$status"
        sleep .1
    done
    printf 'WARNING: retained private test directory: %s\n' "$root" >&2
    return "$status"
}
trap cleanup EXIT
mkdir -m700 "$root/run" "$root/config" "$root/data" "$root/cache" "$root/state"
env -u DISPLAY -u WAYLAND_DISPLAY LEMONVOX_SETTINGS_TEST_ROOT="$root" XDG_RUNTIME_DIR="$root/run" XDG_CONFIG_HOME="$root/config" XDG_DATA_HOME="$root/data" XDG_CACHE_HOME="$root/cache" XDG_STATE_HOME="$root/state" GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 dbus-run-session -- bash "$0" --inside "$binary"
