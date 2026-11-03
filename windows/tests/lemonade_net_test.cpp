// Fetches the model catalog from a real local Lemonade server. Skips (exit 0)
// when no server answers at the default endpoint.
#include "net/model_catalog.h"
#include "net/winhttp_transport.h"

#include <windows.h>

#include <cassert>
#include <cstdio>
#include <optional>

using namespace lemonvox::win;

int main() {
    UiThreadExecutor executor;
    WinHttpTransport transport(executor);
    ModelCatalog catalog(transport, executor);
    std::optional<CatalogResult> result;
    catalog.refresh("http://127.0.0.1:13305/v1", [&](CatalogResult value) { result = std::move(value); });

    const ULONGLONG deadline = GetTickCount64() + 30000;
    MSG message;
    while (!result && GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        if (!result) MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    assert(result.has_value());
    if (result->status == CatalogStatus::network) {
        std::puts("SKIPPED: no Lemonade server reachable at http://127.0.0.1:13305/v1");
        transport.shutdown();
        return 0;
    }
    assert(result->status == CatalogStatus::ok);
    bool whisper = false;
    std::size_t asr = 0;
    std::size_t cleanup = 0;
    for (const auto &model : result->models) {
        if (model.id == "Whisper-Base" && model.asr) whisper = true;
        asr += model.asr ? 1 : 0;
        cleanup += model.cleanup ? 1 : 0;
    }
    std::printf("Lemonade catalog: %zu models (%zu ASR, %zu cleanup)\n", result->models.size(), asr, cleanup);
    assert(whisper);
    transport.shutdown();
    return 0;
}
