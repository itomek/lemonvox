#include "insertion/target.h"

#include <ole2.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <oleacc.h>

#include <cwchar>
#include <iterator>
#include <utility>

namespace lemonvox::win {

namespace {

constexpr DWORD kUiaTimeoutMs = 2000;
constexpr int kChromiumTreeAttempts = 3;
constexpr DWORD kChromiumTreeDelayMs = 100;
constexpr int kMsaaFocusHops = 8;
constexpr int kStaleElementRetries = 3;
constexpr DWORD kStaleElementDelayMs = 50;

void wait_dispatching(DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    for (;;) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return;
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

bool is_chromium_placeholder(IUIAutomationElement *element) {
    CONTROLTYPEID type = 0;
    BSTR framework = nullptr;
    bool placeholder = false;
    if (SUCCEEDED(element->get_CurrentControlType(&type)) && type == UIA_PaneControlTypeId &&
        SUCCEEDED(element->get_CurrentFrameworkId(&framework)) && framework) {
        placeholder = wcscmp(framework, L"Chrome") == 0;
    }
    SysFreeString(framework);
    return placeholder;
}

// Follows MSAA accFocus from the window's client object down to the focused
// object. Chrome enables its web accessibility tree for MSAA clients, which
// also makes the field visible to UI Automation. Only called for a Chromium
// placeholder, i.e. a process that just answered UI Automation, because
// WM_GETOBJECT has no timeout. Returns whether the focused object is protected
// (a password field).
bool msaa_focus_protected(HWND window) {
    IAccessible *accessible = nullptr;
    if (FAILED(AccessibleObjectFromWindow(window, static_cast<DWORD>(OBJID_CLIENT), __uuidof(IAccessible),
            reinterpret_cast<void **>(&accessible))) || !accessible) {
        return false;
    }
    bool is_protected = false;
    for (int hop = 0; accessible && hop < kMsaaFocusHops; ++hop) {
        VARIANT self;
        VariantInit(&self);
        self.vt = VT_I4;
        self.lVal = CHILDID_SELF;
        VARIANT state;
        VariantInit(&state);
        if (SUCCEEDED(accessible->get_accState(self, &state)) && state.vt == VT_I4) {
            is_protected = (state.lVal & STATE_SYSTEM_PROTECTED) != 0;
        }
        // Chrome treats role/name queries as assistive-technology use.
        VARIANT role;
        VariantInit(&role);
        accessible->get_accRole(self, &role);
        VariantClear(&role);
        VARIANT focus;
        VariantInit(&focus);
        IAccessible *next = nullptr;
        if (accessible->get_accFocus(&focus) == S_OK && focus.vt == VT_DISPATCH && focus.pdispVal) {
            focus.pdispVal->QueryInterface(__uuidof(IAccessible), reinterpret_cast<void **>(&next));
        }
        VariantClear(&focus);
        accessible->Release();
        accessible = next;
    }
    if (accessible) accessible->Release();
    return is_protected;
}

template <typename T>
void release(T *&pointer) {
    if (pointer) pointer->Release();
    pointer = nullptr;
}

std::uint64_t process_start_time(DWORD process_id) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    std::uint64_t start = 0;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        start = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }
    CloseHandle(process);
    return start;
}

bool variant_bool(IUIAutomationElement *element, PROPERTYID property, bool fallback) {
    VARIANT value;
    VariantInit(&value);
    bool result = fallback;
    if (SUCCEEDED(element->GetCurrentPropertyValue(property, &value)) && value.vt == VT_BOOL) {
        result = value.boolVal != VARIANT_FALSE;
    }
    VariantClear(&value);
    return result;
}

bool text_read_only(IUIAutomationElement *element) {
    IUIAutomationTextPattern *pattern = nullptr;
    if (FAILED(element->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
            reinterpret_cast<void **>(&pattern))) || !pattern) {
        return true;
    }
    IUIAutomationTextRange *range = nullptr;
    bool read_only = true;
    if (SUCCEEDED(pattern->get_DocumentRange(&range)) && range) {
        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(range->GetAttributeValue(UIA_IsReadOnlyAttributeId, &value))) {
            // A mixed attribute (VT_UNKNOWN) means part of the document is editable.
            read_only = value.vt == VT_BOOL && value.boolVal != VARIANT_FALSE;
        }
        VariantClear(&value);
    }
    release(range);
    release(pattern);
    return read_only;
}

std::vector<int> runtime_id(IUIAutomationElement *element) {
    std::vector<int> id;
    SAFEARRAY *array = nullptr;
    if (FAILED(element->GetRuntimeId(&array)) || !array) return id;
    LONG lower = 0, upper = -1;
    SafeArrayGetLBound(array, 1, &lower);
    SafeArrayGetUBound(array, 1, &upper);
    int *data = nullptr;
    if (upper >= lower && SUCCEEDED(SafeArrayAccessData(array, reinterpret_cast<void **>(&data)))) {
        id.assign(data, data + (upper - lower + 1));
        SafeArrayUnaccessData(array);
    }
    SafeArrayDestroy(array);
    return id;
}

struct UiaFacts {
    bool available = false;
    bool editable = false;
    bool secure = false;
    bool stale = false;
};

UiaFacts uia_facts(IUIAutomationElement *element) {
    UiaFacts facts;
    CONTROLTYPEID type = 0;
    const HRESULT type_result = element->get_CurrentControlType(&type);
    if (FAILED(type_result)) {
        facts.stale = type_result == UIA_E_ELEMENTNOTAVAILABLE;
        return facts;
    }
    facts.available = true;
    BOOL password = FALSE;
    if (SUCCEEDED(element->get_CurrentIsPassword(&password))) facts.secure = password != FALSE;

    const bool text_type = type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId;
    const bool value_type = text_type || type == UIA_ComboBoxControlTypeId;
    if (value_type && variant_bool(element, UIA_IsValuePatternAvailablePropertyId, false)) {
        facts.editable = !variant_bool(element, UIA_ValueIsReadOnlyPropertyId, true);
    }
    if (!facts.editable && text_type && variant_bool(element, UIA_IsTextPatternAvailablePropertyId, false)) {
        facts.editable = !text_read_only(element);
    }
    return facts;
}

} // namespace

bool same_target(const TargetIdentity &a, const TargetIdentity &b) {
    return a.foreground == b.foreground && a.process_id == b.process_id &&
        a.process_start == b.process_start && a.focus == b.focus && a.runtime_id == b.runtime_id;
}

Win32FocusFacts win32_focus_facts(HWND focus, bool caret) {
    Win32FocusFacts facts;
    facts.caret = caret;
    if (!focus) return facts;
    wchar_t name[64] = {};
    if (GetClassNameW(focus, name, static_cast<int>(std::size(name))) > 0) {
        facts.edit_class = _wcsicmp(name, L"Edit") == 0 || _wcsnicmp(name, L"RichEdit", 8) == 0;
    }
    if (facts.edit_class) {
        const LONG_PTR style = GetWindowLongPtrW(focus, GWL_STYLE);
        facts.read_only = (style & ES_READONLY) != 0;
        facts.password = (style & ES_PASSWORD) != 0;
    }
    return facts;
}

TargetProbe::TargetProbe(std::function<std::uint64_t()> input_generation)
    : input_generation_(std::move(input_generation)) {
    IUIAutomation *automation = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void **>(&automation)))) {
        automation = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IUIAutomation), reinterpret_cast<void **>(&automation)))) {
            automation = nullptr;
        }
    }
    automation_ = automation;
    IUIAutomation2 *bounded = nullptr;
    if (automation_ && SUCCEEDED(automation_->QueryInterface(__uuidof(IUIAutomation2),
            reinterpret_cast<void **>(&bounded))) && bounded) {
        // A hung target must not stall the worker indefinitely.
        bounded->put_ConnectionTimeout(kUiaTimeoutMs);
        bounded->put_TransactionTimeout(kUiaTimeoutMs);
        bounded->Release();
    }
}

TargetProbe::~TargetProbe() {
    release(automation_);
}

IUIAutomationElement *TargetProbe::focused_element(DWORD process_id, HWND focus, bool &stale) {
    if (!automation_) return nullptr;
    IUIAutomationElement *element = nullptr;
    const HRESULT focused_result = automation_->GetFocusedElement(&element);
    stale = stale || focused_result == UIA_E_ELEMENTNOTAVAILABLE;
    if (SUCCEEDED(focused_result) && element) {
        int owner = 0;
        if (SUCCEEDED(element->get_CurrentProcessId(&owner)) && static_cast<DWORD>(owner) == process_id) {
            return element;
        }
        release(element);
    }
    const HRESULT handle_result = automation_->ElementFromHandle(focus, &element);
    stale = stale || handle_result == UIA_E_ELEMENTNOTAVAILABLE;
    if (SUCCEEDED(handle_result) && element) return element;
    return nullptr;
}

bool TargetProbe::probe(ProbedTarget &target) {
    const std::uint64_t input = input_generation_ ? input_generation_() : 0;
    bool stale = false;
    if (!probe_once(target, stale)) return false;
    const TargetIdentity original = target.identity;
    // A modifier release can briefly invalidate a modern app's UIA element
    // while its menu/focus transition is still being processed. Reacquire it
    // rather than saving a permanently missing original target. Only retry
    // this specific transient error. Notepad can also restore focus from its
    // WinUI input host to the native editor during the transition, so there
    // is no stable focused HWND to preserve yet. Require the same foreground
    // process/window and no new user input before accepting that first snapshot.
    const auto acquisition_unchanged = [&] {
        if (input != 0) {
            if (input_generation_() != input) return false;
        } else {
            // Without complete input observation, never accept a focus handoff.
            return foreground_focus_matches(original);
        }
        if (GetForegroundWindow() != original.foreground) return false;
        DWORD process_id = 0;
        GetWindowThreadProcessId(original.foreground, &process_id);
        return process_id == original.process_id;
    };
    for (int attempt = 0; stale && !target.facts.editable && !target.facts.secure &&
         attempt < kStaleElementRetries; ++attempt) {
        wait_dispatching(kStaleElementDelayMs);
        if (!acquisition_unchanged()) return false;
        stale = false;
        if (!probe_once(target, stale) || !acquisition_unchanged() ||
            target.identity.foreground != original.foreground ||
            target.identity.process_id != original.process_id ||
            target.identity.process_start != original.process_start) return false;
    }
    return true;
}

bool TargetProbe::probe_once(ProbedTarget &target, bool &stale) {
    target = {};
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(foreground, &process_id);
    if (thread_id == 0 || process_id == 0) return false;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    HWND focus = foreground;
    bool caret = false;
    if (GetGUIThreadInfo(thread_id, &info)) {
        if (info.hwndFocus) focus = info.hwndFocus;
        caret = info.hwndCaret != nullptr;
    }

    target.identity.foreground = foreground;
    target.identity.process_id = process_id;
    target.identity.process_start = process_start_time(process_id);
    target.identity.focus = focus;

    const Win32FocusFacts win32 = win32_focus_facts(focus, caret);
    UiaFacts uia;
    bool msaa_protected = false;
    IUIAutomationElement *element = focused_element(process_id, focus, stale);
    // Chromium apps report only a bare contents pane until their web
    // accessibility is on. Edge turns it on for UI Automation clients after a
    // moment; Chrome only for MSAA clients, so follow MSAA accFocus once
    // (which enables it for the life of that browser process) and re-read.
    if (element && is_chromium_placeholder(element)) {
        msaa_protected = msaa_focus_protected(foreground);
        for (int attempt = 0; element && attempt < kChromiumTreeAttempts && is_chromium_placeholder(element);
             ++attempt) {
            element->Release();
            wait_dispatching(kChromiumTreeDelayMs);
            element = focused_element(process_id, focus, stale);
        }
    }
    if (element) {
        target.identity.runtime_id = runtime_id(element);
        uia = uia_facts(element);
        element->Release();
    }
    stale = !uia.available && (stale || uia.stale);
    const bool win32_editable = win32.edit_class && !win32.read_only;
    target.facts.editable = uia.available ? (uia.editable || win32_editable) : (win32_editable || win32.caret);
    // MSAA only ever adds a secure signal; UI Automation stays the source of
    // editability and identity.
    target.facts.secure = uia.secure || win32.password || msaa_protected;
    return true;
}

bool TargetProbe::foreground_focus_matches(const TargetIdentity &identity) {
    HWND foreground = GetForegroundWindow();
    if (foreground != identity.foreground) return false;
    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(foreground, &process_id);
    if (process_id != identity.process_id) return false;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(thread_id, &info)) return false;
    HWND focus = info.hwndFocus ? info.hwndFocus : foreground;
    return focus == identity.focus;
}

std::uint64_t TargetTable::allocate(OperationId operation, const TargetIdentity &identity) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t token = next_++;
    if (next_ == 0) next_ = 1;
    entries_[token] = Entry{operation, identity};
    return token;
}

bool TargetTable::lookup(std::uint64_t token, OperationId operation, TargetIdentity &identity) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(token);
    if (found == entries_.end() || found->second.operation != operation) return false;
    identity = found->second.identity;
    return true;
}

void TargetTable::invalidate(OperationId operation) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        entry = entry->second.operation == operation ? entries_.erase(entry) : std::next(entry);
    }
}

void TargetTable::retain_only(OperationId operation) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        entry = entry->second.operation != operation ? entries_.erase(entry) : std::next(entry);
    }
}

void TargetTable::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

std::size_t TargetTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

} // namespace lemonvox::win
