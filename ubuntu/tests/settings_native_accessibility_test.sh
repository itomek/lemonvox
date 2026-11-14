#!/usr/bin/env bash
# Disposable GNOME Wayland + AT-SPI acceptance. No host desktop preferences.
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ "${1:-}" == '--session' ]]; then
    shift
    binary="$1"
    ui_binary="$2"
    output="$3"
    gnome-shell --wayland --wayland-display=wayland-0 --headless --no-x11 --virtual-monitor=1920x1600 --unsafe-mode >"$output/compositor.log" 2>&1 &
    shell_pid=$!
    trap 'kill "$shell_pid" 2>/dev/null || true' EXIT
    for _ in $(seq 1 100); do [[ -S "$XDG_RUNTIME_DIR/wayland-0" ]] && break; sleep .1; done
    for _ in $(seq 1 100); do
        if gdbus call --session --dest org.gnome.Mutter.DisplayConfig --object-path /org/gnome/Mutter/DisplayConfig --method org.gnome.Mutter.DisplayConfig.GetCurrentState >"$output/display-config.txt" 2>"$output/display-config-error.txt"; then break; fi
        sleep .1
    done
    test -s "$output/display-config.txt"
    cc -shared -fPIC -Wall -Wextra -Werror "$repo_root/ubuntu/tests/settings_connect_redirect.c" -ldl -o "$XDG_CACHE_HOME/connect-redirect.so"
    LEMONVOX_TEST_CONNECT_SHIM="$XDG_CACHE_HOME/connect-redirect.so" python3 "$repo_root/ubuntu/tests/settings_atspi_test.py" "$binary"
    for scale in 1 2; do
        for theme in light dark; do
            for page in General Models Advanced Diagnostics; do
                GDK_SCALE="$scale" "$ui_binary" "$output/$theme-${page,,}-scale$scale.png" "$theme" "$page" connected
            done
        done
    done
    exit
fi
binary="$(realpath "${1:?usage: settings_native_accessibility_test.sh APP UI_DRIVER OUTPUT}")"
ui_binary="$(realpath "${2:?missing UI driver}")"
output="$(realpath -m "${3:?missing evidence output directory}")"
mkdir -p "$output"
private_root="$(mktemp -d /tmp/lemonvox-settings-accessibility.XXXXXX)"
mkdir -m 700 "$private_root/run" "$private_root/data" "$private_root/config" "$private_root/cache" "$private_root/state"
# Some workspace mounts prohibit execution in the isolated session.
cp "$binary" "$private_root/lemonvox"
cp "$ui_binary" "$private_root/settings-ui"
chmod +x "$private_root/lemonvox" "$private_root/settings-ui"
sha256sum "$binary" "$private_root/lemonvox" >"$output/binary-sha256.txt"
printf 'Private session: %s\n' "$private_root"
env -u DISPLAY -u XAUTHORITY XDG_RUNTIME_DIR="$private_root/run" XDG_DATA_HOME="$private_root/data" XDG_CONFIG_HOME="$private_root/config" XDG_CACHE_HOME="$private_root/cache" XDG_STATE_HOME="$private_root/state" WAYLAND_DISPLAY=wayland-0 GDK_BACKEND=wayland GTK_A11Y=atspi GNOME_SHELL_SESSION_MODE=user XDG_CURRENT_DESKTOP=GNOME GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 dbus-run-session -- bash "$0" --session "$private_root/lemonvox" "$private_root/settings-ui" "$output"
