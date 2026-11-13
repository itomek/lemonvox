# Ubuntu package artifacts

Run `bash scripts/build-ubuntu.sh --output /absolute/new-directory --version X.Y.Z` to build the Ubuntu host, run CTest, and write a local `.deb`, checksum, and source metadata. It does not install the artifact or change the desktop.

Install a reviewed local artifact on the tested Ubuntu 24.04+ GNOME Wayland target:

```sh
sudo apt install ./lemonvox_X.Y.Z_amd64.deb
```

The package installs the GTK application, desktop entry, user D-Bus activation file, and `lemonvox@itomek.com` GNOME Shell extension. Enable the extension in GNOME Extensions before relying on global shortcut delivery. Install and run Lemonade or another OpenAI-compatible server separately, locally or remotely. It must provide audio transcription, chat completions and model-discovery metadata supported by LemonVox. Local and remote HTTP/HTTPS endpoints are accepted; a generic OpenAI-compatible API alone is insufficient. Lemonade 2026.44.1 is the tested baseline; other servers are unverified. The package does not bundle a server or models. Server installation, model download and loading into memory are separate steps. Settings offers an explicit confirmed model download through supported server management APIs; otherwise use the server’s model manager and Refresh models. No health check or launch starts a download. Downloads may need several GB of disk space and write permission on the server.

When processing finishes, the GNOME extension pastes once at current focus with Ctrl+V and never sends Enter. Copy remains available if paste cannot be dispatched.

These package-source descriptions apply to the next build. Published GitHub `.deb` assets and APT package versions, including 0.2.1, are immutable and are not rewritten by a documentation change.

The package does not support X11, Ubuntu versions before 24.04, or other Linux distributions. `sudo apt remove lemonvox` removes packaged files; user settings in the XDG configuration directory are intentionally preserved so an upgrade retains them.

The adjacent `snap/` recipe is a strict-confinement, `grade: devel` evaluation only. It cannot deliver the host GNOME Shell extension required by the tested workflow, and must not be submitted as a release candidate. See `snap/LIMITATIONS.md`.

## Signed APT repository

Build the `.deb` artifacts first. With `apt-utils` and `gnupg` installed, pass
a new output path, the full fingerprint of an available OpenPGP signing key,
and the reviewed packages to include.

```sh
bash scripts/build-ubuntu-apt-repository.sh /tmp/lemonvox-apt \
    "$LEMONVOX_APT_SIGNING_FINGERPRINT" /absolute/artifacts/lemonvox_*.deb
```

The output has a `stable` suite, a `main` component, and package indexes for
the architectures supplied. GnuPG signs the repository metadata and exports
the corresponding public key as `lemonvox-archive-keyring.gpg`. The script
writes a local directory and leaves it ready for static HTTPS hosting.

The signed repository URL is `https://itomek.github.io/lemonvox/apt/`.
The dedicated archive signing key fingerprint is:

```text
D873B423E88EA3EFF8D769E47EE4BCB0334EC5FE
```

Verify this fingerprint against this reviewed GitHub repository through a trusted
channel independent of the hosted key download. Download the public key, check
its complete fingerprint, and install a source restricted to that key:

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

Stop if the fingerprint check fails. APT verifies signed metadata and offers
upgrades when a newer package for the machine's architecture is published.
The package still requires a separately configured inference server and compatible
transcription and cleanup models. Enable the GNOME extension and log out and back
in after first installation or an extension upgrade as described above.

The protected `Prepare signed Ubuntu APT repository` workflow accepts an existing
public Ubuntu release tag and its exact reviewed source commit. It verifies both
architectures and their provenance, then produces a signed public-tree artifact.
It does not publish or replace the GitHub Pages site; that artifact is reviewed
before publication.

Run `bash ubuntu/tests/apt_repository_test.sh` to exercise signature
verification, package download, and install and upgrade resolution with
disposable fixture packages. The test uses isolated APT state.
