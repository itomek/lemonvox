#pragma once

#include "adapters.h"
#include "common/executor.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lemonvox::win {

// Parses a RIFF/WAVE file with PCM or IEEE-float samples and converts it to the
// 16 kHz mono 16-bit upload format through the same path as live capture.
bool convert_wav_for_upload(const std::vector<std::uint8_t> &wav, std::vector<std::uint8_t> &out);

// Capture adapter that "records" a prepared WAV file. Used by automated
// end-to-end checks and the --sample-wav debug launch option so the full
// pipeline can be exercised without recording real microphone audio.
class FileCapture final : public CaptureAdapter {
public:
    FileCapture(Executor &executor, std::vector<std::uint8_t> upload_wav);
    // Reads and converts `path`; returns false when the file is unreadable or unsupported.
    static bool load(const std::wstring &path, std::vector<std::uint8_t> &upload_wav);

    void start(OperationId operation, VoidCompletion completion) override;
    void stop(OperationId operation, BytesCompletion completion) override;
    void cancel(OperationId operation) override;
    void shutdown() override;

private:
    Executor &executor_;
    std::vector<std::uint8_t> wav_;
    OperationId recording_ = 0;
    bool shut_down_ = false;
    // Posted completions check this so none runs after shutdown().
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace lemonvox::win
