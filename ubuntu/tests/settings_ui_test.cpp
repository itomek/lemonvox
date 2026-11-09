#include "settings.h"
#include <gsk/gsk.h>
#include <cassert>
#include <iostream>
#include <memory>
#include <string>
namespace {
struct Test {
    GtkApplication *app;
    std::unique_ptr<lemonvox::ubuntu::SettingsWindow> window;
    std::string output, page;
    int result = 1;
};
gboolean snapshot(gpointer data) {
    auto &test = *static_cast<Test *>(data);
    auto *windows = gtk_application_get_windows(test.app);
    assert(windows && windows->data);
    auto *window = GTK_WINDOW(windows->data);
    assert(std::string(gtk_window_get_title(window)) == "LemonVox Settings");
    auto *widget = GTK_WIDGET(window);
    const int width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
    assert(width >= 600 && height >= 600);
    auto *paintable = gtk_widget_paintable_new(widget);
    auto *drawing = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(drawing), width, height);
    auto *node = gtk_snapshot_free_to_node(drawing);
    assert(node);
    auto *surface = gtk_native_get_surface(GTK_NATIVE(window));
    auto *renderer = gsk_renderer_new_for_surface(surface);
    auto *texture = gsk_renderer_render_texture(renderer, node, nullptr);
    assert(texture);
    if (!test.output.empty()) assert(gdk_texture_save_to_png(texture, test.output.c_str()));
    g_object_unref(texture); gsk_render_node_unref(node); g_object_unref(paintable);
    gsk_renderer_unrealize(renderer); g_object_unref(renderer);
    test.result = 0; test.window.reset();
    g_application_quit(G_APPLICATION(test.app)); return G_SOURCE_REMOVE;
}
}
int main(int argc, char **argv) {
    Test test;
    test.output = argc > 1 ? argv[1] : "";
    test.page = argc > 3 ? argv[3] : "General";
    lemonvox::ubuntu::Settings settings;
    settings.appearance = argc > 2 ? argv[2] : "auto";
    test.app = GTK_APPLICATION(adw_application_new("com.itomek.LemonVox.SettingsTest", G_APPLICATION_NON_UNIQUE));
    struct Start { Test *test; lemonvox::ubuntu::Settings settings; } start{&test, settings};
    g_signal_connect(test.app, "activate", G_CALLBACK(+[](GtkApplication *app, gpointer data) {
        auto &start = *static_cast<Start *>(data); auto &test = *start.test;
        lemonvox::ubuntu::apply_appearance(start.settings);
        test.window = std::make_unique<lemonvox::ubuntu::SettingsWindow>(app, start.settings,
            [](const auto &, std::string &) { return true; }, [](const auto &) {}, [] {}, [](const auto &) {});
        test.window->present();
        auto *windows = gtk_application_get_windows(app);
        assert(windows);
        adw_preferences_window_set_visible_page_name(ADW_PREFERENCES_WINDOW(windows->data), test.page.c_str());
        g_timeout_add(500, snapshot, &test);
    }), &start);
    // Screenshot filenames and appearance arguments are test-driver parameters,
    // not GApplication command-line options.
    g_application_run(G_APPLICATION(test.app), 1, argv);
    g_object_unref(test.app);
    if (test.result == 0) std::cout << "PASS: native Settings window rendered at supported default size\n";
    return test.result;
}
