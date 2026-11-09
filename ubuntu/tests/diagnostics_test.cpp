#include "diagnostics.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <cassert>
#include <fstream>
#include <sys/stat.h>
using namespace lemonvox;
int main() {
    auto *dir = g_dir_make_tmp("lemonvox-log-XXXXXX", nullptr);
    std::string path = std::string(dir) + "/events.log";
    ubuntu::PrivateDiagnostics log(path);
    log.record({DiagnosticEvent::recording_started, AdapterError::none, 0});
    assert(!g_file_test(path.c_str(), G_FILE_TEST_EXISTS));
    log.record({DiagnosticEvent::dictation_failed, AdapterError::network, 503});
    struct stat info{}; assert(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    log.set_debug(true);
    for (int i=0; i<18000; ++i) log.record({DiagnosticEvent::recording_started, AdapterError::none, 0});
    assert(stat(path.c_str(), &info) == 0 && info.st_size <= 262200);
    assert(g_file_test((path + ".1").c_str(), G_FILE_TEST_EXISTS));
    std::ifstream f(path); std::string line; std::getline(f, line);
    assert(line.find("event=0 error=0 http=0") != std::string::npos);
    g_remove(path.c_str()); g_remove((path+".1").c_str()); g_rmdir(dir); g_free(dir);
}
