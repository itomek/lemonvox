#include "capture/device_list.h"

#include "capture/com_scope.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace lemonvox::win {
namespace {

// PKEY_Device_FriendlyName, defined locally to avoid INITGUID ordering rules.
const PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::wstring take_id(IMMDevice *device) {
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return {};
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

std::wstring friendly_name(IMMDevice *device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

} // namespace

CaptureDeviceList list_capture_devices() {
    CaptureDeviceList list;
    ComScope com;
    if (!com.usable()) return list;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return list;
    }
    ComPtr<IMMDevice> default_device;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &default_device))) {
        list.default_id = take_id(default_device.Get());
    }
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) return list;
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) return list;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        CaptureDeviceInfo info;
        info.id = take_id(device.Get());
        if (info.id.empty()) continue;
        info.name = friendly_name(device.Get());
        if (info.name.empty()) info.name = L"Microphone";
        info.is_default = !list.default_id.empty() && _wcsicmp(info.id.c_str(), list.default_id.c_str()) == 0;
        list.devices.push_back(std::move(info));
    }
    list.ok = true;
    return list;
}

} // namespace lemonvox::win
