# PR 105 Ubuntu verification

Date: November 14, 2026. Tested PR head: `249db66f40acb57c4127f0a52b0eabca3d8d2ce3`.
Host: Ubuntu 24.04.5 LTS, aarch64, GNOME Shell 46.0, GCC 13.3.0, CMake 3.28.3, GTK 4.14.5, libadwaita 1.5.0, PipeWire 1.0.5.
Three agents divided build/service, core/contracts/recovery, and production desktop verification.

No product source changes were necessary for the passing copy/recovery checks. This evidence branch preserves logs, disposable verification harnesses, and screenshots independently of the PR head.

## Passing checks

```sh
cmake -S ubuntu -B .build/ubuntu-102 -DCMAKE_BUILD_TYPE=Debug
cmake --build .build/ubuntu-102 --parallel 2
ctest --test-dir .build/ubuntu-102 --output-on-failure
bash scripts/test-core.sh --output /tmp/pr105-core-tests
python3 ubuntu/tests/extension_contract_test.py
gjs -m ubuntu/tests/extension_policy_test.mjs
dbus-run-session -- python3 ubuntu/tests/service_test.py .build/ubuntu-102/lemonvox
LIBGL_ALWAYS_SOFTWARE=1 bash ubuntu/tests/extension_native_shell_test.sh
```

- Debug build passed; CTest 11/11 passed.
- Portable core passed; XML contract 3/3 passed; GJS policy passed; private D-Bus service passed; isolated native Shell suite passed.
- Extended real service/private-bus checks with virtual audio and mocked HTTP passed: initial empty state, repeat non-consuming copies, new raw text after cleanup HTTP failure, cleaned replacement, native insertion-unavailable fallback, ASR HTTP failure preservation, delayed-ASR cancellation preservation, and actual process restart clearing retained text.
- A private GNOME 46 Wayland session staged the production extension and ran the production application. Explicit menu activation twice replaced a clipboard sentinel with the expected retained text; real GTK Entry paste matched, with no Enter; a subsequent empty-ASR failure retained the previous text and the menu copied it again.
- The actual GTK Settings Copy Last Transcription button was activated using Tab/Space; the status changed to “Copied. Paste into your field.” and the independently read Wayland clipboard matched.
- Desktop screenshots contain only disposable test UI and synthetic content. The logged-in extension configuration and clipboard were not used for these checks.

Missing audio utilities were downloaded from Ubuntu packages and extracted under /tmp; no privileged installation was performed. Service and loopback commands used PATH entries for those extracted tools. Harness files are evidence snapshots: desktop scripts refer to /tmp paths and expect execution from the repository root; restore their named paths to reproduce.

## Real-server audio result: acceptance remains open

```sh
python3 ubuntu/tests/audio-loopback.py \
  --driver .build/ubuntu-102/lemonvox_workflow_audio_test \
  --endpoint http://127.0.0.1:13305/v1 \
  --asr Moonshine-Small-Streaming \
  --cleanup Llama-3.2-1B-Instruct-GGUF
```

Both configured models were downloaded and eligible. The unmodified gate failed on both original runs with:
“Synthetic ASR/cleanup did not independently preserve the expected name, number and day.”

A separate disposable driver with additional boolean diagnostics passed once with:
`asr_checked=1 asr_matches=1 cleanup_matches=1 retained_equals_result=1`.
Its phrase assertions were unchanged. This confirms retention on that successful run but does not clear the original gate or establish why the original runs failed. All three outcomes are preserved.

Only generated speech and explicitly named virtual audio monitors were used; no physical microphone or default-device changes. No models were downloaded or configuration changed.

## Remaining acceptance

Keep PR 105 and issue 102 open and unmerged. Real-audio acceptance is unresolved. Physical microphone checks, Windows native/interactive cancellation checks, macOS microphone/server checks, and cancellation during a real desktop insertion followed by explicit copy without delayed overwrite are not claimed passed.
