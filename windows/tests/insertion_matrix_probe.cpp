// Matrix helper for windows/tests/insertion-matrix.md. It drives WinInsertion
// against a window the caller launched itself and never types into any other
// window: the target must match --title (and --pids when given), must become
// the foreground window, and is re-checked immediately before every SendInput.
//
//   insertion_matrix_probe insert --title <substring> [--pids 1,2] [--focus-name <UIA name>]
//       [--change-focus <UIA name> | --expect-unreadable] [--no-lock]
//   insertion_matrix_probe describe --title <substring> [--pids 1,2] [--samples n] [--msaa] [--no-lock]
//   insertion_matrix_probe keys --title <substring> [--pids 1,2] --combo ctrl+s|ctrl+w [--no-lock]
//
// --no-lock is for callers that already hold Local\LemonVoxInteractiveTestLock.

#include "common/executor.h"
#include "common/utf.h"
#include "insertion/clipboard.h"
#include "insertion/win_insertion.h"
#include "support/interactive_lock.h"

#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <oleacc.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace lemonvox;
using namespace lemonvox::win;

namespace {

constexpr char kText[] = "LemonVox fixed text \xE2\x9C\x93 123";
constexpr wchar_t kSentinel[] = L"LemonVox matrix sentinel";

struct Args {
    std::wstring command;
    std::wstring title;
    std::vector<DWORD> pids;
    std::wstring combo;
    bool lock = true;
    int samples = 3;
    bool msaa = false;
    std::wstring change_focus;
    bool expect_unreadable = false;
    std::wstring focus_name;
};

struct Search {
    const Args *args = nullptr;
    std::vector<HWND> found;
};

BOOL CALLBACK collect(HWND window, LPARAM parameter) {
    auto *search = reinterpret_cast<Search *>(parameter);
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
    wchar_t title[512] = {};
    GetWindowTextW(window, title, 512);
    if (!wcsstr(title, search->args->title.c_str())) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!search->args->pids.empty()) {
        bool owned = false;
        for (DWORD candidate : search->args->pids) owned = owned || candidate == pid;
        if (!owned) return TRUE;
    }
    search->found.push_back(window);
    return TRUE;
}

HWND find_target(const Args &args) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        Search search;
        search.args = &args;
        EnumWindows(collect, reinterpret_cast<LPARAM>(&search));
        if (search.found.size() == 1) return search.found.front();
        if (search.found.size() > 1) {
            std::puts("error=ambiguous-window");
            return nullptr;
        }
        Sleep(100);
    }
    std::puts("error=window-not-found");
    return nullptr;
}

void pump_for(DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    MSG message;
    do {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    } while (GetTickCount64() < deadline);
}

bool bring_to_front(HWND window) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        if (GetForegroundWindow() == window) return true;
        const DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const DWORD own_thread = GetCurrentThreadId();
        const bool attached = foreground_thread != 0 && foreground_thread != own_thread &&
            AttachThreadInput(own_thread, foreground_thread, TRUE);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        if (attached) AttachThreadInput(own_thread, foreground_thread, FALSE);
        pump_for(300);
    }
    return GetForegroundWindow() == window;
}

const char *error_name(AdapterError error) {
    switch (error) {
    case AdapterError::none: return "none";
    case AdapterError::canceled: return "canceled";
    case AdapterError::unavailable: return "unavailable";
    case AdapterError::invalid_input: return "invalid_input";
    case AdapterError::network: return "network";
    case AdapterError::response: return "response";
    case AdapterError::target_changed: return "target_changed";
    case AdapterError::insertion_unavailable: return "insertion_unavailable";
    }
    return "?";
}

void print_target(const char *label, const TargetResult &result) {
    std::printf("%s.error=%s %s.token=%llu %s.editable=%d %s.secure=%d %s.frontmost=%d %s.matches=%d\n", label,
        error_name(result.error), label, static_cast<unsigned long long>(result.target.token.value), label,
        result.target.editable, label, result.target.secure, label, result.target.frontmost, label,
        result.target.matches_original);
}

// Reads text through UI Automation: from the element with the given name in
// the launched window, or from the focused element while the launched window
// is still the foreground window. Other windows are never read.
std::wstring target_text(HWND target, const std::wstring &name) {
    std::wstring text;
    if (name.empty() && GetForegroundWindow() != target) return text;
    IUIAutomation *automation = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
            reinterpret_cast<void **>(&automation)))) {
        return text;
    }
    IUIAutomationElement *element = nullptr;
    if (name.empty()) {
        automation->GetFocusedElement(&element);
    } else {
        IUIAutomationElement *root = nullptr;
        IUIAutomationCondition *condition = nullptr;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocString(name.c_str());
        if (SUCCEEDED(automation->ElementFromHandle(target, &root)) && root &&
            SUCCEEDED(automation->CreatePropertyCondition(UIA_NamePropertyId, value, &condition)) && condition) {
            root->FindFirst(TreeScope_Descendants, condition, &element);
        }
        VariantClear(&value);
        if (condition) condition->Release();
        if (root) root->Release();
    }
    if (element) {
        IUIAutomationTextPattern *pattern = nullptr;
        IUIAutomationValuePattern *value = nullptr;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
                reinterpret_cast<void **>(&pattern))) && pattern) {
            IUIAutomationTextRange *range = nullptr;
            BSTR content = nullptr;
            if (SUCCEEDED(pattern->get_DocumentRange(&range)) && range && SUCCEEDED(range->GetText(-1, &content)) &&
                content) {
                text.assign(content, SysStringLen(content));
            }
            if (content) SysFreeString(content);
            if (range) range->Release();
            pattern->Release();
        } else if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern),
                       reinterpret_cast<void **>(&value))) && value) {
            BSTR content = nullptr;
            if (SUCCEEDED(value->get_CurrentValue(&content)) && content) text.assign(content, SysStringLen(content));
            if (content) SysFreeString(content);
            value->Release();
        }
        element->Release();
    }
    automation->Release();
    return text;
}

// Diagnostic (--msaa): prints the MSAA accFocus chain from the window's client
// object. Note that this alone turns on Chrome's web accessibility tree.
void print_msaa_focus(HWND target, int sample) {
    IAccessible *accessible = nullptr;
    const HRESULT hr = AccessibleObjectFromWindow(target, static_cast<DWORD>(OBJID_CLIENT), __uuidof(IAccessible),
        reinterpret_cast<void **>(&accessible));
    std::printf("msaa[%d] hr=0x%lx", sample, static_cast<unsigned long>(hr));
    for (int hop = 0; accessible && hop < 8; ++hop) {
        VARIANT self;
        VariantInit(&self);
        self.vt = VT_I4;
        self.lVal = CHILDID_SELF;
        VARIANT role, state, focus;
        VariantInit(&role);
        VariantInit(&state);
        VariantInit(&focus);
        accessible->get_accRole(self, &role);
        accessible->get_accState(self, &state);
        std::printf(" -> role=%ld state=0x%lx", role.vt == VT_I4 ? role.lVal : -1, state.vt == VT_I4 ? state.lVal : 0);
        const HRESULT focus_hr = accessible->get_accFocus(&focus);
        IAccessible *next = nullptr;
        if (focus_hr == S_OK && focus.vt == VT_DISPATCH && focus.pdispVal) {
            focus.pdispVal->QueryInterface(__uuidof(IAccessible), reinterpret_cast<void **>(&next));
        } else {
            std::printf(" [focus vt=%d hr=0x%lx val=%ld]", focus.vt, static_cast<unsigned long>(focus_hr),
                focus.vt == VT_I4 ? focus.lVal : 0);
        }
        VariantClear(&focus);
        accessible->Release();
        accessible = next;
    }
    if (accessible) accessible->Release();
    std::printf("\n");
}

// Prints the focused element (read-only) to diagnose an unexpected editable/secure fact.
int run_describe(HWND target, int samples, bool msaa_samples = false) {
    IUIAutomation *automation = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
            reinterpret_cast<void **>(&automation)))) {
        return 1;
    }
    for (int sample = 0; sample < samples; ++sample) {
        IUIAutomationElement *element = nullptr;
        if (GetForegroundWindow() != target) {
            // Never describe a window this probe did not launch.
            std::printf("focus[%d] foreign-foreground\n", sample);
            pump_for(250);
            continue;
        }
        if (msaa_samples) print_msaa_focus(target, sample);
        if (SUCCEEDED(automation->GetFocusedElement(&element)) && element) {
            CONTROLTYPEID type = 0;
            BSTR name = nullptr, class_name = nullptr, framework = nullptr;
            BOOL password = FALSE;
            element->get_CurrentControlType(&type);
            element->get_CurrentName(&name);
            element->get_CurrentClassName(&class_name);
            element->get_CurrentFrameworkId(&framework);
            element->get_CurrentIsPassword(&password);
            VARIANT value_available, text_available, read_only;
            VariantInit(&value_available);
            VariantInit(&text_available);
            VariantInit(&read_only);
            element->GetCurrentPropertyValue(UIA_IsValuePatternAvailablePropertyId, &value_available);
            element->GetCurrentPropertyValue(UIA_IsTextPatternAvailablePropertyId, &text_available);
            element->GetCurrentPropertyValue(UIA_ValueIsReadOnlyPropertyId, &read_only);
            std::printf("focus[%d] type=%d class=%ls framework=%ls name_length=%u secret=%d value=%d text=%d "
                        "value_read_only=%d\n",
                sample, type, class_name ? class_name : L"", framework ? framework : L"",
                name ? SysStringLen(name) : 0u, password, value_available.vt == VT_BOOL && value_available.boolVal,
                text_available.vt == VT_BOOL && text_available.boolVal,
                read_only.vt == VT_BOOL && read_only.boolVal);
            SysFreeString(name);
            SysFreeString(class_name);
            SysFreeString(framework);
            if (type == UIA_DocumentControlTypeId && sample == samples - 1) {
                IUIAutomationCondition *all = nullptr;
                IUIAutomationElementArray *found = nullptr;
                automation->CreateTrueCondition(&all);
                if (all) element->FindAll(TreeScope_Descendants, all, &found);
                int length = 0;
                if (found) found->get_Length(&length);
                std::printf("  document.descendants=%d\n", length);
                for (int index = 0; found && index < length && index < 8; ++index) {
                    IUIAutomationElement *child = nullptr;
                    found->GetElement(index, &child);
                    CONTROLTYPEID child_type = 0;
                    BOOL child_focus = FALSE;
                    if (child) {
                        child->get_CurrentControlType(&child_type);
                        child->get_CurrentHasKeyboardFocus(&child_focus);
                        child->Release();
                    }
                    std::printf("  descendant[%d] type=%d focused=%d\n", index, child_type, child_focus);
                }
                if (found) found->Release();
                if (all) all->Release();
            }
            IUIAutomationTreeWalker *walker = nullptr;
            automation->get_ControlViewWalker(&walker);
            IUIAutomationElement *ancestor = nullptr;
            if (walker) walker->GetParentElement(element, &ancestor);
            for (int depth = 1; ancestor && depth <= 6; ++depth) {
                CONTROLTYPEID ancestor_type = 0;
                BSTR ancestor_name = nullptr, ancestor_class = nullptr;
                ancestor->get_CurrentControlType(&ancestor_type);
                ancestor->get_CurrentName(&ancestor_name);
                ancestor->get_CurrentClassName(&ancestor_class);
                std::printf("  parent[%d] type=%d class=%ls name=%.40ls\n", depth, ancestor_type,
                    ancestor_class ? ancestor_class : L"", ancestor_name ? ancestor_name : L"");
                SysFreeString(ancestor_name);
                SysFreeString(ancestor_class);
                IUIAutomationElement *next = nullptr;
                walker->GetParentElement(ancestor, &next);
                ancestor->Release();
                ancestor = next;
            }
            if (ancestor) ancestor->Release();
            if (walker) walker->Release();
            element->Release();
        }
        pump_for(250);
    }
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    GetGUIThreadInfo(GetWindowThreadProcessId(target, nullptr), &info);
    wchar_t focus_class[128] = {};
    if (info.hwndFocus) GetClassNameW(info.hwndFocus, focus_class, 128);
    std::printf("win32.focus_class=%ls win32.caret=%d\n", focus_class, info.hwndCaret != nullptr);
    automation->Release();
    return 0;
}

int count_of(const std::wstring &haystack, const std::wstring &needle) {
    int count = 0;
    for (std::size_t at = haystack.find(needle); at != std::wstring::npos; at = haystack.find(needle, at + 1)) ++count;
    return count;
}

bool element_named(IUIAutomationElement *element, const std::wstring &name) {
    BSTR value = nullptr;
    const bool match = SUCCEEDED(element->get_CurrentName(&value)) && value && name == value;
    SysFreeString(value);
    return match;
}

// Test setup only: makes the element with the given accessible name the
// focused element inside the launched window, through UI Automation rather
// than synthesized input. A browser window activated after its page loaded
// may leave focus on the address bar or the document body instead.
bool focus_named_element(HWND target, const std::wstring &name) {
    IUIAutomation *automation = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
            reinterpret_cast<void **>(&automation)))) {
        return false;
    }
    bool focused = false;
    for (int attempt = 0; attempt < 40 && !focused; ++attempt) {
        if (GetForegroundWindow() != target) break;
        IUIAutomationElement *current = nullptr;
        if (SUCCEEDED(automation->GetFocusedElement(&current)) && current) {
            focused = element_named(current, name);
            current->Release();
        }
        if (focused) break;
        IUIAutomationElement *root = nullptr;
        IUIAutomationCondition *condition = nullptr;
        IUIAutomationElement *element = nullptr;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocString(name.c_str());
        if (SUCCEEDED(automation->ElementFromHandle(target, &root)) && root &&
            SUCCEEDED(automation->CreatePropertyCondition(UIA_NamePropertyId, value, &condition)) && condition &&
            SUCCEEDED(root->FindFirst(TreeScope_Descendants, condition, &element)) && element) {
            element->SetFocus();
        }
        VariantClear(&value);
        if (element) element->Release();
        if (condition) condition->Release();
        if (root) root->Release();
        pump_for(250);
    }
    automation->Release();
    std::printf("setup.focused_named_element=%d\n", focused);
    return focused && GetForegroundWindow() == target;
}
// change_focus: after capture, move focus to that named element and expect
// one paste at the new focus. expect_unreadable: a password field may hide
// readback; require successful dispatch without asserting its hidden contents.
int run_insert(HWND target, const std::wstring &focus_name, const std::wstring &change_focus, bool expect_unreadable) {
    ClipboardSnapshot saved;
    if (snapshot_clipboard(nullptr, saved) != ClipboardStatus::ok) {
        std::puts("error=clipboard-snapshot");
        return 1;
    }
    const UINT marker = RegisterClipboardFormatW(L"LemonVoxMatrixSentinel");
    ClipboardSnapshot sentinel;
    const std::wstring sentinel_text = kSentinel;
    const auto *text_bytes = reinterpret_cast<const std::uint8_t *>(sentinel_text.c_str());
    sentinel.items.push_back({CF_UNICODETEXT, ClipboardItem::Kind::global,
        std::vector<std::uint8_t>(text_bytes, text_bytes + (sentinel_text.size() + 1) * sizeof(wchar_t))});
    sentinel.items.push_back({marker, ClipboardItem::Kind::global, {1, 2, 3, 4}});
    restore_clipboard(nullptr, sentinel, nullptr);

    int status = 0;
    {
        UiThreadExecutor executor;
        InsertionOptions options;
        int sends = 0;
        options.before_send = [&] {
            ++sends;
            const bool own = GetForegroundWindow() == target;
            if (!own) std::puts("error=foreground-changed-before-send");
            return own;
        };
        WinInsertion insertion(executor, options);
        TargetResult original, current;
        AdapterError inserted = AdapterError::unavailable;
        int pending = 1;
        insertion.capture_original(1, [&](OperationId, TargetResult result) { original = result; --pending; });
        while (pending) pump_for(10);
        print_target("capture", original);
        if (!change_focus.empty()) {
            if (!focus_named_element(target, change_focus)) {
                std::puts("error=change-focus-failed");
                insertion.shutdown();
                executor.shutdown();
                restore_clipboard(nullptr, saved, nullptr);
                return 1;
            }
        }
        pending = 1;
        insertion.inspect(1, original.target.token, [&](OperationId, TargetResult result) { current = result; --pending; });
        while (pending) pump_for(10);
        print_target("inspect", current);
        const bool try_insert = !change_focus.empty()
            ? original.target.token.value != 0
            : current.error == AdapterError::none && current.target.matches_original && current.target.token.value;
        if (try_insert) {
            pending = 1;
            // An operation token never pins paste to the original field.
            insertion.insert(1, change_focus.empty() ? current.target.token : original.target.token, kText, [&](OperationId, Completion result) {
                inserted = result.error;
                --pending;
            });
            while (pending) pump_for(10);
        }
        std::printf("insert.error=%s sends=%d\n", error_name(inserted), sends);
        insertion.shutdown();
        executor.shutdown();
        pump_for(400);

        const std::wstring expected = utf8_to_wide(kText);
        // Browsers update their accessibility tree asynchronously after a paste.
        const std::wstring readback_name = change_focus.empty() ? focus_name : change_focus;
        std::wstring content = target_text(target, readback_name);
        for (int attempt = 0; attempt < 10 && content.find(expected) == std::wstring::npos; ++attempt) {
            pump_for(200);
            content = target_text(target, readback_name);
        }
        const std::size_t at = content.find(expected);
        const bool newline_after = at != std::wstring::npos && at + expected.size() < content.size() &&
            (content[at + expected.size()] == L'\r' || content[at + expected.size()] == L'\n');
        std::printf("readback.foreground=%d readback.occurrences=%d readback.newline_after=%d readback.length=%zu\n",
            GetForegroundWindow() == target, count_of(content, expected), newline_after, content.size());
        std::wstring clip;
        const bool restored = read_clipboard_text(clip) && clip == sentinel_text && IsClipboardFormatAvailable(marker);
        std::printf("clipboard.restored=%d\n", restored);
        const bool dispatched = inserted == AdapterError::none && sends == 1 &&
            GetForegroundWindow() == target && restored;
        status = dispatched && (expect_unreadable ||
            (count_of(content, expected) == 1 && !newline_after)) ? 0 : 1;
    }
    restore_clipboard(nullptr, saved, nullptr);
    std::printf("result=%s\n", status == 0 ? "pass" : "fail");
    return status;
}

int run_keys(HWND target, const std::wstring &combo) {
    WORD key = 0;
    if (combo == L"ctrl+s") key = 'S';
    else if (combo == L"ctrl+w") key = 'W';
    if (!key) {
        std::puts("error=unsupported-combo");
        return 2;
    }
    if (GetForegroundWindow() != target) {
        std::puts("error=not-foreground");
        return 1;
    }
    std::array<INPUT, 4> inputs{};
    const WORD keys[4] = {VK_CONTROL, key, key, VK_CONTROL};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = keys[i];
        inputs[i].ki.wScan = static_cast<WORD>(MapVirtualKeyW(keys[i], MAPVK_VK_TO_VSC));
        inputs[i].ki.dwFlags = i >= 2 ? KEYEVENTF_KEYUP : 0;
    }
    const UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    std::printf("keys.sent=%u\n", sent);
    return sent == inputs.size() ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    Args args;
    if (argc > 1) args.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--title" && i + 1 < argc) args.title = argv[++i];
        else if (arg == L"--combo" && i + 1 < argc) args.combo = argv[++i];
        else if (arg == L"--no-lock") args.lock = false;
        else if (arg == L"--msaa") args.msaa = true;
        else if (arg == L"--expect-unreadable") args.expect_unreadable = true;
        else if (arg == L"--change-focus" && i + 1 < argc) args.change_focus = argv[++i];
        else if (arg == L"--samples" && i + 1 < argc) args.samples = _wtoi(argv[++i]);
        else if (arg == L"--focus-name" && i + 1 < argc) args.focus_name = argv[++i];
        else if (arg == L"--pids" && i + 1 < argc) {
            std::wstring list = argv[++i];
            for (wchar_t *context = nullptr, *token = wcstok_s(list.data(), L",", &context); token;
                 token = wcstok_s(nullptr, L",", &context)) {
                args.pids.push_back(static_cast<DWORD>(wcstoul(token, nullptr, 10)));
            }
        }
    }
    if ((args.command != L"insert" && args.command != L"keys" && args.command != L"describe") || args.title.empty()) {
        std::fputs("usage: insertion_matrix_probe insert|keys --title <substring> [--pids a,b] [--combo ctrl+s] "
                   "[--no-lock]\n", stderr);
        return 2;
    }
    std::unique_ptr<test::InteractiveLock> lock;
    if (args.lock) lock = std::make_unique<test::InteractiveLock>();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND target = find_target(args);
    if (!target) return 1;
    if (!bring_to_front(target)) {
        std::puts("error=target-not-foreground");
        return 1;
    }
    // Let the target settle its own focus after activation.
    pump_for(1000);
    if (GetForegroundWindow() != target) {
        std::puts("error=target-lost-foreground");
        return 1;
    }
    if (!args.focus_name.empty() && !focus_named_element(target, args.focus_name)) {
        std::puts("error=focus-name-not-found");
        run_describe(target, 2);
        return 1;
    }
    const int status = args.command == L"insert" ? run_insert(target, args.focus_name, args.change_focus, args.expect_unreadable)
        : args.command == L"describe"          ? run_describe(target, args.samples, args.msaa)
                                               : run_keys(target, args.combo);
    CoUninitialize();
    return status;
}
