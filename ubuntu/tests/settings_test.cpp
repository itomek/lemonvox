#include "settings.h"
#include <cassert>
#include <sys/stat.h>
#include <glib/gstdio.h>
#include <iostream>
using namespace lemonvox::ubuntu;
int main() {
    auto *dir = g_dir_make_tmp("lemonvox-settings-XXXXXX", nullptr);
    assert(dir);
    const std::string path = std::string(dir) + "/settings.ini";
    SettingsStore store(path);
    auto original = store.load();
    assert(original.appearance == "auto");
    assert(SettingsStore::valid_shortcuts(original));
    auto invalid = original;
    invalid.cancel_shortcut = "<Alt><Control>space";
    assert(!SettingsStore::valid_shortcuts(invalid));
    invalid = original;
    invalid.toggle_shortcut = "space";
    assert(!SettingsStore::valid_shortcuts(invalid));
    std::string error;
    assert(!store.save(invalid, error));
    auto custom = original;
    custom.cleanup_prompt = "Preserve 14 invoices.\nAnd names.";
    custom.asr_model = "asr";
    custom.cleanup_model = "cleanup";
    custom.appearance = "dark";
    assert(store.save(custom, error));
    auto restored = store.load();
    assert(restored.cleanup_prompt == custom.cleanup_prompt);
    assert(restored.asr_model == "asr" && restored.cleanup_model == "cleanup");
    assert(restored.appearance == "dark");
    assert(original.asr_model.empty()); // active copied snapshot unchanged
    struct stat info {};
    assert(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    assert(g_remove(path.c_str()) == 0);
    assert(g_rmdir(dir) == 0);
    g_free(dir);
    std::cout << "PASS: preferences roundtrip, private storage, shortcut conflicts, immutable copies\n";
}
