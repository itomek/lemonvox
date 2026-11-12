# LemonVox

LemonVox is a small desktop dictation app that requires a separately running [Lemonade](https://lemonade-server.ai/) or another compatible OpenAI API server. Press a shortcut, speak, and press it again to insert cleaned-up text into the app being used.

A small microphone indicator shows recording activity while the target app keeps focus. The configured server transcribes the recording, then a configurable cleanup prompt improves punctuation and removes disfluencies while preserving meaning.

macOS comes first. Windows and Ubuntu follow, sharing the dictation pipeline and using platform-specific shortcuts and text insertion.

The server must provide audio transcription, chat completion for cleanup, and compatible model-discovery metadata. Local and remote HTTP/HTTPS server endpoints ending in /v1 are supported; an OpenAI-compatible chat API alone is insufficient. Lemonade 2026.44.1 is the tested baseline. LemonVox does not bundle an inference server or models.

## Start here

- [Product specification](docs/spec.md): workflow, technical proposal, and milestones.
- [Issues](https://github.com/itomek/lemonvox/issues): bug reports and planned work.

The local Mac prototype uses Swift/AppKit and a small C++ session core. It supports a fixed-text sample mode and a user-started microphone → local Lemonade transcription → cleanup → insertion path. [Build and test instructions](macos/PROTOTYPE.md) describe configuration, permissions, and remaining live checks.

The Windows port is a native Win32 tray app built on the same C++ core, with a global Alt-twice trigger, a nonactivating indicator, Settings and a per-user installer. See [Windows build, usage and validation](windows/README.md).

In VS Code, run the **Run LemonVox** task (also available from the Agents window's Tasks) or press F5 (**Debug LemonVox**). Both build into `.build` for the current OS first. Debugging uses the CodeLLDB extension on Windows and macOS (see `.vscode/extensions.json`). Ubuntu builds the GTK4/libadwaita host in `.build/ubuntu`; see [Ubuntu build, setup and validation](ubuntu/README.md).

The Ubuntu port targets Ubuntu 24.04+ GNOME Wayland with native Settings and a GNOME Shell extension for shortcuts and the recording bar. It automatically pastes the result into current focus, with Copy available when dispatch fails. X11 is outside scope. See [Ubuntu setup and validation](ubuntu/README.md) for the exact tested desktop and remaining release gates.

Licensed under the [MIT License](LICENSE).
