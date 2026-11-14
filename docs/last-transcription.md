# Recover the last transcription

Choose **Copy Last Transcription** from the LemonVox menu to copy the latest
available dictated text, then paste it where you need it. The action can be used
repeatedly. It is disabled until text is available, and may be temporarily
disabled while LemonVox is inserting text to protect its clipboard transaction.

LemonVox retains the speech recognition result before cleanup starts, then
replaces it with cleaned text when cleanup succeeds. Cleanup or insertion failure
therefore leaves available text recoverable. An attempt that produces no text
does not erase the previous result.

Only the latest result is kept, in process memory. Quitting clears it. This does
not recover audio or text after an app crash or restart. Copying explicitly places
the text on the system clipboard, where normal clipboard retention applies.

## Native verification checklist

Run this on macOS, Windows, and Ubuntu with a working Lemonade server:

1. Start a fresh process; confirm Copy Last Transcription is disabled.
2. Dictate a recognizable sentence and let insertion finish. Copy twice and paste
   into a scratch document after each copy; both should match the cleaned text.
3. Dictate a different sentence; copying should now return the new text.
4. Make cleanup fail after ASR succeeds; copy should return the ASR text.
5. Make insertion fail after cleanup succeeds; copy should return cleaned text.
6. Fail ASR or cancel before it returns; the previous retained result must survive.
7. Restart LemonVox; confirm the action is disabled again.

Record OS/version, build commit, and outcomes. Automated build checks do not
substitute for microphone, desktop menu, and clipboard checks on each native OS.

## Build and test on the target machines

Keep issue #102 and its PR open until these results are recorded.
Check out branch `tmi/102-copy-last-transcription` and record `git rev-parse HEAD`.

Windows, from a Developer PowerShell with the prerequisites in
[Windows development](../windows/DEVELOPMENT.md):

```powershell
powershell -ExecutionPolicy Bypass -File windows\scripts\build.ps1 -Output C:\temp\lemonvox-102
```

Ubuntu, after installing the prerequisites in
[Ubuntu development](../ubuntu/DEVELOPMENT.md):

```sh
cmake -S ubuntu -B .build/ubuntu-102 -DCMAKE_BUILD_TYPE=Debug
cmake --build .build/ubuntu-102 --parallel
ctest --test-dir .build/ubuntu-102 --output-on-failure
python3 ubuntu/tests/extension_contract_test.py
gjs -m ubuntu/tests/extension_policy_test.mjs
```

Install/reload the matching GNOME extension as described in Ubuntu development;
check both the panel menu and the Settings copy control. Run the native checklist
above on both OSes. Also cancel during insertion, then copy after insertion has
settled: no delayed paste or clipboard restoration may overwrite the explicit
copy. Post the commit, command results, and manual outcomes to the PR; report
unavailable scenarios as pending rather than passed.
