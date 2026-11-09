#!/usr/bin/env bash
# Launch a completely disposable GNOME 46 Wayland Shell, stage the extension
# beneath its private XDG data directory, and delegate assertions to the
# session-side runner. This never reads or writes the logged-in user's GNOME
# preferences, enabled extensions, runtime directory, or session bus. It may
# read ordinary system and user defaults, but all mutable GNOME/XDG locations
# used by the test are private.
set -euo pipefail

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repo_root="$(cd -- "$script_dir/../.." && pwd)"
readonly extension_source="$repo_root/ubuntu/extension"
readonly session_runner="$script_dir/extension_native_shell_session.sh"
readonly uuid='lemonvox@itomek.com'

for required in dbus-run-session gdbus gjs gsettings glib-compile-schemas gnome-shell rg fusermount3; do
    command -v "$required" >/dev/null || {
        printf 'SKIP: required native Shell test command is absent: %s\n' "$required"
        exit 77
    }
done

test_root="$(mktemp -d "${TMPDIR:-/tmp}/lemonvox-extension-shell.XXXXXX")"
cleanup() {
    if [[ "${KEEP_LEMONVOX_GNOME_TEST:-}" == '1' ]]; then
        printf 'Keeping isolated test directory: %s\n' "$test_root"
    else
        # Portal/GVFS helpers use only this private runtime directory. Detach
        # their exact test mounts before removing the mktemp-created root.
        fusermount3 -uz "$test_root/run/doc" 2>/dev/null || true
        fusermount3 -uz "$test_root/run/gvfs" 2>/dev/null || true
        # Session-bus services can finish releasing their private XDG runtime
        # directories just after dbus-run-session exits. Retry the known
        # mktemp-created path briefly rather than leaking a test directory.
        for _attempt in $(seq 1 20); do
            rm -rf -- "$test_root" 2>/dev/null || true
            [[ ! -e "$test_root" ]] && return
            sleep 0.1
        done
        printf 'WARNING: isolated test directory was not removed: %s\n' "$test_root" >&2
    fi
}
trap cleanup EXIT

mkdir -m 700 "$test_root/run" "$test_root/data" "$test_root/config" "$test_root/cache"
stage="$test_root/data/gnome-shell/extensions/$uuid"
mkdir -p "$stage"
cp -a "$extension_source/." "$stage/"
glib-compile-schemas --strict "$stage/schemas"

printf 'Starting isolated GNOME Shell 46 Wayland test\n'
session_log="$test_root/session.log"
if ! env -u DISPLAY -u XAUTHORITY \
    XDG_RUNTIME_DIR="$test_root/run" \
    XDG_DATA_HOME="$test_root/data" \
    XDG_CONFIG_HOME="$test_root/config" \
    XDG_CACHE_HOME="$test_root/cache" \
    WAYLAND_DISPLAY='wayland-0' \
    GDK_BACKEND='wayland' \
    GNOME_SHELL_SESSION_MODE='user' \
    XDG_CURRENT_DESKTOP='GNOME' \
    GIO_USE_VFS='local' \
    dbus-run-session -- bash "$session_runner" "$test_root" "$uuid" >"$session_log" 2>&1; then
    tail -160 "$session_log" >&2 || true
    exit 1
fi
rg '^PASS:' "$session_log"

printf 'PASS: isolated GNOME Shell extension enable/disable test\n'
