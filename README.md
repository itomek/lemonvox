# LemonVox

LemonVox is a small desktop dictation app that requires a separately running [Lemonade](https://lemonade-server.ai/) or another compatible OpenAI API server. Press a shortcut, speak, and press it again to insert cleaned-up text into the app being used.

A small microphone indicator shows recording activity while the target app keeps focus. The configured server transcribes the recording, then a configurable cleanup prompt improves punctuation and removes disfluencies while preserving meaning.

macOS comes first. Windows and Ubuntu follow, sharing the dictation pipeline and using platform-specific shortcuts and text insertion.

The server must provide audio transcription, chat completion for cleanup, and compatible model-discovery metadata. Local and remote HTTP/HTTPS server endpoints ending in /v1 are supported; an OpenAI-compatible chat API alone is insufficient. Lemonade 2026.44.1 is the tested baseline. LemonVox does not bundle an inference server or models.

Settings on macOS, Windows and Ubuntu show a live server connection indicator. The app checks the saved endpoint in the background every ten seconds (three-second timeout), including while Settings is closed. Green means the models API is reachable, not that the selected models are ready; red shows the connection failure and server prerequisite.

Server installation, model download and model loading are separate steps. Install and start the inference server first, then open Settings to choose transcription and cleanup models. Missing models require an explicit download action; starting LemonVox, checking server health and dictating do not authorize downloads. An installed model can remain unloaded until its first inference request. See [model setup](docs/model-setup.md) for supported server management, cancellation and recovery.

Cleanup execution and its shared preservation policy are described in [cleanup settings and qualification](docs/cleanup.md). New configurations use Lemonade thinking-off mode; existing saved server configurations keep compatible-server behavior until an execution mode is selected.

## Install

### Windows

Download the [Windows x64 ZIP](https://github.com/itomek/lemonvox/releases/download/v0.7.0/LemonVox-0.7.0-windows-x64.zip)
from the [v0.7.0 release](https://github.com/itomek/lemonvox/releases/tag/v0.7.0)
and extract it. Windows 11 is required. The x64 build has also passed bounded
native tests under Windows 11 ARM64 emulation; there is no native ARM64 package.

Open PowerShell in the extracted folder containing `install.ps1` and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

This installs for your current user into `%LOCALAPPDATA%\Programs\LemonVox`,
adds a Start menu shortcut, and launches the app. No administrator rights are
needed. Add `-StartAtSignIn` to start LemonVox when you sign in. The release is
unsigned, so Windows SmartScreen may display a warning.

For portable use, download the [standalone Windows x64 EXE](https://github.com/itomek/lemonvox/releases/download/v0.7.0/LemonVox-0.7.0-windows-x64.exe)
and run it directly instead.

Install and start [Lemonade](https://lemonade-server.ai/) separately, or use a
compatible server on another computer. Open Settings from the LemonVox tray
icon, enter the server endpoint ending in `/v1` (for example,
`http://127.0.0.1:13305/v1`), refresh models, and select transcription and cleanup
models. Confirm any missing-model downloads explicitly. LemonVox includes
neither the server nor models. Allow microphone access for desktop apps in
**Settings > Privacy & security > Microphone**.

The default shortcut is **Alt twice** to start and again to stop; you can change
it in Settings. To update, extract a newer ZIP and rerun its `install.ps1`;
your settings are retained. To uninstall, use **Settings > Apps > Installed apps >
LemonVox > Uninstall**. Settings are retained unless you run the installed
`uninstall.ps1` with `-RemoveSettings`.

See [Windows setup and validation](windows/README.md) for details and limitations.

### Ubuntu

Supports Ubuntu 24.04 LTS with GNOME Wayland on **amd64** and **arm64**.
The signed APT repository provides the package and future updates.

Add the repository and install LemonVox. This checks the downloaded signing key
against the full fingerprint published here before trusting it:

```sh
(
  set -eu
  sudo apt update
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
)
```

Log out and back in so GNOME discovers the installed extension, then enable it:

```sh
gnome-extensions enable lemonvox@itomek.com
```

Open **LemonVox** from the application launcher. Install and start
[Lemonade](https://lemonade-server.ai/) separately, or use a compatible server on
another computer. In Settings, enter its endpoint ending in `/v1`, refresh
models, and choose transcription and cleanup models. Confirm any missing-model
downloads explicitly. The package includes neither the server nor models.

The default dictation shortcut is **Ctrl+Alt+Space**; press it again to stop.
**Ctrl+Super+Escape** cancels. Settings lets you change these shortcuts.
For upgrades, use `sudo apt update && sudo apt install --only-upgrade lemonvox`;
log out and back in after an extension upgrade.

See [Ubuntu setup and validation](ubuntu/README.md) for usage and troubleshooting,
or [package installation details](ubuntu/packaging/README.md) for the signing key
and local `.deb` installation.

## Start here

- [Product specification](docs/spec.md): workflow, technical proposal, and milestones.
- [Issues](https://github.com/itomek/lemonvox/issues): bug reports and planned work.

The local Mac prototype uses Swift/AppKit and a small C++ session core. It supports a fixed-text sample mode and a user-started microphone → local Lemonade transcription → cleanup → insertion path. [Build and test instructions](macos/PROTOTYPE.md) describe configuration, permissions, and remaining live checks.

The Windows port is a native Win32 tray app built on the same C++ core, with a global Alt-twice trigger, a nonactivating indicator, Settings and a per-user installer. See [Windows build, usage and validation](windows/README.md).

In VS Code, run the **Run LemonVox** task (also available from the Agents window's Tasks) or press F5 (**Debug LemonVox**). Both build into `.build` for the current OS first. Debugging uses the CodeLLDB extension on Windows and macOS (see `.vscode/extensions.json`). Ubuntu builds the GTK4/libadwaita host in `.build/ubuntu`; see [Ubuntu build, setup and validation](ubuntu/README.md).

The Ubuntu port targets Ubuntu 24.04+ GNOME Wayland with native Settings and a GNOME Shell extension for shortcuts and the recording bar. It automatically pastes the result into current focus, with Copy available when dispatch fails. X11 is outside scope. See [Ubuntu setup and validation](ubuntu/README.md) for the exact tested desktop and remaining release gates.

Licensed under the [MIT License](LICENSE).
