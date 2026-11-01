# Signed upgrade verification

Use two signed/notarized updater-enabled builds with the same identity and increasing build numbers. Keep the working installed app intact. A separate directory alone does not isolate preferences: use an existing test user/session or a test build with an explicit separate bundle/preferences domain and controlled feed; report that distinction. Create no new user/security permission without approval.

Verify daily/manual discovery, visible Install/Later UI and menu indicator, optional automatic download/install, cancellation, network failure and no-update UI. Verify actual replacement/relaunch and retained preferences. During pending dictation, paste/clipboard restoration, uninserted text and Settings, installation must defer; new dictation is held during manual update UI. Confirm cancellation/error restores ordinary use. Test changed bytes, wrong key and non-increasing builds. Inspect both Developer ID/Gatekeeper and Sparkle signatures.

Do not record ambient audio or send user content. Use synthetic session/clipboard fixtures. Record actual permission continuity and any filesystem authorization rather than assuming them. Local fixture-key signature tests and ad-hoc packaging do not establish a real signed upgrade.
