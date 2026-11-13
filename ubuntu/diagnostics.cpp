#include "diagnostics.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <utility>
namespace lemonvox::ubuntu {
PrivateDiagnostics::PrivateDiagnostics(std::string path) : path_(std::move(path)) {
    if (path_.empty()) path_ = std::string(g_get_user_state_dir()) + "/lemonvox/events.log";
}
void PrivateDiagnostics::record(DiagnosticEntry e) {
    if (!debug_ && e.event != DiagnosticEvent::dictation_failed && e.event != DiagnosticEvent::transport_failed && e.event != DiagnosticEvent::insertion_fallback) return;
    write_line(std::string("event=") + std::to_string(static_cast<int>(e.event)) +
        " error=" + std::to_string(static_cast<int>(e.error)) + " http=" + std::to_string(e.http_status) + "\n");
}
void PrivateDiagnostics::record_cleanup(CleanupDiagnostic e) {
    if (!debug_) return;
    const auto count = [](bool present, std::uint64_t value) { return present ? std::to_string(value) : "unknown"; };
    write_line("cleanup total_ms=" + std::to_string(e.elapsed_ms) +
        " error=" + std::to_string(static_cast<int>(e.error)) + " http=" + std::to_string(e.http_status) +
        " response_valid=" + std::to_string(e.response_valid) + " finish=" + std::to_string(e.finish_reason) +
        " prompt_tokens=" + count(e.has_prompt_tokens, e.prompt_tokens) +
        " completion_tokens=" + count(e.has_completion_tokens, e.completion_tokens) +
        " reasoning_tokens=" + count(e.has_reasoning_tokens, e.reasoning_tokens) +
        " reasoning_characters=" + count(e.has_reasoning_characters, e.reasoning_characters) +
        " residency=unknown fidelity=unverified\n");
}
void PrivateDiagnostics::write_line(const std::string &line) {
    auto *dir = g_path_get_dirname(path_.c_str());
    const bool made = g_mkdir_with_parents(dir, 0700) == 0; g_free(dir);
    if (!made) return;
    struct stat info {};
    if (lstat(path_.c_str(), &info) == 0) {
        if (!S_ISREG(info.st_mode)) return;
        if (info.st_size >= 256 * 1024 && g_rename(path_.c_str(), (path_ + ".1").c_str()) != 0) return;
    }
    int fd = open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    if (fchmod(fd, 0600) == 0) {
        // Only private fixed-shape numeric records reach this writer.
        const auto ignored = write(fd, line.data(), line.size()); (void)ignored;
    }
    close(fd);
}
}
