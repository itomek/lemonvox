# LemonVox

LemonVox turns speech into text on macOS, Ubuntu and Windows using automatic
speech recognition (ASR). Press a shortcut to record, then stop to transcribe
your speech, clean up punctuation and filler words, and insert the text into
the app you’re using.

## Inference server required

Install and run [Lemonade](https://lemonade-server.ai/), then [connect it to LemonVox](docs/model-setup.md).

## Install

### macOS

```sh
brew install --cask itomek/lemonvox/lemonvox
```

[Setup](macos/README.md)

### Ubuntu

```sh
sudo apt install lemonvox
```

[Repository and desktop setup](ubuntu/README.md)

### Windows

```powershell
winget install --id LemonVox.LemonVox --exact --source winget
```

WinGet pending; [download and setup](windows/README.md).

[Issues](https://github.com/itomek/lemonvox/issues) · [License](LICENSE)

## Desktop comparison

[Native screenshots, design differences and verification status](docs/desktop-design.md)
compare macOS, Windows and Ubuntu. The current images are synthetic native
fixtures; final three-platform parity verification remains open.

| macOS Settings (Light / Dark) | Ubuntu General (Light) | Windows Settings (System Dark, 200%) |
| --- | --- | --- |
| ![macOS native Settings fixture, General, Models and Diagnostics in light and dark](docs/assets/desktop/macos-settings-light-dark.png) | ![Ubuntu native General settings fixture in light appearance](docs/assets/desktop/ubuntu-general-light.png) | ![Windows combined native Settings fixture in system-dark appearance at 200 percent scale](docs/assets/desktop/windows-settings.png) |

The comparison includes native evidence from all three platforms, reproducible
capture commands and remaining appearance, scaling and accessibility checks.
