# LemonVox

LemonVox is a small desktop dictation app backed by [Lemonade](https://lemonade-server.ai/). Press a shortcut, speak, and press it again to insert cleaned-up text into the app being used.

A small microphone indicator shows recording activity while the target app keeps focus. Lemonade transcribes the recording, then a configurable cleanup prompt improves punctuation and removes disfluencies while preserving meaning.

macOS comes first. Windows and Ubuntu follow, sharing the dictation pipeline and using platform-specific shortcuts and text insertion.

## Start here

- [Product specification](docs/spec.md): workflow, technical proposal, and milestones.
- [Issues](https://github.com/itomek/lemonvox/issues): bug reports and planned work.

The local Mac prototype uses Swift/AppKit and a small C++ session core. It supports a fixed-text sample mode and a user-started microphone → local Lemonade transcription → cleanup → insertion path. [Build and test instructions](macos/PROTOTYPE.md) describe configuration, permissions, and remaining live checks.

License selection is pending; this repository does not yet include a license.
