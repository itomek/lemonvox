# In-app updates

The next release includes native update alerts and **Check for Updates…** in the menu. Checks run daily. New updates show an Install / Later prompt and a menu-bar indicator until reviewed. Settings controls automatic checks and installation. Automatic installation starts off; the update prompt also offers an opt-in.

Installation waits while dictation, paste, uninserted text or Settings are active. A manual update check holds new dictation and Settings until its window is dismissed. The updater preserves the app's identity and preferences.

Existing installations need the first updater-enabled version through Homebrew or direct download. Homebrew remains supported. The current published release does not include this updater.

## Release preparation

Sparkle 2.10.1 is fetched from its official release and checked against a pinned SHA-256. Its license ships in the app. The app embeds the public update key; the private key lives in local Keychain and the protected `release` environment as `SPARKLE_ED25519_PRIVATE_KEY`. PR tests use a public RFC 8032 fixture key, not this secret.

Build numbers must increase beyond the latest public release. After Developer ID signing, Apple acceptance and stapling, the final ZIP is signed and the appcast generated from those exact bytes. A draft release contains the ZIP, checksum and appcast. Public feed publication follows binary publication and verification; changing the archive afterward invalidates its signature/checksum.

The proposed feed is `https://itomek.github.io/lemonvox/appcast.xml`. The release and Pages workflows remain disabled. Enabling them requires completed Apple signing credentials, configured/reviewed Pages hosting, a real signed upgrade test and release approval. The Pages job verifies the archive using the embedded public key; it receives no signing secret. Homebrew's cask gets the same release URL/checksum and `auto_updates true` when this updater ships.
