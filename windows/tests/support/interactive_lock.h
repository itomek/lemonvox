#pragma once

// Interactive tests inject keyboard input, move focus or show windows on the
// real desktop. They must hold this lock for their whole duration so parallel
// test runs cannot type into each other's windows, and must verify their own
// window is foreground before injecting anything.

#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace lemonvox::win::test {

class InteractiveLock {
public:
    explicit InteractiveLock(DWORD timeout_ms = 15 * 60 * 1000) {
        mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\LemonVoxInteractiveTestLock");
        if (!mutex_) {
            std::fprintf(stderr, "interactive lock unavailable\n");
            std::exit(2);
        }
        const DWORD wait = WaitForSingleObject(mutex_, timeout_ms);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            std::fprintf(stderr, "timed out waiting for interactive lock\n");
            std::exit(2);
        }
    }
    ~InteractiveLock() {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
    }
    InteractiveLock(const InteractiveLock &) = delete;
    InteractiveLock &operator=(const InteractiveLock &) = delete;

private:
    HANDLE mutex_ = nullptr;
};

} // namespace lemonvox::win::test
