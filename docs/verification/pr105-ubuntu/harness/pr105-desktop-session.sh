#!/bin/bash
set -euo pipefail
gnome-shell --wayland --wayland-display=wayland-0 --headless --no-x11 --virtual-monitor=1280x900 --unsafe-mode >"$XDG_CACHE_HOME/shell.log" 2>&1 &
shell_pid=$!
trap 'kill "$shell_pid" 2>/dev/null || true; wait "$shell_pid" 2>/dev/null || true' EXIT
for i in $(seq 1 100); do
 if gdbus call --session --dest org.gnome.Shell --object-path /org/gnome/Shell --method org.gnome.Shell.Eval '1+1' >/dev/null 2>&1; then break; fi
 sleep .1
done
python3 /tmp/pr105-desktop-service-proof.py "$1"
