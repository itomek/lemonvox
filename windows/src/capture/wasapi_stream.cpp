#include "capture/wasapi_stream.h"

#include "capture/com_scope.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace lemonvox::win {
namespace {

// 200 ms shared-mode buffer; the event fires once per device period (~10 ms).
constexpr REFERENCE_TIME kBufferDuration = 2'000'000;

bool same_id(const wchar_t *a, const std::wstring &b) {
    return a && _wcsicmp(a, b.c_str()) == 0;
}

// Bridges MMDevice notifications (arriving on a system thread) to the capture
// worker: it flags the loss and wakes the worker through its own handle.
class DeviceWatcher final : public IMMNotificationClient {
public:
    DeviceWatcher(std::wstring id, bool follows_default, HANDLE wake)
        : id_(std::move(id)), follows_default_(follows_default) {
        if (!DuplicateHandle(GetCurrentProcess(), wake, GetCurrentProcess(), &wake_, 0, FALSE,
                DUPLICATE_SAME_ACCESS)) {
            wake_ = nullptr;
        }
    }

    bool lost() const { return lost_.load(); }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refs_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        if (same_id(id, id_) && (state & DEVICE_STATE_ACTIVE) == 0) mark_lost();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        if (same_id(id, id_)) mark_lost();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        if (follows_default_ && flow == eCapture && role == eConsole && !same_id(id, id_)) mark_lost();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    ~DeviceWatcher() {
        if (wake_) CloseHandle(wake_);
    }

    void mark_lost() {
        lost_.store(true);
        if (wake_) SetEvent(wake_);
    }

    std::atomic<ULONG> refs_{1};
    std::atomic<bool> lost_{false};
    const std::wstring id_;
    const bool follows_default_;
    HANDLE wake_ = nullptr;
};

class WasapiStream final : public CaptureStream {
public:
    ~WasapiStream() override {
        if (started_) client_->Stop();
        if (watcher_) {
            enumerator_->UnregisterEndpointNotificationCallback(watcher_);
            watcher_->Release();
        }
        capture_.Reset();
        client_.Reset();
        device_.Reset();
        enumerator_.Reset();
        if (event_) CloseHandle(event_);
    }

    CaptureFailureReason open(const std::wstring &device_id) {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) return CaptureFailureReason::internal;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator_));
        if (FAILED(hr)) return capture_reason_from_hresult(hr, true);

        const bool follows_default = device_id.empty();
        hr = follows_default ? enumerator_->GetDefaultAudioEndpoint(eCapture, eConsole, &device_)
                             : enumerator_->GetDevice(device_id.c_str(), &device_);
        if (FAILED(hr)) return capture_reason_from_hresult(hr, true);
        DWORD state = 0;
        if (FAILED(device_->GetState(&state)) || state != DEVICE_STATE_ACTIVE) return CaptureFailureReason::no_device;

        LPWSTR id = nullptr;
        if (FAILED(device_->GetId(&id)) || !id) return CaptureFailureReason::internal;
        std::wstring resolved(id);
        CoTaskMemFree(id);

        // Register before activation so a removal during setup is not missed.
        watcher_ = new DeviceWatcher(std::move(resolved), follows_default, event_);
        hr = enumerator_->RegisterEndpointNotificationCallback(watcher_);
        if (FAILED(hr)) {
            watcher_->Release();
            watcher_ = nullptr;
            return capture_reason_from_hresult(hr, true);
        }

        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
        if (FAILED(hr)) return capture_reason_from_hresult(hr, true);
        WAVEFORMATEX *mix = nullptr;
        hr = client_->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) return capture_reason_from_hresult(hr, true);
        const bool supported = pcm_format_from_wave(mix, format_);
        if (supported) {
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, kBufferDuration, 0, mix,
                nullptr);
        }
        CoTaskMemFree(mix);
        if (!supported) return CaptureFailureReason::format;
        if (FAILED(hr)) return capture_reason_from_hresult(hr, true);
        hr = client_->SetEventHandle(event_);
        if (FAILED(hr)) return capture_reason_from_hresult(hr, true);
        hr = client_->GetService(IID_PPV_ARGS(&capture_));
        if (FAILED(hr)) return capture_reason_from_hresult(hr, true);
        return watcher_->lost() ? CaptureFailureReason::device_lost : CaptureFailureReason::none;
    }

    const audio::PcmFormat &format() const override { return format_; }
    HANDLE ready_event() const override { return event_; }

    CaptureFailureReason start() override {
        if (watcher_ && watcher_->lost()) return CaptureFailureReason::device_lost;
        const HRESULT hr = client_->Start();
        if (FAILED(hr)) return capture_reason_from_hresult(hr, false);
        started_ = true;
        return CaptureFailureReason::none;
    }

    CaptureFailureReason read(const PacketSink &sink) override {
        if (watcher_ && watcher_->lost()) return CaptureFailureReason::device_lost;
        for (;;) {
            UINT32 packet = 0;
            HRESULT hr = capture_->GetNextPacketSize(&packet);
            if (FAILED(hr)) return capture_reason_from_hresult(hr, false);
            if (packet == 0) return CaptureFailureReason::none;
            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) return CaptureFailureReason::none;
            if (FAILED(hr)) return capture_reason_from_hresult(hr, false);
            sink((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data, frames);
            hr = capture_->ReleaseBuffer(frames);
            if (FAILED(hr)) return capture_reason_from_hresult(hr, false);
        }
    }

private:
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    DeviceWatcher *watcher_ = nullptr;
    HANDLE event_ = nullptr;
    audio::PcmFormat format_;
    bool started_ = false;
};

class WasapiBackend final : public CaptureBackend {
public:
    CaptureFailureReason open(const std::wstring &device_id, std::unique_ptr<CaptureStream> &stream) override {
        stream.reset();
        auto opened = std::make_unique<WasapiStream>();
        const auto reason = opened->open(device_id);
        if (reason == CaptureFailureReason::none) stream = std::move(opened);
        return reason;
    }
};

bool is_ks_subtype(const GUID &guid, unsigned long tag) {
    // KSDATAFORMAT_SUBTYPE_* GUIDs are {tag-0000-0010-8000-00aa00389b71}.
    static const unsigned char tail[8] = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    return guid.Data1 == tag && guid.Data2 == 0x0000 && guid.Data3 == 0x0010 &&
        std::memcmp(guid.Data4, tail, sizeof(tail)) == 0;
}

} // namespace

std::shared_ptr<CaptureBackend> make_wasapi_backend() {
    return std::make_shared<WasapiBackend>();
}

CaptureFailureReason capture_reason_from_hresult(HRESULT hr, bool opening) {
    if (SUCCEEDED(hr)) return CaptureFailureReason::none;
    if (hr == E_ACCESSDENIED) return CaptureFailureReason::permission_denied;
    if (hr == AUDCLNT_E_DEVICE_IN_USE) return CaptureFailureReason::device_busy;
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return CaptureFailureReason::format;
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
        hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
        return opening ? CaptureFailureReason::no_device : CaptureFailureReason::device_lost;
    }
    return CaptureFailureReason::internal;
}

bool pcm_format_from_wave(const WAVEFORMATEX *wave, audio::PcmFormat &format) {
    if (!wave || wave->nChannels == 0) return false;
    WORD tag = wave->wFormatTag;
    WORD container = wave->wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (wave->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
        const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wave);
        if (is_ks_subtype(extensible->SubFormat, WAVE_FORMAT_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (is_ks_subtype(extensible->SubFormat, WAVE_FORMAT_PCM)) tag = WAVE_FORMAT_PCM;
        else return false;
    }
    audio::PcmFormat parsed;
    parsed.sample_rate = wave->nSamplesPerSec;
    parsed.channels = wave->nChannels;
    if (tag == WAVE_FORMAT_IEEE_FLOAT && container == 32) parsed.sample = audio::SampleFormat::float32;
    else if (tag == WAVE_FORMAT_PCM && container == 16) parsed.sample = audio::SampleFormat::int16;
    else if (tag == WAVE_FORMAT_PCM && container == 24) parsed.sample = audio::SampleFormat::int24;
    else if (tag == WAVE_FORMAT_PCM && container == 32) parsed.sample = audio::SampleFormat::int32;
    else return false;
    if (!audio::is_valid(parsed) || wave->nBlockAlign != audio::bytes_per_frame(parsed)) return false;
    format = parsed;
    return true;
}

CaptureFailureReason probe_capture_device(const std::wstring &device_id, audio::PcmFormat *mix_format) {
    ComScope com;
    if (!com.usable()) return CaptureFailureReason::internal;
    std::unique_ptr<CaptureStream> stream;
    const auto reason = WasapiBackend().open(device_id, stream);
    if (reason == CaptureFailureReason::none && mix_format) *mix_format = stream->format();
    return reason;
}

const char *capture_failure_reason_name(CaptureFailureReason reason) {
    switch (reason) {
    case CaptureFailureReason::none: return "none";
    case CaptureFailureReason::permission_denied: return "permission_denied";
    case CaptureFailureReason::no_device: return "no_device";
    case CaptureFailureReason::device_busy: return "device_busy";
    case CaptureFailureReason::device_lost: return "device_lost";
    case CaptureFailureReason::format: return "format";
    case CaptureFailureReason::internal: return "internal";
    }
    return "internal";
}

} // namespace lemonvox::win
