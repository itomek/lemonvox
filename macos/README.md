# LemonVox for macOS

Requires Apple Silicon and macOS Tahoe or later.

## Install

```sh
brew install --cask itomek/lemonvox/lemonvox
```

Alternatively, download the macOS ZIP from [Releases](https://github.com/itomek/lemonvox/releases),
extract it, and move **LemonVox.app** to Applications.

Allow Microphone and Accessibility access when prompted. Open Settings to
[connect your server and select models](../docs/model-setup.md), then use your
configured shortcut to start and stop dictation. **X** cancels.

## Help

[Native screenshot comparison and design verification status](../docs/desktop-design.md)


- **No microphone or shortcut:** check LemonVox permissions in System Settings > Privacy & Security.
- **Server or model error:** follow [model setup](../docs/model-setup.md).
- [Cleanup settings](../docs/cleanup.md)
- [Build and development details](PROTOTYPE.md)
- [Report an issue](https://github.com/itomek/lemonvox/issues)
