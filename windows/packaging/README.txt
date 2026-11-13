LemonVox for Windows
====================

LemonVox turns speech into text in the app you are using. It sends audio and
the transcript to a Lemonade server you run yourself.

Requirements
- Windows 11 x64. This is the only tested configuration (see VERSION.txt and
  the repository's windows/README.md); Windows 10 and Arm64 are untested.
- Lemonade installed and running, with a transcription model (for example
  Whisper-Base) and a chat model downloaded. LemonVox does not install,
  start or update Lemonade. Default endpoint: http://127.0.0.1:13305/v1
- Model download is separate from loading a downloaded model into memory.
  Settings > Set up offers explicit downloads only for a supported server;
  confirm the named model and allow several GB of server disk space. Otherwise
  use the server model manager, then Refresh models. No background download.

Install / update
  powershell -ExecutionPolicy Bypass -File install.ps1 [-StartAtSignIn]
Installs for the current user only into %LOCALAPPDATA%\Programs\LemonVox.
No administrator rights are needed or used. Re-run to update; settings are kept.

Uninstall
  Apps & features > LemonVox > Uninstall, or
  powershell -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\Programs\LemonVox\uninstall.ps1" [-RemoveSettings]

Use
- Press Alt twice to start recording, Alt twice again to stop. Choose another
  shortcut in Settings (tray icon > Settings).
- The bar shows recording, transcribing and cleanup progress without taking
  focus. Completed text is pasted once wherever keyboard focus is now; Enter
  is never pressed. You may switch apps or fields while processing.
- No field discovery or target setting is required. With no text field focused,
  the ordinary paste is still attempted. Cancel/X suppresses late paste.
- If Windows cannot dispatch paste (for example to an administrator app),
  LemonVox offers Copy so you can paste manually.
  Do not run LemonVox as administrator to work around this.
- In administrator windows Windows hides key presses from LemonVox, so Alt
  twice does nothing there; use the tray menu's Start dictation or a regular
  shortcut set in Settings.
- Microphone access: Settings > Privacy & security > Microphone > allow
  desktop apps to access your microphone.

Privacy
Audio and transcripts stay in memory and are discarded after each dictation.
The clipboard is used briefly for pasting, excluded from clipboard history and
cloud sync, and restored afterwards. Diagnostics contain no dictated content.

This build is not code-signed; Windows SmartScreen may warn on first launch.
