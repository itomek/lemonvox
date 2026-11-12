# Windows insertion adapter

`lv_insertion` implements the shared `InsertionAdapter` with native Ctrl+V at
current keyboard focus. It never activates a window, discovers an editable
field, queries UI Automation, or sends Enter. Changing fields/apps while
recording or processing is supported. With no field focused, Windows receives
the same paste attempt and the receiving app determines its outcome.

## Operation lifetime

The shared workflow calls `capture_original`, `inspect`, then `insert`. On
Windows these names describe a compatibility interface: capture/inspect return
an operation token and dispatch eligibility, not facts about an OS element.
Tokens contain no field identity and cannot direct paste back to an old field.
Unknown tokens are rejected; cancellation, release, shutdown and newer operations
invalidate old operations. Windows preferences always select completion focus.

Native work is serialized on a worker with a message-only clipboard owner.
Completions return through `Executor::post` once unless shutdown intervenes.
`release()` frees terminal operation tokens. An atomic commit arbitrates cancel
against dispatch without locking across `SendInput` (which may call the UI
thread's global keyboard hook). Cancel before commitment suppresses paste;
cancel after commitment cannot undo input already handed to Windows.

## Paste transaction

Wait for held modifiers, snapshot the clipboard, then publish UTF-16 text with
history/cloud exclusion formats. Check cancellation, current foreground process
integrity and modifier state, then send Ctrl-down, V-down, V-up, Ctrl-up as one
`SendInput` batch. All injected keys carry `kLemonVoxInjectedTag`, so the trigger
ignores them. Partial dispatch releases V/Ctrl without resending the paste.
No input is sent on invalid UTF-8 or clipboard failure.

After dispatch, wait 600 ms for the app to read the clipboard, then restore its
prior contents only if its sequence still belongs to this transaction. A copy
made by the user meanwhile is preserved. Failed/canceled pre-dispatch paths
restore immediately under the same sequence condition. Copy fallback retains
the completed text when clipboard/input dispatch is unavailable. Success means
Windows accepted the gesture, not that the receiving app acknowledged insertion.

`target.cpp` retains legacy probe utilities and the token table; the production
adapter does not construct or call `TargetProbe`. The matrix helper uses its own
accessibility queries only for test setup/readback.

## Verification

`insertion_unit_test` covers tokens, callbacks, clipboard and stale operations
without injecting keys. `insertion_interactive_test` checks native EDIT/RichEdit,
password controls, late field/window switches, an unknown control, no field,
dispatch refusal and cancellation. It guards foreground ownership and restores
the user's clipboard. See `windows/tests/insertion-matrix.md` for app checks.