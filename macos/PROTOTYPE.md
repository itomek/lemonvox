# Mac interaction prototype

Build for macOS 26 or later with the installed Apple command-line tools:

```sh
bash scripts/build.sh
open ".build/LemonVox.app"
```

The app appears as a lemon-and-waveform icon in the menu bar. Its menu offers **Start/Stop Dictation**, **Settings…** and **Quit LemonVox**, with **Cancel** and **Copy Text** when relevant. The packaged app icon uses the same native vector geometry.

The Mac default is **Command twice**: two complete taps of the same Command key, either left or right. Each hold can last at most 250 ms; the gap between taps can last at most 350 ms. Other keys/chords, modifiers, mouse actions and application changes reset detection. Ordinary typing passes through unchanged. Settings suspends dictation triggers.

## Setup and dictation

1. Install and run Lemonade. The tested local baseline is **2026.44.1**; the connected version is reported without an exact-version gate. Make a compatible transcription model and cleanup model available in Lemonade. LemonVox checks metadata; model loading, downloads, provider routing and credentials stay with Lemonade.
2. Open **Settings…** from the menu-bar icon. The window opens immediately and discovers models asynchronously, with loading, error and Refresh states. The default endpoint is `http://127.0.0.1:13305/v1`; this prototype accepts loopback endpoints. Choose transcription and cleanup independently. Local downloaded transcription and local/cloud chat models exposed by Lemonade are supported. A remote cleanup choice sends dictated text through Lemonade to that provider. Remote audio-transcription routing is not advertised by the tested cloud catalog.
3. Configure the trigger, appearance and target policy in Settings. A regular combination recorder and left/right modifier double-tap choices are available; Escape cancels shortcut recording. Each model selector opens a searchable, scrollable native popover. Arrow keys, Return and Escape navigate, choose and dismiss. Saved choices survive offline/loading states or absence from the latest catalog. The conservative cleanup prompt can be edited there. Save applies the choices; a failed trigger replacement retains the previous working binding.
4. For modifier monitoring and automatic insertion, manually allow the built app in **System Settings → Privacy & Security → Accessibility**. Use the add (+) button to select `.build/LemonVox.app` if necessary. Rebuilding or moving this development app can invalidate its permission identity. The app checks trust without requesting or changing permission; it makes no Input Monitoring request. After enabling Accessibility, return to Settings and choose Check Again. If the entry is enabled but access remains unavailable, stop recording and copy pending text before quitting and reopening the app.
5. Place the caret in a disposable text field and press the trigger. The client checks models, then requests microphone access if needed. Grant that permission manually. Speak while the overlay says **Listening…**, then press the trigger again or click Stop. Capture stops after at most 120 seconds. Transcription and cleanup run through Lemonade, followed by guarded insertion.

Cancel stops capture or processing and discards late results. Denied microphone access, unavailable server/devices, incompatible models, failed/empty responses and truncated cleanup produce an error instead of insertion. Upstream errors are displayed with credential patterns redacted. Provider wire-format compatibility is checked by Lemonade when a request is made; compatibility of every provider/model combination has not been established.

## Appearance and data

The nonactivating Liquid Glass overlay follows system light/dark appearance. **Always use dark overlay** in Settings overrides only this app's overlay. Semantic system text and controls respect Reduce Transparency and Reduce Motion. Stop works during recording; Cancel also works during processing. Overlay controls refuse keyboard focus; Settings intentionally activates its own window.

Audio uses a private temporary WAV file removed after completion, failure or cancellation. Transcripts and request data remain in memory during processing. An abnormal process termination can leave temporary audio behind. Settings persist locally; diagnostics contain fixed event names/categories and HTTP codes, without audio, transcripts or credentials. **Open Log** in Settings opens the conventional per-user log at `~/Library/Logs/LemonVox/lemonvox.log`; rotation retains one backup. Server/provider retention depends on Lemonade configuration.

## Targeting and clipboard

The default inserts into the original field only while that element remains focused in the same frontmost app. **Insert into field selected at completion** in Settings selects the alternative policy. Secure fields reject starting dictation and insertion. Unsupported or changed targets offer **Copy Text** for manual paste.

Insertion validates the focused editable element through Accessibility, temporarily supplies text on the clipboard and posts one Command-V pair to that same application. It never sends Return. Previous clipboard items/formats are held opaquely in memory and restored after a short delivery grace period only while the ownership counter and private token match. An intervening user copy is preserved. Snapshots over 32 MiB or unavailable representations use the explicit-copy path. Normal Quit waits for restoration; forced termination or a destination that handles paste after the grace period remain limitations. **Copy Text** deliberately replaces clipboard contents with the pending result.

Focus checks and paste dispatch are separate OS calls. An event dispatch or Accessibility API success alone does not prove visible insertion. Logs therefore record `paste_dispatched`. Chromium/Electron focus may require materializing native Accessibility: recognized frontmost Chromium-framework apps receive a runtime `AXManualAccessibility` request, with `AXEnhancedUserInterface` fallback. This grants no TCC permission and changes no persistent browser preferences. Focus lookup checks application ownership; a bounded, cancellable warmup precedes microphone capture. The app never walks or records surrounding document trees.

## Developer checks

The build script runs session, policy, pipeline, recorder, Settings/model-discovery, clipboard transaction and trigger tests. It also builds these native checks:

```sh
.build/hotkey-test
.build/system-clipboard-test
.build/overlay-test
.build/settings-ui-test
.build/model-picker-test
".build/LemonVox.app/Contents/MacOS/LemonVoxPrototype" --diagnose
".build/LemonVox.app/Contents/MacOS/LemonVoxPrototype" --smoke-menu
".build/LemonVox.app/Contents/MacOS/LemonVoxPrototype" --smoke-cancel
```

Tests use fake model discovery, fake permission responses or in-memory synthetic events. They capture no microphone audio and invoke no real models. The hotkey test temporarily registers its two test combinations, checks collisions/repeats and releases them. The clipboard test uses a private named pasteboard. `--diagnose` checks the calling app's permission identity without prompting. Shell diagnostic trust can differ from a normal Launch Services app launch.

Developer fixtures remain available outside the everyday menu:

```sh
open ".build/LemonVox.app" --args --show
open ".build/LemonVox.app" --args --sample
```

`--show` opens the existing Field A / Field B keyboard test app. `--sample` enables fixed-text recording states labeled **SAMPLE**, with a short processing delay for cancellation/focus tests. These are explicit development launches; quit an existing instance first when switching launch modes. Sample mode uses no microphone.

`--insertion-probe --probe-marker=<unique-document-marker>` uses fixed synthetic text only in an empty, nonce-titled TextEdit/Chrome field. It rechecks the nonce window and target before dispatch. It requires the built app's own manual Accessibility permission. `--smoke-focus-change` and `--smoke-focus-cancel` check cancellation before capture. `--smoke-canceled-completion` checks stale completion suppression. Native Settings tests cover immediate opening, offline/retry, independent selection, prompt preservation, recorder cancellation and save. Trigger tests cover timing/holds/repeats/sides/migration and passive event normalization. The old default preset migrates to Command twice; an explicit new-format choice or legacy alternate preset is preserved. Future Windows/Linux defaults are Alt twice.

Render checks (`overlay-test --render`, `settings-ui-test --render`, `model-picker-test --render`) generate app-only light/dark layouts. Offscreen renders do not establish live backdrop or desktop focus behavior.

## Live validation and remaining work

Validate TextEdit, browser and Electron fields, physical trigger delivery, cancellation, changed-target fallback, secure fields, full-screen apps and Spaces. Confirm exact visible text, one insertion, clipboard restoration and no submission; repeat with both targeting policies. Automated checks and paste dispatch alone do not establish compatibility with every target app.

Modifier-only gestures observe rather than reserve the keys, so another app can react to the same gesture. Physical delivery depends on Accessibility and secure-input behavior. Stable update permission identity, clean-machine packaging, resource measurements and broader platform validation require live testing. This prototype uses native Apple frameworks and a small C++ session core, with no third-party runtime dependency.

For isolated versioned app/ZIP packaging, signature verification and certificate requirements, see [macOS packaging and signing](PACKAGING.md).

For local diagnosis, launch the candidate with `--settings --debug-status` (omit `--sample` for the configured real pipeline). The explicit flags suppress automatic updater startup and do not start capture. `DEBUG_STATE` lines on stdout report changed permission/trigger/session booleans and the configured key choice, without audio, text, prompts, endpoint URLs or credentials. Microphone authorization is queried without requesting consent. Settings intentionally blocks dictation while open; close it before a user-triggered start.

In Settings → Diagnostics, **Accessibility…** requests the missing native event-post/Accessibility consent once per process; later clicks open the system pane. **Check Again** only rechecks status and never prompts, saves a shortcut draft or starts capture. Permission recovery is kept in Settings, not the speech bar. Accessibility targeting and event posting are checked separately; the latter is required for automatic paste.

For an isolated development identity, run `bash scripts/build-debug-macos.sh --output /absolute/path/to/new-build-directory`. This builds `LemonVox Debug.app` with bundle ID `com.itomek.lemonvox.debug`, a separate macOS preferences domain, and update checks disabled. It does not copy production preferences. The normal build and release identity are unchanged. Ad-hoc development builds may lose Accessibility or other grants when rebuilt or moved; no grant retention is guaranteed. Use the running app path shown in Settings → Diagnostics if macOS needs the app added again.
