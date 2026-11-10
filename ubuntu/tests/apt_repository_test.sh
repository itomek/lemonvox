#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
cleanup() {
    GNUPGHOME="$tmp/gnupg" gpgconf --kill gpg-agent 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT
chmod 755 "$tmp"

architecture=$(dpkg --print-architecture)
for version in 0.1.0 0.2.0; do
    mkdir -p "$tmp/stage/DEBIAN"
    cat > "$tmp/stage/DEBIAN/control" <<CONTROL
Package: lemonvox
Version: $version
Architecture: $architecture
Maintainer: LemonVox test <test@example.invalid>
Description: APT repository test fixture
CONTROL
    dpkg-deb --root-owner-group --build "$tmp/stage" "$tmp/lemonvox_$version.deb"
done

export GNUPGHOME="$tmp/gnupg"
mkdir -m 700 "$GNUPGHOME"
gpg --batch --passphrase '' --quick-generate-key 'LemonVox APT test <apt@example.invalid>' ed25519 sign 1d
fingerprint=$(gpg --batch --with-colons --list-secret-keys | awk -F: '$1 == "fpr" { print $10; exit }')
repository="$tmp/repository"
bash "$root/scripts/build-ubuntu-apt-repository.sh" "$repository" "$fingerprint" "$tmp/"*.deb

mkdir -p "$tmp/apt/lists/partial" "$tmp/apt/cache/archives/partial" "$tmp/apt/empty"
chmod 755 "$tmp/apt" "$tmp/apt/lists" "$tmp/apt/cache" "$tmp/apt/cache/archives"
touch "$tmp/apt/status"
cat > "$tmp/apt/lemonvox.sources" <<SOURCE
Types: deb
URIs: file:$repository
Suites: stable
Components: main
Signed-By: $repository/lemonvox-archive-keyring.gpg
SOURCE
cat > "$tmp/apt/apt.conf" <<CONFIG
Dir::Etc::main "/dev/null";
Dir::Etc::parts "$tmp/apt/empty";
Dir::Etc::sourcelist "$tmp/apt/lemonvox.sources";
Dir::Etc::sourceparts "$tmp/apt/empty";
Dir::Etc::preferences "/dev/null";
Dir::Etc::preferencesparts "$tmp/apt/empty";
Dir::State::lists "$tmp/apt/lists";
Dir::State::status "$tmp/apt/status";
Dir::Cache "$tmp/apt/cache";
APT::Architecture "$architecture";
APT::Get::List-Cleanup "false";
APT::Update::Error-Mode "any";
CONFIG
export APT_CONFIG="$tmp/apt/apt.conf"
export LC_ALL=C

apt-get update
apt-get --simulate install lemonvox > "$tmp/install.txt"
grep -F 'Inst lemonvox (0.2.0 LemonVox:stable' "$tmp/install.txt"
(
    cd "$tmp"
    apt-get download lemonvox
    cmp "lemonvox_0.2.0_$architecture.deb" lemonvox_0.2.0.deb
)

dpkg-deb -f "$tmp/lemonvox_0.1.0.deb" > "$tmp/apt/status"
printf 'Status: install ok installed\n\n' >> "$tmp/apt/status"
apt-get --simulate install lemonvox > "$tmp/upgrade.txt"
grep -F 'Inst lemonvox [0.1.0] (0.2.0 LemonVox:stable' "$tmp/upgrade.txt"

# Remove the cached index and prove APT rejects the repository when the
# trusted keyring does not contain the signer.
: > "$repository/lemonvox-archive-keyring.gpg"
rm -f "$tmp/apt/lists/"*InRelease
if apt-get update > "$tmp/untrusted.txt" 2>&1; then
    echo 'APT accepted a repository without its signing key.' >&2
    exit 1
fi
grep -F 'NO_PUBKEY' "$tmp/untrusted.txt"
