#pragma once

#include "capture/capture_backend.h"

#include <mmreg.h>

#include <memory>
#include <string>

namespace lemonvox::win {

// Shared-mode, event-driven WASAPI endpoints. Watches for removal, disable and
// (when following the default) default-endpoint changes and reports them from
// read() as device_lost.
std::shared_ptr<CaptureBackend> make_wasapi_backend();

// Maps a WASAPI/MMDevice HRESULT to a capture failure reason.
CaptureFailureReason capture_reason_from_hresult(HRESULT hr, bool opening);

// Parses a mix format into a convertible PcmFormat. Returns false for formats
// the converter does not support.
bool pcm_format_from_wave(const WAVEFORMATEX *wave, audio::PcmFormat &format);

// Opens and initializes an IAudioClient on the endpoint (empty = default)
// without ever calling Start, so no audio is captured. Initializes COM on the
// calling thread if needed.
CaptureFailureReason probe_capture_device(const std::wstring &device_id, audio::PcmFormat *mix_format = nullptr);

} // namespace lemonvox::win
