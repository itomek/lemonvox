#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "adapters.h"

struct IUIAutomation;
struct IUIAutomationElement;

namespace lemonvox::win {

// Identity of a focused text target. Two snapshots refer to the same target
// only when every field matches: the same top-level window, owned by the same
// process instance (process IDs are reused, start times are not), with the
// same focused child window and the same UI Automation element.
struct TargetIdentity {
    HWND foreground = nullptr;
    DWORD process_id = 0;
    std::uint64_t process_start = 0;
    HWND focus = nullptr;
    // UIA runtime ID; empty when UI Automation could not reach the element.
    std::vector<int> runtime_id;
};

bool same_target(const TargetIdentity &a, const TargetIdentity &b);

struct TargetFacts {
    bool editable = false;
    bool secure = false;
};

struct ProbedTarget {
    TargetIdentity identity;
    TargetFacts facts;
};

// Win32 facts about the focused child window, used when UI Automation is
// unavailable (for example for a higher-integrity target) and to refine it.
struct Win32FocusFacts {
    bool edit_class = false;
    bool read_only = false;
    bool password = false;
    bool caret = false;
};

Win32FocusFacts win32_focus_facts(HWND focus, bool caret);

// Reads the current foreground target. Must be used on one thread that has
// initialized COM; the object is not thread-safe.
class TargetProbe {
public:
    explicit TargetProbe(std::function<std::uint64_t()> input_generation = {});
    ~TargetProbe();
    TargetProbe(const TargetProbe &) = delete;
    TargetProbe &operator=(const TargetProbe &) = delete;

    // Returns false when there is no foreground window.
    bool probe(ProbedTarget &target);
    // Cheap Win32-only check used immediately before synthesizing input.
    static bool foreground_focus_matches(const TargetIdentity &identity);

private:
    IUIAutomation *automation_ = nullptr;
    std::function<std::uint64_t()> input_generation_;
    IUIAutomationElement *focused_element(DWORD process_id, HWND focus, bool &stale);
    bool probe_once(ProbedTarget &target, bool &stale);
};

// Adapter-owned token storage. Tokens are opaque indexes, never pointers.
// Thread-safe: cancel() may run on the core executor while the worker reads.
class TargetTable {
public:
    std::uint64_t allocate(OperationId operation, const TargetIdentity &identity);
    bool lookup(std::uint64_t token, OperationId operation, TargetIdentity &identity) const;
    void invalidate(OperationId operation);
    // Drops every token that does not belong to the given operation.
    void retain_only(OperationId operation);
    void clear();
    std::size_t size() const;

private:
    struct Entry {
        OperationId operation = 0;
        TargetIdentity identity;
    };
    mutable std::mutex mutex_;
    std::map<std::uint64_t, Entry> entries_;
    std::uint64_t next_ = 1;
};

} // namespace lemonvox::win
