#include "insertion/clipboard.h"

#include "common/utf.h"

#include <array>
#include <cstring>
#include <utility>

namespace lemonvox::win {

namespace {

constexpr wchar_t kOwnerClass[] = L"LemonVoxClipboardOwner";
constexpr int kOpenAttempts = 50;
constexpr DWORD kOpenRetryMs = 10;

// OLE formats that describe or reference live objects (storages, monikers,
// in-process data objects). Their bytes are meaningless without the source.
constexpr std::array<const wchar_t *, 12> kLiveObjectFormats = {
    L"OLE Private Data", L"Ole Private Data", L"DataObject", L"Object Descriptor",
    L"Link Source Descriptor", L"Embed Source", L"Link Source", L"Embedded Object",
    L"Native", L"OwnerLink", L"ObjectLink", L"Ole Private Data Format",
};

LRESULT CALLBACK owner_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

// Clipboard owners receive sent messages (WM_DESTROYCLIPBOARD, WM_RENDERFORMAT)
// from other processes. Waiting without dispatching them would stall those
// processes while they hold the clipboard open.
void wait_dispatching_sent(DWORD milliseconds) {
    MsgWaitForMultipleObjectsEx(0, nullptr, milliseconds, QS_SENDMESSAGE, 0);
    MSG message;
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
}

class TemporaryOwner {
public:
    explicit TemporaryOwner(HWND owner) : window_(owner) {
        if (window_) return;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = owner_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kOwnerClass;
        RegisterClassExW(&window_class);
        window_ = CreateWindowExW(0, kOwnerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        created_ = window_ != nullptr;
    }
    ~TemporaryOwner() {
        if (created_) DestroyWindow(window_);
    }
    TemporaryOwner(const TemporaryOwner &) = delete;
    TemporaryOwner &operator=(const TemporaryOwner &) = delete;
    HWND get() const { return window_; }

private:
    HWND window_ = nullptr;
    bool created_ = false;
};

class OpenClipboardScope {
public:
    explicit OpenClipboardScope(HWND owner) {
        for (int attempt = 0; attempt < kOpenAttempts; ++attempt) {
            if (OpenClipboard(owner)) {
                open_ = true;
                return;
            }
            wait_dispatching_sent(kOpenRetryMs);
        }
    }
    ~OpenClipboardScope() {
        if (open_) CloseClipboard();
    }
    OpenClipboardScope(const OpenClipboardScope &) = delete;
    OpenClipboardScope &operator=(const OpenClipboardScope &) = delete;
    bool is_open() const { return open_; }
    void close() {
        if (open_) CloseClipboard();
        open_ = false;
    }

private:
    bool open_ = false;
};

bool is_text_family(UINT format) {
    return format == CF_UNICODETEXT || format == CF_TEXT || format == CF_OEMTEXT;
}

bool is_dib_family(UINT format) {
    return format == CF_DIB || format == CF_DIBV5;
}

bool is_live_object_format(UINT format) {
    wchar_t name[256] = {};
    const int length = GetClipboardFormatNameW(format, name, static_cast<int>(std::size(name)));
    if (length <= 0) return false;
    for (const wchar_t *blocked : kLiveObjectFormats) {
        if (_wcsicmp(name, blocked) == 0) return true;
    }
    return false;
}

bool set_global(UINT format, const void *data, std::size_t size) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size == 0 ? 1 : size);
    if (!memory) return false;
    void *target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        return false;
    }
    if (size != 0) std::memcpy(target, data, size);
    GlobalUnlock(memory);
    if (!SetClipboardData(format, memory)) {
        GlobalFree(memory);
        return false;
    }
    return true;
}

bool set_dword(UINT format, DWORD value) {
    return format != 0 && set_global(format, &value, sizeof(value));
}

UINT registered(const wchar_t *name) {
    return RegisterClipboardFormatW(name);
}

// Called with the clipboard open and emptied.
bool set_privacy_formats(const ClipboardSnapshot *keep) {
    bool ok = true;
    const UINT exclude = clipboard_exclude_monitor_format();
    const UINT history = clipboard_history_format();
    const UINT cloud = clipboard_cloud_format();
    if (!keep || !keep->contains(exclude)) ok = set_dword(exclude, 0) && ok;
    if (!keep || !keep->contains(history)) ok = set_dword(history, 0) && ok;
    if (!keep || !keep->contains(cloud)) ok = set_dword(cloud, 0) && ok;
    return ok;
}

} // namespace

bool ClipboardSnapshot::contains(UINT format) const {
    for (const auto &item : items) {
        if (item.format == format) return true;
    }
    return false;
}

UINT clipboard_exclude_monitor_format() {
    return registered(L"ExcludeClipboardContentFromMonitorProcessing");
}

UINT clipboard_history_format() {
    return registered(L"CanIncludeInClipboardHistory");
}

UINT clipboard_cloud_format() {
    return registered(L"CanUploadToCloudClipboard");
}

bool clipboard_format_is_capturable(UINT format) {
    switch (format) {
    case CF_TEXT:
    case CF_OEMTEXT:
    case CF_UNICODETEXT:
    case CF_LOCALE:
    case CF_DIB:
    case CF_DIBV5:
    case CF_HDROP:
    case CF_SYLK:
    case CF_DIF:
    case CF_TIFF:
    case CF_RIFF:
    case CF_WAVE:
    case CF_PENDATA:
    case CF_ENHMETAFILE:
        return true;
    default:
        break;
    }
    // CF_BITMAP is re-synthesized from the captured DIB; CF_METAFILEPICT,
    // CF_PALETTE, owner-display, display-only, private and GDI-object formats
    // hold handles that cannot be copied as bytes.
    if (format < 0xC000 || format > 0xFFFF) return false;
    return !is_live_object_format(format);
}

ClipboardStatus snapshot_clipboard(HWND owner, ClipboardSnapshot &snapshot) {
    snapshot = {};
    OpenClipboardScope clipboard(owner);
    if (!clipboard.is_open()) return ClipboardStatus::busy;
    snapshot.sequence = GetClipboardSequenceNumber();

    std::vector<UINT> formats;
    SetLastError(ERROR_SUCCESS);
    for (UINT format = EnumClipboardFormats(0); format != 0; format = EnumClipboardFormats(format)) {
        formats.push_back(format);
    }
    if (GetLastError() != ERROR_SUCCESS) return ClipboardStatus::failed;

    // EnumClipboardFormats lists formats the owner placed before the formats
    // the system synthesizes, so the first member of each family is original.
    bool have_text = false;
    bool have_dib = false;
    std::size_t total = 0;
    for (UINT format : formats) {
        if (!clipboard_format_is_capturable(format)) continue;
        if (is_text_family(format)) {
            if (have_text) continue;
            have_text = true;
        }
        if (is_dib_family(format)) {
            if (have_dib) continue;
            have_dib = true;
        }
        HANDLE handle = GetClipboardData(format);
        if (!handle) continue;

        ClipboardItem item;
        item.format = format;
        if (format == CF_ENHMETAFILE) {
            const auto metafile = static_cast<HENHMETAFILE>(handle);
            const UINT size = GetEnhMetaFileBits(metafile, 0, nullptr);
            if (size == 0) continue;
            if (total + size > kClipboardSnapshotLimit) return ClipboardStatus::failed;
            item.kind = ClipboardItem::Kind::enhanced_metafile;
            item.bytes.resize(size);
            if (GetEnhMetaFileBits(metafile, size, item.bytes.data()) != size) continue;
        } else {
            const SIZE_T size = GlobalSize(handle);
            if (size == 0) continue;
            if (total + size > kClipboardSnapshotLimit) return ClipboardStatus::failed;
            const void *data = GlobalLock(handle);
            if (!data) continue;
            item.bytes.assign(static_cast<const std::uint8_t *>(data),
                static_cast<const std::uint8_t *>(data) + size);
            GlobalUnlock(handle);
        }
        total += item.bytes.size();
        snapshot.items.push_back(std::move(item));
    }
    return ClipboardStatus::ok;
}

ClipboardStatus write_private_text(HWND owner, std::wstring_view text, DWORD *sequence) {
    TemporaryOwner window(owner);
    if (!window.get()) return ClipboardStatus::failed;
    OpenClipboardScope clipboard(window.get());
    if (!clipboard.is_open()) return ClipboardStatus::busy;
    if (!EmptyClipboard()) return ClipboardStatus::failed;
    std::wstring terminated(text);
    const bool ok = set_global(CF_UNICODETEXT, terminated.c_str(), (terminated.size() + 1) * sizeof(wchar_t)) &&
        set_privacy_formats(nullptr);
    clipboard.close();
    if (sequence) *sequence = GetClipboardSequenceNumber();
    return ok ? ClipboardStatus::ok : ClipboardStatus::failed;
}

ClipboardStatus restore_clipboard(HWND owner, const ClipboardSnapshot &snapshot, DWORD *sequence) {
    TemporaryOwner window(owner);
    if (!window.get()) return ClipboardStatus::failed;
    OpenClipboardScope clipboard(window.get());
    if (!clipboard.is_open()) return ClipboardStatus::busy;
    if (!EmptyClipboard()) return ClipboardStatus::failed;
    bool ok = true;
    for (const auto &item : snapshot.items) {
        if (item.kind == ClipboardItem::Kind::enhanced_metafile) {
            HENHMETAFILE metafile = SetEnhMetaFileBits(static_cast<UINT>(item.bytes.size()), item.bytes.data());
            if (!metafile || !SetClipboardData(item.format, metafile)) {
                if (metafile) DeleteEnhMetaFile(metafile);
                ok = false;
            }
        } else if (!set_global(item.format, item.bytes.data(), item.bytes.size())) {
            ok = false;
        }
    }
    if (!snapshot.items.empty()) ok = set_privacy_formats(&snapshot) && ok;
    clipboard.close();
    if (sequence) *sequence = GetClipboardSequenceNumber();
    return ok ? ClipboardStatus::ok : ClipboardStatus::failed;
}

bool copy_text_to_clipboard(const std::string &utf8) {
    bool valid = false;
    const std::wstring wide = utf8_to_wide(utf8, &valid);
    if (!valid) return false;
    return write_private_text(nullptr, wide, nullptr) == ClipboardStatus::ok;
}

bool read_clipboard_text(std::wstring &text) {
    text.clear();
    OpenClipboardScope clipboard(nullptr);
    if (!clipboard.is_open()) return false;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) return false;
    const auto *data = static_cast<const wchar_t *>(GlobalLock(handle));
    if (!data) return false;
    const SIZE_T capacity = GlobalSize(handle) / sizeof(wchar_t);
    SIZE_T length = 0;
    while (length < capacity && data[length] != L'\0') ++length;
    text.assign(data, length);
    GlobalUnlock(handle);
    return true;
}

} // namespace lemonvox::win
