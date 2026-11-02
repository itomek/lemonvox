#pragma once

#include <objbase.h>

namespace lemonvox::win {

// Joins the MTA for the scope when the thread has no apartment yet. A thread
// already in an STA keeps it; MMDevice and WASAPI work from either.
class ComScope {
public:
    ComScope() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;
    bool usable() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

} // namespace lemonvox::win
