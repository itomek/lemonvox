# LemonVox

LemonVox is a small desktop dictation app backed by [Lemonade](https://lemonade-server.ai/). Press a shortcut, speak, and press it again to insert cleaned-up text into the app being used.

A small microphone indicator shows recording activity while the target app keeps focus. Lemonade transcribes the recording, then a configurable cleanup prompt improves punctuation and removes disfluencies while preserving meaning.

macOS comes first. Windows and Ubuntu follow, sharing the dictation pipeline and using platform-specific shortcuts and text insertion.

## Start here

- [Product specification](docs/spec.md): workflow, technical proposal, and milestones.
- [Issues](https://github.com/itomek/lemonvox/issues): bug reports and planned work.

The local Mac prototype uses Swift/AppKit and a small C++ session core. It supports a fixed-text sample mode and a user-started microphone → local Lemonade transcription → cleanup → insertion path. [Build and test instructions](macos/PROTOTYPE.md) describe configuration, permissions, and remaining live checks.

The Windows port is a native Win32 tray app built on the same C++ core, with a global Alt-twice trigger, a nonactivating indicator, Settings and a per-user installer. See [Windows build, usage and validation](windows/README.md).

In VS Code, run the **Run LemonVox** task (also available from the Agents window's Tasks) or press F5 (**Debug LemonVox**). Both build into `.build` for the current OS first. Debugging uses the CodeLLDB extension on Windows and macOS (see `.vscode/extensions.json`). Ubuntu has no host yet, so these tasks report that and stop.

Licensed under the [MIT License](LICENSE).
