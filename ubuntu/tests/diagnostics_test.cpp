#include "diagnostics.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <cassert>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
using namespace lemonvox;
int main() {
    auto *dir = g_dir_make_tmp("lemonvox-log-XXXXXX", nullptr);
    std::string path = std::string(dir) + "/events.log";
    ubuntu::PrivateDiagnostics log(path);
    log.record({DiagnosticEvent::recording_started, AdapterError::none, 0});
    log.record_cleanup({});
    assert(!g_file_test(path.c_str(), G_FILE_TEST_EXISTS));
    log.record({DiagnosticEvent::dictation_failed, AdapterError::network, 503});
    struct stat info{}; assert(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    log.set_debug(true);
    CleanupDiagnostic metrics;
    metrics.elapsed_ms = 123.5; metrics.completion_tokens = 42; metrics.has_completion_tokens = true;
    log.record_cleanup(metrics);
    {
        std::ifstream input(path);
        const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        assert(content.find("cleanup total_ms=123.500000") != std::string::npos);
        assert(content.find("completion_tokens=42") != std::string::npos);
        assert(content.find("reasoning_tokens=unknown") != std::string::npos);
        assert(content.find("residency=unknown fidelity=unverified") != std::string::npos);
    }
    for (int i=0; i<18000; ++i) log.record({DiagnosticEvent::recording_started, AdapterError::none, 0});
    assert(stat(path.c_str(), &info) == 0 && info.st_size <= 262200);
    assert(g_file_test((path + ".1").c_str(), G_FILE_TEST_EXISTS));
    std::ifstream f(path); std::string line; std::getline(f, line);
    assert(line.find("event=0 error=0 http=0") != std::string::npos);
    g_remove(path.c_str()); g_remove((path+".1").c_str()); g_rmdir(dir); g_free(dir);
}
