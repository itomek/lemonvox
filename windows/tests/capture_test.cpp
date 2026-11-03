// Non-interactive capture tests. They never open a real microphone: the
// WasapiCapture contract tests run against an in-memory fake backend.

#include "capture/wasapi_capture.h"
#include "capture/wasapi_stream.h"
#include "capture/wav.h"

#include <windows.h>

#include <audioclient.h>
#include <mmreg.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

using namespace lemonvox;
using namespace lemonvox::win;
using namespace lemonvox::win::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint32_t u32(const std::vector<std::uint8_t> &b, std::size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
std::uint16_t u16(const std::vector<std::uint8_t> &b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}
bool tag(const std::vector<std::uint8_t> &b, std::size_t at, const char *text) {
    return std::memcmp(b.data() + at, text, 4) == 0;
}

void check_header(const std::vector<std::uint8_t> &wav, std::size_t samples) {
    assert(wav.size() == kWavHeaderBytes + samples * 2);
    assert(tag(wav, 0, "RIFF"));
    assert(u32(wav, 4) == wav.size() - 8);
    assert(tag(wav, 8, "WAVE"));
    assert(tag(wav, 12, "fmt "));
    assert(u32(wav, 16) == 16);
    assert(u16(wav, 20) == 1);
    assert(u16(wav, 22) == 1);
    assert(u32(wav, 24) == 16000);
    assert(u32(wav, 28) == 32000);
    assert(u16(wav, 32) == 2);
    assert(u16(wav, 34) == 16);
    assert(tag(wav, 36, "data"));
    assert(u32(wav, 40) == samples * 2);
}

std::vector<std::int16_t> wav_samples(const std::vector<std::uint8_t> &wav) {
    std::vector<std::int16_t> samples((wav.size() - kWavHeaderBytes) / 2);
    std::memcpy(samples.data(), wav.data() + kWavHeaderBytes, samples.size() * 2);
    return samples;
}

// Rising zero crossings per second over the middle of the signal.
double estimate_frequency(const std::vector<std::int16_t> &samples, std::uint32_t rate) {
    const std::size_t skip = rate / 20;
    std::size_t first = 0;
    std::size_t last = 0;
    int crossings = -1;
    for (std::size_t i = skip + 1; i + skip < samples.size(); ++i) {
        if (samples[i - 1] < 0 && samples[i] >= 0) {
            if (crossings < 0) first = i;
            last = i;
            ++crossings;
        }
    }
    assert(crossings > 0);
    return crossings * static_cast<double>(rate) / static_cast<double>(last - first);
}

std::vector<std::uint8_t> float_stereo_sine(std::uint32_t rate, double seconds, double hz, double amplitude) {
    const auto frames = static_cast<std::size_t>(rate * seconds);
    std::vector<std::uint8_t> bytes(frames * 8);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto value = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * i / rate));
        std::memcpy(&bytes[i * 8], &value, 4);
        std::memcpy(&bytes[i * 8 + 4], &value, 4);
    }
    return bytes;
}

std::vector<std::uint8_t> int16_mono_sine(std::uint32_t rate, double seconds, double hz, double amplitude) {
    const auto frames = static_cast<std::size_t>(rate * seconds);
    std::vector<std::uint8_t> bytes(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto value = static_cast<std::int16_t>(std::lround(amplitude * 32767.0 * std::sin(2.0 * kPi * hz * i / rate)));
        std::memcpy(&bytes[i * 2], &value, 2);
    }
    return bytes;
}

void wav_from_48k_stereo_float() {
    const auto input = float_stereo_sine(48000, 1.0, 1000.0, 0.5);
    const auto wav = convert_to_wav(input.data(), input.size(), {SampleFormat::float32, 48000, 2});
    check_header(wav, 16000);
    const auto samples = wav_samples(wav);
    assert(std::fabs(duration_seconds(samples.size(), 16000) - 1.0) <= 1.0 / 16000);
    assert(std::fabs(estimate_frequency(samples, 16000) - 1000.0) < 5.0);
    const auto levels = measure_levels(samples.data() + 800, samples.size() - 1600);
    assert(std::fabs(levels.rms_dbfs - (-9.03f)) < 0.2f);
    assert(std::fabs(levels.peak_dbfs - (-6.02f)) < 0.2f);
}

void wav_from_44k_mono_int16() {
    const auto input = int16_mono_sine(44100, 0.75, 440.0, 0.25);
    const auto wav = convert_to_wav(input.data(), input.size(), {SampleFormat::int16, 44100, 1});
    const std::size_t expected = 33075ull * 16000 / 44100;
    check_header(wav, expected);
    const auto samples = wav_samples(wav);
    assert(std::fabs(duration_seconds(samples.size(), 16000) - 0.75) <= 1.0 / 16000);
    assert(std::fabs(estimate_frequency(samples, 16000) - 440.0) < 3.0);
    const auto levels = measure_levels(samples.data() + 800, samples.size() - 1600);
    assert(std::fabs(levels.rms_dbfs - (-15.05f)) < 0.2f);
}

void other_formats_and_rates() {
    // 24-bit packed stereo at 96 kHz.
    const std::size_t frames = 48000;
    std::vector<std::uint8_t> packed(frames * 6);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto value = static_cast<std::int32_t>(std::lround(0.5 * 8388607.0 * std::sin(2.0 * kPi * 500.0 * i / 96000)));
        for (int channel = 0; channel < 2; ++channel) {
            for (int b = 0; b < 3; ++b) packed[i * 6 + channel * 3 + b] = static_cast<std::uint8_t>((value >> (8 * b)) & 0xff);
        }
    }
    auto wav = convert_to_wav(packed.data(), packed.size(), {SampleFormat::int24, 96000, 2});
    check_header(wav, 8000);
    assert(std::fabs(estimate_frequency(wav_samples(wav), 16000) - 500.0) < 5.0);

    // 32-bit integer mono at 16 kHz is copied, not resampled.
    std::vector<std::uint8_t> int32(4 * 4);
    const std::int32_t values[4] = {0, 1 << 30, -(1 << 30), INT32_MIN};
    std::memcpy(int32.data(), values, sizeof(values));
    wav = convert_to_wav(int32.data(), int32.size(), {SampleFormat::int32, 16000, 1});
    const auto copied = wav_samples(wav);
    assert(copied.size() == 4 && copied[0] == 0 && copied[1] == 16384 && copied[2] == -16384 && copied[3] == -32768);

    // 8 kHz upsampling doubles the length and keeps the tone.
    const auto narrow = int16_mono_sine(8000, 0.5, 300.0, 0.5);
    wav = convert_to_wav(narrow.data(), narrow.size(), {SampleFormat::int16, 8000, 1});
    check_header(wav, 8000);
    assert(std::fabs(estimate_frequency(wav_samples(wav), 16000) - 300.0) < 3.0);

    // A 12 kHz tone at 48 kHz is above the 8 kHz output Nyquist and must be filtered, not aliased.
    const auto high = float_stereo_sine(48000, 0.5, 12000.0, 0.5);
    wav = convert_to_wav(high.data(), high.size(), {SampleFormat::float32, 48000, 2});
    const auto filtered = wav_samples(wav);
    assert(measure_levels(filtered.data() + 800, filtered.size() - 1600).rms_dbfs < -60.0f);

    // DC passes at unity gain, including at the edges.
    const std::vector<float> dc(4410, 0.25f);
    const auto dc_out = resample(dc.data(), dc.size(), 44100, 16000);
    assert(dc_out.size() == 1600);
    for (float value : dc_out) assert(std::fabs(value - 0.25f) < 1e-3f);
}

void edge_cases() {
    const auto empty = convert_to_wav(nullptr, 0, {SampleFormat::float32, 48000, 2});
    check_header(empty, 0);
    assert(build_wav(nullptr, 5).size() == kWavHeaderBytes);
    assert(resample(nullptr, 10, 48000, 16000).empty());
    assert(resampled_length(2, 48000, 16000) == 0);
    assert(resampled_length(3, 48000, 16000) == 1);
    assert(resampled_length(0, 0, 16000) == 0);

    // A trailing partial frame (odd byte count) is ignored.
    std::vector<float> mono;
    std::vector<std::uint8_t> bytes(4 * 2 * 3 + 5, 0);
    assert(append_mono(bytes.data(), bytes.size(), {SampleFormat::float32, 48000, 2}, mono) == 3);
    assert(mono.size() == 3);
    std::vector<std::uint8_t> odd = {0x00, 0x40, 0x00};
    mono.clear();
    assert(append_mono(odd.data(), odd.size(), {SampleFormat::int16, 16000, 1}, mono) == 1);
    assert(mono[0] == 0.5f);
    assert(append_mono(odd.data(), odd.size(), {SampleFormat::int16, 0, 1}, mono) == 0);
    assert(append_mono(odd.data(), odd.size(), {SampleFormat::int16, 16000, 0}, mono) == 0);

    // Stereo downmix averages channels; opposite channels cancel.
    const float frame[2] = {0.5f, -0.5f};
    mono.clear();
    append_mono(reinterpret_cast<const std::uint8_t *>(frame), sizeof(frame), {SampleFormat::float32, 16000, 2}, mono);
    assert(mono.size() == 1 && mono[0] == 0.0f);

    // Clipping, NaN and rounding.
    const float extreme[5] = {2.0f, -2.0f, NAN, 1.0f / 65536.0f, -1.0f};
    const auto pcm = to_int16(extreme, 5);
    assert(pcm[0] == 32767 && pcm[1] == -32768 && pcm[2] == 0 && pcm[3] == 0 && pcm[4] == -32768);

    // Levels and the meter mapping.
    const std::vector<float> silence(100, 0.0f);
    assert(measure_levels(silence.data(), silence.size()).rms_dbfs == kSilenceDbfs);
    assert(measure_levels(static_cast<const float *>(nullptr), 0).peak_dbfs == kSilenceDbfs);
    const std::vector<float> full(10, 1.0f);
    assert(std::fabs(measure_levels(full.data(), full.size()).rms_dbfs) < 1e-4f);
    assert(meter_level(kSilenceDbfs) == 0.0f);
    assert(meter_level(-60.0f) == 0.0f);
    assert(std::fabs(meter_level(-30.0f) - 0.5f) < 1e-6f);
    assert(meter_level(0.0f) == 1.0f);
    assert(meter_level(6.0f) == 1.0f);
    assert(duration_seconds(16000, 16000) == 1.0);
    assert(duration_seconds(1, 0) == 0.0);
}

void deterministic_output() {
    const auto input = float_stereo_sine(44100, 0.2, 777.0, 0.7);
    const PcmFormat format{SampleFormat::float32, 44100, 2};
    assert(convert_to_wav(input.data(), input.size(), format) == convert_to_wav(input.data(), input.size(), format));
}

void wave_format_parsing() {
    WAVEFORMATEXTENSIBLE ext{};
    ext.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    ext.Format.nChannels = 2;
    ext.Format.nSamplesPerSec = 48000;
    ext.Format.wBitsPerSample = 32;
    ext.Format.nBlockAlign = 8;
    ext.Format.nAvgBytesPerSec = 48000 * 8;
    ext.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    ext.Samples.wValidBitsPerSample = 32;
    ext.SubFormat = {WAVE_FORMAT_IEEE_FLOAT, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    PcmFormat format;
    assert(pcm_format_from_wave(&ext.Format, format));
    assert(format.sample == SampleFormat::float32 && format.sample_rate == 48000 && format.channels == 2);

    ext.SubFormat.Data1 = WAVE_FORMAT_PCM;
    ext.Samples.wValidBitsPerSample = 24;
    assert(pcm_format_from_wave(&ext.Format, format) && format.sample == SampleFormat::int32);

    ext.SubFormat.Data1 = 0x0092; // Dolby AC3 SPDIF
    assert(!pcm_format_from_wave(&ext.Format, format));

    WAVEFORMATEX plain{};
    plain.wFormatTag = WAVE_FORMAT_PCM;
    plain.nChannels = 1;
    plain.nSamplesPerSec = 44100;
    plain.wBitsPerSample = 16;
    plain.nBlockAlign = 2;
    assert(pcm_format_from_wave(&plain, format) && format.sample == SampleFormat::int16 && format.channels == 1);
    plain.wBitsPerSample = 24;
    plain.nBlockAlign = 3;
    assert(pcm_format_from_wave(&plain, format) && format.sample == SampleFormat::int24);
    plain.nBlockAlign = 4; // inconsistent block alignment
    assert(!pcm_format_from_wave(&plain, format));
    plain.wBitsPerSample = 8;
    plain.nBlockAlign = 1;
    assert(!pcm_format_from_wave(&plain, format));
    assert(!pcm_format_from_wave(nullptr, format));
}

void hresult_mapping() {
    assert(capture_reason_from_hresult(S_OK, true) == CaptureFailureReason::none);
    assert(capture_reason_from_hresult(E_ACCESSDENIED, true) == CaptureFailureReason::permission_denied);
    assert(capture_reason_from_hresult(E_ACCESSDENIED, false) == CaptureFailureReason::permission_denied);
    assert(capture_reason_from_hresult(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), true) == CaptureFailureReason::no_device);
    assert(capture_reason_from_hresult(AUDCLNT_E_DEVICE_INVALIDATED, true) == CaptureFailureReason::no_device);
    assert(capture_reason_from_hresult(AUDCLNT_E_DEVICE_INVALIDATED, false) == CaptureFailureReason::device_lost);
    assert(capture_reason_from_hresult(AUDCLNT_E_DEVICE_IN_USE, true) == CaptureFailureReason::device_busy);
    assert(capture_reason_from_hresult(AUDCLNT_E_UNSUPPORTED_FORMAT, true) == CaptureFailureReason::format);
    assert(capture_reason_from_hresult(E_FAIL, false) == CaptureFailureReason::internal);
    assert(std::strcmp(capture_failure_reason_name(CaptureFailureReason::permission_denied), "permission_denied") == 0);
}

// ---- WasapiCapture contract against a fake backend ----

class TestExecutor final : public Executor {
public:
    bool post(std::function<void()> task) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        ready_.notify_one();
        return true;
    }
    void run_until(const std::function<bool()> &done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done()) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                const bool ready = ready_.wait_until(lock, deadline, [&] { return !tasks_.empty(); });
                assert(ready);
                if (!ready) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }
    std::size_t drain() {
        std::size_t ran = 0;
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (tasks_.empty()) return ran;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
            ++ran;
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> tasks_;
};

struct FakeDevice {
    CaptureFailureReason open_result = CaptureFailureReason::none;
    CaptureFailureReason start_result = CaptureFailureReason::none;
    PcmFormat format{SampleFormat::float32, 48000, 2};
    std::vector<std::uint8_t> pcm;
    std::wstring last_id;
    std::atomic<bool> lose{false};
    std::atomic<int> opened{0};
    std::atomic<int> closed{0};
};

class FakeStream final : public CaptureStream {
public:
    explicit FakeStream(std::shared_ptr<FakeDevice> device)
        : device_(std::move(device)), event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        ++device_->opened;
    }
    ~FakeStream() override {
        CloseHandle(event_);
        ++device_->closed;
    }
    const PcmFormat &format() const override { return device_->format; }
    HANDLE ready_event() const override { return event_; }
    CaptureFailureReason start() override {
        SetEvent(event_);
        return device_->start_result;
    }
    CaptureFailureReason read(const PacketSink &sink) override {
        if (device_->lose) return CaptureFailureReason::device_lost;
        const std::size_t frame = bytes_per_frame(device_->format);
        const std::size_t chunk = frame * (device_->format.sample_rate / 100);
        while (offset_ < device_->pcm.size()) {
            const std::size_t bytes = std::min(chunk, device_->pcm.size() - offset_);
            sink(device_->pcm.data() + offset_, static_cast<std::uint32_t>(bytes / frame));
            offset_ += bytes;
        }
        ResetEvent(event_);
        return CaptureFailureReason::none;
    }

private:
    std::shared_ptr<FakeDevice> device_;
    HANDLE event_;
    std::size_t offset_ = 0;
};

class FakeBackend final : public CaptureBackend {
public:
    explicit FakeBackend(std::shared_ptr<FakeDevice> device) : device_(std::move(device)) {}
    CaptureFailureReason open(const std::wstring &device_id, std::unique_ptr<CaptureStream> &stream) override {
        device_->last_id = device_id;
        if (device_->open_result != CaptureFailureReason::none) return device_->open_result;
        stream = std::make_unique<FakeStream>(device_);
        return CaptureFailureReason::none;
    }

private:
    std::shared_ptr<FakeDevice> device_;
};

struct Harness {
    TestExecutor executor;
    std::shared_ptr<FakeDevice> device = std::make_shared<FakeDevice>();
    std::unique_ptr<WasapiCapture> capture;

    Harness() { capture = std::make_unique<WasapiCapture>(executor, L"", std::make_shared<FakeBackend>(device)); }

    AdapterError start(OperationId operation) {
        bool done = false;
        AdapterError error = AdapterError::canceled;
        capture->start(operation, [&](OperationId completed, Completion result) {
            assert(completed == operation);
            error = result.error;
            done = true;
        });
        executor.run_until([&] { return done; });
        return error;
    }

    BytesResult stop(OperationId operation) {
        bool done = false;
        BytesResult out;
        capture->stop(operation, [&](OperationId completed, BytesResult result) {
            assert(completed == operation);
            out = std::move(result);
            done = true;
        });
        executor.run_until([&] { return done; });
        return out;
    }
};

void contract_records_and_converts() {
    Harness h;
    h.device->pcm = float_stereo_sine(48000, 0.5, 1000.0, 0.5);
    std::vector<float> levels;
    h.capture->set_level_callback([&](float level) { levels.push_back(level); });
    h.capture->set_device_id(L"{fake-endpoint}");
    assert(h.capture->device_id() == L"{fake-endpoint}");
    assert(h.start(1) == AdapterError::none);
    assert(h.device->last_id == L"{fake-endpoint}");
    assert(h.capture->last_failure_reason() == CaptureFailureReason::none);
    const auto result = h.stop(1);
    assert(result.error == AdapterError::none);
    check_header(result.bytes, 8000);
    assert(std::fabs(estimate_frequency(wav_samples(result.bytes), 16000) - 1000.0) < 5.0);
    h.executor.drain();
    // 0.5 s at 20 Hz; -9 dBFS RMS maps to ~0.85 on the meter.
    assert(levels.size() == 10);
    for (float level : levels) assert(level > 0.8f && level < 0.9f);
    assert(h.device->opened == 1 && h.device->closed == 1);
    // The same operation cannot be stopped twice.
    assert(h.stop(1).error == AdapterError::invalid_input);
    h.capture->shutdown();
}

void contract_stop_without_start_and_short_recording() {
    Harness h;
    assert(h.stop(7).error == AdapterError::invalid_input);
    h.device->pcm = float_stereo_sine(48000, 0.05, 1000.0, 0.5);
    assert(h.start(8) == AdapterError::none);
    assert(h.stop(9).error == AdapterError::invalid_input);
    const auto result = h.stop(8);
    // Too short: empty bytes, which the core reports as a recording failure.
    assert(result.error == AdapterError::none && result.bytes.empty());
}

void contract_start_failures() {
    const CaptureFailureReason reasons[] = {CaptureFailureReason::permission_denied, CaptureFailureReason::no_device,
        CaptureFailureReason::device_busy, CaptureFailureReason::format};
    for (const auto reason : reasons) {
        Harness h;
        h.device->open_result = reason;
        assert(h.start(1) == AdapterError::unavailable);
        assert(h.capture->last_failure_reason() == reason);
        assert(h.stop(1).error == AdapterError::invalid_input);
    }
    Harness h;
    h.device->start_result = CaptureFailureReason::permission_denied;
    assert(h.start(1) == AdapterError::unavailable);
    assert(h.capture->last_failure_reason() == CaptureFailureReason::permission_denied);
    assert(h.device->opened == 1 && h.device->closed == 1);
    // A later successful start clears the reason.
    h.device->start_result = CaptureFailureReason::none;
    assert(h.start(2) == AdapterError::none);
    assert(h.capture->last_failure_reason() == CaptureFailureReason::none);

    Harness invalid;
    invalid.device->format = {SampleFormat::float32, 0, 2};
    assert(invalid.start(1) == AdapterError::unavailable);
    assert(invalid.capture->last_failure_reason() == CaptureFailureReason::format);
}

void contract_device_lost_mid_recording() {
    Harness h;
    std::vector<CaptureFailureReason> failures;
    h.capture->set_failure_callback([&](OperationId operation, CaptureFailureReason reason) {
        assert(operation == 3);
        failures.push_back(reason);
    });
    assert(h.start(3) == AdapterError::none);
    h.device->lose = true;
    h.executor.run_until([&] { return !failures.empty(); });
    assert(failures.size() == 1 && failures[0] == CaptureFailureReason::device_lost);
    assert(h.capture->last_failure_reason() == CaptureFailureReason::device_lost);
    assert(h.device->closed == 1);
    const auto result = h.stop(3);
    assert(result.error == AdapterError::unavailable && result.bytes.empty());
    // The next start works once the device is back.
    h.device->lose = false;
    assert(h.start(4) == AdapterError::none);
}

void contract_cancel_suppresses_completions() {
    Harness h;
    int started = 0;
    h.capture->start(1, [&](OperationId, Completion) { ++started; });
    h.capture->cancel(1);
    // stop() of an unknown operation acts as a barrier for the worker queue.
    assert(h.stop(2).error == AdapterError::invalid_input);
    h.executor.drain();
    assert(started == 0);
    assert(h.device->opened == h.device->closed);

    // Cancel while recording discards audio and drops the stale stop completion.
    h.device->pcm = float_stereo_sine(48000, 0.5, 440.0, 0.5);
    assert(h.start(3) == AdapterError::none);
    int stopped = 0;
    h.capture->stop(3, [&](OperationId, BytesResult) { ++stopped; });
    h.capture->cancel(3);
    assert(h.stop(4).error == AdapterError::invalid_input);
    h.executor.drain();
    assert(stopped == 0);
    assert(h.device->opened == h.device->closed);

    // A new operation records normally after a cancel.
    assert(h.start(5) == AdapterError::none);
    const auto result = h.stop(5);
    assert(result.error == AdapterError::none);
    check_header(result.bytes, 8000);
}

void contract_shutdown_is_a_barrier() {
    Harness h;
    h.device->pcm = float_stereo_sine(48000, 0.5, 440.0, 0.5);
    int callbacks = 0;
    h.capture->set_level_callback([&](float) { ++callbacks; });
    h.capture->set_failure_callback([&](OperationId, CaptureFailureReason) { ++callbacks; });
    assert(h.start(1) == AdapterError::none);
    h.capture->stop(1, [&](OperationId, BytesResult) { ++callbacks; });
    h.capture->shutdown();
    assert(h.device->opened == h.device->closed);
    h.capture->start(2, [&](OperationId, Completion) { ++callbacks; });
    h.capture->stop(2, [&](OperationId, BytesResult) { ++callbacks; });
    h.capture->cancel(2);
    h.capture->shutdown();
    h.executor.drain();
    assert(callbacks == 0);
    h.capture.reset();
    h.executor.drain();
    assert(callbacks == 0);
}

void contract_destructor_releases_device() {
    Harness h;
    h.device->pcm = float_stereo_sine(48000, 0.2, 440.0, 0.5);
    assert(h.start(1) == AdapterError::none);
    h.capture.reset();
    assert(h.device->opened == 1 && h.device->closed == 1);
    h.executor.drain();
}

} // namespace

int main() {
    wav_from_48k_stereo_float();
    wav_from_44k_mono_int16();
    other_formats_and_rates();
    edge_cases();
    deterministic_output();
    wave_format_parsing();
    hresult_mapping();
    contract_records_and_converts();
    contract_stop_without_start_and_short_recording();
    contract_start_failures();
    contract_device_lost_mid_recording();
    contract_cancel_suppresses_completions();
    contract_shutdown_is_a_barrier();
    contract_destructor_releases_device();
    std::puts("capture_test passed");
    return 0;
}
