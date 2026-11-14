#!/bin/bash
set -euo pipefail
root=$(mktemp -d /tmp/pr105-production-desktop.XXXXXX)
mkdir -m700 "$root/run" "$root/data" "$root/config" "$root/cache"
stage="$root/data/gnome-shell/extensions/lemonvox@itomek.com"
mkdir -p "$stage"
cp -a ubuntu/extension/. "$stage/"
glib-compile-schemas --strict "$stage/schemas"
printf 'Private desktop root: %s\n' "$root"
env -u DISPLAY -u XAUTHORITY XDG_RUNTIME_DIR="$root/run" XDG_DATA_HOME="$root/data" XDG_CONFIG_HOME="$root/config" XDG_CACHE_HOME="$root/cache" WAYLAND_DISPLAY=wayland-0 GDK_BACKEND=wayland GNOME_SHELL_SESSION_MODE=user XDG_CURRENT_DESKTOP=GNOME GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 dbus-run-session -- bash /tmp/pr105-desktop-session.sh "$PWD/.build/ubuntu-102/lemonvox"
