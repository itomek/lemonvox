// Non-interactive tests for the Windows insertion adapter. They use the real
// clipboard, so they hold the interactive lock while touching it and always
// restore the person's clipboard, including when a check fails.

#include "common/executor.h"
#include "insertion/clipboard.h"
#include "insertion/integrity.h"
#include "insertion/target.h"
#include "insertion/win_insertion.h"
#include "support/interactive_lock.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace lemonvox;
using namespace lemonvox::win;

namespace {

ClipboardSnapshot *g_saved_clipboard = nullptr;

void restore_saved_clipboard() {
    if (!g_saved_clipboard) return;
    ClipboardSnapshot *saved = g_saved_clipboard;
    g_saved_clipboard = nullptr;
    restore_clipboard(nullptr, *saved, nullptr);
}

[[noreturn]] void fail(const char *expression, int line) {
    std::fprintf(stderr, "insertion_unit_test:%d: check failed: %s\n", line, expression);
    restore_saved_clipboard();
    std::fflush(stderr);
    std::exit(1);
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS *) {
    restore_saved_clipboard();
    return EXCEPTION_CONTINUE_SEARCH;
}

#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

using Bytes = std::vector<std::uint8_t>;

Bytes bytes_of(const void *data, std::size_t size) {
    const auto *begin = static_cast<const std::uint8_t *>(data);
    return Bytes(begin, begin + size);
}

Bytes wide_bytes(const std::wstring &text) {
    return bytes_of(text.c_str(), (text.size() + 1) * sizeof(wchar_t));
}

Bytes ansi_bytes(const std::string &text) {
    return bytes_of(text.c_str(), text.size() + 1);
}

LRESULT CALLBACK owner_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND owner_window() {
    static HWND window = nullptr;
    if (window) return window;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = owner_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"LemonVoxInsertionUnitOwner";
    RegisterClassExW(&window_class);
    window = CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
        window_class.hInstance, nullptr);
    CHECK(window != nullptr);
    return window;
}

void set_clipboard(const std::vector<std::pair<UINT, Bytes>> &formats) {
    bool open = false;
    for (int attempt = 0; attempt < 50 && !open; ++attempt) {
        open = OpenClipboard(owner_window()) != FALSE;
        if (!open) Sleep(10);
    }
    CHECK(open);
    CHECK(EmptyClipboard());
    for (const auto &[format, data] : formats) {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, data.size());
        CHECK(memory != nullptr);
        std::memcpy(GlobalLock(memory), data.data(), data.size());
        GlobalUnlock(memory);
        CHECK(SetClipboardData(format, memory) != nullptr);
    }
    CloseClipboard();
}

const ClipboardItem *find(const ClipboardSnapshot &snapshot, UINT format) {
    for (const auto &item : snapshot.items) {
        if (item.format == format) return &item;
    }
    return nullptr;
}

Bytes small_dib() {
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = 2;
    header.biHeight = 2;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = 16;
    Bytes dib = bytes_of(&header, sizeof(header));
    const std::uint8_t pixels[16] = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255};
    dib.insert(dib.end(), pixels, pixels + sizeof(pixels));
    return dib;
}

Bytes file_drop() {
    DROPFILES drop{};
    drop.pFiles = sizeof(drop);
    drop.fWide = TRUE;
    Bytes data = bytes_of(&drop, sizeof(drop));
    const wchar_t files[] = L"C:\\LemonVox\\insertion-test.txt\0";
    const Bytes names = bytes_of(files, sizeof(files));
    data.insert(data.end(), names.begin(), names.end());
    return data;
}

bool clipboard_has(UINT format) {
    return IsClipboardFormatAvailable(format) != FALSE;
}

void capturable_formats_exclude_live_handles() {
    CHECK(clipboard_format_is_capturable(CF_UNICODETEXT));
    CHECK(clipboard_format_is_capturable(CF_DIB));
    CHECK(clipboard_format_is_capturable(CF_DIBV5));
    CHECK(clipboard_format_is_capturable(CF_HDROP));
    CHECK(clipboard_format_is_capturable(CF_ENHMETAFILE));
    CHECK(clipboard_format_is_capturable(RegisterClipboardFormatW(L"HTML Format")));
    CHECK(clipboard_format_is_capturable(RegisterClipboardFormatW(L"Rich Text Format")));
    CHECK(!clipboard_format_is_capturable(CF_BITMAP));
    CHECK(!clipboard_format_is_capturable(CF_METAFILEPICT));
    CHECK(!clipboard_format_is_capturable(CF_PALETTE));
    CHECK(!clipboard_format_is_capturable(CF_OWNERDISPLAY));
    CHECK(!clipboard_format_is_capturable(CF_DSPTEXT));
    CHECK(!clipboard_format_is_capturable(CF_PRIVATEFIRST));
    CHECK(!clipboard_format_is_capturable(CF_GDIOBJFIRST));
    CHECK(!clipboard_format_is_capturable(RegisterClipboardFormatW(L"OLE Private Data")));
    CHECK(!clipboard_format_is_capturable(RegisterClipboardFormatW(L"DataObject")));
}

void clipboard_snapshot_round_trips_formats() {
    const UINT html = RegisterClipboardFormatW(L"HTML Format");
    const UINT rtf = RegisterClipboardFormatW(L"Rich Text Format");
    const UINT custom = RegisterClipboardFormatW(L"LemonVoxInsertionUnitTestFormat");
    const std::wstring text = L"LemonVox original \u2713 \U0001F34B";
    const Bytes html_bytes = ansi_bytes("Version:0.9\r\nStartHTML:-1\r\n<b>LemonVox</b>");
    const Bytes rtf_bytes = ansi_bytes("{\\rtf1\\ansi LemonVox}");
    const Bytes custom_bytes = {0, 1, 2, 3, 250, 251, 252, 253};
    const Bytes dib = small_dib();
    const Bytes drop = file_drop();
    set_clipboard({{CF_UNICODETEXT, wide_bytes(text)}, {html, html_bytes}, {rtf, rtf_bytes},
        {custom, custom_bytes}, {CF_DIB, dib}, {CF_HDROP, drop}});

    ClipboardSnapshot snapshot;
    CHECK(snapshot_clipboard(nullptr, snapshot) == ClipboardStatus::ok);
    CHECK(snapshot.sequence == GetClipboardSequenceNumber());
    CHECK(find(snapshot, CF_UNICODETEXT) && find(snapshot, CF_UNICODETEXT)->bytes == wide_bytes(text));
    // Synthesized ANSI text and the synthesized bitmap/DIBV5 are not stored twice.
    CHECK(!find(snapshot, CF_TEXT) && !find(snapshot, CF_OEMTEXT));
    CHECK(!find(snapshot, CF_BITMAP) && !find(snapshot, CF_DIBV5));
    CHECK(find(snapshot, html) && find(snapshot, html)->bytes == html_bytes);
    CHECK(find(snapshot, rtf) && find(snapshot, rtf)->bytes == rtf_bytes);
    CHECK(find(snapshot, custom) && find(snapshot, custom)->bytes == custom_bytes);
    CHECK(find(snapshot, CF_DIB) && find(snapshot, CF_DIB)->bytes == dib);
    CHECK(find(snapshot, CF_HDROP) && find(snapshot, CF_HDROP)->bytes == drop);

    DWORD written = 0;
    CHECK(write_private_text(nullptr, L"pasted \u2713", &written) == ClipboardStatus::ok);
    CHECK(written == GetClipboardSequenceNumber());
    CHECK(written != snapshot.sequence);
    std::wstring current;
    CHECK(read_clipboard_text(current) && current == L"pasted \u2713");
    CHECK(clipboard_has(clipboard_exclude_monitor_format()));
    CHECK(clipboard_has(clipboard_history_format()));
    CHECK(clipboard_has(clipboard_cloud_format()));
    CHECK(!clipboard_has(custom) && !clipboard_has(html) && !clipboard_has(CF_HDROP));

    DWORD restored = 0;
    CHECK(restore_clipboard(nullptr, snapshot, &restored) == ClipboardStatus::ok);
    CHECK(restored != written);
    ClipboardSnapshot after;
    CHECK(snapshot_clipboard(nullptr, after) == ClipboardStatus::ok);
    for (const auto &item : snapshot.items) {
        const ClipboardItem *copy = find(after, item.format);
        CHECK(copy != nullptr);
        if (item.format == CF_DIB) {
            // The system may normalize a DIB it re-synthesizes; the header and pixels must survive.
            CHECK(copy->bytes.size() >= item.bytes.size());
            CHECK(std::memcmp(copy->bytes.data(), item.bytes.data(), item.bytes.size()) == 0);
        } else {
            CHECK(copy->bytes == item.bytes);
        }
    }
    CHECK(read_clipboard_text(current) && current == text);
    // The restored copy is kept out of clipboard history and cloud sync.
    CHECK(clipboard_has(clipboard_history_format()));
}

void empty_clipboard_round_trips() {
    set_clipboard({});
    ClipboardSnapshot snapshot;
    CHECK(snapshot_clipboard(nullptr, snapshot) == ClipboardStatus::ok);
    CHECK(snapshot.items.empty());
    CHECK(write_private_text(nullptr, L"x", nullptr) == ClipboardStatus::ok);
    CHECK(restore_clipboard(nullptr, snapshot, nullptr) == ClipboardStatus::ok);
    CHECK(CountClipboardFormats() == 0);
}

void copy_fallback_marks_private_content() {
    CHECK(copy_text_to_clipboard("Copy fallback \xE2\x9C\x93 123"));
    std::wstring current;
    CHECK(read_clipboard_text(current) && current == L"Copy fallback \u2713 123");
    CHECK(clipboard_has(clipboard_exclude_monitor_format()));
    CHECK(clipboard_has(clipboard_history_format()));
    CHECK(clipboard_has(clipboard_cloud_format()));
    CHECK(!copy_text_to_clipboard("\xC3\x28"));
    CHECK(read_clipboard_text(current) && current == L"Copy fallback \u2713 123");
}

void integrity_comparison() {
    CHECK(compare_integrity(SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_HIGH_RID) ==
        IntegrityDecision::target_higher);
    CHECK(compare_integrity(SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_SYSTEM_RID) ==
        IntegrityDecision::target_higher);
    CHECK(compare_integrity(SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_MEDIUM_RID) ==
        IntegrityDecision::allowed);
    CHECK(compare_integrity(SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_LOW_RID) ==
        IntegrityDecision::allowed);
    CHECK(compare_integrity(SECURITY_MANDATORY_HIGH_RID, SECURITY_MANDATORY_MEDIUM_RID) ==
        IntegrityDecision::allowed);
    DWORD own = 0;
    CHECK(current_process_integrity(own));
    CHECK(own >= SECURITY_MANDATORY_LOW_RID);
    CHECK(input_integrity(GetCurrentProcessId()) == IntegrityDecision::allowed);
    // The System process token is never readable from a user process.
    CHECK(input_integrity(4) != IntegrityDecision::allowed);
    CHECK(input_integrity(0xFFFFFFF0u) == IntegrityDecision::unknown);
}

void target_table_invalidates_by_operation() {
    TargetTable table;
    TargetIdentity identity;
    identity.process_id = 42;
    identity.runtime_id = {1, 2, 3};
    const auto first = table.allocate(1, identity);
    const auto second = table.allocate(1, identity);
    const auto other = table.allocate(2, identity);
    CHECK(first != 0 && second != 0 && other != 0 && first != second && second != other);
    TargetIdentity found;
    CHECK(table.lookup(first, 1, found) && same_target(found, identity));
    CHECK(!table.lookup(first, 2, found));
    CHECK(!table.lookup(999, 1, found));
    table.invalidate(1);
    CHECK(table.size() == 1);
    CHECK(!table.lookup(first, 1, found) && !table.lookup(second, 1, found));
    CHECK(table.lookup(other, 2, found));
    table.retain_only(3);
    CHECK(table.size() == 0);
    table.allocate(3, identity);
    table.clear();
    CHECK(table.size() == 0);

    TargetIdentity changed = identity;
    changed.runtime_id = {1, 2, 4};
    CHECK(!same_target(identity, changed));
    changed = identity;
    changed.process_start = 7;
    CHECK(!same_target(identity, changed));
}

void pump_for(DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    MSG message;
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

template <typename Predicate>
void pump_until(Predicate done, DWORD timeout = 10000) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    MSG message;
    while (!done() && GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!done()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

struct Calls {
    std::vector<int> counts;
    std::vector<AdapterError> errors;
    std::vector<TargetSnapshot> targets;
    int add() {
        counts.push_back(0);
        errors.push_back(AdapterError::none);
        targets.emplace_back();
        return static_cast<int>(counts.size() - 1);
    }
    TargetCompletion target(int index, OperationId expected, DWORD owner) {
        return [this, index, expected, owner](OperationId operation, TargetResult result) {
            CHECK(GetCurrentThreadId() == owner);
            CHECK(operation == expected);
            ++counts[static_cast<std::size_t>(index)];
            errors[static_cast<std::size_t>(index)] = result.error;
            targets[static_cast<std::size_t>(index)] = result.target;
        };
    }
    VoidCompletion done(int index, OperationId expected, DWORD owner) {
        return [this, index, expected, owner](OperationId operation, Completion result) {
            CHECK(GetCurrentThreadId() == owner);
            CHECK(operation == expected);
            ++counts[static_cast<std::size_t>(index)];
            errors[static_cast<std::size_t>(index)] = result.error;
        };
    }
    bool all_done() const {
        for (int count : counts) {
            if (count == 0) return false;
        }
        return true;
    }
};

// Requests only read the current foreground target. No input is synthesized:
// every insert below uses a token that cannot resolve to a target.
void adapter_completes_exactly_once_and_invalidates() {
    UiThreadExecutor executor;
    const DWORD owner = GetCurrentThreadId();
    Calls calls;
    {
        WinInsertion insertion(executor);
        const int capture = calls.add();
        insertion.capture_original(1, calls.target(capture, 1, owner));
        pump_until([&] { return calls.all_done(); });
        CHECK(calls.counts[0] == 1);
        const TargetSnapshot captured = calls.targets[0];
        if (calls.errors[0] == AdapterError::none && captured.token.value != 0) {
            CHECK(insertion.token_count() == 1);
            CHECK(captured.editable && !captured.secure && captured.matches_original);
        } else {
            CHECK(insertion.token_count() == 0);
        }
        insertion.cancel(1);
        CHECK(insertion.token_count() == 0);

        const int inspect = calls.add();
        insertion.inspect(1, captured.token, calls.target(inspect, 1, owner));
        const int insert = calls.add();
        insertion.insert(1, captured.token, "never typed", calls.done(insert, 1, owner));
        pump_until([&] { return calls.all_done(); });
        CHECK(calls.errors[static_cast<std::size_t>(inspect)] == AdapterError::canceled);
        CHECK(calls.errors[static_cast<std::size_t>(insert)] == AdapterError::canceled);

        // A fresh operation: unknown tokens never resolve to a target.
        const int capture2 = calls.add();
        insertion.capture_original(2, calls.target(capture2, 2, owner));
        const int unknown = calls.add();
        insertion.insert(2, TargetToken{0xFFFFFFFFu}, "never typed", calls.done(unknown, 2, owner));
        const int unknown_inspect = calls.add();
        insertion.inspect(2, TargetToken{0xFFFFFFFFu}, calls.target(unknown_inspect, 2, owner));
        std::vector<int> current;
        for (int i = 0; i < 10; ++i) {
            current.push_back(calls.add());
            insertion.inspect(2, TargetToken{}, calls.target(current.back(), 2, owner));
        }
        pump_until([&] { return calls.all_done(); });
        CHECK(calls.errors[static_cast<std::size_t>(unknown)] == AdapterError::target_changed);
        CHECK(calls.errors[static_cast<std::size_t>(unknown_inspect)] == AdapterError::target_changed);
        for (int index : current) {
            const auto error = calls.errors[static_cast<std::size_t>(index)];
            CHECK(error == AdapterError::none || error == AdapterError::unavailable);
            CHECK(!calls.targets[static_cast<std::size_t>(index)].matches_original);
        }
        insertion.release(2);
        CHECK(insertion.token_count() == 0);

        // A newer operation makes every older one stale.
        const int capture3 = calls.add();
        insertion.capture_original(3, calls.target(capture3, 3, owner));
        const int stale = calls.add();
        insertion.inspect(2, TargetToken{}, calls.target(stale, 2, owner));
        pump_until([&] { return calls.all_done(); });
        CHECK(calls.errors[static_cast<std::size_t>(stale)] == AdapterError::canceled);

        pump_for(200);
        for (int count : calls.counts) CHECK(count == 1);

        // shutdown() is a barrier: queued and later requests never call back.
        const std::size_t before = calls.counts.size();
        const int queued = calls.add();
        insertion.capture_original(4, calls.target(queued, 4, owner));
        insertion.shutdown();
        CHECK(insertion.token_count() == 0);
        const int late = calls.add();
        insertion.inspect(4, TargetToken{}, calls.target(late, 4, owner));
        insertion.cancel(4);
        insertion.shutdown();
        pump_for(300);
        for (std::size_t index = before; index < calls.counts.size(); ++index) CHECK(calls.counts[index] == 0);
    }
    pump_for(50);
    executor.shutdown();
}

} // namespace

int main() {
    SetUnhandledExceptionFilter(crash_filter);
    capturable_formats_exclude_live_handles();
    integrity_comparison();
    target_table_invalidates_by_operation();
    {
        lemonvox::win::test::InteractiveLock lock;
        auto saved = std::make_unique<ClipboardSnapshot>();
        CHECK(snapshot_clipboard(nullptr, *saved) == ClipboardStatus::ok);
        g_saved_clipboard = saved.get();
        clipboard_snapshot_round_trips_formats();
        empty_clipboard_round_trips();
        copy_fallback_marks_private_content();
        restore_saved_clipboard();
    }
    adapter_completes_exactly_once_and_invalidates();
    std::puts("insertion_unit_test passed");
    return 0;
}
