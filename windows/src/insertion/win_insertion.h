#pragma once

#include "adapters.h"
#include "common/executor.h"
#include "insertion/clipboard.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace lemonvox::win {

class TargetProbe;
class TargetTable;

struct InsertionOptions {
    // How long insert() waits for physically held modifiers (for example the
    // Alt of the trigger) to be released before falling back to copy.
    DWORD modifier_release_timeout_ms = 2000;
    // How long the pasted text stays on the clipboard before the previous
    // content is restored. There is no cross-process paste acknowledgement.
    DWORD restore_delay_ms = 600;
    // Thread-safe activity sequence from the global keyboard/mouse listener.
    // Without it, transient retries cannot follow a focused-HWND handoff.
    std::function<std::uint64_t()> input_generation;
    // Test hooks, invoked on the insertion worker thread.
    std::function<void(OperationId)> insert_started;
    // Called immediately before SendInput; returning false refuses the paste.
    std::function<bool()> before_send;
};

// Windows InsertionAdapter: identifies the focused field with Win32 and UI
// Automation and inserts by pasting through a clipboard transaction.
//
// Every native call runs on one worker thread that owns a COM MTA apartment,
// the UI Automation client and a message-only clipboard owner window. Entry
// points only queue work, so the UI thread is never blocked by a target.
// Each request completes exactly once through Executor::post unless
// shutdown() ran first. shutdown() must be called on the executor thread; it
// waits for the worker and guarantees that no callback runs afterwards.
//
// Higher-integrity windows (elevated Task Manager, admin terminals) cannot
// receive input from a medium-integrity LemonVox because of UIPI. insert()
// reports insertion_unavailable for them so the core offers manual copy.
class WinInsertion final : public InsertionAdapter {
public:
    explicit WinInsertion(Executor &executor, InsertionOptions options = {});
    ~WinInsertion() override;
    WinInsertion(const WinInsertion &) = delete;
    WinInsertion &operator=(const WinInsertion &) = delete;

    void capture_original(OperationId operation, TargetCompletion completion) override;
    void inspect(OperationId operation, TargetToken original, TargetCompletion completion) override;
    void insert(OperationId operation, TargetToken target, std::string text, VoidCompletion completion) override;
    void cancel(OperationId operation) override;
    void shutdown() override;

    // WorkflowCore does not cancel the adapter after a successful insertion
    // or a copy fallback. The shell calls this on every terminal outcome to
    // release the operation's tokens; a new capture also drops older tokens.
    void release(OperationId operation);
    std::size_t token_count() const;

private:
    using Job = std::function<void(TargetProbe &)>;

    bool enqueue(Job job);
    void run_worker();
    bool canceled(OperationId operation);
    // Waits while dispatching the worker's messages. Returns false early when
    // cancellable and the operation is canceled.
    bool wait(DWORD milliseconds, OperationId operation, bool cancellable);
    bool modifiers_released(OperationId operation, AdapterError &error);
    AdapterError run_insert(TargetProbe &probe, OperationId operation, TargetToken token, const std::string &text);
    void post(TargetCompletion completion, OperationId operation, TargetResult result);
    void post(VoidCompletion completion, OperationId operation, Completion result);

    Executor &executor_;
    InsertionOptions options_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::unique_ptr<TargetTable> table_;

    std::mutex mutex_;
    std::deque<Job> jobs_;
    std::set<OperationId> canceled_;
    OperationId newest_operation_ = 0;
    bool stopping_ = false;
    bool shut_down_ = false;
    HANDLE wake_ = nullptr;
    HWND clipboard_owner_ = nullptr;
    std::thread worker_;
};

} // namespace lemonvox::win
