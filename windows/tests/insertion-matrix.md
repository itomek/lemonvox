# Windows insertion matrix

LemonVox inserts by pasting: it snapshots the clipboard, places the text (marked
private), synthesizes Ctrl+V into the focused field and restores the previous
clipboard. It never synthesizes Enter. This matrix records which targets accept
that paste and which fall back to the manual Copy action.

Automated coverage lives in two places:

- `insertion_interactive_test` (ctest label `interactive`) opens its own window
  with a native EDIT, a password EDIT and a RichEdit and checks single insertion
  without a newline, secure-field rejection, changed control and changed window,
  clipboard restoration and cancellation.
- `insertion-matrix.ps1` drives `insertion_matrix_probe.exe` against
  applications it launches itself.

## Safety rules for any run

- Hold `Local\LemonVoxInteractiveTestLock` for the whole run (the script does;
  the probe takes it itself unless given `--no-lock`).
- Only target windows you launched: a unique title plus the process IDs you
  started. Never type into a pre-existing window.
- Confirm the target is the foreground window before every injected key. The
  probe refuses to start when activation fails, and `before_send` re-checks the
  foreground immediately before the adapter's `SendInput`.
- Launch browsers with a temporary `--user-data-dir` and editors with a
  temporary profile; delete them and any temporary file afterwards.

## Running the semi-automated matrix

```powershell
powershell -ExecutionPolicy Bypass -File windows\scripts\build.ps1 -Output $env:TEMP\lv-insertion\build
powershell -ExecutionPolicy Bypass -File windows\tests\insertion-matrix.ps1 `
    -Probe $env:TEMP\lv-insertion\build\src\insertion\insertion_matrix_probe.exe `
    -Targets notepad,edge,chrome,vscode
```

Each probe run prints the captured and inspected facts (`editable`, `secure`,
`frontmost`, `matches`), the insertion result, a read-back of the target field
through UI Automation and whether the clipboard returned to a sentinel value set
before the run. The person's own clipboard is saved first and restored last.

A busy desktop can take the foreground away between steps; the probe then
reports `error=target-lost-foreground` or `readback.foreground=0` and injects
nothing further. Rerun when the desktop is idle.

### Notepad

The script refuses to run when Notepad is already open, because Windows 11
Notepad may adopt the file as a tab in an existing window. It creates an empty
temporary `.txt`, launches Notepad on it, pastes, saves with Ctrl+S, compares the
saved file byte for byte, closes the tab with Ctrl+W, ends only the processes it
started and deletes the file. Saving first means closing discards nothing and
session restore has no unsaved tab to bring back.

### Edge and Chrome

The page is `data:text/html,` with a `<textarea aria-label=LemonVoxMatrixField
autofocus>`, an `<input type=password aria-label=LemonVoxMatrixSecret>` and a
script that focuses the textarea once and then appends `ready` to the title. A
browser window activated after its page loaded may keep focus on the address bar
or the document body, so the probe can additionally wait until UI Automation
reports a named field as focused (moving focus with UIA `SetFocus`, never with
input). Each browser gets three probe runs in the same window:

1. insert into the textarea (exactly once, no newline, clipboard restored);
2. capture the textarea, move focus to the password field, then inspect and
   insert with the original token: `matches_original=false`, `target_changed`,
   no input;
3. focus the password field: captured `secure`, no token, no input.

Edge runs `--inprivate` to avoid the implicit sign-in and first-run panes of a
fresh profile. Chrome runs `-Cold`: its first insertion skips the probe's UIA
focus step, so the adapter is the first accessibility client of that browser
and must make the field visible itself.

Chrome exposes only a placeholder pane to UI Automation until a client uses
MSAA; `--force-renderer-accessibility` and `--enable-features=UiaProvider` did
not change that, while walking MSAA `accFocus` from the top-level window turned
the tree on at once (UI Automation then reported the focused textarea with a
runtime ID and `IsPassword` for the password field). The adapter does that walk
only when focus is that placeholder, then re-reads UI Automation.
### VS Code (Electron)

The script launches `Code.exe` with temporary `--user-data-dir` and
`--extensions-dir`, `--disable-extensions` and a temporary file, pastes, saves
with Ctrl+S and compares the file. The editor's accessible text only covers a
window around the caret, so the saved file is the evidence.

### Elevated window (manual)

Synthesized input cannot reach a higher-integrity window (UIPI), and a
medium-integrity LemonVox cannot read its UI Automation tree. To check the
fallback by hand:

1. Open an elevated Command Prompt or Windows Terminal ("Run as administrator").
2. Run LemonVox normally (not elevated) and dictate into the elevated window.
3. Expect no typed text and a copy fallback (`insertion_unavailable` from
   `WinInsertion::insert`, which the core reports as `paste_unavailable`). The
   clipboard keeps its previous content.

Do not run LemonVox as administrator to work around this; an elevated LemonVox
would type into every elevated window, including credential prompts of admin
tools. The integrity comparison itself is covered by `insertion_unit_test`.

## Results

Windows 11 Enterprise 26H2, build 26300.9711 (`ver`: 10.0.26300.9711), x64,
Notepad 11.2607.14.0, Chrome and Edge current stable, 2026-11-02. "Pass" means: captured as editable and not
secure, inspect matched the original target, exactly one paste, the field (or
saved file) contains `LemonVox fixed text ✓ 123` exactly once with no newline,
and the clipboard was restored.

| Target | Result | Notes |
| --- | --- | --- |
| Native EDIT, RichEdit (test window) | Pass | `insertion_interactive_test`. |
| Password EDIT (test window) | Pass (rejected) | Reported `secure`, no token, nothing typed. |
| Changed control / changed window | Pass (rejected) | `matches_original=false`; insert returns `target_changed`, nothing typed. |
| Notepad (Windows 11) | Pass | Saved file identical. |
| Edge (Chromium, data: textarea) | Pass | Insert, changed focus (rejected) and password field (rejected) all pass. Exposes web content through UI Automation. |
| Chrome (data: textarea) | Pass | Same three checks pass, including a cold browser where the adapter is the first accessibility client (2 full matrix runs plus 3 cold inserts and 1 cold password check). |
| VS Code (Electron) | Pass | Monaco's input exposes an editable Edit; saved file identical. |
| Elevated window | Not automated | Requires a UAC prompt; see the manual steps. The adapter refuses on integrity before any clipboard or input work. |

Earlier result (before the MSAA step): Chrome ended every dictation in the Copy
fallback because its text fields were invisible to UI Automation.
