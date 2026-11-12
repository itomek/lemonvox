#include "ui/settings_window.h"

#include "common/utf.h"
#include "ui/settings_layout.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <oleacc.h>
#include <uxtheme.h>

#include <optional>
#include <utility>

namespace lemonvox::win {

namespace {

using settings::DeviceChoices;
using settings::ModelChoices;
using settings::ModelRole;
using settings::RecordStep;
using settings::StatusText;
using settings::TriggerChoices;

constexpr wchar_t kClassName[] = L"LemonVoxSettingsWindow";
constexpr wchar_t kTitle[] = L"LemonVox Settings";
constexpr int kControlCount = static_cast<int>(SettingsControl::count);
constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
constexpr DWORD kWindowExStyle = WS_EX_CONTROLPARENT;
constexpr DWORD kImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr UINT_PTR kSubclassId = 0x4C56;
constexpr UINT kFocusRecorderMessage = WM_APP + 1;
// LiveSetting_Property_GUID {C12BCD8E-2A8E-4950-8AE7-3625111D58EB}; UIA LiveSetting Polite = 1.
constexpr GUID kLiveSettingProperty = {0xc12bcd8e, 0x2a8e, 0x4950, {0x8a, 0xe7, 0x36, 0x25, 0x11, 0x1d, 0x58, 0xeb}};
// PROPID_ACC_DESCRIPTION; oleacc.lib does not export the PROPID_ACC_* GUIDs.
constexpr GUID kDescriptionProperty = {0x4d48dfe4, 0xbd3f, 0x491f, {0xa6, 0x48, 0x49, 0x2d, 0x6f, 0x20, 0xc5, 0x88}};

enum class Kind { label, note, edit, multiline, recorder, combo, button, default_button };

struct Spec {
    const wchar_t *text;
    Kind kind;
};

// Each label precedes its control so MSAA/UIA name the control after it and
// mnemonics (&) move focus to it.
constexpr Spec kSpecs[kControlCount] = {
    {L"Server &endpoint", Kind::label},
    {L"", Kind::edit},
    {L"&Refresh models", Kind::button},
    {L"Requires separate Lemonade or another OpenAI-compatible server on this PC. Only loopback endpoints are supported.", Kind::note},
    {L"", Kind::note},
    {L"&Transcription model", Kind::label},
    {L"", Kind::combo},
    {L"C&leanup model", Kind::label},
    {L"", Kind::combo},
    {L"The server must provide transcription, chat and discoverable compatible model metadata. It manages models, providers "
     L"and API keys; remote cleanup sends transcribed text through it to that provider.", Kind::note},
    {L"Cleanup &prompt", Kind::label},
    {L"", Kind::multiline},
    {L"Reset to &default", Kind::button},
    {L"S&hortcut", Kind::label},
    {L"", Kind::combo},
    {L"&Custom shortcut", Kind::label},
    {L"", Kind::recorder},
    {L"&Microphone", Kind::label},
    {L"", Kind::combo},
    {L"Microphone pri&vacy settings\u2026", Kind::button},
    {L"&Save", Kind::default_button},
    {L"Cancel", Kind::button},
};

bool system_prefers_dark() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return false;
    }
    return value == 0;
}

bool high_contrast_on() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

std::uint32_t held_modifiers() {
    std::uint32_t modifiers = 0;
    if (GetKeyState(VK_CONTROL) < 0) modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) modifiers |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) modifiers |= MOD_WIN;
    return modifiers;
}

bool is_plain_tab(const MSG &message) {
    return message.message == WM_KEYDOWN && message.wParam == VK_TAB
        && (held_modifiers() & (MOD_CONTROL | MOD_ALT | MOD_WIN)) == 0;
}

std::wstring window_text(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
    text.resize(static_cast<size_t>(copied > 0 ? copied : 0));
    return text;
}

void fill_combo(HWND combo, const std::vector<std::wstring> &labels, int selected) {
    SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto &label : labels) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(combo, nullptr, TRUE);
}

int combo_selection(HWND combo) {
    return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
}

} // namespace

int settings_control_id(SettingsControl control) {
    if (control == SettingsControl::save_button) return IDOK;
    if (control == SettingsControl::cancel_button) return IDCANCEL;
    return 1000 + static_cast<int>(control);
}

class SettingsWindow::Impl : public std::enable_shared_from_this<SettingsWindow::Impl> {
public:
    Impl(HINSTANCE instance, SettingsServices services, std::string default_prompt)
        : instance_(instance), services_(std::move(services)), default_prompt_(std::move(default_prompt)) {}

    ~Impl() {
        destroy();
        if (font_) DeleteObject(font_);
        delete_brushes();
    }

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    void show(const WinPreferences &current) {
        if (hwnd_) {
            if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
            bring_forward();
            return;
        }
        if (!register_class()) return;
        original_.emplace(current);
        trigger_ = settings::trigger_round_trips(current.trigger) ? current.trigger : default_trigger();
        models_.clear();
        models_loaded_ = false;
        ++generation_;

        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        // Creating on the target monitor gives the right DPI before the first layout.
        CreateWindowExW(kWindowExStyle, kClassName, kTitle, kWindowStyle, monitor.rcWork.left + 32,
            monitor.rcWork.top + 32, 320, 240, nullptr, nullptr, instance_, this);
        if (!hwnd_) return;
        dpi_ = static_cast<int>(GetDpiForWindow(hwnd_));
        create_controls();
        update_font();
        populate(current);
        apply_theme();
        apply_layout(true, nullptr);
        saved_focus_ = control(SettingsControl::endpoint_edit);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        bring_forward();
        SetFocus(control(SettingsControl::endpoint_edit));
        refresh();
    }

    void destroy() {
        if (hwnd_) DestroyWindow(hwnd_);
    }

    bool pre_translate(MSG *message) {
        if (!hwnd_ || !message) return false;
        if (message->hwnd != hwnd_ && !IsChild(hwnd_, message->hwnd)) return false;
        // While recording, keys go straight to the recorder untranslated so
        // Alt+letter cannot trigger a mnemonic and Enter/Esc stay recordable.
        if (recording_ && message->hwnd == control(SettingsControl::recorder_edit)
            && message->message >= WM_KEYFIRST && message->message <= WM_KEYLAST && !is_plain_tab(*message)) {
            DispatchMessageW(message);
            return true;
        }
        return IsDialogMessageW(hwnd_, message) != FALSE;
    }

    HWND hwnd() const { return hwnd_; }
    bool dark() const { return dark_; }

private:
    HWND control(SettingsControl which) const { return controls_[static_cast<int>(which)]; }

    bool register_class() {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &Impl::window_proc;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = kClassName;
        return RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void bring_forward() {
        SetForegroundWindow(hwnd_);
        BringWindowToTop(hwnd_);
    }

    void create_controls() {
        for (int index = 0; index < kControlCount; ++index) {
            const Spec &spec = kSpecs[index];
            const wchar_t *window_class = WC_STATICW;
            DWORD style = WS_CHILD | WS_VISIBLE;
            DWORD ex_style = 0;
            switch (spec.kind) {
            case Kind::label: style |= SS_LEFT; break;
            case Kind::note: style |= SS_LEFT | SS_NOPREFIX; break;
            case Kind::edit:
                window_class = WC_EDITW;
                style |= WS_TABSTOP | WS_GROUP | ES_AUTOHSCROLL;
                ex_style = WS_EX_CLIENTEDGE;
                break;
            case Kind::multiline:
                window_class = WC_EDITW;
                style |= WS_TABSTOP | WS_GROUP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
                ex_style = WS_EX_CLIENTEDGE;
                break;
            case Kind::recorder:
                window_class = WC_EDITW;
                style |= WS_TABSTOP | WS_GROUP | ES_AUTOHSCROLL; // keys are swallowed while focused
                ex_style = WS_EX_CLIENTEDGE;
                break;
            case Kind::combo:
                window_class = WC_COMBOBOXW;
                style |= WS_TABSTOP | WS_GROUP | WS_VSCROLL | CBS_DROPDOWNLIST;
                break;
            case Kind::button:
                window_class = WC_BUTTONW;
                style |= WS_TABSTOP | WS_GROUP | BS_PUSHBUTTON;
                break;
            case Kind::default_button:
                window_class = WC_BUTTONW;
                style |= WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON;
                break;
            }
            const int id = settings_control_id(static_cast<SettingsControl>(index));
            controls_[index] = CreateWindowExW(ex_style, window_class, spec.text, style, 0, 0, 10, 10, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        }
        SetWindowSubclass(control(SettingsControl::recorder_edit), &Impl::recorder_proc, kSubclassId,
            reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(control(SettingsControl::prompt_edit), &Impl::prompt_proc, kSubclassId,
            reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(control(SettingsControl::endpoint_edit), EM_SETCUEBANNER, FALSE,
            reinterpret_cast<LPARAM>(L"http://127.0.0.1:13305/v1"));
        SendMessageW(control(SettingsControl::recorder_edit), EM_SETCUEBANNER, FALSE,
            reinterpret_cast<LPARAM>(L"Focus here, then press keys"));
        for (const auto combo : {SettingsControl::asr_combo, SettingsControl::cleanup_combo}) {
            SendMessageW(control(combo), CB_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(L"Choose a model"));
        }
        for (const auto combo : {SettingsControl::asr_combo, SettingsControl::cleanup_combo,
                 SettingsControl::shortcut_combo, SettingsControl::microphone_combo}) {
            SendMessageW(control(combo), CB_SETMINVISIBLE, 10, 0);
        }
        annotate_accessibility();
    }

    // Live status announcements and descriptions for screen readers. Needs COM
    // on this thread; without it the window still works, only less chatty.
    void annotate_accessibility() {
        IAccPropServices *services = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(CAccPropServices), nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IAccPropServices), reinterpret_cast<void **>(&services)))) {
            return;
        }
        VARIANT polite;
        VariantInit(&polite);
        polite.vt = VT_I4;
        polite.lVal = 1;
        services->SetHwndProp(control(SettingsControl::status), static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF,
            kLiveSettingProperty, polite);
        services->SetHwndPropStr(control(SettingsControl::endpoint_edit), static_cast<DWORD>(OBJID_CLIENT),
            CHILDID_SELF, kDescriptionProperty, kSpecs[static_cast<int>(SettingsControl::endpoint_hint)].text);
        services->SetHwndPropStr(control(SettingsControl::cleanup_combo), static_cast<DWORD>(OBJID_CLIENT),
            CHILDID_SELF, kDescriptionProperty, kSpecs[static_cast<int>(SettingsControl::routing_note)].text);
        services->SetHwndPropStr(control(SettingsControl::recorder_edit), static_cast<DWORD>(OBJID_CLIENT),
            CHILDID_SELF, kDescriptionProperty,
            L"While focused, press the shortcut to record it. Esc cancels.");
        acc_services_ = services;
    }

    void clear_accessibility() {
        if (!acc_services_) return;
        MSAAPROPID live = kLiveSettingProperty;
        MSAAPROPID description = kDescriptionProperty;
        acc_services_->ClearHwndProps(control(SettingsControl::status), static_cast<DWORD>(OBJID_CLIENT),
            CHILDID_SELF, &live, 1);
        for (const auto which : {SettingsControl::endpoint_edit, SettingsControl::cleanup_combo,
                 SettingsControl::recorder_edit}) {
            acc_services_->ClearHwndProps(control(which), static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF,
                &description, 1);
        }
        acc_services_->Release();
        acc_services_ = nullptr;
    }

    void populate(const WinPreferences &current) {
        populating_ = true;
        const std::wstring endpoint = utf8_to_wide(current.endpoint);
        SetWindowTextW(control(SettingsControl::endpoint_edit), endpoint.c_str());
        last_endpoint_ = settings::trim(endpoint);
        rebuild_models(current.asr_model, current.cleanup_model);
        SetWindowTextW(control(SettingsControl::prompt_edit),
            settings::to_edit_newlines(utf8_to_wide(current.cleanup_prompt)).c_str());
        rebuild_trigger();
        rebuild_devices(current.capture_device_id);
        populating_ = false;
    }

    std::string selected_model(ModelRole role) const {
        const ModelChoices &choices = role == ModelRole::transcription ? asr_choices_ : cleanup_choices_;
        const int index = combo_selection(
            control(role == ModelRole::transcription ? SettingsControl::asr_combo : SettingsControl::cleanup_combo));
        return index >= 0 && index < static_cast<int>(choices.ids.size()) ? choices.ids[index] : std::string();
    }

    void rebuild_models(const std::string &asr, const std::string &cleanup) {
        asr_choices_ = settings::build_model_choices(models_, ModelRole::transcription, asr, models_loaded_);
        cleanup_choices_ = settings::build_model_choices(models_, ModelRole::cleanup, cleanup, models_loaded_);
        fill_combo(control(SettingsControl::asr_combo), asr_choices_.labels, asr_choices_.selected);
        fill_combo(control(SettingsControl::cleanup_combo), cleanup_choices_.labels, cleanup_choices_.selected);
    }

    std::wstring selected_device() const {
        const int index = combo_selection(control(SettingsControl::microphone_combo));
        return index >= 0 && index < static_cast<int>(device_choices_.ids.size()) ? device_choices_.ids[index]
                                                                                   : std::wstring();
    }

    void rebuild_devices(const std::wstring &selected) {
        std::vector<SettingsDevice> devices;
        if (services_.list_devices) devices = services_.list_devices();
        device_choices_ = settings::build_device_choices(devices, selected);
        fill_combo(control(SettingsControl::microphone_combo), device_choices_.labels, device_choices_.selected);
    }

    void rebuild_trigger() {
        trigger_choices_ = settings::build_trigger_choices(trigger_);
        std::vector<std::wstring> labels;
        for (const auto &item : trigger_choices_.items) labels.push_back(item.label);
        fill_combo(control(SettingsControl::shortcut_combo), labels, trigger_choices_.selected);
        update_recorder_text();
    }

    void update_recorder_text() {
        const std::wstring text = trigger_.kind == TriggerKind::combination ? describe_trigger(trigger_) : L"";
        SetWindowTextW(control(SettingsControl::recorder_edit), text.c_str());
    }

    void set_status(StatusText status) {
        status_ = std::move(status);
        const HWND label = control(SettingsControl::status);
        SetWindowTextW(label, status_.text.c_str());
        InvalidateRect(label, nullptr, TRUE);
        RECT rect{};
        GetWindowRect(label, &rect);
        if (measure(SettingsControl::status, rect.right - rect.left) > rect.bottom - rect.top) apply_layout(false, nullptr);
        NotifyWinEvent(EVENT_OBJECT_LIVEREGIONCHANGED, label, OBJID_CLIENT, CHILDID_SELF);
    }

    void refresh() {
        const std::wstring endpoint = settings::trim(window_text(control(SettingsControl::endpoint_edit)));
        const std::wstring error = settings::validate_endpoint(endpoint);
        rebuild_devices(selected_device());
        if (!error.empty()) {
            ++generation_;
            set_status({error, true});
            return;
        }
        const std::uint64_t generation = ++generation_;
        last_endpoint_ = endpoint;
        set_status(settings::loading_status());
        if (!services_.refresh_models) return;
        std::weak_ptr<Impl> weak = weak_from_this();
        services_.refresh_models(wide_to_utf8(endpoint), [weak, generation](SettingsCatalogResult result) {
            if (const auto self = weak.lock()) self->on_models(generation, std::move(result));
        });
    }

    void on_models(std::uint64_t generation, SettingsCatalogResult result) {
        if (generation != generation_ || !hwnd_) return;
        if (result.status == SettingsCatalogResult::Status::ok) {
            const std::string asr = selected_model(ModelRole::transcription);
            const std::string cleanup = selected_model(ModelRole::cleanup);
            models_ = result.models;
            models_loaded_ = true;
            rebuild_models(asr, cleanup);
        }
        set_status(settings::catalog_status(result, last_endpoint_));
    }

    void set_suspended(bool suspended) {
        if (suspended_ == suspended) return;
        suspended_ = suspended;
        if (services_.set_trigger_suspended) services_.set_trigger_suspended(suspended);
    }

    void begin_recording() {
        if (recording_) return;
        recording_ = true;
        set_suspended(true);
        SetWindowTextW(control(SettingsControl::recorder_edit), settings::kRecorderPrompt);
    }

    void end_recording() {
        if (!recording_) return;
        recording_ = false;
        set_suspended(false);
        update_recorder_text();
    }

    void on_recorder_key(std::uint32_t virtual_key) {
        const RecordStep step = settings::record_key(virtual_key, held_modifiers());
        switch (step.outcome) {
        case RecordStep::Outcome::pending:
            SetWindowTextW(control(SettingsControl::recorder_edit), step.display.c_str());
            break;
        case RecordStep::Outcome::rejected:
            SetWindowTextW(control(SettingsControl::recorder_edit), settings::kRecorderPrompt);
            set_status({step.display, true});
            break;
        case RecordStep::Outcome::canceled:
            SetFocus(control(SettingsControl::shortcut_combo));
            break;
        case RecordStep::Outcome::captured:
            trigger_ = step.binding;
            recording_ = false;
            set_suspended(false);
            rebuild_trigger();
            SetFocus(control(SettingsControl::shortcut_combo));
            break;
        }
    }

    void on_shortcut_command(WORD code) {
        const HWND combo = control(SettingsControl::shortcut_combo);
        const int index = combo_selection(combo);
        if (index < 0 || index >= static_cast<int>(trigger_choices_.items.size())) return;
        const auto &item = trigger_choices_.items[index];
        if (code == CBN_SELCHANGE && !item.custom) {
            trigger_ = item.binding;
            update_recorder_text();
        } else if (code == CBN_CLOSEUP && item.custom) {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(trigger_choices_.selected), 0);
            PostMessageW(hwnd_, kFocusRecorderMessage, 0, 0);
        } else if (code == CBN_KILLFOCUS && item.custom) {
            rebuild_trigger();
        }
        if (!item.custom) trigger_choices_.selected = index;
    }

    void save() {
        end_recording();
        const std::wstring endpoint = settings::trim(window_text(control(SettingsControl::endpoint_edit)));
        std::wstring error = settings::validate_endpoint(endpoint);
        if (!error.empty()) {
            set_status({error, true});
            SetFocus(control(SettingsControl::endpoint_edit));
            return;
        }
        const std::wstring prompt = settings::from_edit_newlines(window_text(control(SettingsControl::prompt_edit)));
        error = settings::validate_prompt(prompt);
        if (!error.empty()) {
            set_status({error, true});
            SetFocus(control(SettingsControl::prompt_edit));
            return;
        }
        WinPreferences preferences = *original_;
        preferences.endpoint = wide_to_utf8(endpoint);
        preferences.asr_model = selected_model(ModelRole::transcription);
        preferences.cleanup_model = selected_model(ModelRole::cleanup);
        preferences.cleanup_prompt = wide_to_utf8(prompt);
        preferences.trigger = trigger_;
        preferences.capture_device_id = selected_device();

        const bool trigger_changed = preferences.trigger != original_->trigger;
        if (trigger_changed && services_.apply_trigger) {
            std::wstring reason;
            if (!services_.apply_trigger(preferences.trigger, &reason)) {
                set_status({L"The shortcut " + describe_trigger(preferences.trigger) + L" is unavailable"
                        + (reason.empty() ? L"" : L": " + reason) + L". Choose another shortcut.",
                    true});
                SetFocus(control(SettingsControl::shortcut_combo));
                return;
            }
        }
        if (services_.save && !services_.save(preferences)) {
            if (trigger_changed && services_.apply_trigger) services_.apply_trigger(original_->trigger, nullptr);
            set_status({L"Settings could not be saved. Try again.", true});
            return;
        }
        destroy();
    }

    void update_font() {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, static_cast<UINT>(dpi_));
        const HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
        for (const HWND child : controls_) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        if (font_) DeleteObject(font_);
        font_ = font;
        const HDC dc = GetDC(hwnd_);
        const HGDIOBJ old = SelectObject(dc, font_);
        TEXTMETRICW text_metrics{};
        GetTextMetricsW(dc, &text_metrics);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        line_height_ = text_metrics.tmHeight > 0 ? text_metrics.tmHeight : MulDiv(16, dpi_, 96);
    }

    int measure(SettingsControl which, int width) const {
        const std::wstring text = window_text(control(which));
        if (text.empty()) return 0;
        const HDC dc = GetDC(hwnd_);
        const HGDIOBJ old = SelectObject(dc, font_);
        RECT rect{0, 0, width > 0 ? width : 10000, 0};
        const UINT format = DT_CALCRECT | (width > 0 ? DT_WORDBREAK | DT_NOPREFIX : DT_SINGLELINE);
        DrawTextW(dc, text.c_str(), -1, &rect, format);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        return width > 0 ? rect.bottom - rect.top : rect.right - rect.left;
    }

    void apply_layout(bool center, const RECT *suggested) {
        const settings::Layout layout = settings::compute_layout(dpi_, line_height_,
            [this](SettingsControl which, int width) { return measure(which, width); });
        const HDWP batch = BeginDeferWindowPos(kControlCount);
        HDWP positions = batch;
        for (int index = 0; index < kControlCount; ++index) {
            const RECT &rect = layout.rects[index];
            int height = rect.bottom - rect.top;
            if (kSpecs[index].kind == Kind::combo) height += MulDiv(240, dpi_, 96);
            if (positions) {
                positions = DeferWindowPos(positions, controls_[index], nullptr, rect.left, rect.top,
                    rect.right - rect.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        if (positions) EndDeferWindowPos(positions);

        RECT frame{0, 0, layout.client.cx, layout.client.cy};
        AdjustWindowRectExForDpi(&frame, kWindowStyle, FALSE, kWindowExStyle, static_cast<UINT>(dpi_));
        const int width = frame.right - frame.left;
        const int height = frame.bottom - frame.top;
        if (suggested) {
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (center) {
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
            const RECT &work = monitor.rcWork;
            const int x = work.left + std::max(0L, (work.right - work.left - width) / 2);
            const int y = work.top + std::max(0L, (work.bottom - work.top - height) / 2);
            SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        }
    }

    void delete_brushes() {
        if (background_brush_) DeleteObject(background_brush_);
        if (field_brush_) DeleteObject(field_brush_);
        background_brush_ = field_brush_ = nullptr;
    }

    void apply_theme() {
        const bool contrast = high_contrast_on();
        dark_ = !contrast && system_prefers_dark();
        const BOOL dark_frame = dark_ ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, kImmersiveDarkMode, &dark_frame, sizeof(dark_frame));
        if (dark_) {
            background_ = RGB(32, 32, 32);
            field_ = RGB(45, 45, 45);
            text_ = RGB(255, 255, 255);
            secondary_ = RGB(204, 204, 204);
            error_ = RGB(255, 153, 164);
        } else {
            background_ = GetSysColor(COLOR_WINDOW);
            field_ = GetSysColor(COLOR_WINDOW);
            text_ = GetSysColor(COLOR_WINDOWTEXT);
            secondary_ = contrast ? text_ : RGB(92, 92, 92);
            error_ = contrast ? text_ : RGB(196, 43, 28);
        }
        delete_brushes();
        background_brush_ = CreateSolidBrush(background_);
        field_brush_ = CreateSolidBrush(field_);
        for (int index = 0; index < kControlCount; ++index) {
            const wchar_t *theme = nullptr;
            if (dark_) {
                switch (kSpecs[index].kind) {
                case Kind::edit: case Kind::recorder: case Kind::combo: theme = L"DarkMode_CFD"; break;
                case Kind::multiline: case Kind::button: case Kind::default_button: theme = L"DarkMode_Explorer"; break;
                case Kind::label: case Kind::note: break;
                }
            }
            if (kSpecs[index].kind == Kind::label || kSpecs[index].kind == Kind::note) continue;
            SetWindowTheme(controls_[index], theme, nullptr);
            if (kSpecs[index].kind == Kind::combo) {
                COMBOBOXINFO info{};
                info.cbSize = sizeof(info);
                if (GetComboBoxInfo(controls_[index], &info) && info.hwndList) {
                    SetWindowTheme(info.hwndList, dark_ ? L"DarkMode_Explorer" : nullptr, nullptr);
                }
            }
        }
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }

    LRESULT color_control(UINT message, HDC dc, HWND child) {
        const bool field = message != WM_CTLCOLORSTATIC && message != WM_CTLCOLORBTN;
        COLORREF color = text_;
        if (child == control(SettingsControl::status) && status_.error) color = error_;
        if (child == control(SettingsControl::endpoint_hint) || child == control(SettingsControl::routing_note)) {
            color = secondary_;
        }
        SetTextColor(dc, color);
        SetBkColor(dc, field ? field_ : background_);
        return reinterpret_cast<LRESULT>(field ? field_brush_ : background_brush_);
    }

    void on_command(int id, WORD code) {
        if (id == IDOK) { save(); return; }
        if (id == IDCANCEL) { destroy(); return; }
        switch (static_cast<SettingsControl>(id - 1000)) {
        case SettingsControl::refresh_button:
            if (code == BN_CLICKED) refresh();
            break;
        case SettingsControl::reset_prompt_button:
            if (code == BN_CLICKED) {
                SetWindowTextW(control(SettingsControl::prompt_edit),
                    settings::to_edit_newlines(utf8_to_wide(default_prompt_)).c_str());
            }
            break;
        case SettingsControl::privacy_button:
            if (code == BN_CLICKED && services_.open_microphone_privacy) services_.open_microphone_privacy();
            break;
        case SettingsControl::endpoint_edit:
            if (code == EN_CHANGE && !populating_
                && settings::trim(window_text(control(SettingsControl::endpoint_edit))) != last_endpoint_) {
                ++generation_;
                set_status(settings::endpoint_changed_status());
            }
            break;
        case SettingsControl::shortcut_combo:
            on_shortcut_command(code);
            break;
        default:
            break;
        }
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_COMMAND:
            on_command(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case DM_GETDEFID:
            return MAKELRESULT(IDOK, DC_HASDEFID);
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
            if (background_brush_) {
                return color_control(message, reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam));
            }
            break;
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd_, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client,
                background_brush_ ? background_brush_ : GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) {
                const HWND focus = GetFocus();
                if (focus && IsChild(hwnd_, focus)) saved_focus_ = focus;
            } else {
                // Returning to the recorder would silently re-arm recording.
                if (saved_focus_ == control(SettingsControl::recorder_edit)) {
                    saved_focus_ = control(SettingsControl::shortcut_combo);
                }
                if (saved_focus_ && IsWindow(saved_focus_)) SetFocus(saved_focus_);
            }
            return 0;
        case kFocusRecorderMessage:
            SetFocus(control(SettingsControl::recorder_edit));
            return 0;
        case WM_DPICHANGED:
            dpi_ = HIWORD(wparam);
            update_font();
            apply_layout(false, reinterpret_cast<const RECT *>(lparam));
            return 0;
        case WM_SETTINGCHANGE:
            if (wparam == SPI_SETNONCLIENTMETRICS) {
                update_font();
                apply_layout(false, nullptr);
            }
            if (lparam && lstrcmpiW(reinterpret_cast<const wchar_t *>(lparam), L"ImmersiveColorSet") == 0) {
                apply_theme();
            }
            break;
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            apply_theme();
            break;
        case WM_DESTROY:
            recording_ = false;
            set_suspended(false);
            clear_accessibility();
            break;
        case WM_NCDESTROY:
            ++generation_;
            hwnd_ = nullptr;
            for (auto &child : controls_) child = nullptr;
            saved_focus_ = nullptr;
            break;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto *self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self || self->hwnd_ != hwnd) return DefWindowProcW(hwnd, message, wparam, lparam);
        return self->handle(message, wparam, lparam);
    }

    static LRESULT CALLBACK recorder_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
        DWORD_PTR data) {
        auto *self = reinterpret_cast<Impl *>(data);
        switch (message) {
        case WM_GETDLGCODE: {
            const auto *pending = reinterpret_cast<const MSG *>(lparam);
            const LRESULT code = DefSubclassProc(hwnd, message, wparam, lparam);
            if (pending && is_plain_tab(*pending)) return code & ~(DLGC_WANTALLKEYS | DLGC_WANTTAB | DLGC_WANTMESSAGE);
            return code | DLGC_WANTALLKEYS | DLGC_WANTCHARS | (pending ? DLGC_WANTMESSAGE : 0);
        }
        case WM_SETFOCUS: {
            const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
            self->begin_recording();
            HideCaret(hwnd);
            return result;
        }
        case WM_KILLFOCUS:
            self->end_recording();
            break;
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            if (self->recording_) {
                self->on_recorder_key(static_cast<std::uint32_t>(wparam));
                return 0;
            }
            break;
        case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
        case WM_IME_STARTCOMPOSITION: case WM_IME_COMPOSITION: case WM_IME_ENDCOMPOSITION: case WM_IME_CHAR:
            if (self->recording_) return 0;
            break;
        case WM_CONTEXTMENU: case WM_PASTE: case WM_CUT: case WM_CLEAR:
            return 0;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, &Impl::recorder_proc, kSubclassId);
            break;
        default:
            break;
        }
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }

    // A multiline edit wants every key; give Tab and Esc back to the dialog so
    // keyboard users are never trapped and Esc still cancels.
    static LRESULT CALLBACK prompt_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
        if (message == WM_GETDLGCODE) {
            LRESULT code = DefSubclassProc(hwnd, message, wparam, lparam);
            const auto *pending = reinterpret_cast<const MSG *>(lparam);
            if (pending && pending->message == WM_KEYDOWN && (pending->wParam == VK_TAB || pending->wParam == VK_ESCAPE)) {
                code &= ~(DLGC_WANTALLKEYS | DLGC_WANTTAB | DLGC_WANTMESSAGE);
            }
            return code;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, &Impl::prompt_proc, kSubclassId);
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }

    HINSTANCE instance_;
    SettingsServices services_;
    std::string default_prompt_;
    HWND hwnd_ = nullptr;
    HWND controls_[kControlCount] = {};
    HWND saved_focus_ = nullptr;
    HFONT font_ = nullptr;
    int dpi_ = 96;
    int line_height_ = 16;
    IAccPropServices *acc_services_ = nullptr;

    std::optional<WinPreferences> original_;
    std::vector<SettingsModelOption> models_;
    bool models_loaded_ = false;
    ModelChoices asr_choices_;
    ModelChoices cleanup_choices_;
    DeviceChoices device_choices_;
    TriggerChoices trigger_choices_;
    TriggerBinding trigger_;
    bool recording_ = false;
    bool suspended_ = false;
    bool populating_ = false;
    std::uint64_t generation_ = 0;
    std::wstring last_endpoint_;
    StatusText status_;

    bool dark_ = false;
    COLORREF background_ = 0;
    COLORREF field_ = 0;
    COLORREF text_ = 0;
    COLORREF secondary_ = 0;
    COLORREF error_ = 0;
    HBRUSH background_brush_ = nullptr;
    HBRUSH field_brush_ = nullptr;
};

SettingsWindow::SettingsWindow(HINSTANCE instance, SettingsServices services, std::string default_cleanup_prompt)
    : impl_(std::make_shared<Impl>(instance, std::move(services), std::move(default_cleanup_prompt))) {}

SettingsWindow::~SettingsWindow() = default;

void SettingsWindow::show(const WinPreferences &current) { impl_->show(current); }
void SettingsWindow::close() { impl_->destroy(); }
bool SettingsWindow::is_open() const { return impl_->hwnd() != nullptr; }
HWND SettingsWindow::hwnd() const { return impl_->hwnd(); }
bool SettingsWindow::pre_translate(MSG *message) { return impl_->pre_translate(message); }
bool SettingsWindow::dark_mode_active() const { return impl_->dark(); }

} // namespace lemonvox::win
