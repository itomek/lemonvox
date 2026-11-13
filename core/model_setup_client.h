#pragma once

#include "protocol.h"
#include <functional>
#include <atomic>
#include <memory>
#include <utility>

namespace lemonvox {

// Native adapters supply byte delivery and a run-loop timer. All entry points,
// timer callbacks and transport completions run on the same serialized executor.
// This controller never persists preferences or starts installation from refresh.
class ModelSetupClient {
public:
    struct Snapshot {
        ProtocolInventory catalog;
        std::string endpoint;
        bool available = false;
        bool management_supported = false;
        bool active = false;
        double percent = -1;
        std::string message;
        std::string installed_id; // Set only after refreshed catalog confirms success.
        LVModelPurpose installed_purpose = LV_MODEL_ASR;
    };
    using Changed = std::function<void(Snapshot)>;
    using Schedule = std::function<void(unsigned, std::function<void()>)>;

    ModelSetupClient(ByteTransport &transport, Schedule schedule, Changed changed)
        : state_(std::make_shared<State>(transport, std::move(schedule), std::move(changed))) {}
    ~ModelSetupClient() {
        state_->alive = false;
        ++state_->generation;
        state_->transport.cancel(state_->operation);
    }
    ModelSetupClient(const ModelSetupClient &) = delete;
    ModelSetupClient &operator=(const ModelSetupClient &) = delete;

    void refresh(ProtocolConfiguration config) {
        const auto &s = state_;
        if (s->view.active) { emit(s); return; }
        s->transport.cancel(s->operation);
        ++s->generation;
        s->config = std::move(config);
        s->view.endpoint = s->config.endpoint.canonical_base_url;
        s->view.installed_id.clear();
        s->view.message = "Checking server model catalog…";
        s->view.management_supported = false;
        emit(s);
        catalog(s, false);
    }
    const Snapshot &snapshot() const { return state_->view; }

    // Only the native confirmation action may pass explicit_consent=true.
    void install(const std::string &id, LVModelPurpose purpose, bool explicit_consent) {
        const auto &s = state_;
        if (s->view.active) { emit(s); return; }
        if (s->uncertain) {
            s->view.message = "The previous server download state is unconfirmed. Refresh models to check its existing job before retrying.";
            emit(s); return;
        }
        TransportRequest request;
        if (Protocol::make_install_request(s->config, s->view.catalog, id, purpose,
                s->view.management_supported, explicit_consent, request) != LV_PROTOCOL_OK) {
            s->view.message = Protocol::model_setup(s->view.catalog, id, purpose,
                s->view.available, s->view.management_supported).message;
            emit(s); return;
        }
        ++s->generation;
        s->model = id; s->purpose = purpose; s->polls = 0;
        s->canceling = false; s->view.active = true; s->view.percent = -1;
        s->view.installed_id.clear();
        s->view.message = "Starting the explicitly requested model download on the inference server…";
        emit(s);
        send(s, std::move(request), [](const auto &state, TransportResponse response) {
            if (!ok(response)) { fail(state, "Installation could not start. Check server network, free disk space and model-directory permissions, then refresh before retrying.", true); return; }
            const auto progress = Protocol::parse_install_progress_for_model(response.bytes, state->model);
            if (progress.status != LV_PROTOCOL_OK) { fail(state, "The server returned an incompatible installation response. Refresh models before retrying.", true); return; }
            progress_result(state, progress);
        });
    }
    void cancel() {
        const auto &s = state_;
        if (!s->view.active || s->canceling) return;
        s->canceling = true;
        s->transport.cancel(s->operation);
        ++s->generation;
        TransportRequest request;
        if (Protocol::make_cancel_install_request(s->config, s->view.catalog, s->model,
                s->purpose, s->view.management_supported, request) != LV_PROTOCOL_OK) {
            fail(s, "Cancellation could not be confirmed. Check the server model manager before retrying.", true); return;
        }
        s->view.message = "Canceling on the server; waiting for its download worker to stop…";
        emit(s);
        send(s, std::move(request), [](const auto &state, TransportResponse response) {
            if (!ok(response)) { fail(state, "Cancellation could not be confirmed. Check the server model manager, then refresh before retrying.", true); return; }
            const auto progress = Protocol::parse_install_progress_for_model(response.bytes, state->model);
            if (progress.status != LV_PROTOCOL_OK) { fail(state, "Cancellation response was incompatible. Check the server model manager before retrying.", true); return; }
            progress_result(state, progress);
        });
    }

private:
    struct State {
        State(ByteTransport &t, Schedule timer, Changed c) : transport(t), schedule(std::move(timer)), changed(std::move(c)) {}
        ByteTransport &transport;
        Schedule schedule;
        Changed changed;
        Snapshot view;
        ProtocolConfiguration config;
        std::string model;
        LVModelPurpose purpose = LV_MODEL_ASR;
        bool alive = true, canceling = false, uncertain = false;
        unsigned polls = 0;
        std::uint64_t generation = 1;
        OperationId operation = 0;
    };
    using Ptr = std::shared_ptr<State>;
    using Completion = std::function<void(const Ptr &, TransportResponse)>;
    static bool ok(const TransportResponse &r) { return r.error == AdapterError::none && r.status >= 200 && r.status < 300; }
    static void emit(const Ptr &s) { if (s->alive && s->changed) s->changed(s->view); }
    static void send(const Ptr &s, TransportRequest request, Completion completion) {
        static std::atomic<OperationId> next{0x6000000000000000ULL};
        const auto operation = s->operation = ++next;
        const auto generation = s->generation;
        request.timeout_ms = 10000;
        std::weak_ptr<State> weak = s;
        s->transport.send(operation, std::move(request), [weak, generation, operation, completion = std::move(completion)](OperationId id, TransportResponse response) {
            const auto state = weak.lock();
            if (!state || !state->alive || generation != state->generation || id != operation || operation != state->operation) return;
            completion(state, std::move(response));
        });
    }
    static void fail(const Ptr &s, std::string message, bool uncertain = false) {
        s->view.active = false; s->view.percent = -1; s->uncertain = uncertain;
        s->view.installed_id.clear();
        s->view.message = std::move(message); emit(s);
    }
    static void catalog(const Ptr &s, bool verifying) {
        TransportRequest request;
        if (Protocol::make_catalog_request(s->config, request) != LV_PROTOCOL_OK) {
            s->view.available = false; fail(s, "Configure an HTTP/HTTPS server endpoint ending in /v1."); return;
        }
        send(s, std::move(request), [verifying](const auto &state, TransportResponse response) {
            ProtocolInventory inventory;
            if (!ok(response)) {
                state->view.available = false;
                fail(state, "Server unavailable. Start the configured inference server, check its address and network, then refresh. Settings were preserved.", verifying); return;
            }
            if (Protocol::parse_catalog(response.bytes, inventory) != LV_PROTOCOL_OK) {
                state->view.available = true; state->view.catalog = {};
                state->view.management_supported = false;
                fail(state, "The server catalog is incompatible. Configure compatible transcription/chat model metadata in the server application, then refresh. Settings were preserved.", verifying); return;
            }
            state->view.catalog = std::move(inventory); state->view.available = true;
            if (verifying) {
                const auto info = Protocol::model_setup(state->view.catalog, state->model, state->purpose, true, state->view.management_supported);
                if (info.state != LV_SETUP_READY && info.state != LV_SETUP_INSTALLED_NOT_LOADED) {
                    fail(state, "The server finished setup but the model is not installed and compatible in its refreshed catalog. Settings were preserved."); return;
                }
                state->view.installed_id = state->model;
                state->view.installed_purpose = state->purpose;
                state->view.active = false; state->uncertain = false;
                state->view.message = "Model installed and verified. Save settings to use the selected model.";
                emit(state); return;
            }
            // Health is a read-only version/loaded-state probe at the configured base.
            TransportRequest health;
            health.url = state->config.endpoint.canonical_base_url;
            while (!health.url.empty() && health.url.back() == '/') health.url.pop_back();
            health.url += "/health";
            send(state, std::move(health), [](const auto &current, TransportResponse health_response) {
                if (ok(health_response)) {
                    Protocol::catalog_apply_health(current->view.catalog, health_response.bytes);
                    current->view.management_supported = Protocol::supports_model_management(health_response.bytes);
                }
                if (!current->view.management_supported) {
                    current->view.message = "Catalog refreshed. This server/version has no verified installation API. Install missing models in the server application, then refresh.";
                    emit(current); return;
                }
                TransportRequest probe;
                if (Protocol::make_downloads_request(current->config, probe) != LV_PROTOCOL_OK) { fail(current, "Model-management API is unavailable."); return; }
                send(current, std::move(probe), [](const auto &ready, TransportResponse downloads) {
                    const auto progress = Protocol::parse_install_progress_for_model(downloads.bytes, ready->model);
                    ready->view.management_supported = ok(downloads) && progress.status == LV_PROTOCOL_OK;
                    if (ready->uncertain && ready->view.management_supported) {
                        ready->uncertain = progress.running;
                        if (progress.running) {
                            ready->view.active = true; ready->polls = 0;
                            progress_result(ready, progress); return;
                        }
                    }
                    ready->view.message = ready->view.management_supported ? "Catalog refreshed. Choose models; Set up requests an explicit server-owned download." : "Model installation API unavailable. Install models in the server model manager, then refresh.";
                    emit(ready);
                });
            });
        });
    }
    static void progress_result(const Ptr &s, const ModelInstallProgress &progress) {
        s->view.percent = progress.percent;
        if (progress.complete) { catalog(s, true); return; }
        if (progress.failed || progress.canceled) { fail(s, progress.message); return; }
        if (progress.running) s->polls = 0;
        else ++s->polls;
        if (s->polls >= 5) {
            fail(s, "The server no longer reports this download. It may have restarted. Refresh the catalog and check its model manager before retrying.", true); return;
        }
        s->view.message = progress.message;
        if (progress.percent >= 0) s->view.message += " (" + std::to_string(static_cast<int>(progress.percent)) + "%)";
        emit(s);
        const auto generation = s->generation;
        std::weak_ptr<State> weak = s;
        s->schedule(1000, [weak, generation] {
            const auto state = weak.lock();
            if (!state || !state->alive || state->generation != generation || !state->view.active) return;
            TransportRequest request;
            if (Protocol::make_downloads_request(state->config, request) != LV_PROTOCOL_OK) { fail(state, "Download status is unavailable.", true); return; }
            send(state, std::move(request), [](const auto &current, TransportResponse response) {
                const auto progress = Protocol::parse_install_progress_for_model(response.bytes, current->model);
                if (!ok(response) || progress.status != LV_PROTOCOL_OK) {
                    fail(current, "Server download status lost. Check the server, then refresh. Settings were preserved.", true); return;
                }
                progress_result(current, progress);
            });
        });
    }
    std::shared_ptr<State> state_;
};
} // namespace lemonvox
