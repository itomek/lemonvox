#include "app/file_capture.h"

#include "capture/wav.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <utility>

namespace lemonvox::win {

namespace {

std::uint32_t read_u32(const std::vector<std::uint8_t> &b, size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::uint16_t read_u16(const std::vector<std::uint8_t> &b, size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

constexpr std::uint16_t kPcm = 1;
constexpr std::uint16_t kFloat = 3;
constexpr std::uint16_t kExtensible = 0xFFFE;

} // namespace

bool convert_wav_for_upload(const std::vector<std::uint8_t> &wav, std::vector<std::uint8_t> &out) {
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
            std::uint16_t tag = read_u16(wav, body);
            const std::uint16_t bits = read_u16(wav, body + 14);
            // WAVE_FORMAT_EXTENSIBLE stores the real format in its subformat GUID.
            if (tag == kExtensible && size >= 40 && body + 26 <= wav.size()) tag = read_u16(wav, body + 24);
            format.channels = read_u16(wav, body + 2);
            format.sample_rate = read_u32(wav, body + 4);
            if (tag == kFloat && bits == 32) format.sample = audio::SampleFormat::float32;
            else if (tag == kPcm && bits == 16) format.sample = audio::SampleFormat::int16;
            else if (tag == kPcm && bits == 24) format.sample = audio::SampleFormat::int24;
            else if (tag == kPcm && bits == 32) format.sample = audio::SampleFormat::int32;
            else return false;
            have_format = audio::is_valid(format);
            if (!have_format) return false;
        } else if (std::memcmp(wav.data() + at, "data", 4) == 0 && have_format) {
            const size_t available = std::min<size_t>(size, wav.size() - body);
            out = audio::convert_to_wav(wav.data() + body, available, format);
            return out.size() > audio::kWavHeaderBytes;
        }
        if (size > wav.size() - body) return false;
        at = body + size + (size & 1);
    }
    return false;
}

FileCapture::FileCapture(Executor &executor, std::vector<std::uint8_t> upload_wav)
    : executor_(executor), wav_(std::move(upload_wav)) {}

bool FileCapture::load(const std::wstring &path, std::vector<std::uint8_t> &upload_wav) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return convert_wav_for_upload(bytes, upload_wav);
}

void FileCapture::start(OperationId operation, VoidCompletion completion) {
    if (shut_down_) return;
    recording_ = operation;
    executor_.post([operation, completion, alive = alive_] { if (*alive) completion(operation, {}); });
}

void FileCapture::stop(OperationId operation, BytesCompletion completion) {
    if (shut_down_) return;
    const bool current = operation == recording_;
    recording_ = 0;
    executor_.post([operation, completion, current, bytes = wav_, alive = alive_] {
        if (!*alive) return;
        completion(operation, {current ? AdapterError::none : AdapterError::invalid_input,
            current ? bytes : std::vector<std::uint8_t>{}});
    });
}

void FileCapture::cancel(OperationId operation) {
    if (operation == recording_) recording_ = 0;
}

void FileCapture::shutdown() {
    shut_down_ = true;
    *alive_ = false;
    recording_ = 0;
}

} // namespace lemonvox::win
