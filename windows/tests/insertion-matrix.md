# Windows current-focus paste matrix

Issue #80 replaces field discovery and original-field matching with one native
Ctrl+V at completion focus. No Enter or app activation is performed. No selected
field or unknown accessibility element is a normal paste attempt, not a warning.

## Automated native checks

```powershell
powershell -ExecutionPolicy Bypass -File windows\scripts\build.ps1 -Output $env:TEMP\lv-insertion\build -Interactive
```

`insertion_interactive_test` owns both its windows and covers:

- EDIT and RichEdit: exact synthetic text once, no newline, clipboard restored.
- Focus moved to a different field or window after inspect: final focus receives
  text once, original text unchanged, final focus preserved.
- Password EDIT: ordinary paste, without discovery or a policy warning.
- A custom control with no caret/UIA provider: handles Ctrl+V and reads the text.
- No text field selected: top-level window receives Ctrl+V; success is dispatch.
- Cancel queued, cancel before insert, cancel immediately before dispatch: no text.
- Dispatch refusal: `insertion_unavailable`, text unchanged, clipboard restored.

Controller tests cover Stop/repeat shortcut completion, completion-target policy,
Copy on dispatch failure and cancellation of late pipeline results. Trigger
tests check ordinary idle typing passes through. Settings tests cover tab order,
accessibility names and saving without a target choice. Preference tests seed a
legacy `InsertCurrentTarget=0` and verify completion focus and removal on save.

## Real apps

```powershell
powershell -ExecutionPolicy Bypass -File windows\tests\insertion-matrix.ps1 `
  -Probe $env:TEMP\lv-insertion\build\src\insertion\insertion_matrix_probe.exe `
  -Targets notepad,edge,chrome,vscode
```

The script owns temporary files/browser profiles and holds
`Local\LemonVoxInteractiveTestLock`. The probe verifies the launched window's
title/process IDs and foreground immediately before sending input. Accessibility
is used by the helper to arrange focus and read results, never by WinInsertion.

Notepad and VS Code save a temporary file for exact comparison. Browser runs
paste into a textarea, switch from that field to a second textarea and paste
there, then dispatch into a password field (`--expect-unreadable`: hidden text
cannot be read back). The cold-browser run starts without accessibility setup.
Each readable destination must contain the synthetic text exactly once without
a following newline, and clipboard sentinel data must be restored.

For an end-to-end focus switch, start synthetic-file dictation in the real app,
switch fields/apps while cleanup is pending, and verify one paste at final focus.
Repeat with Stop, the repeat shortcut, Cancel and the bar's X. With no field
selected, expect the ordinary paste outcome and no field-selection warning.
Verify an administrator window offers Copy when UIPI blocks dispatch. Do not
elevate LemonVox to run this check.

## Current evidence (2026-11-11)

The regressions and matrix were updated for issue #80. Local native execution
is pending: the Windows build script stops at missing `vswhere.exe`/MSVC Build
Tools on this machine. Earlier successful field-refusal results do not validate
the new behavior. Record fresh native/browser/Electron results before release.