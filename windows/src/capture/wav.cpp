#include "capture/wav.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace lemonvox::win::audio {
namespace {

constexpr int kZeroCrossings = 16;
constexpr int kTableOversample = 512;
constexpr int kTableSize = kZeroCrossings * kTableOversample + 2;
constexpr double kKaiserBeta = 8.6;
// Keep the passband slightly under the output Nyquist so the Kaiser transition
// band falls mostly below it.
constexpr double kCutoffMargin = 0.94;
constexpr double kPi = 3.14159265358979323846;

double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double half = x / 2.0;
    for (int k = 1; k < 64; ++k) {
        term *= (half / k) * (half / k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// kernel(x) = sinc(x) * kaiser(x / kZeroCrossings), sampled for x in [0, Z].
const std::array<float, kTableSize> &kernel_table() {
    static const std::array<float, kTableSize> table = [] {
        std::array<float, kTableSize> values{};
        const double norm = bessel_i0(kKaiserBeta);
        for (int i = 0; i < kTableSize; ++i) {
            const double x = static_cast<double>(i) / kTableOversample;
            if (x >= kZeroCrossings) {
                values[static_cast<std::size_t>(i)] = 0.0f;
                continue;
            }
            const double sinc = i == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double r = x / kZeroCrossings;
            const double window = bessel_i0(kKaiserBeta * std::sqrt(1.0 - r * r)) / norm;
            values[static_cast<std::size_t>(i)] = static_cast<float>(sinc * window);
        }
        return values;
    }();
    return table;
}

double kernel(const std::array<float, kTableSize> &table, double x) {
    if (x >= kZeroCrossings) return 0.0;
    const double position = x * kTableOversample;
    const auto index = static_cast<std::size_t>(position);
    const double fraction = position - static_cast<double>(index);
    return table[index] + (table[index + 1] - table[index]) * fraction;
}

float read_sample(const std::uint8_t *p, SampleFormat sample) {
    switch (sample) {
    case SampleFormat::float32: {
        float value;
        std::memcpy(&value, p, sizeof(value));
        if (!std::isfinite(value)) return 0.0f;
        return value;
    }
    case SampleFormat::int16: {
        const auto value = static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] | (p[1] << 8)));
        return static_cast<float>(value) / 32768.0f;
    }
    case SampleFormat::int24: {
        std::int32_t value = p[0] | (p[1] << 8) | (p[2] << 16);
        if (value & 0x800000) value -= 0x1000000;
        return static_cast<float>(static_cast<double>(value) / 8388608.0);
    }
    case SampleFormat::int32: {
        std::int32_t value;
        std::memcpy(&value, p, sizeof(value));
        return static_cast<float>(static_cast<double>(value) / 2147483648.0);
    }
    }
    return 0.0f;
}

void put_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
}

void put_tag(std::vector<std::uint8_t> &out, const char (&tag)[5]) {
    out.insert(out.end(), tag, tag + 4);
}

float to_dbfs(double linear) {
    if (!(linear > 0.0)) return kSilenceDbfs;
    const double db = 20.0 * std::log10(linear);
    return static_cast<float>(std::max(db, static_cast<double>(kSilenceDbfs)));
}

} // namespace

std::size_t bytes_per_sample(SampleFormat sample) {
    switch (sample) {
    case SampleFormat::float32: return 4;
    case SampleFormat::int16: return 2;
    case SampleFormat::int24: return 3;
    case SampleFormat::int32: return 4;
    }
    return 0;
}

std::size_t bytes_per_frame(const PcmFormat &format) {
    return bytes_per_sample(format.sample) * format.channels;
}

bool is_valid(const PcmFormat &format) {
    return format.sample_rate >= 1000 && format.sample_rate <= 768000 && format.channels >= 1 &&
        format.channels <= 64 && bytes_per_sample(format.sample) != 0;
}

std::size_t append_mono(const std::uint8_t *data, std::size_t bytes, const PcmFormat &format,
    std::vector<float> &out) {
    if (!data || !is_valid(format)) return 0;
    const std::size_t sample_bytes = bytes_per_sample(format.sample);
    const std::size_t frame_bytes = bytes_per_frame(format);
    const std::size_t frames = bytes / frame_bytes;
    out.reserve(out.size() + frames);
    const float scale = 1.0f / static_cast<float>(format.channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::uint8_t *p = data + frame * frame_bytes;
        float sum = 0.0f;
        for (std::uint16_t channel = 0; channel < format.channels; ++channel) {
            sum += read_sample(p + channel * sample_bytes, format.sample);
        }
        out.push_back(sum * scale);
    }
    return frames;
}

void append_silence(std::size_t frames, std::vector<float> &out) {
    out.insert(out.end(), frames, 0.0f);
}

std::size_t resampled_length(std::size_t count, std::uint32_t in_rate, std::uint32_t out_rate) {
    if (in_rate == 0 || out_rate == 0) return 0;
    if (in_rate == out_rate) return count;
    return static_cast<std::size_t>(static_cast<std::uint64_t>(count) * out_rate / in_rate);
}

std::vector<float> resample(const float *input, std::size_t count, std::uint32_t in_rate,
    std::uint32_t out_rate) {
    const std::size_t out_count = resampled_length(count, in_rate, out_rate);
    if (!input || out_count == 0) return {};
    if (in_rate == out_rate) return std::vector<float>(input, input + count);

    const auto &table = kernel_table();
    const double cutoff = (out_rate < in_rate ? static_cast<double>(out_rate) / in_rate : 1.0) * kCutoffMargin;
    const double half_width = kZeroCrossings / cutoff;
    const auto reach = static_cast<std::int64_t>(std::ceil(half_width));
    const auto last = static_cast<std::int64_t>(count) - 1;

    std::vector<float> output(out_count);
    for (std::size_t i = 0; i < out_count; ++i) {
        // Exact rational source position avoids accumulated drift.
        const std::uint64_t numerator = static_cast<std::uint64_t>(i) * in_rate;
        const auto base = static_cast<std::int64_t>(numerator / out_rate);
        const double fraction = static_cast<double>(numerator % out_rate) / out_rate;
        const std::int64_t first = std::max<std::int64_t>(0, base - reach);
        const std::int64_t end = std::min<std::int64_t>(last, base + reach + 1);
        double sum = 0.0;
        double weights = 0.0;
        for (std::int64_t k = first; k <= end; ++k) {
            const double distance = std::fabs(static_cast<double>(base - k) + fraction);
            const double weight = kernel(table, distance * cutoff);
            if (weight == 0.0) continue;
            sum += weight * input[k];
            weights += weight;
        }
        // Normalizing by the weight sum keeps unity DC gain, including at the edges.
        output[i] = weights > 1e-9 ? static_cast<float>(sum / weights) : 0.0f;
    }
    return output;
}

std::vector<std::int16_t> to_int16(const float *input, std::size_t count) {
    std::vector<std::int16_t> output;
    if (!input) return output;
    output.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        float value = std::isfinite(input[i]) ? input[i] : 0.0f;
        const float scaled = std::nearbyint(value * 32768.0f);
        const float clamped = std::min(32767.0f, std::max(-32768.0f, scaled));
        output.push_back(static_cast<std::int16_t>(clamped));
    }
    return output;
}

std::vector<std::uint8_t> build_wav(const std::int16_t *samples, std::size_t count,
    std::uint32_t sample_rate, std::uint16_t channels) {
    if (!samples) count = 0;
    if (channels == 0) channels = 1;
    const std::size_t max_samples = (std::numeric_limits<std::uint32_t>::max() - kWavHeaderBytes) / 2;
    count = std::min(count, max_samples);
    const auto data_bytes = static_cast<std::uint32_t>(count * 2);
    std::vector<std::uint8_t> out;
    out.reserve(kWavHeaderBytes + data_bytes);
    put_tag(out, "RIFF");
    put_u32(out, 36 + data_bytes);
    put_tag(out, "WAVE");
    put_tag(out, "fmt ");
    put_u32(out, 16);
    put_u16(out, 1); // WAVE_FORMAT_PCM
    put_u16(out, channels);
    put_u32(out, sample_rate);
    put_u32(out, sample_rate * channels * 2);
    put_u16(out, static_cast<std::uint16_t>(channels * 2));
    put_u16(out, 16);
    put_tag(out, "data");
    put_u32(out, data_bytes);
    for (std::size_t i = 0; i < count; ++i) put_u16(out, static_cast<std::uint16_t>(samples[i]));
    return out;
}

std::vector<std::uint8_t> mono_to_wav(const float *mono, std::size_t count, std::uint32_t in_rate) {
    const auto resampled = resample(mono, count, in_rate, kTargetSampleRate);
    const auto pcm = to_int16(resampled.data(), resampled.size());
    return build_wav(pcm.data(), pcm.size());
}

std::vector<std::uint8_t> convert_to_wav(const std::uint8_t *data, std::size_t bytes,
    const PcmFormat &format) {
    std::vector<float> mono;
    append_mono(data, bytes, format, mono);
    return mono_to_wav(mono.data(), mono.size(), format.sample_rate);
}

double duration_seconds(std::size_t frames, std::uint32_t sample_rate) {
    return sample_rate == 0 ? 0.0 : static_cast<double>(frames) / sample_rate;
}

Levels measure_levels(const float *samples, std::size_t count) {
    Levels levels;
    if (!samples || count == 0) return levels;
    double squares = 0.0;
    double peak = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double value = std::isfinite(samples[i]) ? std::fabs(samples[i]) : 0.0;
        squares += value * value;
        peak = std::max(peak, value);
    }
    levels.rms_dbfs = to_dbfs(std::sqrt(squares / static_cast<double>(count)));
    levels.peak_dbfs = to_dbfs(peak);
    return levels;
}

Levels measure_levels(const std::int16_t *samples, std::size_t count) {
    Levels levels;
    if (!samples || count == 0) return levels;
    double squares = 0.0;
    double peak = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double value = std::fabs(samples[i] / 32768.0);
        squares += value * value;
        peak = std::max(peak, value);
    }
    levels.rms_dbfs = to_dbfs(std::sqrt(squares / static_cast<double>(count)));
    levels.peak_dbfs = to_dbfs(peak);
    return levels;
}

float meter_level(float rms_dbfs) {
    if (!std::isfinite(rms_dbfs)) return 0.0f;
    return std::min(1.0f, std::max(0.0f, (rms_dbfs + 60.0f) / 60.0f));
}

} // namespace lemonvox::win::audio
