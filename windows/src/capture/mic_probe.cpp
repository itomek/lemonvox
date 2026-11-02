// Manual microphone check for the device owner. It records only when run with
// an explicit --seconds N, keeps audio in memory, prints duration and level
// statistics, and discards the audio. It never writes audio anywhere.

#include "capture/device_list.h"
#include "capture/wasapi_capture.h"
#include "capture/wav.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

using namespace lemonvox;
using namespace lemonvox::win;

namespace {

class QueueExecutor final : public Executor {
public:
    bool post(std::function<void()> task) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        ready_.notify_one();
        return true;
    }
    // Runs tasks on the calling thread until done() or the timeout.
    bool run_until(const std::function<bool()> &done, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (!ready_.wait_until(lock, deadline, [&] { return !tasks_.empty(); })) return done();
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> tasks_;
};

void usage() {
    std::fwprintf(stderr,
        L"usage: lemonvox_mic_probe --list\n"
        L"       lemonvox_mic_probe --seconds N [--device ENDPOINT_ID]\n"
        L"Records N seconds (1-120) from the microphone into memory, prints duration and\n"
        L"levels, then discards the audio. Nothing is saved.\n");
}

int list_devices() {
    const auto list = list_capture_devices();
    if (!list.ok) {
        std::fwprintf(stderr, L"device enumeration failed\n");
        return 1;
    }
    if (list.devices.empty()) std::wprintf(L"no active capture devices\n");
    for (const auto &device : list.devices) {
        std::wprintf(L"%ls %ls\n    %ls\n", device.is_default ? L"*" : L" ", device.name.c_str(), device.id.c_str());
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    int seconds = 0;
    std::wstring device;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--list") == 0) return list_devices();
        if (std::wcscmp(argv[i], L"--seconds") == 0 && i + 1 < argc) seconds = _wtoi(argv[++i]);
        else if (std::wcscmp(argv[i], L"--device") == 0 && i + 1 < argc) device = argv[++i];
        else {
            usage();
            return 2;
        }
    }
    if (seconds < 1 || seconds > 120) {
        usage();
        return 2;
    }

    QueueExecutor executor;
    WasapiCapture capture(executor, device);
    int meter_updates = 0;
    float meter_max = 0.0f;
    capture.set_level_callback([&](float level) {
        ++meter_updates;
        if (level > meter_max) meter_max = level;
    });
    bool lost = false;
    capture.set_failure_callback([&](OperationId, CaptureFailureReason) { lost = true; });

    bool started = false;
    AdapterError start_error = AdapterError::none;
    capture.start(1, [&](OperationId, Completion completion) {
        start_error = completion.error;
        started = true;
    });
    if (!executor.run_until([&] { return started; }, std::chrono::seconds(10)) || start_error != AdapterError::none) {
        std::fwprintf(stderr, L"start failed: %hs\n", capture_failure_reason_name(capture.last_failure_reason()));
        capture.shutdown();
        return 1;
    }
    std::wprintf(L"recording %d s...\n", seconds);
    executor.run_until([&] { return lost; }, std::chrono::seconds(seconds));

    bool stopped = false;
    BytesResult result;
    capture.stop(1, [&](OperationId, BytesResult bytes) {
        result = std::move(bytes);
        stopped = true;
    });
    executor.run_until([&] { return stopped; }, std::chrono::seconds(10));
    capture.shutdown();
    if (!stopped || result.error != AdapterError::none) {
        std::fwprintf(stderr, L"stop failed: %hs\n", capture_failure_reason_name(capture.last_failure_reason()));
        return 1;
    }
    if (result.bytes.size() <= audio::kWavHeaderBytes) {
        std::wprintf(L"recording too short\n");
        return 1;
    }

    const std::size_t samples = (result.bytes.size() - audio::kWavHeaderBytes) / 2;
    std::vector<std::int16_t> pcm(samples);
    std::memcpy(pcm.data(), result.bytes.data() + audio::kWavHeaderBytes, samples * 2);
    const auto levels = audio::measure_levels(pcm.data(), pcm.size());
    std::wprintf(L"duration  %.3f s (%zu samples @ 16 kHz mono)\n", audio::duration_seconds(samples, 16000), samples);
    std::wprintf(L"rms       %.1f dBFS\npeak      %.1f dBFS\n", levels.rms_dbfs, levels.peak_dbfs);
    std::wprintf(L"meter     %d updates, max %.2f\n", meter_updates, meter_max);
    SecureZeroMemory(pcm.data(), pcm.size() * sizeof(std::int16_t));
    SecureZeroMemory(result.bytes.data(), result.bytes.size());
    return 0;
}
