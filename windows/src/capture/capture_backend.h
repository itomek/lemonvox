#pragma once

#include "capture/wav.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace lemonvox::win {

// Native reason behind a capture failure. AdapterError stays coarse for the
// core; the UI reads this to choose user-facing text and recovery actions.
enum class CaptureFailureReason : std::uint8_t {
    none,
    permission_denied, // Windows microphone privacy setting blocks this app
    no_device,         // no active capture endpoint, or the selected one is gone
    device_busy,       // endpoint is held exclusively by another app
    device_lost,       // removed, disabled or default switched while recording
    format,            // endpoint mix format is not convertible
    internal,          // COM/WASAPI failure without a more specific cause
};

const char *capture_failure_reason_name(CaptureFailureReason reason);

// One opened, initialized capture endpoint. Destruction stops it and releases
// every native resource. All methods run on the thread that opened it.
class CaptureStream {
public:
    using PacketSink = std::function<void(const std::uint8_t *data, std::uint32_t frames)>;

    virtual ~CaptureStream() = default;
    virtual const audio::PcmFormat &format() const = 0;
    // Signaled when audio is ready or a device change needs attention.
    virtual HANDLE ready_event() const = 0;
    virtual CaptureFailureReason start() = 0;
    // Hands every available packet to sink; data is nullptr for silent packets.
    virtual CaptureFailureReason read(const PacketSink &sink) = 0;
};

// Opens and initializes an endpoint without starting it. An empty device_id
// selects the current default capture endpoint. Runs on a thread that has
// initialized COM.
class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual CaptureFailureReason open(const std::wstring &device_id, std::unique_ptr<CaptureStream> &stream) = 0;
};

} // namespace lemonvox::win
