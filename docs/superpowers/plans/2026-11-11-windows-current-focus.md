# Windows current-focus paste Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement Windows issue #80's completion-focus paste workflow.
**Architecture:** Keep the portable workflow unchanged. Windows operation tokens
carry no field identity; dispatch Ctrl+V at current focus using the existing
clipboard transaction. Remove the Windows preference and Settings controls.
**Tech Stack:** C++17, Win32, MSVC, CMake/Ninja, CTest.
**Spec:** ../specs/2026-11-11-windows-current-focus-design.md

## Global Constraints

No field discovery or app activation. One paste, no Enter. Cancel suppresses late
paste. Legacy settings cannot retain original-field behavior. Copy on OS failure.

## Review Focus

Focus changes after inspect; cancellation just before dispatch; held modifiers;
no editable/accessibility field; failed dispatch preserves clipboard.

## Task 1: Native completion paste

Files: windows/src/insertion/win_insertion.{h,cpp}, windows/tests/insertion_{unit,interactive}_test.cpp.
Interface: retain InsertionAdapter and operation-token lifetime API.
- [ ] Replace field-refusal regressions with current-focus and unrecognized/no-field dispatch assertions; run and observe failures.
- [ ] Remove probes from capture/inspect/insert; retain operation validity, clipboard, modifier and cancellation checks; check current foreground integrity immediately before dispatch.
- [ ] Run insertion tests, including final focus change and cancellation before send.

## Task 2: Preferences, Settings and wording

Files: windows/src/prefs/preferences.{h,cpp}, windows/src/ui/settings*, windows/src/app/dictation_controller.cpp, windows/tests/{prefs,settings_interactive}_test.cpp.
Interface: WinPreferences::snapshot() always sets shared insert_current_target=true.
- [ ] Add failing preference regression for saved InsertCurrentTarget=0.
- [ ] Remove Windows target preference/control; ignore legacy value and delete it on save.
- [ ] Update Settings tests and dispatch-failure wording; run Windows suite.

## Task 3: Matrix and documentation

Files: windows/tests/insertion_matrix_probe.cpp, windows/tests/insertion-matrix.{ps1,md}, windows/README.md, windows/src/insertion/README.md.
- [ ] Update matrix assertions to final-focus paste and normal password/no-field behavior.
- [ ] Document behavior, build and run available checks, record results and limitations.
- [ ] Review final diff against all issue acceptance criteria.
