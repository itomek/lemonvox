#include "ui/settings_layout.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace lemonvox::win;
using namespace lemonvox::win::settings;

#define CHECK(condition)                                                                         \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            std::exit(1);                                                                        \
        }                                                                                        \
    } while (0)

static std::vector<SettingsModelOption> sample_models() {
    return {
        {"whisper-b", true, false, false},
        {"llama", false, true, false},
        {"whisper-a", true, false, false},
        {"gpt-x", false, true, true},
        {"both", true, true, false},
        {"whisper-a", true, false, false}, // duplicate is listed once
        {"", true, true, false},            // unusable ID is skipped
    };
}

static void model_choices_filter_sort_and_label() {
    const auto asr = build_model_choices(sample_models(), ModelRole::transcription, "whisper-b", true);
    CHECK((asr.ids == std::vector<std::string>{"both", "whisper-a", "whisper-b"}));
    CHECK(asr.selected == 2);
    const auto cleanup = build_model_choices(sample_models(), ModelRole::cleanup, "", true);
    CHECK((cleanup.ids == std::vector<std::string>{"both", "gpt-x", "llama"}));
    CHECK(cleanup.labels[1] == L"gpt-x (remote via server)");
    CHECK(cleanup.labels[2] == L"llama (installed)");
    CHECK(cleanup.selected == -1);
    CHECK(model_label({"m\xC3\xA9", false, true, true}) == L"m\u00E9 (remote via server)");
}

static void model_setup_labels() {
    SettingsModelOption absent{"Whisper-Base", true, false, false, false, false};
    CHECK(model_label(absent) == L"Whisper-Base (not installed)");
    absent.installed = true;
    CHECK(model_label(absent) == L"Whisper-Base (installed)");
    absent.loaded_known = true;
    CHECK(model_label(absent) == L"Whisper-Base (installed; not loaded)");
    absent.loaded = true;
    CHECK(model_label(absent) == L"Whisper-Base (loaded)");
}

static void model_choices_preserve_selection() {
    const auto missing = build_model_choices(sample_models(), ModelRole::cleanup, "gone", true);
    CHECK(missing.ids.back() == "gone");
    CHECK(missing.labels.back() == L"(unavailable) gone");
    CHECK(missing.selected == static_cast<int>(missing.ids.size()) - 1);
    const auto before_load = build_model_choices({}, ModelRole::transcription, "saved", false);
    CHECK(before_load.ids == std::vector<std::string>{"saved"});
    CHECK(before_load.labels == std::vector<std::wstring>{L"saved"});
    CHECK(before_load.selected == 0);
    CHECK(build_model_choices({}, ModelRole::transcription, "", false).ids.empty());
    const auto empty_inventory = build_model_choices({}, ModelRole::transcription, "saved", true);
    CHECK(empty_inventory.labels == std::vector<std::wstring>{L"(unavailable) saved"});
}

static void device_choices() {
    const std::vector<SettingsDevice> devices{{L"{a}", L"USB mic"}, {L"{b}", L""}};
    auto choices = build_device_choices(devices, L"");
    CHECK(choices.labels == (std::vector<std::wstring>{L"System default", L"USB mic", L"{b}"}));
    CHECK(choices.selected == 0);
    choices = build_device_choices(devices, L"{b}");
    CHECK(choices.selected == 2);
    choices = build_device_choices(devices, L"{gone}");
    CHECK(choices.selected == 3 && choices.ids[3] == L"{gone}");
    CHECK(choices.labels[3] == L"(unavailable) saved microphone");
}

static TriggerBinding combo(std::uint32_t modifiers, std::uint32_t vk) {
    TriggerBinding binding = default_trigger();
    binding.kind = TriggerKind::combination;
    binding.modifiers = modifiers;
    binding.virtual_key = vk;
    return binding;
}

static void trigger_choices() {
    auto choices = build_trigger_choices(default_trigger());
    CHECK(choices.items.size() == 4);
    CHECK(choices.items[0].label == L"Alt twice");
    CHECK(choices.items[1].label == L"Ctrl twice");
    CHECK(choices.items[2].label == L"Shift twice");
    CHECK(choices.items[3].custom && choices.items[3].label == L"Custom shortcut\u2026");
    CHECK(choices.selected == 0);

    TriggerBinding ctrl = default_trigger();
    ctrl.tap_key = LV_KEY_CONTROL;
    CHECK(build_trigger_choices(ctrl).selected == 1);

    choices = build_trigger_choices(combo(MOD_CONTROL | MOD_SHIFT, VK_SPACE));
    CHECK(choices.items.size() == 5);
    CHECK(choices.selected == 3);
    CHECK(choices.items[3].label == L"Ctrl+Shift+Space");

    TriggerBinding left_alt = default_trigger();
    left_alt.tap_side = LV_KEY_SIDE_LEFT;
    choices = build_trigger_choices(left_alt);
    CHECK(choices.selected == 3 && choices.items[3].label == L"Left Alt twice");

    // An invalid stored binding falls back to the default rather than showing garbage.
    CHECK(build_trigger_choices(combo(MOD_SHIFT, 'K')).selected == 0);
}

static void recorder_logic() {
    CHECK(record_key(VK_ESCAPE, 0).outcome == RecordStep::Outcome::canceled);

    auto step = record_key(VK_CONTROL, MOD_CONTROL);
    CHECK(step.outcome == RecordStep::Outcome::pending);
    CHECK(step.display == L"Ctrl+\u2026");
    step = record_key(VK_SHIFT, MOD_CONTROL | MOD_SHIFT);
    CHECK(step.display == L"Ctrl+Shift+\u2026");
    CHECK(record_key(VK_MENU, 0).display == kRecorderPrompt);

    step = record_key('K', MOD_CONTROL | MOD_SHIFT);
    CHECK(step.outcome == RecordStep::Outcome::captured);
    CHECK(step.binding == combo(MOD_CONTROL | MOD_SHIFT, 'K'));
    CHECK(step.display == L"Ctrl+Shift+K");
    CHECK(trigger_round_trips(step.binding));

    step = record_key(VK_SPACE, MOD_ALT);
    CHECK(step.outcome == RecordStep::Outcome::captured && step.display == L"Alt+Space");
    step = record_key(VK_ESCAPE, MOD_CONTROL);
    CHECK(step.outcome == RecordStep::Outcome::captured);

    CHECK(record_key('K', 0).outcome == RecordStep::Outcome::rejected);
    CHECK(record_key('K', MOD_SHIFT).outcome == RecordStep::Outcome::rejected);
    CHECK(record_key(0x07, MOD_CONTROL).outcome == RecordStep::Outcome::rejected);
    CHECK(record_key(VK_PROCESSKEY, MOD_CONTROL).outcome == RecordStep::Outcome::pending);
}

static void endpoint_validation() {
    CHECK(validate_endpoint(L"http://127.0.0.1:13305/v1").empty());
    CHECK(validate_endpoint(L"  http://localhost:13305/v1/  ").empty());
    CHECK(validate_endpoint(L"https://[::1]:8000/api/v1").empty());
    CHECK(validate_endpoint(L"HTTP://LOCALHOST/v1").empty());
    CHECK(validate_endpoint(L"").find(L"Enter the server endpoint") == 0);
    CHECK(validate_endpoint(L"   ").find(L"Enter the server endpoint") == 0);
    CHECK(validate_endpoint(L"https://inference.example.com/v1").empty());
    CHECK(validate_endpoint(L"http://192.168.1.231:13305/api/v1").empty());
    CHECK(validate_endpoint(L"http://10.0.0.2/v1").empty());
    CHECK(validate_endpoint(L"http://[fd00::2]:13305/api/v1").empty());
    for (const wchar_t *bad : {L"127.0.0.1:13305/v1", L"ftp://127.0.0.1/v1", L"http://127.0.0.1:13305/v2",
             L"http://127.0.0.1:13305", L"http://user@127.0.0.1/v1", L"http://127.0.0.1/v1?x=1",
             L"http://127.0.0.1/v1#frag", L"http://127.0.0.1:abc/v1", L"http://127.0.0.1:99999/v1",
             L"http://127.0.0.1:/v1", L"http://[::1/v1", L"http:///v1", L"http://bad host/v1", L"http://[zz::2]/v1",
             L"http://[:::]/v1", L"http://[1:2:3]/v1"}) {
        CHECK(validate_endpoint(bad).find(L"Enter an endpoint like") == 0);
    }
    CHECK(validate_prompt(L"  \r\n ").find(L"Enter a cleanup prompt") == 0);
    CHECK(validate_prompt(L"Fix it").empty());
}

static void newline_conversion() {
    CHECK(to_edit_newlines(L"a\nb\r\nc\rd") == L"a\r\nb\r\nc\r\nd");
    CHECK(from_edit_newlines(L"a\r\nb\rc\nd") == L"a\nb\nc\nd");
    CHECK(from_edit_newlines(to_edit_newlines(L"x\ny\n")) == L"x\ny\n");
}

static void status_text() {
    const std::wstring endpoint = L"http://127.0.0.1:13305/v1";
    CHECK(loading_status().text == L"Loading models from server\u2026" && !loading_status().error);
    SettingsCatalogResult result;
    result.status = SettingsCatalogResult::Status::network;
    auto status = catalog_status(result, endpoint);
    CHECK(status.error);
    CHECK(status.text == L"Server unavailable at http://127.0.0.1:13305/v1 \u2014 start your server and press Refresh models.");
    result.status = SettingsCatalogResult::Status::http;
    result.message = L"HTTP 500";
    CHECK(catalog_status(result, endpoint).text.find(L"(HTTP 500)") != std::wstring::npos);
    result.status = SettingsCatalogResult::Status::response;
    CHECK(catalog_status(result, endpoint).error);
    result.status = SettingsCatalogResult::Status::configuration;
    CHECK(catalog_status(result, endpoint).text.find(L"HTTP/HTTPS") != std::wstring::npos);

    result.status = SettingsCatalogResult::Status::ok;
    result.message.clear();
    result.models = sample_models();
    status = catalog_status(result, endpoint);
    CHECK(!status.error);
    result.models = {{"a", true, false, false}};
    result.message = L"No cleanup model is downloaded.";
    status = catalog_status(result, endpoint);
    CHECK(status.error && status.text == L"The server offered no cleanup models. No cleanup model is downloaded.");
    result.models.clear();
    CHECK(catalog_status(result, endpoint).text.find(L"no compatible models") != std::wstring::npos);
}

static void server_health_copy() {
    CHECK(server_health_text(LV_SERVER_HEALTH_CHECKING).find(L"Checking server") != std::wstring::npos);
    CHECK(server_health_text(LV_SERVER_HEALTH_CONNECTED).find(L"Server connected") != std::wstring::npos);
    const auto failed = server_health_text(LV_SERVER_HEALTH_DISCONNECTED);
    CHECK(failed.find(L"Could not connect") != std::wstring::npos);
    CHECK(failed.find(L"Lemonade or another compatible OpenAI server") != std::wstring::npos);
}

static bool overlaps(const RECT &a, const RECT &b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

static void layout_scales_without_overlap() {
    for (const int dpi : {96, 120, 144, 192, 288}) {
        const int line = MulDiv(16, dpi, 96);
        const Layout layout = compute_layout(dpi, line, [line](SettingsControl control, int width) {
            if (width == 0) return line * 5; // button text width
            if (control == SettingsControl::endpoint_hint) return 3 * line;
            return control == SettingsControl::routing_note ? 3 * line : line;
        });
        CHECK(layout.client.cx == MulDiv(560, dpi, 96) - 2 * MulDiv(16, dpi, 96) + 2 * MulDiv(16, dpi, 96));
        CHECK(layout.row_height >= line);
        const int count = static_cast<int>(SettingsControl::count);
        for (int i = 0; i < count; ++i) {
            const RECT &rect = layout.rects[i];
            CHECK(rect.right > rect.left && rect.bottom > rect.top);
            CHECK(rect.left >= 0 && rect.top >= 0 && rect.right <= layout.client.cx && rect.bottom <= layout.client.cy);
            for (int j = i + 1; j < count; ++j) CHECK(!overlaps(rect, layout.rects[j]));
        }
        // Every label sits above or beside (never below) the control it names.
        CHECK(layout.rects[static_cast<int>(SettingsControl::asr_label)].bottom
            <= layout.rects[static_cast<int>(SettingsControl::asr_combo)].top);
        if (dpi == 96) CHECK(layout.client.cy < 700);
    }
}

int main() {
    model_choices_filter_sort_and_label();
    model_choices_preserve_selection();
    model_setup_labels();
    device_choices();
    trigger_choices();
    recorder_logic();
    endpoint_validation();
    newline_conversion();
    status_text();
    server_health_copy();
    layout_scales_without_overlap();
    std::puts("settings_unit_test passed");
    return 0;
}
