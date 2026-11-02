#include "common/executor.h"
#include "common/trigger_binding.h"
#include "common/utf.h"

#include <windows.h>

#include <cassert>
#include <thread>
#include <vector>

using lemonvox::win::UiThreadExecutor;

static void pump_until(const bool &done) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    MSG message;
    while (!done && GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!done) MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

static void executor_runs_in_order_on_owner_thread() {
    UiThreadExecutor executor;
    std::vector<int> order;
    bool done = false;
    const DWORD owner = GetCurrentThreadId();
    std::thread worker([&] {
        for (int value = 0; value < 100; ++value) {
            assert(executor.post([&, value] {
                assert(GetCurrentThreadId() == owner);
                order.push_back(value);
                if (value == 99) done = true;
            }));
        }
    });
    worker.join();
    pump_until(done);
    assert(done);
    for (int value = 0; value < 100; ++value) assert(order[static_cast<size_t>(value)] == value);
}

static void executor_discards_after_shutdown() {
    UiThreadExecutor executor;
    bool ran = false;
    assert(executor.post([&] { ran = true; }));
    executor.shutdown();
    assert(!executor.post([&] { ran = true; }));
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    assert(!ran);
}

static void utf_round_trips_and_rejects_invalid() {
    using namespace lemonvox::win;
    const std::string text = "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 \xF0\x9F\x8D\x8B";
    bool ok = false;
    const std::wstring wide = utf8_to_wide(text, &ok);
    assert(ok && !wide.empty());
    assert(wide_to_utf8(wide, &ok) == text && ok);
    assert(utf8_to_wide("\xC3\x28", &ok).empty() && !ok);
    const wchar_t lone[] = {0xD800, 0};
    assert(wide_to_utf8(lone, &ok).empty() && !ok);
}

static void trigger_bindings_round_trip_and_reject_unsafe_values() {
    using namespace lemonvox::win;
    const TriggerBinding fallback = default_trigger();
    assert(fallback.kind == TriggerKind::double_tap && fallback.tap_key == LV_KEY_OPTION);
    assert(serialize_trigger(fallback) == "double:alt:any");
    assert(describe_trigger(fallback) == L"Alt twice");

    TriggerBinding parsed;
    assert(parse_trigger("double:ctrl:right", parsed));
    assert(parsed.tap_key == LV_KEY_CONTROL && parsed.tap_side == LV_KEY_SIDE_RIGHT);
    assert(describe_trigger(parsed) == L"Right Ctrl twice");
    assert(serialize_trigger(parsed) == "double:ctrl:right");

    assert(parse_trigger("combo:ctrl+shift:0x20", parsed));
    assert(parsed.kind == TriggerKind::combination && parsed.modifiers == (MOD_CONTROL | MOD_SHIFT));
    assert(parsed.virtual_key == VK_SPACE);
    assert(serialize_trigger(parsed) == "combo:ctrl+shift:0x20");
    assert(describe_trigger(parsed) == L"Ctrl+Shift+Space");
    TriggerBinding again;
    assert(parse_trigger(serialize_trigger(parsed), again) && again == parsed);

    const TriggerBinding before = parsed;
    const char *rejected[] = {
        "", "double:", "double:alt", "double:altgr:any", "double:alt:middle",
        "combo:shift:0x41",       // Shift alone captures typing.
        "combo:ctrl:0x12",        // A modifier is not a key.
        "combo:ctrl+ctrl:0x41", "combo::0x41", "combo:ctrl+:0x41", "combo:ctrl:0x4",
        "combo:ctrl:0x4G", "combo:ctrl:0x100", "combo:hyper:0x41", "other:alt:any",
    };
    for (const char *text : rejected) {
        assert(!parse_trigger(text, parsed));
        assert(parsed == before);
    }
}

int main() {
    executor_runs_in_order_on_owner_thread();
    executor_discards_after_shutdown();
    utf_round_trips_and_rejects_invalid();
    trigger_bindings_round_trip_and_reject_unsafe_values();
    return 0;
}
