// End-to-end Windows pipeline against a running Lemonade server. Speech comes
// from Lemonade's own TTS model, so no microphone audio is ever recorded. The
// synthesized WAV passes through the same 16 kHz conversion as live capture,
// then WinHTTP transcription and cleanup, and a fake insertion target.
// Exits 77 (skipped) when Lemonade or the required models are unavailable.
#include "app/dictation_controller.h"
#include "capture/wav.h"
#include "common/executor.h"
#include "net/winhttp_transport.h"
#include "net/endpoint_facts.h"

#include <windows.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace lemonvox;
using namespace lemonvox::win;

namespace {

constexpr char kEndpoint[] = "http://127.0.0.1:13305/v1";
constexpr char kAsr[] = "Whisper-Base";
constexpr char kSpeech[] = "Um, send Alex the fourteen invoices on Friday.";

void pump_until(const bool &done, ULONGLONG timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    MSG message;
    while (!done && GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        if (!done) MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

TransportResponse send_now(UiThreadExecutor &executor, WinHttpTransport &transport, OperationId id,
    TransportRequest request) {
    TransportResponse result;
    bool done = false;
    transport.send(id, std::move(request), [&](OperationId, TransportResponse response) {
        result = std::move(response);
        done = true;
    });
    pump_until(done, 180000);
    if (!done) result.error = AdapterError::network;
    (void)executor;
    return result;
}

std::uint32_t read_u32(const std::vector<std::uint8_t> &b, size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::uint16_t read_u16(const std::vector<std::uint8_t> &b, size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

// Parses a RIFF/WAVE file with PCM or IEEE-float samples and converts it like live capture.
bool convert_like_capture(const std::vector<std::uint8_t> &wav, std::vector<std::uint8_t> &out) {
    if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0 || std::memcmp(wav.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    audio::PcmFormat format;
    bool have_format = false;
    size_t at = 12;
    while (at + 8 <= wav.size()) {
        const std::uint32_t size = read_u32(wav, at + 4);
        const size_t body = at + 8;
        if (std::memcmp(wav.data() + at, "fmt ", 4) == 0 && size >= 16 && body + 16 <= wav.size()) {
            const std::uint16_t tag = read_u16(wav, body);
            const std::uint16_t bits = read_u16(wav, body + 14);
            format.channels = read_u16(wav, body + 2);
            format.sample_rate = read_u32(wav, body + 4);
            if (tag == 3 && bits == 32) format.sample = audio::SampleFormat::float32;
            else if ((tag == 1 || tag == 0xFFFE) && bits == 16) format.sample = audio::SampleFormat::int16;
            else if ((tag == 1 || tag == 0xFFFE) && bits == 32) format.sample = audio::SampleFormat::int32;
            else if (tag == 0xFFFE && bits == 32) format.sample = audio::SampleFormat::float32;
            else return false;
            have_format = true;
        } else if (std::memcmp(wav.data() + at, "data", 4) == 0 && have_format) {
            const size_t available = std::min<size_t>(size, wav.size() - body);
            out = audio::convert_to_wav(wav.data() + body, available, format);
            return !out.empty();
        }
        at = body + size + (size & 1);
    }
    return false;
}

struct FileCapture final : CaptureAdapter {
    Executor &executor;
    std::vector<std::uint8_t> wav;
    explicit FileCapture(Executor &e) : executor(e) {}
    void start(OperationId op, VoidCompletion done) override { executor.post([op, done] { done(op, {}); }); }
    void stop(OperationId op, BytesCompletion done) override {
        executor.post([op, done, bytes = wav] { done(op, {AdapterError::none, bytes}); });
    }
    void cancel(OperationId) override {}
    void shutdown() override {}
};

struct RecordingInsertion final : InsertionAdapter {
    Executor &executor;
    std::string inserted;
    int inserts = 0;
    explicit RecordingInsertion(Executor &e) : executor(e) {}
    void capture_original(OperationId op, TargetCompletion done) override {
        TargetResult r;
        r.target = {{1}, true, false, true, true};
        executor.post([op, done, r] { done(op, r); });
    }
    void inspect(OperationId op, TargetToken, TargetCompletion done) override {
        TargetResult r;
        r.target = {{1}, true, false, true, true};
        executor.post([op, done, r] { done(op, r); });
    }
    void insert(OperationId op, TargetToken, std::string text, VoidCompletion done) override {
        inserted = std::move(text);
        ++inserts;
        executor.post([op, done] { done(op, {}); });
    }
    void cancel(OperationId) override {}
    void shutdown() override {}
};

std::string pick_cleanup_model(const std::string &inventory) {
    // Prefer small non-reasoning chat models present on the owner machine.
    const char *candidates[] = {"Gemma-4-E4B-it-GGUF", "Qwen3-0.6B-GGUF", "Qwen3-8B-GGUF"};
    for (const char *candidate : candidates) {
        if (inventory.find(std::string("\"") + candidate + "\"") != std::string::npos) return candidate;
    }
    return {};
}

} // namespace

int main() {
    UiThreadExecutor executor;
    WinHttpTransport helper(executor);

    ProtocolEndpointFacts facts;
    assert(endpoint_is_valid(kEndpoint, &facts));
    ProtocolConfiguration config;
    config.endpoint = facts;
    TransportRequest models_request;
    assert(Protocol::make_models_request(config, models_request) == LV_PROTOCOL_OK);
    models_request.timeout_ms = 5000;
    const auto models = send_now(executor, helper, 1, std::move(models_request));
    if (models.error != AdapterError::none || models.status != 200) {
        std::puts("SKIP: Lemonade is not reachable at 127.0.0.1:13305");
        return 77;
    }
    const std::string inventory(models.bytes.begin(), models.bytes.end());
    const std::string cleanup_model = pick_cleanup_model(inventory);
    if (inventory.find("\"Whisper-Base\"") == std::string::npos || inventory.find("kokoro-v1") == std::string::npos
        || cleanup_model.empty()) {
        std::puts("SKIP: Whisper-Base, kokoro-v1 or a cleanup model is not installed");
        return 77;
    }

    TransportRequest speech;
    speech.method = HttpMethod::post;
    speech.url = std::string(kEndpoint) + "/audio/speech";
    speech.headers.push_back({"Content-Type", "application/json"});
    const std::string body = std::string(R"({"model":"kokoro-v1","voice":"af_heart","response_format":"wav","input":")")
        + kSpeech + "\"}";
    speech.body.assign(body.begin(), body.end());
    speech.timeout_ms = 180000;
    const auto tts = send_now(executor, helper, 2, std::move(speech));
    if (tts.error != AdapterError::none || tts.status != 200) {
        std::printf("SKIP: TTS unavailable (error %d, HTTP %d)\n", static_cast<int>(tts.error), tts.status);
        return 77;
    }

    FileCapture capture(executor);
    assert(convert_like_capture(tts.bytes, capture.wav));
    assert(std::memcmp(capture.wav.data(), "RIFF", 4) == 0);
    assert(read_u32(capture.wav, 24) == 16000 && read_u16(capture.wav, 22) == 1 && read_u16(capture.wav, 34) == 16);

    WinHttpTransport transport(executor);
    RecordingInsertion insertion(executor);
    WinPreferences preferences;
    preferences.asr_model = kAsr;
    preferences.cleanup_model = cleanup_model;
    SnapshotPreferences adapter(preferences);

    bool inserted = false;
    bool failed = false;
    std::wstring failure;
    ControllerEvents events;
    events.inserted = [&] { inserted = true; };
    events.failed = [&](const UserMessage &message) { failure = message.text; failed = true; };
    events.copy_fallback = [&](const std::wstring &reason) { failure = reason; failed = true; };
    DictationController controller(capture, transport, insertion, adapter, events);

    controller.toggle();
    const ULONGLONG deadline = GetTickCount64() + 30000;
    MSG message;
    while (controller.phase() != WorkflowPhase::recording && !failed && GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(5);
    }
    assert(!failed && controller.phase() == WorkflowPhase::recording);
    const ULONGLONG started = GetTickCount64();
    controller.toggle();
    bool finished = false;
    while (!inserted && !failed && GetTickCount64() < started + 240000) {
        pump_until(finished, 50);
    }
    if (failed) std::wprintf(L"FAIL: %ls\n", failure.c_str());
    assert(inserted && !failed && insertion.inserts == 1);
    std::printf("cleanup model %s, %llu ms, %zu characters inserted\n", cleanup_model.c_str(),
        GetTickCount64() - started, insertion.inserted.size());
    assert(insertion.inserted.find("Alex") != std::string::npos);
    assert(insertion.inserted.find("14") != std::string::npos || insertion.inserted.find("ourteen") != std::string::npos);
    assert(insertion.inserted.find("Friday") != std::string::npos);
    controller.shutdown();
    helper.shutdown();
    return 0;
}
