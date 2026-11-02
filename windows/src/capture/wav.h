#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Pure, deterministic audio helpers. Nothing here touches devices, files or logs.
namespace lemonvox::win::audio {

// LemonVox uploads 16 kHz, mono, 16-bit little-endian PCM WAV, matching macOS.
constexpr std::uint32_t kTargetSampleRate = 16000;

enum class SampleFormat : std::uint8_t {
    float32,
    int16,
    int24, // packed, three bytes per sample
    int32, // also used for 24-bit samples in a 32-bit container
};

// Interleaved, little-endian PCM.
struct PcmFormat {
    SampleFormat sample = SampleFormat::float32;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
};

std::size_t bytes_per_sample(SampleFormat sample);
std::size_t bytes_per_frame(const PcmFormat &format);
bool is_valid(const PcmFormat &format);

// Downmixes interleaved frames to mono floats in [-1, 1] by averaging channels
// and appends them to out. A trailing partial frame is ignored. Returns the
// number of frames appended.
std::size_t append_mono(const std::uint8_t *data, std::size_t bytes, const PcmFormat &format,
    std::vector<float> &out);
// Appends frames of digital silence.
void append_silence(std::size_t frames, std::vector<float> &out);

// Number of output samples resample() produces: floor(count * out_rate / in_rate).
std::size_t resampled_length(std::size_t count, std::uint32_t in_rate, std::uint32_t out_rate);
// Band-limited (Kaiser-windowed sinc) resampler. Downsampling low-passes below
// the output Nyquist frequency. Equal rates copy the input.
std::vector<float> resample(const float *input, std::size_t count, std::uint32_t in_rate,
    std::uint32_t out_rate);

// Rounds and clamps to the int16 range.
std::vector<std::int16_t> to_int16(const float *input, std::size_t count);

constexpr std::size_t kWavHeaderBytes = 44;

// Builds a canonical 44-byte-header RIFF/WAVE PCM file in memory.
std::vector<std::uint8_t> build_wav(const std::int16_t *samples, std::size_t count,
    std::uint32_t sample_rate = kTargetSampleRate, std::uint16_t channels = 1);
// Mono floats at in_rate -> 16 kHz mono int16 WAV.
std::vector<std::uint8_t> mono_to_wav(const float *mono, std::size_t count, std::uint32_t in_rate);
// Interleaved PCM in any supported format -> 16 kHz mono int16 WAV.
std::vector<std::uint8_t> convert_to_wav(const std::uint8_t *data, std::size_t bytes,
    const PcmFormat &format);

double duration_seconds(std::size_t frames, std::uint32_t sample_rate);

// Levels in dBFS relative to full scale; silence reports kSilenceDbfs.
constexpr float kSilenceDbfs = -120.0f;
struct Levels {
    float rms_dbfs = kSilenceDbfs;
    float peak_dbfs = kSilenceDbfs;
};
Levels measure_levels(const float *samples, std::size_t count);
Levels measure_levels(const std::int16_t *samples, std::size_t count);

// Indicator meter value in [0, 1]: -60 dBFS and below is 0, 0 dBFS is 1.
// Matches the macOS recorder meter mapping.
float meter_level(float rms_dbfs);

} // namespace lemonvox::win::audio
