# In-app updates

Approved design for the next macOS release.

LemonVox checks daily for updates and shows Sparkle's native Install / Later prompt. A menu-bar indicator remains until the update is reviewed. **Check for Updates…** is available in the menu. Settings controls automatic checks and optional automatic installation; automatic installation starts off.

Installation and restart wait for dictation, transcription, cleanup, paste/clipboard restoration, uninserted text and Settings edits to finish. A manual update check holds off new dictation and opening Settings until its UI is dismissed. Starting an installation prevents a new dictation session until installation is canceled or the app relaunches. Reviewing an update preserves the active app and dictated text.

Sparkle 2 integrates with the native AppKit shell and existing build scripts. GitHub Pages hosts the HTTPS appcast; GitHub Releases hosts the signed and notarized archives. Each final archive also has a dedicated Sparkle Ed25519 signature. The public key ships in the app; the private key stays in local Keychain and protected release secrets.

Homebrew remains an installation and upgrade option. Existing users install the first updater-enabled version once through Homebrew or direct download. The bundle identity, app name and preferences remain stable; release build numbers increase.

Implementation is verified with signed older-to-newer upgrades in an isolated app location, including deferred restart, cancellation, damaged updates and failed downloads. The working installed app remains intact during development.
