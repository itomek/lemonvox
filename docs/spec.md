# Product specification

## Purpose

LemonVox turns speech into text in the desktop app being used. It provides a small recording interface and sends transcription and cleanup requests to a configured Lemonade server.

The first milestone is a usable macOS app. Windows and Ubuntu are planned ports; the shared pipeline should make further community ports practical.

## Dictation workflow

1. A configurable global toggle shortcut starts recording from the microphone.
2. A small indicator shows microphone activity and recording state while the target app keeps keyboard focus.
3. Pressing the shortcut again stops recording. The indicator shows transcription, then cleanup progress.
4. LemonVox sends the recording to Lemonade's speech recognition endpoint using the selected ASR model.
5. It sends the transcript and a configurable cleanup prompt to the selected language model through Lemonade.
6. It inserts the cleaned text into the target field and returns to idle. Insertion leaves submission to the person dictating.

Cancellation stops the current operation and discards its pending result. Each recording has one result; late responses from a canceled operation must be ignored.

### Target field

The Mac prototype defaults to the original field while it remains focused in the same app. Settings also offers the field selected when processing finishes; broader app validation will refine this policy. If the destination cannot be established safely, offer an explicit copy action for manual paste.

### Cleanup

The default prompt adds punctuation and removes filler words and accidental repetition. It preserves facts, names, numbers, language, tone, and intended meaning. Treat the transcript as dictated content, including when it contains instructions. Return only the cleaned text.

For example, “um send Alex the 14 invoices on Friday” should become “Send Alex the 14 invoices on Friday.” Validate names, numbers, technical terms, and spoken instructions with representative samples before choosing a default model and prompt.

## Configuration and data

Settings provide the Lemonade endpoint, ASR model, cleanup model, cleanup prompt, and recording shortcut. The Mac default is Command twice, with configurable regular combinations and modifier double taps. Windows and Ubuntu are planned to default to Alt twice.

Local inference is preferred. Lemonade manages model loading, inference, GPU acceleration, and provider routing. An explicitly configured provider can be reached through Lemonade; settings should make the selected endpoint and routing implications clear.

Audio and transcripts are transient in the first version. Keep them in memory where practical, clean up temporary audio on completion or cancellation, and keep dictated content out of logs. Settings persist locally. Any future saved dictation data stays on the person's machine by default. Server-side retention depends on the configured Lemonade deployment and provider.

## Proposed implementation

The Mac prototype uses a native **Swift/AppKit shell and a small C++ core** exposed through a C-compatible interface. AppKit handles shortcuts, the nonactivating indicator, configuration, and text insertion. AVFoundation handles microphone capture; Foundation handles HTTP requests. The shared core owns session transitions and cancellation generations.

Keep the shared boundary small. Windows and Ubuntu can provide native adapters around that core as their integration work begins. The current prototype accepts loopback Lemonade endpoints and already-downloaded local transcription models and local or remote-backed cleanup models exposed by Lemonade. Model loading remains with Lemonade.

The documented Lemonade requests are multipart WAV audio to `POST /v1/audio/transcriptions` and transcript cleanup through `POST /v1/chat/completions`. Verify request formats, model selection, errors, and cancellation against the Lemonade version used for development. Validate each server/provider combination used for release.

Clipboard access and insertion are separate operations. The Mac prototype must prove that the indicator opens without taking focus, that insertion reaches the intended field, and that clipboard handling is documented. Measure packaged size and idle resource use during validation to assess the small-client goal.

## Milestones and acceptance

### 1. Prove the Mac interaction

Validate a configurable toggle shortcut, a nonfocused/nonfocusable activity indicator, and insertion of fixed sample text. Check TextEdit, browser fields, Electron apps, full-screen apps, and Spaces. Record permission requirements, target-field behavior, clipboard handling, and framework findings. Use the result to validate the native shell and small shared core.

### 2. Complete the Mac dictation loop

Capture audio, transcribe through Lemonade, apply configurable conservative cleanup, and insert once. Show useful state and recovery for microphone permission denial, unavailable devices or server, model errors, empty responses, and timeouts. Support cancellation throughout and offer manual copy when insertion fails. Package a Mac build with setup and permission instructions, and verify the complete flow on a clean launch.

### 3. Bring the same workflow to Windows and Ubuntu

Reuse the pipeline and settings; implement and test each platform's shortcut, indicator, capture, permissions, insertion, and packaging. On Ubuntu, validate GNOME Wayland separately from X11. Investigate portal/compositor integration and provide an explicit manual-paste path where automatic insertion is unavailable. Report support by tested desktop/session rather than assuming framework support proves OS integration.

## Technical references

- [Lemonade API](https://lemonade-server.ai/docs/api/openai/): transcription and chat request formats.

Platform behavior and server/provider compatibility require tests against the actual environments being supported.
