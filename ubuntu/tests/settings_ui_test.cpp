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
    LVServerHealthStatus health_status = LV_SERVER_HEALTH_CHECKING;
    int result = 1;
    int saves = 0;
    int snapshot_attempts = 0;
    bool reject_write = false;
    bool show_error = false;
    std::string appearance;
    lemonvox::ubuntu::Settings saved;
};
bool has_label(GtkWidget *widget, const std::string &text) {
    if (GTK_IS_LABEL(widget) && std::string(gtk_label_get_text(GTK_LABEL(widget))) == text) return true;
    for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        if (has_label(child, text)) return true;
    return false;
}
GtkWidget *find_row(GtkWidget *widget, const char *title) {
    if (ADW_IS_PREFERENCES_ROW(widget) && std::string(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget))) == title) return widget;
    for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        if (auto *found = find_row(child, title)) return found;
    return nullptr;
}
GtkWidget *find_type(GtkWidget *widget, GType type) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type)) return widget;
    for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        if (auto *found = find_type(child, type)) return found;
    return nullptr;
}
gboolean snapshot(gpointer data) {
    auto &test = *static_cast<Test *>(data);
    auto *windows = gtk_application_get_windows(test.app);
    assert(windows && windows->data);
    auto *window = GTK_WINDOW(windows->data);
    assert(std::string(gtk_window_get_title(window)) == "LemonVox Settings");
    auto *widget = GTK_WIDGET(window);
    const auto expected = test.health_status == LV_SERVER_HEALTH_CONNECTED ? "Server API connected" :
        test.health_status == LV_SERVER_HEALTH_DISCONNECTED ? "Server API unavailable" : "Checking server API…";
    assert(has_label(widget, expected));
    assert(!has_label(widget, "Server required"));
    assert(!has_label(widget, "Save settings"));
    if (test.show_error)
        assert(has_label(widget, "API endpoint: Use an HTTP/HTTPS endpoint ending in /v1, without credentials, query or fragment."));
    auto *style = adw_style_manager_get_default();
    if (test.appearance != "auto")
        assert(static_cast<bool>(adw_style_manager_get_dark(style)) == (test.appearance == "dark"));
    assert(std::string(adw_preferences_window_get_visible_page_name(ADW_PREFERENCES_WINDOW(window))) == test.page);
    assert(has_label(widget, "Single tap modifier unavailable"));
    const int width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
    assert(width >= 600 && height >= 600);
    auto *paintable = gtk_widget_paintable_new(widget);
    auto *drawing = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(drawing), width, height);
    auto *node = gtk_snapshot_free_to_node(drawing);
    // Theme/page transitions can briefly leave the paintable without a frame.
    // Retry on the timer instead of treating that compositor timing as a failure.
    if (!node) {
        g_object_unref(paintable);
        assert(++test.snapshot_attempts < 5);
        gtk_widget_queue_draw(widget);
        return G_SOURCE_CONTINUE;
    }
    auto *surface = gtk_native_get_surface(GTK_NATIVE(window));
    auto *renderer = gsk_renderer_new_for_surface(surface);
    auto *texture = gsk_renderer_render_texture(renderer, node, nullptr);
    assert(texture);
    if (!test.output.empty()) assert(gdk_texture_save_to_png(texture, test.output.c_str()));
    g_object_unref(texture); gsk_render_node_unref(node); g_object_unref(paintable);
    gsk_renderer_unrealize(renderer); g_object_unref(renderer);
    // Close flushes the latest text without waiting for its debounce timer.
    auto *prompt = GTK_TEXT_VIEW(find_type(widget, GTK_TYPE_TEXT_VIEW));
    assert(prompt);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(prompt), "Keep 42 invoices.", -1);
    gtk_window_close(window);
    assert(test.saved.cleanup_prompt == "Keep 42 invoices.");
    test.result = 0; test.window.reset();
    g_application_quit(G_APPLICATION(test.app)); return G_SOURCE_REMOVE;
}
}
int main(int argc, char **argv) {
    Test test;
    test.output = argc > 1 ? argv[1] : "";
    test.page = argc > 3 ? argv[3] : "General";
    test.show_error = argc > 5 && std::string(argv[5]) == "errors";
    if (argc > 4 && std::string(argv[4]) == "connected") test.health_status = LV_SERVER_HEALTH_CONNECTED;
    if (argc > 4 && std::string(argv[4]) == "disconnected") test.health_status = LV_SERVER_HEALTH_DISCONNECTED;
    lemonvox::ubuntu::Settings settings;
    settings.appearance = argc > 2 ? argv[2] : "auto";
    test.appearance = settings.appearance;
    test.app = GTK_APPLICATION(adw_application_new("com.itomek.LemonVox.SettingsTest", G_APPLICATION_NON_UNIQUE));
    struct Start { Test *test; lemonvox::ubuntu::Settings settings; } start{&test, settings};
    g_signal_connect(test.app, "activate", G_CALLBACK(+[](GtkApplication *app, gpointer data) {
        auto &start = *static_cast<Start *>(data); auto &test = *start.test;
        lemonvox::ubuntu::apply_appearance(start.settings);
        test.window = std::make_unique<lemonvox::ubuntu::SettingsWindow>(app, start.settings,
            [&test](const auto &value, std::string &error) {
                if (test.reject_write) { error = "Simulated persistence failure"; return false; }
                test.saved = value; ++test.saves;
                lemonvox::ubuntu::apply_appearance(value); return true;
            }, [](const auto &) {}, [] {}, [](const auto &) {});
        test.window->health(test.health_status);
        test.window->present();
        auto *windows = gtk_application_get_windows(app);
        assert(windows);
        auto *widget = GTK_WIDGET(windows->data);
        assert(test.saves == 0); // Initialization and model population never write.
        test.window->models({}, "No models");
        assert(test.saves == 0);
        auto *endpoint = find_row(widget, "API endpoint");
        auto *appearance = find_row(widget, "Theme");
        auto *debug_row = find_row(widget, "Debug traces");
        assert(endpoint && appearance && debug_row);
        auto *debug = GTK_SWITCH(find_type(debug_row, GTK_TYPE_SWITCH));
        assert(debug);
        gtk_editable_set_text(GTK_EDITABLE(endpoint), "invalid endpoint");
        gtk_switch_set_active(debug, TRUE);
        assert(test.saved.debug && test.saved.endpoint == start.settings.endpoint);
        test.reject_write = true;
        adw_combo_row_set_selected(ADW_COMBO_ROW(appearance), 1);
        assert(test.saved.appearance == start.settings.appearance);
        test.reject_write = false;
        adw_combo_row_set_selected(ADW_COMBO_ROW(appearance), 2);
        assert(test.saved.appearance == "dark");
        adw_combo_row_set_selected(ADW_COMBO_ROW(appearance),
            start.settings.appearance == "dark" ? 2 : start.settings.appearance == "light" ? 1 : 0);
        assert(test.saved.appearance == start.settings.appearance);
        gtk_editable_set_text(GTK_EDITABLE(endpoint), test.show_error ? "invalid endpoint" : start.settings.endpoint.c_str());
        adw_preferences_window_set_visible_page_name(ADW_PREFERENCES_WINDOW(windows->data), test.page.c_str());
        g_timeout_add(1000, snapshot, &test);
    }), &start);
    // Screenshot filenames and appearance arguments are test-driver parameters,
    // not GApplication command-line options.
    g_application_run(G_APPLICATION(test.app), 1, argv);
    g_object_unref(test.app);
    if (test.result == 0) std::cout << "PASS: native Settings window rendered at supported default size\n";
    return test.result;
}
