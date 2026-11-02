// Device probe. Enumerates capture endpoints and initializes an IAudioClient on
// the default endpoint WITHOUT starting it, so no microphone audio is ever
// captured. Exits 77 (ctest skip) when no usable device or access is available.

#include "capture/device_list.h"
#include "capture/wasapi_stream.h"

#include <cassert>
#include <cstdio>
#include <cwchar>

using namespace lemonvox::win;

namespace {
constexpr int kSkip = 77;
}

int main() {
    const auto list = list_capture_devices();
    assert(list.ok);
    std::printf("capture endpoints: %zu\n", list.devices.size());
    if (list.devices.empty() || list.default_id.empty()) {
        std::puts("SKIP: no active capture device");
        return kSkip;
    }
    int defaults = 0;
    for (const auto &device : list.devices) {
        assert(!device.id.empty() && !device.name.empty());
        if (device.is_default) {
            ++defaults;
            assert(device.id == list.default_id);
        }
    }
    assert(defaults == 1);

    lemonvox::win::audio::PcmFormat format;
    const auto reason = probe_capture_device(L"", &format);
    std::printf("default endpoint probe: %s\n", capture_failure_reason_name(reason));
    if (reason == CaptureFailureReason::permission_denied || reason == CaptureFailureReason::device_busy ||
        reason == CaptureFailureReason::no_device) {
        std::puts("SKIP: default capture device is not usable here");
        return kSkip;
    }
    assert(reason == CaptureFailureReason::none);
    assert(lemonvox::win::audio::is_valid(format));
    std::printf("mix format: %u Hz, %u ch\n", format.sample_rate, static_cast<unsigned>(format.channels));

    // The same endpoint by explicit ID, and an unknown ID.
    assert(probe_capture_device(list.default_id) == CaptureFailureReason::none);
    assert(probe_capture_device(L"{0.0.1.00000000}.{00000000-0000-0000-0000-000000000000}") ==
        CaptureFailureReason::no_device);
    std::puts("capture_device_test passed");
    return 0;
}
