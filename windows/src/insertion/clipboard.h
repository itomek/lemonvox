#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lemonvox::win {

// One clipboard format captured as owned bytes. Every captured kind can be
// re-materialized without the original owner: HGLOBAL formats are copied
// verbatim and CF_ENHMETAFILE is stored as its metafile bits.
struct ClipboardItem {
    enum class Kind : std::uint8_t { global, enhanced_metafile };
    UINT format = 0;
    Kind kind = Kind::global;
    std::vector<std::uint8_t> bytes;
};

struct ClipboardSnapshot {
    std::vector<ClipboardItem> items;
    // GetClipboardSequenceNumber() observed while the snapshot was taken.
    DWORD sequence = 0;
    bool contains(UINT format) const;
};

enum class ClipboardStatus : std::uint8_t {
    ok,
    // Another process kept the clipboard open for the whole retry window.
    busy,
    // The content is larger than the snapshot limit or a native call failed.
    failed,
};

// Snapshot limit. A clipboard that cannot be captured completely is never
// overwritten, so the caller falls back to manual copy instead.
inline constexpr std::size_t kClipboardSnapshotLimit = 64u * 1024u * 1024u;

// Registered formats that ask clipboard history, cloud sync and clipboard
// monitors to ignore the content (see "Clipboard formats" in the Win32 docs).
UINT clipboard_exclude_monitor_format();
UINT clipboard_history_format();
UINT clipboard_cloud_format();

// Whether a format is ever captured by snapshot_clipboard. Exposed for tests.
// Owner-display, display-only, private (CF_PRIVATEFIRST..LAST), GDI object,
// bitmap, palette and metafile-picture handles and OLE formats that reference
// live objects are skipped. snapshot_clipboard additionally skips text and
// DIB formats the system synthesizes from an already captured sibling.
bool clipboard_format_is_capturable(UINT format);

// Captures every format that can be safely re-materialized. Requesting a
// delayed-rendered format asks its owner to render it; skipped formats are
// never requested. owner may be null for reading.
ClipboardStatus snapshot_clipboard(HWND owner, ClipboardSnapshot &snapshot);

// Replaces the clipboard with CF_UNICODETEXT plus the history, cloud and
// monitor exclusion formats. owner should be a window on the calling thread;
// pass null to use a temporary message-only window. On success *sequence
// receives the clipboard sequence number after the write.
ClipboardStatus write_private_text(HWND owner, std::wstring_view text, DWORD *sequence);

// Restores a snapshot. The exclusion formats are added (unless the snapshot
// already carries its own) so the restored copy does not create a duplicate
// clipboard-history or cloud entry for content the person already copied.
// An empty snapshot restores an empty clipboard.
ClipboardStatus restore_clipboard(HWND owner, const ClipboardSnapshot &snapshot, DWORD *sequence);

// Manual "Copy" fallback: places UTF-8 text on the clipboard as
// CF_UNICODETEXT with the history, cloud and monitor exclusion formats.
// Callable from any thread that may create a window. Returns false for
// invalid UTF-8 or when the clipboard could not be written.
bool copy_text_to_clipboard(const std::string &utf8);

// Reads CF_UNICODETEXT; returns false when it is absent or unreadable.
bool read_clipboard_text(std::wstring &text);

} // namespace lemonvox::win
