#!/usr/bin/env bash
# Runs explicit Copy -> paste checks against disposable Chrome and Electron
# Wayland clients inside a private GNOME Shell. It never uses the live desktop,
# a real browser profile, microphone hardware, or X11.
set -euo pipefail

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repo_root="$(cd -- "$script_dir/../.." && pwd)"
readonly extension_source="$repo_root/ubuntu/extension"
readonly session_runner="$script_dir/extension_native_matrix_session.sh"
readonly uuid='lemonvox@itomek.com'
readonly chrome_binary='/usr/bin/google-chrome'
readonly electron_version='40.10.2'

for required in dbus-run-session gdbus gjs gsettings glib-compile-schemas \
    gnome-shell node npm rg unzip fusermount3; do
    command -v "$required" >/dev/null || {
        printf 'SKIP: required matrix command is absent: %s\n' "$required"
        exit 77
    }
done
[[ -x "$chrome_binary" ]] || {
    printf 'SKIP: required Chrome binary is absent: %s\n' "$chrome_binary"
    exit 77
}

test_root="$(mktemp -d "${TMPDIR:-/tmp}/lemonvox-extension-matrix.XXXXXX")"
cleanup() {
    if [[ "${KEEP_LEMONVOX_GNOME_TEST:-}" == '1' ]]; then
        printf 'Keeping isolated matrix directory: %s\n' "$test_root"
    else
        # Local Chromium starts portal/GVFS helpers in this private runtime
        # directory. Lazily detach only their exact test mounts before
        # deleting the mktemp root; neither path can name a host mount.
        fusermount3 -uz "$test_root/run/doc" 2>/dev/null || true
        fusermount3 -uz "$test_root/run/gvfs" 2>/dev/null || true
        # Chromium child processes can finish releasing their disposable
        # profile just after the private session exits. Retry only the exact
        # mktemp-created root instead of leaking test data.
        for _attempt in $(seq 1 20); do
            rm -rf -- "$test_root" 2>/dev/null || true
            [[ ! -e "$test_root" ]] && return
            sleep 0.1
        done
        printf 'WARNING: isolated matrix directory was not removed: %s\n' "$test_root" >&2
    fi
}
trap cleanup EXIT

mkdir -m 700 "$test_root/run" "$test_root/data" "$test_root/config" "$test_root/cache"
stage="$test_root/data/gnome-shell/extensions/$uuid"
mkdir -p "$stage"
cp -a "$extension_source/." "$stage/"
glib-compile-schemas --strict "$stage/schemas"

# Electron is test-only and always unpacked into this temporary root. Prefer a
# pre-existing fixed-version archive, then make one bounded npm attempt into
# the same root if the archive is not available. Neither route alters this
# project nor a real user profile.
account_home="$(getent passwd "$(id -u)" | cut -d: -f6)"
cache_archive=''
if [[ -n "$account_home" && -d "$account_home/.cache/electron" ]]; then
    cache_archive="$(find "$account_home/.cache/electron" -type f -name "electron-v${electron_version}-linux-x64.zip" -print -quit)"
fi
if [[ -n "$cache_archive" ]]; then
    unzip -q "$cache_archive" -d "$test_root/electron-runtime"
    electron_binary="$test_root/electron-runtime/electron"
else
    printf 'Electron %s archive not cached; obtaining test-only npm runtime\n' "$electron_version"
    mkdir -p "$test_root/electron-package"
    XDG_CACHE_HOME="$test_root/cache" \
    electron_config_cache="$test_root/electron-download-cache" \
    npm --prefix "$test_root/electron-package" \
        --cache "$test_root/npm-cache" \
        install --no-save --package-lock=false "electron@$electron_version"
    electron_binary="$test_root/electron-package/node_modules/electron/dist/electron"
fi
[[ -x "$electron_binary" ]] || {
    printf 'FAIL: Electron %s test runtime was not provisioned\n' "$electron_version" >&2
    exit 1
}
electron_runtime_version="$($electron_binary --version)"

printf 'Starting isolated GNOME Shell browser/Electron matrix\n'
session_log="$test_root/session.log"
session_shell=(bash)
if [[ "${LEMONVOX_MATRIX_TRACE:-}" == '1' ]]; then
    session_shell=(bash -x)
fi
if ! env -u DISPLAY -u XAUTHORITY \
    XDG_RUNTIME_DIR="$test_root/run" \
    XDG_DATA_HOME="$test_root/data" \
    XDG_CONFIG_HOME="$test_root/config" \
    XDG_CACHE_HOME="$test_root/cache" \
    WAYLAND_DISPLAY='wayland-0' \
    GDK_BACKEND='wayland' \
    LEMONVOX_MATRIX_CHROME="$chrome_binary" \
    LEMONVOX_MATRIX_ELECTRON="$electron_binary" \
    LEMONVOX_MATRIX_ELECTRON_VERSION="$electron_runtime_version" \
    GNOME_SHELL_SESSION_MODE='user' \
    XDG_CURRENT_DESKTOP='GNOME' \
    GIO_USE_VFS='local' \
    dbus-run-session -- "${session_shell[@]}" "$session_runner" "$test_root" "$uuid" >"$session_log" 2>&1; then
    tail -160 "$session_log" >&2 || true
    exit 1
fi
rg '^PASS:' "$session_log"
printf 'PASS: isolated Wayland browser/Electron explicit Copy matrix\n'
