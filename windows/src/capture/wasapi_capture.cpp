#include "capture/wasapi_capture.h"

#include "capture/com_scope.h"
#include "capture/wasapi_stream.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <thread>
#include <utility>
#include <vector>

namespace lemonvox::win {

struct WasapiCapture::Shared {
    std::atomic<bool> closed{false};
    std::atomic<OperationId> canceled_through{0};
    std::atomic<CaptureFailureReason> last_failure{CaptureFailureReason::none};
    mutable std::mutex mutex;
    std::wstring device_id;
    LevelCallback level;
    FailureCallback failure;

    bool canceled(OperationId operation) const {
        return operation != 0 && operation <= canceled_through.load();
    }
    bool deliverable(OperationId operation) const {
        return !closed.load() && !canceled(operation);
    }
};

struct WasapiCapture::Worker {
    enum class Kind : std::uint8_t { start, stop, cancel, shutdown };
    struct Command {
        Kind kind = Kind::shutdown;
        OperationId operation = 0;
        VoidCompletion started;
        BytesCompletion stopped;
    };

    Worker(Executor &executor, std::shared_ptr<Shared> shared, std::shared_ptr<CaptureBackend> backend)
        : executor_(executor), shared_(std::move(shared)), backend_(std::move(backend)),
          command_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        thread_ = std::thread([this] { run(); });
    }

    ~Worker() {
        if (thread_.joinable()) {
            enqueue({Kind::shutdown, 0, {}, {}});
            thread_.join();
        }
        if (command_event_) CloseHandle(command_event_);
    }

    void enqueue(Command command) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push_back(std::move(command));
        }
        SetEvent(command_event_);
    }

    void join() {
        enqueue({Kind::shutdown, 0, {}, {}});
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        ComScope com;
        com_ready_ = com.usable();
        for (;;) {
            HANDLE handles[2] = {command_event_, stream_ ? stream_->ready_event() : nullptr};
            const DWORD count = stream_ ? 2 : 1;
            // The timeout keeps device-loss detection alive if a driver stops signaling.
            WaitForMultipleObjects(count, handles, FALSE, stream_ ? 100 : INFINITE);
            if (!drain_commands()) break;
            if (stream_) pump();
        }
        teardown();
    }

    // Returns false once shutdown is requested.
    bool drain_commands() {
        for (;;) {
            Command command;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (queue_.empty()) return true;
                command = std::move(queue_.front());
                queue_.pop_front();
            }
            switch (command.kind) {
            case Kind::start: handle_start(command.operation, std::move(command.started)); break;
            case Kind::stop: handle_stop(command.operation, std::move(command.stopped)); break;
            case Kind::cancel:
                if (recording_ != 0 && recording_ <= command.operation) teardown();
                break;
            case Kind::shutdown: return false;
            }
        }
    }

    void handle_start(OperationId operation, VoidCompletion completion) {
        if (shared_->canceled(operation)) return;
        teardown();
        shared_->last_failure.store(CaptureFailureReason::none);
        std::wstring device_id;
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            device_id = shared_->device_id;
        }

        CaptureFailureReason reason = CaptureFailureReason::internal;
        if (com_ready_ && backend_) reason = backend_->open(device_id, stream_);
        if (reason == CaptureFailureReason::none && !(stream_ && audio::is_valid(stream_->format()))) {
            reason = CaptureFailureReason::format;
        }
        if (reason == CaptureFailureReason::none) reason = stream_->start();
        if (reason != CaptureFailureReason::none) {
            stream_.reset();
            shared_->last_failure.store(reason);
            deliver(operation, [completion, operation] { completion(operation, {AdapterError::unavailable}); });
            return;
        }

        recording_ = operation;
        failed_ = CaptureFailureReason::none;
        format_ = stream_->format();
        frame_bytes_ = audio::bytes_per_frame(format_);
        max_frames_ = static_cast<std::size_t>(format_.sample_rate * kMaxSeconds);
        level_window_ = std::max<std::size_t>(1, format_.sample_rate / 20);
        level_frames_ = 0;
        level_squares_ = 0.0;
        deliver(operation, [completion, operation] { completion(operation, {AdapterError::none}); });
    }

    void handle_stop(OperationId operation, BytesCompletion completion) {
        if (shared_->canceled(operation)) return;
        if (operation == 0 || operation != recording_) {
            deliver(operation, [completion, operation] { completion(operation, {AdapterError::invalid_input, {}}); });
            return;
        }
        if (stream_) {
            pump();
            stream_.reset();
        }
        BytesResult result;
        if (failed_ != CaptureFailureReason::none) {
            result.error = AdapterError::unavailable;
        } else if (audio::duration_seconds(mono_.size(), format_.sample_rate) >= kMinSeconds) {
            result.bytes = audio::mono_to_wav(mono_.data(), mono_.size(), format_.sample_rate);
        }
        teardown();
        deliver(operation, [completion, operation, result = std::move(result)]() mutable {
            completion(operation, std::move(result));
        });
    }

    void pump() {
        bool limit_reached = false;
        const auto reason = stream_->read([&](const std::uint8_t *data, std::uint32_t frames) {
            consume(data, frames, limit_reached);
        });
        if (reason != CaptureFailureReason::none) {
            fail(reason);
            return;
        }
        // Like macOS record(forDuration:), capture ends at the limit and keeps the audio.
        if (limit_reached) stream_.reset();
    }

    void consume(const std::uint8_t *data, std::uint32_t frames, bool &limit_reached) {
        const std::size_t room = max_frames_ > mono_.size() ? max_frames_ - mono_.size() : 0;
        const std::size_t take = std::min<std::size_t>(frames, room);
        const std::size_t before = mono_.size();
        if (data) audio::append_mono(data, take * frame_bytes_, format_, mono_);
        else audio::append_silence(take, mono_);
        if (mono_.size() >= max_frames_) limit_reached = true;

        for (std::size_t i = before; i < mono_.size(); ++i) {
            level_squares_ += static_cast<double>(mono_[i]) * mono_[i];
            if (++level_frames_ < level_window_) continue;
            const auto rms = static_cast<float>(std::sqrt(level_squares_ / static_cast<double>(level_frames_)));
            const float level = audio::meter_level(rms > 0.0f ? 20.0f * std::log10(rms) : audio::kSilenceDbfs);
            level_frames_ = 0;
            level_squares_ = 0.0;
            post_level(level);
        }
    }

    void post_level(float level) {
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            if (!shared_->level) return;
        }
        auto shared = shared_;
        deliver(recording_, [shared, level] {
            LevelCallback callback;
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                callback = shared->level;
            }
            if (callback) callback(level);
        });
    }

    void fail(CaptureFailureReason reason) {
        const OperationId operation = recording_;
        stream_.reset();
        discard_audio();
        failed_ = reason;
        shared_->last_failure.store(reason);
        auto shared = shared_;
        deliver(operation, [shared, operation, reason] {
            FailureCallback callback;
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                callback = shared->failure;
            }
            if (callback) callback(operation, reason);
        });
    }

    void discard_audio() {
        if (!mono_.empty()) SecureZeroMemory(mono_.data(), mono_.size() * sizeof(float));
        std::vector<float>().swap(mono_);
    }

    void teardown() {
        stream_.reset();
        discard_audio();
        recording_ = 0;
        failed_ = CaptureFailureReason::none;
    }

    void deliver(OperationId operation, std::function<void()> task) {
        if (!shared_->deliverable(operation)) return;
        auto shared = shared_;
        executor_.post([shared, operation, task = std::move(task)] {
            if (shared->deliverable(operation)) task();
        });
    }

    Executor &executor_;
    std::shared_ptr<Shared> shared_;
    std::shared_ptr<CaptureBackend> backend_;
    HANDLE command_event_ = nullptr;
    std::mutex queue_mutex_;
    std::deque<Command> queue_;
    std::thread thread_;

    // Owned by the worker thread.
    bool com_ready_ = false;
    std::unique_ptr<CaptureStream> stream_;
    OperationId recording_ = 0;
    CaptureFailureReason failed_ = CaptureFailureReason::none;
    audio::PcmFormat format_;
    std::size_t frame_bytes_ = 0;
    std::size_t max_frames_ = 0;
    std::vector<float> mono_;
    std::size_t level_window_ = 1;
    std::size_t level_frames_ = 0;
    double level_squares_ = 0.0;
};

WasapiCapture::WasapiCapture(Executor &executor, std::wstring device_id, std::shared_ptr<CaptureBackend> backend)
    : executor_(executor), shared_(std::make_shared<Shared>()) {
    shared_->device_id = std::move(device_id);
    if (!backend) backend = make_wasapi_backend();
    worker_ = std::make_unique<Worker>(executor_, shared_, std::move(backend));
}

WasapiCapture::~WasapiCapture() {
    shutdown();
}

void WasapiCapture::set_device_id(std::wstring device_id) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->device_id = std::move(device_id);
}

std::wstring WasapiCapture::device_id() const {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->device_id;
}

void WasapiCapture::set_level_callback(LevelCallback callback) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->level = std::move(callback);
}

void WasapiCapture::set_failure_callback(FailureCallback callback) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->failure = std::move(callback);
}

CaptureFailureReason WasapiCapture::last_failure_reason() const {
    return shared_->last_failure.load();
}

void WasapiCapture::start(OperationId operation, VoidCompletion completion) {
    std::lock_guard<std::mutex> lock(shutdown_mutex_);
    if (!worker_ || shared_->closed.load()) return;
    worker_->enqueue({Worker::Kind::start, operation, std::move(completion), {}});
}

void WasapiCapture::stop(OperationId operation, BytesCompletion completion) {
    std::lock_guard<std::mutex> lock(shutdown_mutex_);
    if (!worker_ || shared_->closed.load()) return;
    worker_->enqueue({Worker::Kind::stop, operation, {}, std::move(completion)});
}

void WasapiCapture::cancel(OperationId operation) {
    std::lock_guard<std::mutex> lock(shutdown_mutex_);
    if (!worker_ || shared_->closed.load()) return;
    // Raise the watermark first so completions already queued on the executor are dropped.
    OperationId current = shared_->canceled_through.load();
    while (operation > current && !shared_->canceled_through.compare_exchange_weak(current, operation)) {
    }
    worker_->enqueue({Worker::Kind::cancel, operation, {}, {}});
}

void WasapiCapture::shutdown() {
    std::unique_ptr<Worker> worker;
    {
        std::lock_guard<std::mutex> lock(shutdown_mutex_);
        shared_->closed.store(true);
        worker = std::move(worker_);
    }
    if (worker) worker->join();
}

} // namespace lemonvox::win
