#pragma once

#include <string>
#include <vector>

namespace lemonvox::win {

struct CaptureDeviceInfo {
    std::wstring id;   // stable MMDevice endpoint ID; persist this in Settings
    std::wstring name; // friendly name for display
    bool is_default = false;
};

struct CaptureDeviceList {
    bool ok = false; // false when the MMDevice enumerator itself failed
    std::vector<CaptureDeviceInfo> devices;
    std::wstring default_id; // empty when there is no default capture endpoint
};

// Active capture endpoints in system order, with the console-role default
// marked. Initializes COM on the calling thread if needed. Never opens a stream.
CaptureDeviceList list_capture_devices();

} // namespace lemonvox::win
