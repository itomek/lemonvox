#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'USAGE'
Usage: bash scripts/build-ubuntu.sh --output ABSOLUTE_NEW_DIRECTORY --version X.Y.Z [--build-dir ABSOLUTE_DIRECTORY]

Builds and tests the Ubuntu GNOME Wayland host, then creates a local Ubuntu
.deb artifact. It never installs packages or changes the running desktop.
USAGE
}

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT=
VERSION=
BUILD_DIR="$ROOT/.build/ubuntu"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output) [[ $# -ge 2 ]] || { usage; exit 2; }; OUTPUT="$2"; shift 2 ;;
        --version) [[ $# -ge 2 ]] || { usage; exit 2; }; VERSION="$2"; shift 2 ;;
        --build-dir) [[ $# -ge 2 ]] || { usage; exit 2; }; BUILD_DIR="$2"; shift 2 ;;
        --help) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done

[[ "$OUTPUT" == /* && "$BUILD_DIR" == /* ]] || { echo 'Output and build directories must be absolute.' >&2; exit 2; }
[[ -n "$VERSION" && "$VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || { echo 'Version must be semantic X.Y.Z.' >&2; exit 2; }
[[ ! -e "$OUTPUT" && ! -L "$OUTPUT" ]] || { echo 'Output directory must be new; existing paths are preserved.' >&2; exit 2; }

for command in cmake ctest dpkg-deb dpkg glib-compile-schemas sha256sum; do
    command -v "$command" >/dev/null || { echo "Missing required command: $command" >&2; exit 1; }
done

EXTENSION="$ROOT/ubuntu/extension"
[[ -f "$EXTENSION/metadata.json" ]] || { echo 'Missing GNOME Shell extension metadata.' >&2; exit 1; }
[[ -d "$EXTENSION/schemas" ]] || { echo 'Missing GNOME Shell extension schemas.' >&2; exit 1; }
compgen -G "$EXTENSION/schemas/*.gschema.xml" >/dev/null || { echo 'Missing GNOME Shell extension schema XML.' >&2; exit 1; }

SOURCE_REVISION="$(git -C "$ROOT" rev-parse HEAD)"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" show -s --format=%ct HEAD)}"
[[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] || { echo 'SOURCE_DATE_EPOCH must be an integer Unix timestamp.' >&2; exit 2; }
export SOURCE_DATE_EPOCH
if [[ -z "$(git -C "$ROOT" status --porcelain)" ]]; then
    SOURCE_DIRTY=false
else
    SOURCE_DIRTY=true
fi

cmake -S "$ROOT/ubuntu" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -UNDEBUG" \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$BUILD_DIR" \
    -DLEMONVOX_VERSION="$VERSION"
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure

APP="$BUILD_DIR/lemonvox"
[[ -x "$APP" ]] || { echo "Expected executable at $APP" >&2; exit 1; }

ARCH="$(dpkg --print-architecture)"
PACKAGE_NAME="lemonvox_${VERSION}_${ARCH}.deb"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/lemonvox-deb.XXXXXX")"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

mkdir "$OUTPUT"
mkdir -p "$STAGE/DEBIAN" "$STAGE/usr/bin" \
    "$STAGE/usr/share/applications" "$STAGE/usr/share/dbus-1/services" \
    "$STAGE/usr/share/doc/lemonvox" "$STAGE/usr/share/gnome-shell/extensions"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@ARCH@/$ARCH/g" \
    "$ROOT/ubuntu/packaging/control.in" > "$STAGE/DEBIAN/control"
install -m 0755 "$APP" "$STAGE/usr/bin/lemonvox"
install -m 0644 "$ROOT/ubuntu/packaging/com.itomek.LemonVox.desktop" \
    "$STAGE/usr/share/applications/com.itomek.LemonVox.desktop"
install -m 0644 "$ROOT/ubuntu/packaging/com.itomek.LemonVox.service" \
    "$STAGE/usr/share/dbus-1/services/com.itomek.LemonVox.service"
install -d "$STAGE/usr/share/gnome-shell/extensions/lemonvox@itomek.com"
cp -a "$EXTENSION/." "$STAGE/usr/share/gnome-shell/extensions/lemonvox@itomek.com/"
glib-compile-schemas "$STAGE/usr/share/gnome-shell/extensions/lemonvox@itomek.com/schemas"
install -m 0644 "$ROOT/LICENSE" "$STAGE/usr/share/doc/lemonvox/copyright"
install -m 0644 "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/usr/share/doc/lemonvox/THIRD_PARTY_NOTICES"
install -m 0644 "$ROOT/third_party/yyjson/LICENSE" "$STAGE/usr/share/doc/lemonvox/yyjson-LICENSE"
printf 'version: %s\narchitecture: %s\nsource_revision: %s\nsource_dirty: %s\nsource_date_epoch: %s\nscope: Ubuntu 24.04+ GNOME Wayland only; X11 is unsupported.\nlemonade: install and run Lemonade separately.\n' \
    "$VERSION" "$ARCH" "$SOURCE_REVISION" "$SOURCE_DIRTY" "$SOURCE_DATE_EPOCH" > "$STAGE/usr/share/doc/lemonvox/build-info.txt"
# Normalize every staged path, including the generated schema cache.  Combined
# with dpkg-deb's SOURCE_DATE_EPOCH support, this keeps source checkout mtimes
# out of both tar members rather than merely clamping newly-created files.
find "$STAGE" -exec touch -h --date="@$SOURCE_DATE_EPOCH" {} +

dpkg-deb --root-owner-group --uniform-compression --build "$STAGE" "$OUTPUT/$PACKAGE_NAME"
sha256sum "$OUTPUT/$PACKAGE_NAME" > "$OUTPUT/SHA256SUMS-ubuntu-$ARCH.txt"
printf '%s\narchitecture: %s\nsource: %s\nsource_dirty: %s\nartifact: %s\ninstall: sudo apt install ./%s\n' \
    "$VERSION" "$ARCH" "$SOURCE_REVISION" "$SOURCE_DIRTY" "$PACKAGE_NAME" "$PACKAGE_NAME" > "$OUTPUT/VERSION-ubuntu-$ARCH.txt"

echo "Built $OUTPUT/$PACKAGE_NAME"
