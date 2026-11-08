#pragma once

#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <UIAutomation.h>

#include <atomic>

namespace lemonvox::win::test {

// A real HWND-backed UIA provider whose focused element can temporarily be
// unavailable, as happens while Notepad processes the second Alt release.
class TransientUiaProvider final : public IRawElementProviderSimple, public IValueProvider {
public:
    explicit TransientUiaProvider(HWND window) : window_(window) {}

    std::atomic<ULONGLONG> unavailable_until{0};
    std::atomic<int> unavailable_reads{0};
    std::atomic<bool> password{false};
    std::atomic<bool> read_only{false};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple))
            *out = static_cast<IRawElementProviderSimple *>(this);
        else if (iid == __uuidof(IValueProvider))
            *out = static_cast<IValueProvider *>(this);
        if (!*out) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions *out) override {
        *out = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown **out) override {
        *out = nullptr;
        if (pattern == UIA_ValuePatternId) {
            *out = static_cast<IValueProvider *>(this);
            AddRef();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT *out) override {
        VariantInit(out);
        if (property == UIA_ControlTypePropertyId) {
            if (GetTickCount64() < unavailable_until.load()) {
                ++unavailable_reads;
                return UIA_E_ELEMENTNOTAVAILABLE;
            }
            out->vt = VT_I4;
            out->lVal = UIA_EditControlTypeId;
        } else if (property == UIA_IsPasswordPropertyId || property == UIA_IsKeyboardFocusablePropertyId ||
                   property == UIA_IsControlElementPropertyId || property == UIA_IsContentElementPropertyId) {
            out->vt = VT_BOOL;
            out->boolVal = property != UIA_IsPasswordPropertyId || password ? VARIANT_TRUE : VARIANT_FALSE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple **out) override {
        return UiaHostProviderFromHwnd(window_, out);
    }
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR) override { return UIA_E_NOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR *out) override {
        *out = SysAllocString(L"");
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL *out) override {
        *out = read_only;
        return S_OK;
    }

private:
    HWND window_;
    std::atomic<ULONG> references_{1};
};

inline LRESULT CALLBACK transient_uia_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto *provider = reinterpret_cast<TransientUiaProvider *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_GETOBJECT && static_cast<LONG>(lparam) == UiaRootObjectId && provider)
        return UiaReturnRawElementProvider(window, wparam, lparam, provider);
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace lemonvox::win::test
