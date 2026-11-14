# LemonVox

LemonVox turns speech into text on macOS, Ubuntu and Windows using automatic
speech recognition (ASR). Press a shortcut to record, then stop to transcribe
your speech, clean up punctuation and filler words, and insert the text into
the app you’re using.

If insertion fails, use **Copy Last Transcription** in the LemonVox menu to
[recover your latest dictated text](docs/last-transcription.md).

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
