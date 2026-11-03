// Full-application end-to-end check on the real desktop: launches LemonVox.exe
// with an isolated settings profile and a synthesized speech WAV (Lemonade TTS)
// in place of the microphone, presses Alt twice to start and stop through the
// real global hook, and verifies that cleaned text lands once in this test's
// own multiline EDIT, with no Enter, focus kept and the clipboard restored.
// Exits 77 (skipped) when Lemonade, its models or a free app instance are unavailable.
#include "app/file_capture.h"
#include "common/executor.h"
#include "insertion/clipboard.h"
#include "net/endpoint_facts.h"
#include "net/winhttp_transport.h"
#include "prefs/preferences.h"
#include "support/interactive_lock.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace lemonvox;
using namespace lemonvox::win;

namespace {

HWND g_window = nullptr;
HWND g_edit = nullptr;

#define REQUIRE(condition)                                                                 \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            return false;                                                                  \
        }                                                                                  \
    } while (false)

void pump_for(DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    MSG message;
    do {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (GetTickCount64() < deadline);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SIZE && g_edit) MoveWindow(g_edit, 8, 8, LOWORD(lparam) - 16, HIWORD(lparam) - 16, TRUE);
    return DefWindowProcW(window, message, wparam, lparam);
}

bool foreground_is_ours() {
    return GetForegroundWindow() == g_window;
}

bool bring_to_foreground() {
    ShowWindow(g_window, SW_SHOWNORMAL);
    for (int attempt = 0; attempt < 30 && !foreground_is_ours(); ++attempt) {
        const HWND current = GetForegroundWindow();
        const DWORD current_thread = current ? GetWindowThreadProcessId(current, nullptr) : 0;
        const DWORD self = GetCurrentThreadId();
        const bool attached = current_thread != 0 && current_thread != self
            && AttachThreadInput(self, current_thread, TRUE);
        if (attempt % 2 == 1) {
            ShowWindow(g_window, SW_MINIMIZE);
            ShowWindow(g_window, SW_RESTORE);
        }
        BringWindowToTop(g_window);
        SetForegroundWindow(g_window);
        if (attached) AttachThreadInput(self, current_thread, FALSE);
        pump_for(100);
    }
    SetFocus(g_edit);
    pump_for(100);
    return foreground_is_ours() && GetFocus() == g_edit;
}

bool double_alt() {
    if (!foreground_is_ours()) {
        std::fprintf(stderr, "test window lost the foreground; refusing to inject\n");
        return false;
    }
    INPUT inputs[4] = {};
    const UINT scan = MapVirtualKeyW(VK_LMENU, MAPVK_VK_TO_VSC);
    for (int index = 0; index < 4; ++index) {
        inputs[index].type = INPUT_KEYBOARD;
        inputs[index].ki.wVk = VK_LMENU;
        inputs[index].ki.wScan = static_cast<WORD>(scan);
        inputs[index].ki.dwFlags = index % 2 ? KEYEVENTF_KEYUP : 0;
    }
    return SendInput(4, inputs, sizeof(INPUT)) == 4;
}

std::wstring edit_text() {
    const int length = GetWindowTextLengthW(g_edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(g_edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

TransportResponse send_now(WinHttpTransport &transport, OperationId id, TransportRequest request) {
    TransportResponse result;
    bool done = false;
    transport.send(id, std::move(request), [&](OperationId, TransportResponse response) {
        result = std::move(response);
        done = true;
    });
    const ULONGLONG deadline = GetTickCount64() + 180000;
    while (!done && GetTickCount64() < deadline) pump_for(20);
    if (!done) result.error = AdapterError::network;
    return result;
}

std::string pick_cleanup(const std::string &inventory) {
    for (const char *candidate : {"Gemma-4-E4B-it-GGUF", "Qwen3-0.6B-GGUF", "Qwen3-8B-GGUF"}) {
        if (inventory.find(std::string("\"") + candidate + "\"") != std::string::npos) return candidate;
    }
    return {};
}

int run(const std::wstring &app_path) {
    UiThreadExecutor executor;
    WinHttpTransport transport(executor);
    ProtocolEndpointFacts facts;
    if (!endpoint_is_valid(kDefaultEndpoint, &facts)) return 1;
    ProtocolConfiguration config;
    config.endpoint = facts;
    TransportRequest models;
    Protocol::make_models_request(config, models);
    models.timeout_ms = 5000;
    const auto inventory_response = send_now(transport, 1, std::move(models));
    if (inventory_response.error != AdapterError::none || inventory_response.status != 200) {
        std::puts("SKIP: Lemonade is not reachable");
        return 77;
    }
    const std::string inventory(inventory_response.bytes.begin(), inventory_response.bytes.end());
    const std::string cleanup = pick_cleanup(inventory);
    if (cleanup.empty() || inventory.find("\"Whisper-Base\"") == std::string::npos
        || inventory.find("kokoro-v1") == std::string::npos) {
        std::puts("SKIP: Whisper-Base, kokoro-v1 or a cleanup model is missing");
        return 77;
    }
    TransportRequest speech;
    speech.method = HttpMethod::post;
    speech.url = std::string(kDefaultEndpoint) + "/audio/speech";
    speech.headers.push_back({"Content-Type", "application/json"});
    const std::string body = R"({"model":"kokoro-v1","voice":"af_heart","response_format":"wav",)"
        R"("input":"Um, send Alex the fourteen invoices on Friday."})";
    speech.body.assign(body.begin(), body.end());
    speech.timeout_ms = 180000;
    const auto tts = send_now(transport, 2, std::move(speech));
    transport.shutdown();
    std::vector<std::uint8_t> check;
    if (tts.error != AdapterError::none || tts.status != 200 || !convert_wav_for_upload(tts.bytes, check)) {
        std::puts("SKIP: Lemonade TTS unavailable");
        return 77;
    }

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring wav_path = std::wstring(temp) + L"lemonvox-e2e-" + std::to_wstring(GetCurrentProcessId()) + L".wav";
    {
        std::ofstream file(wav_path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(tts.bytes.data()), static_cast<std::streamsize>(tts.bytes.size()));
    }
    const std::wstring key = L"Software\\LemonVoxTest\\e2e-" + std::to_wstring(GetCurrentProcessId());
    PreferencesStore store(key);
    WinPreferences preferences;
    preferences.asr_model = "Whisper-Base";
    preferences.cleanup_model = cleanup;
    if (!store.save(preferences)) return 1;

    HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\LemonVox.Windows.SingleInstance");
    if (existing) {
        CloseHandle(existing);
        store.erase();
        DeleteFileW(wav_path.c_str());
        std::puts("SKIP: LemonVox is already running");
        return 77;
    }

    std::wstring command = L"\"" + app_path + L"\" --settings-key \"" + key + L"\" --sample-wav \"" + wav_path + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(app_path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        std::fprintf(stderr, "could not launch %ls\n", app_path.c_str());
        return 1;
    }
    WaitForInputIdle(process.hProcess, 10000);
    pump_for(2500);

    const std::wstring sentinel = L"LemonVox e2e clipboard sentinel";
    const bool sentinel_set = copy_text_to_clipboard("LemonVox e2e clipboard sentinel");

    const auto attempt = [&]() -> bool {
        REQUIRE(sentinel_set);
        REQUIRE(bring_to_foreground());
        SetWindowTextW(g_edit, L"Note: ");
        SendMessageW(g_edit, EM_SETSEL, 6, 6);
        REQUIRE(double_alt());
        pump_for(2500);
        REQUIRE(foreground_is_ours() && GetFocus() == g_edit);
        REQUIRE(double_alt());
        const ULONGLONG started = GetTickCount64();
        while (edit_text().find(L"Friday") == std::wstring::npos && GetTickCount64() < started + 180000) pump_for(100);
        const ULONGLONG elapsed = GetTickCount64() - started;
        pump_for(1500);
        const std::wstring text = edit_text();
        std::printf("inserted after %llu ms: %ls\n", elapsed, text.c_str());
        REQUIRE(text.rfind(L"Note: ", 0) == 0);
        REQUIRE(text.find(L"Alex") != std::wstring::npos && text.find(L"Friday") != std::wstring::npos);
        REQUIRE(text.find(L"Friday") == text.rfind(L"Friday"));
        REQUIRE(text.find(L'\r') == std::wstring::npos && text.find(L'\n') == std::wstring::npos);
        REQUIRE(foreground_is_ours() && GetFocus() == g_edit);
        std::wstring clipboard;
        REQUIRE(read_clipboard_text(clipboard) && clipboard == sentinel);
        return true;
    };
    const bool ok = attempt();

    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    store.erase();
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\LemonVoxTest");
    DeleteFileW(wav_path.c_str());
    return ok ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) return 2;
    test::InteractiveLock lock;

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"LemonVoxAppE2E";
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&window_class);
    g_window = CreateWindowExW(0, window_class.lpszClassName, L"LemonVox app E2E target", WS_OVERLAPPEDWINDOW,
        200, 200, 640, 240, nullptr, nullptr, window_class.hInstance, nullptr);
    g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL,
        8, 8, 600, 180, g_window, nullptr, window_class.hInstance, nullptr);
    // Keep the user's clipboard intact across the whole test.
    ClipboardSnapshot original;
    const bool saved = snapshot_clipboard(g_window, original) == ClipboardStatus::ok;
    const int result = run(argv[1]);
    if (saved) restore_clipboard(g_window, original, nullptr);
    DestroyWindow(g_window);
    return result;
}
