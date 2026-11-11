# Mac interaction prototype

Build for macOS 26 or later with the installed Apple command-line tools:

```sh
bash scripts/build.sh
open ".build/LemonVox.app"
```

The app appears as a lemon-and-waveform icon in the menu bar. Its menu offers **Start/Stop Dictation**, **Settings…** and **Quit LemonVox**, with **Cancel** and **Copy Text** when relevant. The packaged app icon uses the same native vector geometry.

The Mac default is **Command twice**: two complete taps of the same Command key, either left or right. Each hold can last at most 250 ms; the gap between taps can last at most 350 ms. Other keys/chords, modifiers, mouse actions and application changes reset detection. Ordinary typing passes through unchanged. Settings blocks starting a new dictation; an active recording can still be finished or canceled.

## Setup and dictation

1. Install and run Lemonade. The tested local baseline is **2026.44.1**; the connected version is reported without an exact-version gate. Make a compatible transcription model and cleanup model available in Lemonade. LemonVox checks metadata; model loading, downloads, provider routing and credentials stay with Lemonade.
2. Open **Settings…** from the menu-bar icon. The window opens immediately and discovers models asynchronously, with loading, error and Refresh states. The default endpoint is `http://127.0.0.1:13305/v1`; this prototype accepts loopback endpoints. Choose transcription and cleanup independently. Local downloaded transcription and local/cloud chat models exposed by Lemonade are supported. A remote cleanup choice sends dictated text through Lemonade to that provider. Remote audio-transcription routing is not advertised by the tested cloud catalog.
3. Configure the dictation shortcut, cancellation shortcut, appearance in Settings → General. The dictation shortcut supports a regular combination recorder and left/right modifier double-tap choices; the cancellation shortcut is a regular combination and defaults to **Right Command–Escape**. Saving a custom cancellation shortcut replaces that default gesture. The two shortcuts must differ. Escape cancels shortcut recording. Appearance offers **Auto**, **Light**, and **Dark**; Auto follows macOS. The running app version and build are shown in General. Each model selector opens a searchable, scrollable native popover. Arrow keys, Return and Escape navigate, choose and dismiss. Saved choices survive offline/loading states or absence from the latest catalog. The conservative cleanup prompt can be edited there. Save validates custom cancellation chords before replacing the dictation binding; a failed replacement retains the previous working binding.
4. For modifier monitoring and automatic insertion, manually allow the built app in **System Settings → Privacy & Security → Accessibility**. Use the add (+) button to select `.build/LemonVox.app` if necessary. Rebuilding or moving this development app can invalidate its permission identity. The app checks trust without requesting or changing permission; it makes no Input Monitoring request. After enabling Accessibility, return to Settings and choose Check Again. If the entry is enabled but access remains unavailable, stop recording and copy pending text before quitting and reopening the app.
5. Place the caret in a disposable text field and press the dictation trigger. The client checks models, then requests microphone access if needed. Grant that permission manually. Speak while the overlay is active, then click **Stop** or press the dictation trigger again to finish capture. **X** discards recording or processing. With the default cancellation shortcut, hold Right Command while pressing Escape to cancel. A saved custom cancellation shortcut replaces that gesture and works during an active session. Right Command–Escape uses Accessibility-backed observation and can reach the foreground app as well. Capture stops after at most 120 seconds. Transcription and cleanup run through Lemonade, followed by ordinary paste at current keyboard focus.

Cancel stops capture or processing and discards late results. Denied microphone access, unavailable server/devices, incompatible models, failed/empty responses and truncated cleanup produce an error instead of insertion. Upstream errors are displayed with credential patterns redacted. Provider wire-format compatibility is checked by Lemonade when a request is made; compatibility of every provider/model combination has not been established.

## Appearance and data

The nonactivating Liquid Glass overlay and Settings follow the selected app appearance: **Auto** follows macOS, while **Light** and **Dark** apply the corresponding native AppKit appearance. The waveform, text and controls use semantic system colors; no custom state coloring is added. Reduce Transparency and Reduce Motion are respected. Stop is available while recording. X cancels an active session or dismisses a message. Overlay controls refuse keyboard focus; Settings intentionally activates its own window.

Audio uses a private temporary WAV file removed after completion, failure or cancellation. Transcripts and request data remain in memory during processing. An abnormal process termination can leave temporary audio behind. Settings persist locally; diagnostics contain fixed event names/categories and HTTP codes, without audio, transcripts or credentials. **Open Log** in Settings opens the conventional per-user log at `~/Library/Logs/LemonVox/lemonvox.log`; rotation retains one backup. Server/provider retention depends on Lemonade configuration.

## Targeting and clipboard

When processing finishes, LemonVox sends one Command-V pair to current keyboard focus. Moving the cursor or switching applications while recording or processing changes the paste destination. A recognized Accessibility field is not required, and starting dictation is not blocked by the focused field type. If no field is selected, the destination handles the normal paste attempt. LemonVox never reactivates an earlier app or sends Return. The former target-field setting is removed; old saved values have no effect.

Paste deliberately replaces the clipboard with the transcript and leaves it there, following the desktop clipboard's normal lifetime. Previous clipboard contents are not snapshotted or restored. Held Command, Control, Option or Shift modifiers delay paste for up to five seconds; cancellation remains available during that wait. If modifiers remain held, clipboard writing fails, or event-post permission/event creation is unavailable, **Copy Text** offers manual recovery. A missing or unrecognized field does not cause a warning or Copy prompt.

Event dispatch does not acknowledge visible insertion. Diagnostics therefore record `paste_dispatched`; a dispatched gesture may have no effect if the focused destination does not accept paste. Accessibility/event-post permission is still needed for native key delivery and modifier monitoring, but production dictation never queries Accessibility fields or enables browser Accessibility modes. The separate nonce-limited developer insertion probe retains stricter field checks to protect disposable test documents.

## Developer checks

The build script runs session, policy, pipeline, recorder, Settings/model-discovery, clipboard paste and trigger tests. It also builds these native checks:

```sh
.build/hotkey-test
.build/system-clipboard-test
.build/overlay-test
.build/settings-ui-test
.build/model-picker-test
".build/LemonVox.app/Contents/MacOS/LemonVoxPrototype" --diagnose
".build/LemonVox.app/Contents/MacOS/LemonVoxPrototype" --smoke-menu
".build/LemonVox.app/Contents/MacOS/LemonVoxPrototype" --smoke-cancel
".build/LemonVox.app/Contents/MacOS/LemonVoxPrototype" --smoke-cancellation-shortcut
```

Tests use fake model discovery, fake permission responses or in-memory synthetic events. They capture no microphone audio and invoke no real models. The hotkey test temporarily registers its two test combinations, checks collisions/repeats and releases them. The clipboard test uses a private named pasteboard. `--diagnose` checks the calling app's permission identity without prompting. Shell diagnostic trust can differ from a normal Launch Services app launch.

Developer fixtures remain available outside the everyday menu:

```sh
open ".build/LemonVox.app" --args --show
open ".build/LemonVox.app" --args --sample
```

`--show` opens the existing Field A / Field B keyboard test app. `--sample` enables fixed-text recording states labeled **SAMPLE**, with a short processing delay for cancellation/focus tests. These are explicit development launches; quit an existing instance first when switching launch modes. Sample mode uses no microphone.

`--insertion-probe --probe-marker=<unique-document-marker>` uses fixed synthetic text only in an empty, nonce-titled TextEdit/Chrome field. It rechecks the nonce window and target before dispatch. It requires the built app's own manual Accessibility permission. `--smoke-cancel` verifies that X cancels both recording and processing while the repeated dictation trigger finishes capture. `--smoke-cancellation-shortcut` synthesizes cancellation event ID 2, verifies it is active-session-only, and verifies a colliding saved choice retains the prior binding/preferences. `--smoke-focus-change` and `--smoke-focus-cancel` check field-free launch and cancellation before capture. `--smoke-canceled-completion` checks stale completion suppression. Native Settings tests cover immediate opening, offline/retry, independent selection, prompt preservation, recorder cancellation and save. Trigger tests cover timing/holds/repeats/sides/migration and passive event normalization. The old default preset migrates to Command twice; an explicit new-format choice or legacy alternate preset is preserved. Future Windows/Linux defaults are Alt twice.

Render checks (`overlay-test --render`, `settings-ui-test --render`, `model-picker-test --render`) generate app-only light/dark layouts. Offscreen renders do not establish live backdrop or desktop focus behavior.

## Live validation and remaining work

Validate TextEdit, browser and Electron fields, physical trigger delivery, cancellation, focus changes, unrecognized Accessibility fields, no selected field, full-screen apps and Spaces. Confirm exact visible text, one paste at final focus, the retained transcript clipboard and no submission. Automated checks and paste dispatch alone do not establish compatibility with every target app.

Modifier-only gestures observe rather than reserve the keys, so another app can react to the same gesture. Physical delivery depends on Accessibility and secure-input behavior. Stable update permission identity, clean-machine packaging, resource measurements and broader platform validation require live testing. This prototype uses native Apple frameworks and a small C++ session core, with no third-party runtime dependency.

For isolated versioned app/ZIP packaging, signature verification and certificate requirements, see [macOS packaging and signing](PACKAGING.md).

For local diagnosis, launch the candidate with `--settings --debug-status` (omit `--sample` for the configured real pipeline). The explicit flags suppress automatic updater startup and do not start capture. `DEBUG_STATE` lines on stdout report changed permission/trigger/session booleans and the configured key choice, without audio, text, prompts, endpoint URLs or credentials. Microphone authorization is queried without requesting consent. Opening Settings keeps the active recording running and still permits the repeated dictation trigger to finish capture or X/cancellation to discard it. New sessions wait until Settings closes. Changed endpoint, models, cleanup prompt, dictation shortcut, cancellation shortcut and appearance apply after the active session reaches idle.

In Settings → Diagnostics, **Accessibility…** requests the missing native event-post/Accessibility consent once per process; later clicks open the system pane. **Check Again** only rechecks status and never prompts, saves a shortcut draft or starts capture. Permission recovery is kept in Settings, not the speech bar. Accessibility trust and event posting are checked separately; event posting is required for automatic paste.

For an isolated development identity, run `bash scripts/build-debug-macos.sh --output /absolute/path/to/new-build-directory`. This builds `LemonVox Debug.app` with bundle ID `com.itomek.lemonvox.debug`, a separate macOS preferences domain, and update checks disabled. It does not copy production preferences. The normal build and release identity are unchanged. Ad-hoc development builds may lose Accessibility or other grants when rebuilt or moved; no grant retention is guaranteed. Use the running app path shown in Settings → Diagnostics if macOS needs the app added again.
