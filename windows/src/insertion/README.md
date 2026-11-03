# Windows insertion adapter

`lv_insertion` implements `lemonvox::InsertionAdapter` (`core/adapters.h`) for
Windows with Win32 and UI Automation. The shell owns one `WinInsertion` per
`WorkflowCore` and calls `copy_text_to_clipboard` for the manual Copy action.

## Threading

Every native call runs on one worker thread that owns a COM MTA apartment, the
`IUIAutomation` client (2 s connection and transaction timeouts) and a
message-only clipboard owner window. `capture_original`, `inspect`, `insert`,
`cancel` and `release` only queue work or update token storage, so the UI thread
never waits for another application. Each request completes exactly once through
`Executor::post`. `cancel` marks the operation and invalidates its tokens; work
already queued completes with `canceled`. A newer `capture_original` makes every
older operation stale. `shutdown` (on the executor thread) stops the worker,
dispatches only sent messages while waiting so a UI Automation call into one of
LemonVox's own windows can finish, and guarantees no callback afterwards.

## Targets

A token is an index into adapter-owned storage. A target is identified by its
foreground window, process ID and process start time, focused child window
(`GetGUIThreadInfo`) and UI Automation runtime ID; all must match.

- Editable: UIA Edit, Document or ComboBox with a writable ValuePattern, Edit or
  Document with a TextPattern that is not read-only, or a writable Win32 Edit or
  RichEdit; without UI Automation, a Win32 edit class or a caret.
- Secure: UIA `IsPassword` or `ES_PASSWORD`. Secure targets get no token.
- `inspect` with token 0 describes the field focused now
  (`insert_current_target`). A changed field gets its own token with
  `matches_original=false`, so the default policy falls back to Copy.

Chromium apps report only a bare contents pane until their web accessibility
is on. Edge turns it on for UI Automation clients; Chrome only for MSAA
clients. When focus is that placeholder, the probe follows MSAA `accFocus`/
`accRole`/`accState` from the top-level window once (which turns Chrome's tree on
for the life of that browser process) and re-reads UI Automation for up to
300 ms. UI Automation stays the source of editability and identity (runtime ID);
MSAA only adds a secure signal (`STATE_SYSTEM_PROTECTED`). MSAA is used only for
that placeholder, i.e. a process that just answered UI Automation, because
`WM_GETOBJECT` has no timeout. If the field still cannot be read, the result is
the Copy fallback. See `windows/tests/insertion-matrix.md`.

## Insertion

`insert` refuses before touching the clipboard when the target runs at a higher
integrity level than LemonVox or its token cannot be read (`insertion_unavailable`,
reported by the core as a copy fallback). It waits up to 2 s for the person to
release Shift, Ctrl, Alt and Windows keys (for example the Alt of the trigger),
re-checks the target, snapshots the clipboard, writes the text, re-checks the
target and the foreground/focus window, then sends Ctrl+V with virtual keys and
scan codes. Enter is never sent. After 600 ms the previous clipboard is restored
only if `GetClipboardSequenceNumber` shows nobody changed it since the write.

Clipboard snapshots keep every format that can be re-materialized from bytes:
text (the first of Unicode/ANSI/OEM; the system synthesizes the others), locale,
DIB or DIBV5 (bitmap is synthesized), file drops, enhanced metafiles, SYLK, DIF,
TIFF, RIFF, WAVE, pen data and registered formats such as HTML Format and Rich
Text Format. Owner-display, display-only, private, GDI-object, palette,
metafile-picture handles and OLE formats that reference live objects are
skipped. A clipboard over 64 MB is not overwritten. Pasted text, Copy-fallback
text and the restored clipboard carry `ExcludeClipboardContentFromMonitorProcessing`,
`CanIncludeInClipboardHistory`=0 and `CanUploadToCloudClipboard`=0, so neither
the dictated text nor a duplicate of the restored item enters clipboard history,
cloud clipboard or clipboard managers.

## Elevated windows

User Interface Privilege Isolation drops synthesized input sent from a
medium-integrity process to a higher-integrity window (an elevated Task Manager,
an administrator terminal, installers). LemonVox detects this by comparing
process token integrity levels and offers the Copy fallback instead. LemonVox
must not be run as administrator to hide this: an elevated LemonVox could type
into every elevated window.
