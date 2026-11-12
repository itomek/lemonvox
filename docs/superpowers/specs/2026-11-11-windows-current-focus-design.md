# Windows completion paste

Approved in chat on 2026-11-11. Implements issue #80, following Ubuntu PR #78.

Completed dictation dispatches one native Ctrl+V to current keyboard focus. No
field discovery, original-target matching, focus activation or Enter. Stop and
repeat shortcut process once; Cancel/X suppress late paste. Remove the target
setting and ignore legacy saved values. Offer Copy for clipboard/input dispatch
failure, without attributing failure to an uninspectable field.

## Approach

Adapt the Windows insertion adapter, keeping the shared core and clipboard
transaction. Its target tokens authorize an operation, not an HWND/UIA field.
Capture/inspect satisfy the shared interface without accessibility queries.
Windows preferences always select completion focus. Keep modifier-release waits,
clipboard restoration, integrity checks at dispatch, cancellation and tagged input.
Changing the shared workflow would affect other platforms unnecessarily; keeping
field inspection would violate the requested paste-anywhere behavior.

Verify native, browser and Electron synthetic-text paste, focus changes, unknown
fields/no field, cancellation, no Enter, legacy preferences and Settings removal.
Record unavailable environment checks explicitly.
