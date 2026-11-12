#pragma once

#include "../core/adapters.h"
#include "../core/protocol.h"

#include <glib.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace lemonvox::ubuntu {

// Normalizes a user-entered Lemonade base URL without doing I/O. The result
// follows the shared core's endpoint rules: explicit local or remote http(s),
// no userinfo/query/fragment, and a final /v1 segment. Invalid input resets
// facts and returns false.
bool endpoint_facts(const std::string &endpoint_utf8, ProtocolEndpointFacts &facts);
bool endpoint_is_valid(const std::string &endpoint_utf8, ProtocolEndpointFacts *facts = nullptr);

// Lightweight owner for the GLib context on which the host runs WorkflowCore.
// It refs but does not run or otherwise take ownership of the application's
// default context. Adapters use it only to marshal native completions.
class GlibExecutor final {
public:
    explicit GlibExecutor(GMainContext *context = nullptr);
    ~GlibExecutor();
    GlibExecutor(const GlibExecutor &) = delete;
    GlibExecutor &operator=(const GlibExecutor &) = delete;

    GMainContext *context() const;
    bool invoke(std::function<void()> callback) const;

private:
    GMainContext *context_ = nullptr;
};

// The source factory defaults to pulsesrc, which resolves the desktop's
// ordinary capture source through PipeWire's PulseAudio service. Set
// source_device to an exact PulseAudio source name (including a virtual sink
// monitor) for an explicit, isolated test input. `audiotestsrc` is also useful
// for deterministic adapter tests and ignores source_device because it has no
// device property.
struct PipeWireCaptureOptions {
    std::string source_factory = "pulsesrc";
    std::string source_device;
    std::uint32_t max_capture_seconds = 120;
    std::uint32_t sample_rate = 16000;
    std::uint16_t channels = 1;
};

// All calls and completions run on `context`; a null context means the GLib
// default context. The app owns that context. The adapter writes no audio to
// disk. stop() returns a canonical 16 kHz mono S16LE RIFF/WAVE buffer.
class PipeWireCaptureAdapter final : public CaptureAdapter {
public:
    struct State;
    explicit PipeWireCaptureAdapter(GMainContext *context = nullptr,
        PipeWireCaptureOptions options = {});
    explicit PipeWireCaptureAdapter(const GlibExecutor &executor,
        PipeWireCaptureOptions options = {});
    ~PipeWireCaptureAdapter() override;
    PipeWireCaptureAdapter(const PipeWireCaptureAdapter &) = delete;
    PipeWireCaptureAdapter &operator=(const PipeWireCaptureAdapter &) = delete;

    void start(OperationId operation, VoidCompletion completion) override;
    void stop(OperationId operation, BytesCompletion completion) override;
    void cancel(OperationId operation) override;
    void shutdown() override;

    void set_source_device(std::string device);
    std::string source_device() const;

private:
    std::shared_ptr<State> state_;
};

struct SoupTransportOptions {
    std::size_t max_response_bytes = 8U * 1024U * 1024U;
    std::uint32_t default_timeout_ms = 60000;
};

// ByteTransport backed by libsoup. It uses a direct proxy resolver, disables
// redirects, never adds a cookie jar or credential storage, accepts only
// local or remote HTTP(S) request URLs, and limits each owned response body.
class SoupByteTransport final : public ByteTransport {
public:
    struct State;
    explicit SoupByteTransport(GMainContext *context = nullptr,
        SoupTransportOptions options = {});
    explicit SoupByteTransport(const GlibExecutor &executor,
        SoupTransportOptions options = {});
    ~SoupByteTransport() override;
    SoupByteTransport(const SoupByteTransport &) = delete;
    SoupByteTransport &operator=(const SoupByteTransport &) = delete;

    void send(OperationId operation, TransportRequest request,
        TransportCompletion completion) override;
    void cancel(OperationId operation) override;
    void shutdown() override;

private:
    std::shared_ptr<State> state_;
};

// Wayland provides no safe, generic cross-application target inspection or
// insertion API on the supported GNOME session. This adapter deliberately
// reports insertion_unavailable at target capture, causing WorkflowCore to
// emit its explicit Copy fallback through text_ready rather than guessing.
class ManualCopyInsertionAdapter final : public InsertionAdapter {
public:
    struct State;
    explicit ManualCopyInsertionAdapter(GMainContext *context = nullptr);
    explicit ManualCopyInsertionAdapter(const GlibExecutor &executor);
    ~ManualCopyInsertionAdapter() override;
    ManualCopyInsertionAdapter(const ManualCopyInsertionAdapter &) = delete;
    ManualCopyInsertionAdapter &operator=(const ManualCopyInsertionAdapter &) = delete;

    void capture_original(OperationId operation, TargetCompletion completion) override;
    void inspect(OperationId operation, TargetToken original, TargetCompletion completion) override;
    void insert(OperationId operation, TargetToken target, std::string text,
        VoidCompletion completion) override;
    void cancel(OperationId operation) override;
    void shutdown() override;

private:
    std::shared_ptr<State> state_;
};

} // namespace lemonvox::ubuntu
