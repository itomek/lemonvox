# LemonVox for Windows

Simple dictation from your microphone into the app you’re using. Requires Windows 11;
the x64 app also runs under ARM64 emulation.

## Install

[Download and run the Windows EXE](https://github.com/itomek/lemonvox/releases/download/v0.7.0/LemonVox-0.7.0-windows-x64.exe).
The portable app needs no installer. The release is unsigned, so SmartScreen may warn.

The [WinGet submission](https://github.com/microsoft/winget-pkgs/pull/475080) is pending. After acceptance:

```powershell
winget install --id LemonVox.LemonVox --exact --source winget
```

## Use

Allow desktop microphone access in **Settings > Privacy & security > Microphone**.
Open LemonVox Settings from the tray icon and [connect your server and models](../docs/model-setup.md).

Press **Alt twice** to start and again to stop. **X** cancels. The result is pasted
wherever your cursor is when processing finishes. Change shortcuts in Settings.

## Help and development

[Cross-platform design and native screenshot status](../docs/desktop-design.md)


- [Model setup](../docs/model-setup.md) and [cleanup settings](../docs/cleanup.md)
- [Build, test and packaging](DEVELOPMENT.md)
- [Release publishing](DISTRIBUTION.md)
