#pragma once

#include <functional>

namespace lemonvox::win {

// The single serialized core executor. Every WorkflowCore entry point and every
// adapter completion runs through post(); adapters on worker threads must never
// call into the core directly.
class Executor {
public:
    virtual ~Executor() = default;
    // Thread-safe. Returns false once the executor has shut down; the task is
    // then destroyed without running.
    virtual bool post(std::function<void()> task) = 0;
};

// Runs tasks on the thread that created it, using a hidden message-only window.
// That thread must pump messages. shutdown() must be called on the owner thread;
// tasks posted afterwards are discarded.
class UiThreadExecutor final : public Executor {
public:
    UiThreadExecutor();
    ~UiThreadExecutor() override;
    UiThreadExecutor(const UiThreadExecutor &) = delete;
    UiThreadExecutor &operator=(const UiThreadExecutor &) = delete;

    bool post(std::function<void()> task) override;
    // Runs already-queued tasks synchronously on the owner thread.
    void drain();
    void shutdown();
    bool is_owner_thread() const;

private:
    struct State;
    State *state_;
};

} // namespace lemonvox::win
