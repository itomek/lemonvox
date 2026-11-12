# Ubuntu package artifacts

Run `bash scripts/build-ubuntu.sh --output /absolute/new-directory --version X.Y.Z` to build the Ubuntu host, run CTest, and write a local `.deb`, checksum, and source metadata. It does not install the artifact or change the desktop.

Install a reviewed local artifact on the tested Ubuntu 24.04+ GNOME Wayland target:

```sh
sudo apt install ./lemonvox_X.Y.Z_amd64.deb
```

The package installs the GTK application, desktop entry, user D-Bus activation file, and `lemonvox@itomek.com` GNOME Shell extension. Enable the extension in GNOME Extensions before relying on global shortcut delivery. Install and run Lemonade or another OpenAI-compatible server separately, locally or remotely. It must provide audio transcription, chat completions and model-discovery metadata supported by LemonVox. Local and remote HTTP/HTTPS endpoints are accepted; a generic OpenAI-compatible API alone is insufficient. Lemonade 2026.44.1 is the tested baseline; other servers are unverified. The package does not bundle a server or models.

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

To configure APT for a hosted repository, verify the key fingerprint through
a trusted channel, install the public key as
`/etc/apt/keyrings/lemonvox-archive-keyring.gpg`, and give APT read access.
Save a source file under `/etc/apt/sources.list.d/lemonvox.sources` with the
actual repository URL in the `URIs` field.

```text
Types: deb
URIs: https://example.invalid/lemonvox-apt/
Suites: stable
Components: main
Signed-By: /etc/apt/keyrings/lemonvox-archive-keyring.gpg
```

Run `sudo apt update` and `sudo apt install lemonvox`. APT offers upgrades when
the repository contains a newer version for your architecture.

Run `bash ubuntu/tests/apt_repository_test.sh` to exercise signature
verification, package download, and install and upgrade resolution with
disposable fixture packages. The test uses isolated APT state.
