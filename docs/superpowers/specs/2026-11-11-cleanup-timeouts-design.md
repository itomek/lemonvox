# Cleanup timeout reporting

The user reports cleanup failures on Windows and macOS after at least a minute.
The configured remote server remains reachable and transcription succeeds.
Synthetic Qwen3.5-4B cleanup with default reasoning exceeded a 100-second client
timeout; the same prompt with enable_thinking=false completed in 0.69 seconds.
The installed non-reasoning Qwen3-4B-Instruct-2507 model completed in 7.13 seconds.
Switch the Windows cleanup setting to the tested non-reasoning model; do not
change global server settings or blindly add vendor extensions to generic APIs.

Represent request timeout as a distinct shared adapter error, appended to keep
existing enum values stable. Windows maps WinHTTP timeout and resource expiry;
macOS preserves URLSession timeout through the workflow bridge; Ubuntu separates
deadline-triggered cancellation from explicit user cancellation. Show a timeout
message recommending retry/faster model instead of claiming server unavailability.
Keep existing deadlines, cancellation, content-free diagnostics and no Enter.

Tests cover WinHTTP idle/resource timeouts and successful recovery, Windows
presentation, macOS cleanup/bridge propagation and Ubuntu deadline classification.
Verify platform CI, download the Windows artifact, and return to live capture.
