# LemonVox for Ubuntu

Requires Ubuntu 24.04 GNOME Wayland (amd64 or arm64).

## Install

LemonVox is available from our signed APT repository, not Ubuntu's default
repositories. Add it once using the setup below, then install:

```sh
sudo apt update
sudo apt install lemonvox
```

### Add the repository once

<details>
<summary>Show repository setup commands</summary>

The commands verify the signing key and add a package source restricted to it.
The expected fingerprint is `D873B423E88EA3EFF8D769E47EE4BCB0334EC5FE`.
Stop if verification fails.

```sh
sudo apt install ca-certificates curl gnupg
key=$(mktemp)
trap 'rm -f "$key"' EXIT
curl --fail --silent --show-error --location \
  https://itomek.github.io/lemonvox/apt/lemonvox-archive-keyring.gpg --output "$key"
fingerprints=$(gpg --batch --show-keys --with-colons "$key" 2>/dev/null | \
  awk -F: '$1 == "pub" { primary=1; next } primary && $1 == "fpr" { print $10; primary=0 }')
if test "$fingerprints" != D873B423E88EA3EFF8D769E47EE4BCB0334EC5FE; then
  echo 'Archive signing key fingerprint mismatch.' >&2
  exit 1
fi
sudo install -d -m 0755 /etc/apt/keyrings
sudo install -m 0644 "$key" /etc/apt/keyrings/lemonvox-archive-keyring.gpg
sudo tee /etc/apt/sources.list.d/lemonvox.sources > /dev/null <<'SOURCE'
Types: deb
URIs: https://itomek.github.io/lemonvox/apt/
Suites: stable
Components: main
Signed-By: /etc/apt/keyrings/lemonvox-archive-keyring.gpg
SOURCE
sudo apt update
sudo apt install lemonvox
```

</details>

### Updates

After setup, Ubuntu's APT updates can update LemonVox. To update it manually:

```sh
sudo apt update
sudo apt install --only-upgrade lemonvox
```

Quit LemonVox before updating. After an extension update, log out and back in.

## Desktop setup

APT installs the required libraries automatically. Alternatively, install a
[downloaded `.deb`](https://github.com/itomek/lemonvox/releases) with `sudo apt install ./lemonvox_*.deb`.

Log out and back in, then enable the desktop integration:

```sh
gnome-extensions enable lemonvox@itomek.com
```

Open LemonVox and [connect Lemonade and select models](../docs/model-setup.md).
Press **Ctrl+Alt+Space** to start and stop; **Ctrl+Super+Escape** cancels.

[Cleanup settings](../docs/cleanup.md) · [Build, test and packaging](DEVELOPMENT.md) · [Report an issue](https://github.com/itomek/lemonvox/issues)

[Native screenshot comparison and design verification status](../docs/desktop-design.md)
