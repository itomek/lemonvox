#include "adapters.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <libsoup/soup.h>
#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace lemonvox::ubuntu {

namespace {

struct DeferredCall {
    std::function<void()> callback;
};

gboolean run_deferred(gpointer data) {
    auto *call = static_cast<DeferredCall *>(data);
    call->callback();
    return G_SOURCE_REMOVE;
}

void destroy_deferred(gpointer data) {
    delete static_cast<DeferredCall *>(data);
}

void defer_to(GMainContext *context, std::function<void()> callback) {
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, run_deferred, new DeferredCall{std::move(callback)}, destroy_deferred);
    g_source_attach(source, context);
    g_source_unref(source);
}

bool is_alpha(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool is_digit(char value) {
    return value >= '0' && value <= '9';
}

bool is_hex(char value) {
    return is_digit(value) || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

char lower(char value) {
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

bool is_unreserved(char value) {
    return is_alpha(value) || is_digit(value) || value == '-' || value == '.' || value == '_' || value == '~';
}

bool is_sub_delim(char value) {
    switch (value) {
    case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+': case ',': case ';': case '=':
        return true;
    default:
        return false;
    }
}

bool valid_component(std::string_view text, std::string_view extra) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char value = text[index];
        if (value == '%') {
            if (text.size() - index < 3 || !is_hex(text[index + 1]) || !is_hex(text[index + 2])) return false;
            index += 2;
            continue;
        }
        if (is_unreserved(value) || is_sub_delim(value) || extra.find(value) != std::string_view::npos) continue;
        return false;
    }
    return true;
}

bool parse_host(std::string_view host, std::string &canonical) {
    if (host.empty()) return false;
    canonical.clear();
    if (host.front() == '[') {
        if (host.size() < 4 || host.back() != ']') return false;
        bool colon = false;
        for (std::size_t index = 1; index + 1 < host.size(); ++index) {
            const char value = host[index];
            if (value == ':') colon = true;
            else if (!is_hex(value) && value != '.') return false;
        }
        if (!colon) return false;
        in6_addr address{};
        const std::string literal(host.substr(1, host.size() - 2));
        if (inet_pton(AF_INET6, literal.c_str(), &address) != 1) return false;
        for (char value : host) canonical.push_back(lower(value));
        return true;
    }
    std::size_t label = 0;
    for (char value : host) {
        if (value == '.') {
            if (label == 0) return false;
            label = 0;
        } else if (is_alpha(value) || is_digit(value) || value == '-') {
            if (++label > 63) return false;
        } else {
            return false;
        }
        canonical.push_back(lower(value));
    }
    return label != 0 && canonical.size() <= 253;
}

bool parse_port(std::string_view port, std::string &canonical) {
    if (port.empty() || port.size() > 5 || port.front() == '0') return false;
    unsigned value = 0;
    for (char character : port) {
        if (!is_digit(character)) return false;
        value = value * 10U + static_cast<unsigned>(character - '0');
    }
    if (value == 0 || value > 65535) return false;
    canonical = std::to_string(value);
    return true;
}

} // namespace

bool endpoint_facts(const std::string &endpoint_utf8, ProtocolEndpointFacts &facts) {
    facts = {};
    const std::string_view text(endpoint_utf8);
    if (text.empty() || text.size() > 2048) return false;
    for (char value : text) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte <= 0x20 || byte >= 0x7F || value == '\\' || value == '"' || value == '<' || value == '>' ||
            value == '^' || value == '`' || value == '{' || value == '|' || value == '}') {
            return false;
        }
    }
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || !is_alpha(text.front())) return false;
    std::string scheme;
    for (char value : text.substr(0, colon)) {
        if (!is_alpha(value) && !is_digit(value) && value != '+' && value != '-' && value != '.') return false;
        scheme.push_back(lower(value));
    }
    std::string_view rest = text.substr(colon + 1);
    if (rest.substr(0, 2) != "//") return false;
    rest.remove_prefix(2);
    const std::size_t authority_end = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, authority_end);
    const std::string_view tail = authority_end == std::string_view::npos ? std::string_view() : rest.substr(authority_end);
    bool has_userinfo = false;
    const std::size_t at = authority.rfind('@');
    if (at != std::string_view::npos) {
        if (!valid_component(authority.substr(0, at), ":")) return false;
        has_userinfo = true;
        authority.remove_prefix(at + 1);
    }
    std::string_view host_text = authority;
    std::string_view port_text;
    bool has_port = false;
    if (!authority.empty() && authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return false;
        host_text = authority.substr(0, close + 1);
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return false;
            has_port = true;
            port_text = after.substr(1);
        }
    } else {
        const std::size_t port_colon = authority.find(':');
        if (port_colon != std::string_view::npos) {
            host_text = authority.substr(0, port_colon);
            has_port = true;
            port_text = authority.substr(port_colon + 1);
        }
    }
    std::string host;
    std::string port;
    if (!parse_host(host_text, host) || (has_port && !parse_port(port_text, port))) return false;
    const std::size_t query_start = tail.find_first_of("?#");
    std::string_view path = tail.substr(0, query_start);
    const std::string_view suffix = query_start == std::string_view::npos ? std::string_view() : tail.substr(query_start);
    if (!suffix.empty()) {
        const std::size_t hash = suffix.find('#');
        const std::string_view query = suffix.front() == '?' ? suffix.substr(1, hash == std::string_view::npos
            ? std::string_view::npos : hash - 1) : std::string_view();
        const std::string_view fragment = hash == std::string_view::npos ? std::string_view() : suffix.substr(hash + 1);
        if (!valid_component(query, ":@/?") || !valid_component(fragment, ":@/?")) return false;
    }
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    std::string_view last_segment;
    if (!path.empty()) {
        if (path.front() != '/') return false;
        std::string_view remaining = path.substr(1);
        for (;;) {
            const std::size_t slash = remaining.find('/');
            const std::string_view segment = remaining.substr(0, slash);
            if (segment.empty() || segment == "." || segment == ".." || !valid_component(segment, ":@")) return false;
            last_segment = segment;
            if (slash == std::string_view::npos) break;
            remaining.remove_prefix(slash + 1);
        }
    }
    facts.canonical_base_url = scheme + "://" + host;
    if (has_port) facts.canonical_base_url += ":" + port;
    facts.canonical_base_url.append(path.data(), path.size());
    facts.scheme_allowed = scheme == "http" || scheme == "https";
    facts.host_loopback = host == "127.0.0.1" || host == "localhost" || host == "[::1]";
    facts.no_userinfo = !has_userinfo;
    facts.no_query_fragment = suffix.empty();
    facts.path_ends_v1 = last_segment == "v1";
    return true;
}

bool endpoint_is_valid(const std::string &endpoint_utf8, ProtocolEndpointFacts *facts) {
    ProtocolEndpointFacts parsed;
    const bool valid = endpoint_facts(endpoint_utf8, parsed) && Protocol::validate_endpoint(parsed) == LV_PROTOCOL_OK;
    if (facts) *facts = std::move(parsed);
    return valid;
}

GlibExecutor::GlibExecutor(GMainContext *context)
    : context_(g_main_context_ref(context ? context : g_main_context_default())) {}

GlibExecutor::~GlibExecutor() {
    g_main_context_unref(context_);
}

GMainContext *GlibExecutor::context() const {
    return context_;
}

bool GlibExecutor::invoke(std::function<void()> callback) const {
    if (!callback) return false;
    defer_to(context_, std::move(callback));
    return true;
}

struct PipeWireCaptureAdapter::State {
    explicit State(GMainContext *target, PipeWireCaptureOptions settings)
        : context(g_main_context_ref(target ? target : g_main_context_default())), options(std::move(settings)) {}

    ~State() {
        g_main_context_unref(context);
    }

    mutable std::mutex mutex;
    GMainContext *context = nullptr;
    PipeWireCaptureOptions options;
    GstElement *pipeline = nullptr;
    GstBus *bus = nullptr;
    OperationId active = 0;
    OperationId pending_completion = 0;
    std::uint64_t generation = 0;
    std::uint64_t pending_generation = 0;
    bool stopping = false;
    bool closed = false;
    std::vector<std::uint8_t> pcm;
    BytesResult pending_stop_result;
    BytesCompletion pending_stop_completion;
};

namespace {

void clear_bytes(std::vector<std::uint8_t> &bytes) {
    std::fill(bytes.begin(), bytes.end(), 0);
    std::vector<std::uint8_t>().swap(bytes);
}

void clear_pending_stop(PipeWireCaptureAdapter::State &state) {
    clear_bytes(state.pending_stop_result.bytes);
    state.pending_stop_result = {};
    state.pending_stop_completion = {};
}

void put_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

std::vector<std::uint8_t> canonical_wav(std::vector<std::uint8_t> pcm, std::uint32_t sample_rate,
    std::uint16_t channels) {
    if (pcm.empty() || sample_rate == 0 || channels == 0 || pcm.size() > std::numeric_limits<std::uint32_t>::max() - 36U) {
        clear_bytes(pcm);
        return {};
    }
    const auto size = static_cast<std::uint32_t>(pcm.size());
    std::vector<std::uint8_t> wav;
    wav.reserve(44U + pcm.size());
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    put_u32(wav, 36U + size);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put_u32(wav, 16U);
    put_u16(wav, 1U);
    put_u16(wav, channels);
    put_u32(wav, sample_rate);
    put_u32(wav, sample_rate * static_cast<std::uint32_t>(channels) * 2U);
    put_u16(wav, static_cast<std::uint16_t>(channels * 2U));
    put_u16(wav, 16U);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    put_u32(wav, size);
    wav.insert(wav.end(), pcm.begin(), pcm.end());
    clear_bytes(pcm);
    return wav;
}

std::once_flag gstreamer_once;
std::atomic_bool gstreamer_ready{false};

bool ensure_gstreamer() {
    std::call_once(gstreamer_once, [] {
        GError *error = nullptr;
        gstreamer_ready.store(gst_init_check(nullptr, nullptr, &error));
        if (error) g_error_free(error);
    });
    return gstreamer_ready.load();
}

struct CaptureResources {
    GstElement *pipeline = nullptr;
    GstBus *bus = nullptr;
};

void dispose_capture(CaptureResources resources) {
    if (resources.pipeline) {
        gst_element_set_state(resources.pipeline, GST_STATE_NULL);
        gst_element_get_state(resources.pipeline, nullptr, nullptr, GST_SECOND);
    }
    if (resources.bus) gst_object_unref(resources.bus);
    if (resources.pipeline) gst_object_unref(resources.pipeline);
}

CaptureResources detach_capture(PipeWireCaptureAdapter::State &state) {
    CaptureResources resources;
    resources.pipeline = std::exchange(state.pipeline, nullptr);
    resources.bus = std::exchange(state.bus, nullptr);
    return resources;
}

GstFlowReturn capture_sample(GstAppSink *sink, gpointer data) {
    auto *state = static_cast<PipeWireCaptureAdapter::State *>(data);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map{};
    if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->closed && state->active != 0 && !state->stopping) {
            const std::uint64_t bytes_per_second = static_cast<std::uint64_t>(state->options.sample_rate) *
                static_cast<std::uint64_t>(state->options.channels) * 2U;
            const std::uint64_t bounded = bytes_per_second * state->options.max_capture_seconds;
            const std::size_t maximum = bounded > std::numeric_limits<std::size_t>::max()
                ? std::numeric_limits<std::size_t>::max() : static_cast<std::size_t>(bounded);
            const std::size_t room = state->pcm.size() < maximum ? maximum - state->pcm.size() : 0;
            const std::size_t take = std::min<std::size_t>(room, map.size) & ~std::size_t{1};
            state->pcm.insert(state->pcm.end(), map.data, map.data + take);
        }
        gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

bool make_capture_pipeline(const std::shared_ptr<PipeWireCaptureAdapter::State> &state,
    CaptureResources &resources) {
    PipeWireCaptureOptions options;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        options = state->options;
    }
    if (!ensure_gstreamer() || options.source_factory.empty() || options.sample_rate == 0 || options.channels == 0 ||
        options.max_capture_seconds == 0) {
        return false;
    }
    GstElement *pipeline = gst_pipeline_new("lemonvox-capture");
    GstElement *source = gst_element_factory_make(options.source_factory.c_str(), "source");
    GstElement *convert = gst_element_factory_make("audioconvert", "convert");
    GstElement *resample = gst_element_factory_make("audioresample", "resample");
    GstElement *filter = gst_element_factory_make("capsfilter", "format");
    GstElement *sink = gst_element_factory_make("appsink", "sink");
    if (!pipeline || !source || !convert || !resample || !filter || !sink) {
        if (pipeline) gst_object_unref(pipeline);
        if (source) gst_object_unref(source);
        if (convert) gst_object_unref(convert);
        if (resample) gst_object_unref(resample);
        if (filter) gst_object_unref(filter);
        if (sink) gst_object_unref(sink);
        return false;
    }
    if (!options.source_device.empty() && g_object_class_find_property(G_OBJECT_GET_CLASS(source), "device")) {
        g_object_set(source, "device", options.source_device.c_str(), nullptr);
    }
    GstCaps *caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "rate", G_TYPE_INT,
        static_cast<gint>(options.sample_rate), "channels", G_TYPE_INT, static_cast<gint>(options.channels), nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(sink, "sync", FALSE, "emit-signals", FALSE, "max-buffers", 32U, "drop", TRUE, nullptr);
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = capture_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, state.get(), nullptr);
    gst_bin_add_many(GST_BIN(pipeline), source, convert, resample, filter, sink, nullptr);
    if (!gst_element_link_many(source, convert, resample, filter, sink, nullptr)) {
        gst_object_unref(pipeline);
        return false;
    }
    // Pipeline-held state keeps the appsink callback pointer valid during
    // teardown, even if the host releases the adapter after shutdown().
    g_object_set_data_full(G_OBJECT(pipeline), "lemonvox-capture-state",
        new std::shared_ptr<PipeWireCaptureAdapter::State>(state),
        [](gpointer value) { delete static_cast<std::shared_ptr<PipeWireCaptureAdapter::State> *>(value); });
    resources.pipeline = pipeline;
    resources.bus = gst_element_get_bus(pipeline);
    return resources.bus != nullptr;
}

bool capture_has_error(GstBus *bus) {
    if (!bus) return true;
    bool failed = false;
    while (GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
        failed = true;
        gst_message_unref(message);
    }
    return failed;
}

template <typename Callback>
void defer_capture(const std::shared_ptr<PipeWireCaptureAdapter::State> &state, std::uint64_t generation,
    Callback callback) {
    defer_to(state->context, [state, generation, callback = std::move(callback)]() mutable {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closed || state->generation != generation) return;
        }
        callback();
    });
}

template <typename Callback>
void defer_capture_open(const std::shared_ptr<PipeWireCaptureAdapter::State> &state, Callback callback) {
    defer_to(state->context, [state, callback = std::move(callback)]() mutable {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closed) return;
        }
        callback();
    });
}

} // namespace

PipeWireCaptureAdapter::PipeWireCaptureAdapter(GMainContext *context, PipeWireCaptureOptions options)
    : state_(std::make_shared<State>(context, std::move(options))) {}

PipeWireCaptureAdapter::PipeWireCaptureAdapter(const GlibExecutor &executor, PipeWireCaptureOptions options)
    : PipeWireCaptureAdapter(executor.context(), std::move(options)) {}

PipeWireCaptureAdapter::~PipeWireCaptureAdapter() {
    shutdown();
}

void PipeWireCaptureAdapter::start(OperationId operation, VoidCompletion completion) {
    auto state = state_;
    if (!state) return;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return;
        if (operation == 0 || state->active != 0) {
            defer_capture_open(state, [operation, completion = std::move(completion)]() mutable {
                completion(operation, {AdapterError::invalid_input});
            });
            return;
        }
        clear_pending_stop(*state);
        state->pending_completion = 0;
        state->pending_generation = 0;
        state->active = operation;
        state->stopping = false;
        clear_bytes(state->pcm);
        generation = ++state->generation;
    }
    CaptureResources resources;
    const bool pipeline_ready = make_capture_pipeline(state, resources);
    const GstStateChangeReturn changed = pipeline_ready
        ? gst_element_set_state(resources.pipeline, GST_STATE_PLAYING) : GST_STATE_CHANGE_FAILURE;
    const GstStateChangeReturn settled = changed == GST_STATE_CHANGE_FAILURE ? GST_STATE_CHANGE_FAILURE
        : gst_element_get_state(resources.pipeline, nullptr, nullptr, 100 * GST_MSECOND);
    if (!pipeline_ready || changed == GST_STATE_CHANGE_FAILURE || settled == GST_STATE_CHANGE_FAILURE ||
        capture_has_error(resources.bus)) {
        dispose_capture(resources);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->generation == generation) {
                state->active = 0;
                state->pending_completion = operation;
                state->pending_generation = generation;
            }
        }
        defer_capture(state, generation, [state, generation, operation, completion = std::move(completion)]() mutable {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->pending_completion == operation && state->pending_generation == generation) {
                    state->pending_completion = 0;
                }
            }
            completion(operation, {AdapterError::unavailable});
        });
        return;
    }
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->closed && state->generation == generation && state->active == operation) {
            state->pipeline = resources.pipeline;
            state->bus = resources.bus;
            resources = {};
            accepted = true;
        }
    }
    if (!accepted) {
        dispose_capture(resources);
        return;
    }
    defer_capture(state, generation, [operation, completion = std::move(completion)]() mutable {
        completion(operation, {AdapterError::none});
    });
}

void PipeWireCaptureAdapter::stop(OperationId operation, BytesCompletion completion) {
    auto state = state_;
    if (!state) return;
    std::uint64_t generation = 0;
    CaptureResources resources;
    std::vector<std::uint8_t> pcm;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return;
        if (operation == 0 || state->active != operation || state->stopping) {
            defer_capture_open(state, [operation, completion = std::move(completion)]() mutable {
                completion(operation, {AdapterError::invalid_input, {}});
            });
            return;
        }
        state->stopping = true;
        generation = state->generation;
        resources = detach_capture(*state);
        pcm = std::move(state->pcm);
        sample_rate = state->options.sample_rate;
        channels = state->options.channels;
        state->active = 0;
        state->pending_completion = operation;
        state->pending_generation = generation;
    }
    const bool failed = capture_has_error(resources.bus);
    dispose_capture(resources);
    BytesResult result;
    if (failed) clear_bytes(pcm);
    else result.bytes = canonical_wav(std::move(pcm), sample_rate, channels);
    if (failed) result.error = AdapterError::unavailable;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->closed && state->generation == generation && state->pending_completion == operation &&
            state->pending_generation == generation) {
            state->stopping = false;
            clear_pending_stop(*state);
            state->pending_stop_result = std::move(result);
            state->pending_stop_completion = std::move(completion);
            queued = true;
        }
    }
    if (!queued) {
        clear_bytes(result.bytes);
        return;
    }
    std::weak_ptr<State> weak_state = state;
    defer_to(state->context, [weak_state, generation, operation] {
        const auto state = weak_state.lock();
        if (!state) return;
        BytesCompletion completion;
        BytesResult result;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closed || state->generation != generation || state->pending_completion != operation ||
                state->pending_generation != generation) return;
            state->pending_completion = 0;
            state->pending_generation = 0;
            completion = std::move(state->pending_stop_completion);
            result = std::move(state->pending_stop_result);
            state->pending_stop_result = {};
        }
        if (completion) completion(operation, std::move(result));
    });
}

void PipeWireCaptureAdapter::cancel(OperationId operation) {
    auto state = state_;
    if (!state) return;
    CaptureResources resources;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed || (state->active != operation && state->pending_completion != operation)) return;
        ++state->generation;
        state->active = 0;
        state->pending_completion = 0;
        state->pending_generation = 0;
        state->stopping = false;
        clear_bytes(state->pcm);
        clear_pending_stop(*state);
        resources = detach_capture(*state);
    }
    dispose_capture(resources);
}

void PipeWireCaptureAdapter::shutdown() {
    auto state = state_;
    if (!state) return;
    CaptureResources resources;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return;
        state->closed = true;
        ++state->generation;
        state->active = 0;
        state->pending_completion = 0;
        state->pending_generation = 0;
        state->stopping = false;
        clear_bytes(state->pcm);
        clear_pending_stop(*state);
        resources = detach_capture(*state);
    }
    dispose_capture(resources);
}

void PipeWireCaptureAdapter::set_source_device(std::string device) {
    auto state = state_;
    if (!state) return;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->closed && state->active == 0) state->options.source_device = std::move(device);
}

std::string PipeWireCaptureAdapter::source_device() const {
    auto state = state_;
    if (!state) return {};
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->options.source_device;
}

struct SoupByteTransport::State {
    struct Request;

    explicit State(GMainContext *target, SoupTransportOptions settings)
        : context(g_main_context_ref(target ? target : g_main_context_default())), options(std::move(settings)) {
        GProxyResolver *resolver = g_simple_proxy_resolver_new(nullptr, nullptr);
        session = soup_session_new();
        soup_session_set_proxy_resolver(session, resolver);
        soup_session_set_timeout(session, static_cast<guint>(std::max<std::uint32_t>(1, options.default_timeout_ms / 1000U)));
        soup_session_set_idle_timeout(session, 15U);
        soup_session_set_user_agent(session, "LemonVox");
        g_object_unref(resolver);
        soup_session_remove_feature_by_type(session, SOUP_TYPE_AUTH_MANAGER);
    }

    ~State();

    mutable std::mutex mutex;
    GMainContext *context = nullptr;
    SoupTransportOptions options;
    SoupSession *session = nullptr;
    bool closed = false;
    std::uint64_t next_key = 1;
    std::map<std::uint64_t, std::shared_ptr<Request>> requests;
};

struct SoupByteTransport::State::Request {
    std::weak_ptr<State> state;
    std::uint64_t key = 0;
    OperationId operation = 0;
    TransportCompletion completion;
    SoupMessage *message = nullptr;
    GCancellable *cancellable = nullptr;
    GInputStream *stream = nullptr;
    GSource *timeout = nullptr;
    std::vector<std::uint8_t> response_bytes;
    TransportResponse response;
    bool canceled = false;
    bool finished = false;
};

namespace {

bool valid_header_name(const std::string &name) {
    if (name.empty()) return false;
    for (char value : name) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte <= 0x20 || byte >= 0x7F || value == ':' || value == '(' || value == ')' || value == '<' ||
            value == '>' || value == '@' || value == ',' || value == ';' || value == '\\' || value == '"' ||
            value == '/' || value == '[' || value == ']' || value == '?' || value == '=' || value == '{' ||
            value == '}') {
            return false;
        }
    }
    return true;
}

bool valid_header_value(const std::string &value) {
    return value.find('\r') == std::string::npos && value.find('\n') == std::string::npos &&
        value.find('\0') == std::string::npos;
}

bool request_url_allowed(const std::string &url) {
    ProtocolEndpointFacts facts;
    return endpoint_facts(url, facts) && facts.scheme_allowed && facts.no_userinfo &&
        facts.no_query_fragment;
}

bool equal_ascii_case_insensitive(const std::string &left, const char *right) {
    const std::size_t length = std::strlen(right);
    if (left.size() != length) return false;
    for (std::size_t index = 0; index < length; ++index) {
        if (lower(left[index]) != lower(right[index])) return false;
    }
    return true;
}

using TransportState = SoupByteTransport::State;
using NativeRequest = SoupByteTransport::State::Request;

constexpr std::size_t kTransportReadChunkBytes = 64U * 1024U;

// Native GLib APIs retain only opaque user-data pointers. These capsules keep
// no State ownership, so shutdown can tear State down even when its context is
// no longer dispatching cancellation callbacks.
struct WeakRequestCallback final {
    explicit WeakRequestCallback(const std::shared_ptr<NativeRequest> &value) : request(value) {}
    std::weak_ptr<NativeRequest> request;
};

struct ReadRequestCallback final {
    explicit ReadRequestCallback(const std::shared_ptr<NativeRequest> &value) : request(value) {}
    ~ReadRequestCallback() { std::fill(chunk.begin(), chunk.end(), 0); }

    std::weak_ptr<NativeRequest> request;
    std::array<std::uint8_t, kTransportReadChunkBytes> chunk{};
};

void destroy_weak_request_callback(gpointer data) {
    delete static_cast<WeakRequestCallback *>(data);
}

bool request_is_current_locked(const TransportState &state, const std::shared_ptr<NativeRequest> &request,
    bool allow_finished = false) {
    if (state.closed || request->canceled || (!allow_finished && request->finished)) return false;
    const auto found = state.requests.find(request->key);
    return found != state.requests.end() && found->second.get() == request.get();
}

bool request_is_current(const std::shared_ptr<NativeRequest> &request) {
    const auto state = request->state.lock();
    if (!state) return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    return request_is_current_locked(*state, request);
}

void clear_transport_response(TransportResponse &response) {
    clear_bytes(response.bytes);
    response = {};
}

void release_timeout(NativeRequest &request) {
    if (!request.timeout) return;
    g_source_destroy(request.timeout);
    g_source_unref(request.timeout);
    request.timeout = nullptr;
}

void release_native_request(NativeRequest &request) {
    clear_bytes(request.response_bytes);
    if (request.stream) {
        g_object_unref(request.stream);
        request.stream = nullptr;
    }
    if (request.message) {
        g_object_unref(request.message);
        request.message = nullptr;
    }
    if (request.cancellable) {
        g_object_unref(request.cancellable);
        request.cancellable = nullptr;
    }
}

void abandon_transport_request(NativeRequest &request) {
    request.canceled = true;
    request.finished = true;
    release_timeout(request);
    if (request.cancellable) g_cancellable_cancel(request.cancellable);
    release_native_request(request);
    clear_transport_response(request.response);
    request.completion = {};
}

void deliver_transport(const std::shared_ptr<NativeRequest> &request) {
    const auto state = request->state.lock();
    if (!state) {
        clear_transport_response(request->response);
        request->completion = {};
        return;
    }
    GMainContext *context = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!request_is_current_locked(*state, request, true)) {
            clear_transport_response(request->response);
            request->completion = {};
            return;
        }
        context = state->context;
    }
    std::weak_ptr<TransportState> weak_state = state;
    defer_to(context, [weak_state, request] {
        const auto state = weak_state.lock();
        if (!state) {
            clear_transport_response(request->response);
            request->completion = {};
            return;
        }
        TransportCompletion completion;
        TransportResponse response;
        OperationId operation = 0;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto found = state->requests.find(request->key);
            if (found == state->requests.end() || found->second.get() != request.get()) return;
            state->requests.erase(found);
            if (state->closed || request->canceled) {
                clear_transport_response(request->response);
                request->completion = {};
                return;
            }
            completion = std::move(request->completion);
            response = std::move(request->response);
            operation = request->operation;
        }
        if (completion) completion(operation, std::move(response));
    });
}

void finish_transport(const std::shared_ptr<NativeRequest> &request, TransportResponse response) {
    const auto state = request->state.lock();
    if (!state) {
        clear_transport_response(response);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!request_is_current_locked(*state, request)) {
            clear_transport_response(response);
            return;
        }
        request->finished = true;
        request->response = std::move(response);
        release_timeout(*request);
    }
    deliver_transport(request);
}

gboolean transport_timeout(gpointer data) {
    const auto request = static_cast<WeakRequestCallback *>(data)->request.lock();
    if (!request) return G_SOURCE_REMOVE;
    const auto state = request->state.lock();
    if (!state) return G_SOURCE_REMOVE;
    GCancellable *cancellable = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!request_is_current_locked(*state, request) || !request->cancellable) return G_SOURCE_REMOVE;
        cancellable = G_CANCELLABLE(g_object_ref(request->cancellable));
    }
    g_cancellable_cancel(cancellable);
    g_object_unref(cancellable);
    return G_SOURCE_REMOVE;
}

void read_transport_chunk(const std::shared_ptr<NativeRequest> &request);

void transport_read_ready(GObject *source, GAsyncResult *result, gpointer data) {
    std::unique_ptr<ReadRequestCallback> callback(static_cast<ReadRequestCallback *>(data));
    GError *error = nullptr;
    const gssize count = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);
    const auto request = callback->request.lock();
    if (!request || !request_is_current(request)) {
        if (error) g_error_free(error);
        return;
    }
    TransportResponse response;
    if (error) {
        response.error = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
            ? AdapterError::canceled : AdapterError::network;
        g_error_free(error);
        release_native_request(*request);
        finish_transport(request, std::move(response));
        return;
    }
    if (count < 0) {
        response.error = AdapterError::network;
        release_native_request(*request);
        finish_transport(request, std::move(response));
        return;
    }
    if (count == 0) {
        const guint status = request->message ? static_cast<guint>(soup_message_get_status(request->message)) : 0U;
        if (status < 100U || status > 999U) response.error = AdapterError::response;
        else {
            response.status = static_cast<int>(status);
            response.bytes = std::move(request->response_bytes);
        }
        release_native_request(*request);
        finish_transport(request, std::move(response));
        return;
    }
    const std::size_t size = static_cast<std::size_t>(count);
    const std::size_t used = request->response_bytes.size();
    const auto state = request->state.lock();
    if (!state) return;
    const std::size_t maximum = state->options.max_response_bytes;
    if (size > maximum || used > maximum - size) {
        response.error = AdapterError::response;
        g_cancellable_cancel(request->cancellable);
        release_native_request(*request);
        finish_transport(request, std::move(response));
        return;
    }
    request->response_bytes.insert(request->response_bytes.end(), callback->chunk.begin(), callback->chunk.begin() + count);
    read_transport_chunk(request);
}

void read_transport_chunk(const std::shared_ptr<NativeRequest> &request) {
    const auto state = request->state.lock();
    if (!state) return;
    GInputStream *stream = nullptr;
    GCancellable *cancellable = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!request_is_current_locked(*state, request) || !request->stream || !request->cancellable) return;
        stream = G_INPUT_STREAM(g_object_ref(request->stream));
        cancellable = G_CANCELLABLE(g_object_ref(request->cancellable));
    }
    auto callback = std::make_unique<ReadRequestCallback>(request);
    auto *raw_callback = callback.get();
    g_input_stream_read_async(stream, raw_callback->chunk.data(), raw_callback->chunk.size(), G_PRIORITY_DEFAULT,
        cancellable, transport_read_ready, raw_callback);
    callback.release();
    g_object_unref(cancellable);
    g_object_unref(stream);
}

void transport_send_ready(GObject *source, GAsyncResult *result, gpointer data) {
    std::unique_ptr<WeakRequestCallback> callback(static_cast<WeakRequestCallback *>(data));
    GError *error = nullptr;
    GInputStream *stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
    const auto request = callback->request.lock();
    if (!request || !request_is_current(request)) {
        if (stream) g_object_unref(stream);
        if (error) g_error_free(error);
        return;
    }
    TransportResponse response;
    if (error || !stream) {
        response.error = error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
            ? AdapterError::canceled : AdapterError::network;
        if (error) g_error_free(error);
        if (stream) g_object_unref(stream);
        release_native_request(*request);
        finish_transport(request, std::move(response));
        return;
    }
    const auto state = request->state.lock();
    if (!state) {
        g_object_unref(stream);
        return;
    }
    SoupMessage *message = nullptr;
    std::size_t maximum = 0;
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (request_is_current_locked(*state, request)) {
            request->stream = stream;
            stream = nullptr;
            message = request->message ? SOUP_MESSAGE(g_object_ref(request->message)) : nullptr;
            maximum = state->options.max_response_bytes;
            accepted = true;
        }
    }
    if (stream) g_object_unref(stream);
    if (!accepted) return;
    if (!message) {
        response.error = AdapterError::network;
        release_native_request(*request);
        finish_transport(request, std::move(response));
        return;
    }
    const goffset content_length = soup_message_headers_get_content_length(
        soup_message_get_response_headers(message));
    g_object_unref(message);
    if (content_length > static_cast<goffset>(maximum)) {
        response.error = AdapterError::response;
        g_cancellable_cancel(request->cancellable);
        release_native_request(*request);
        finish_transport(request, std::move(response));
        return;
    }
    read_transport_chunk(request);
}

TransportResponse transport_error(AdapterError error) {
    TransportResponse response;
    response.error = error;
    return response;
}

} // namespace

SoupByteTransport::State::~State() {
    std::vector<std::shared_ptr<Request>> detached;
    SoupSession *closing_session = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        closed = true;
        closing_session = std::exchange(session, nullptr);
        for (const auto &entry : requests) detached.push_back(entry.second);
        requests.clear();
    }
    for (const auto &request : detached) abandon_transport_request(*request);
    if (closing_session) {
        soup_session_abort(closing_session);
        g_object_unref(closing_session);
    }
    g_main_context_unref(context);
}

SoupByteTransport::SoupByteTransport(GMainContext *context, SoupTransportOptions options)
    : state_(std::make_shared<State>(context, std::move(options))) {}

SoupByteTransport::SoupByteTransport(const GlibExecutor &executor, SoupTransportOptions options)
    : SoupByteTransport(executor.context(), std::move(options)) {}

SoupByteTransport::~SoupByteTransport() {
    shutdown();
}

void SoupByteTransport::send(OperationId operation, TransportRequest request, TransportCompletion completion) {
    const auto state = state_;
    if (!state) return;
    auto native = std::make_shared<State::Request>();
    native->state = state;
    native->operation = operation;
    native->completion = std::move(completion);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return;
        native->key = state->next_key++;
        state->requests.emplace(native->key, native);
    }
    const bool headers_valid = std::all_of(request.headers.begin(), request.headers.end(), [](const TransportHeader &header) {
        return valid_header_name(header.name) && valid_header_value(header.value);
    });
    if (operation == 0 || !headers_valid || !request_url_allowed(request.url)) {
        finish_transport(native, transport_error(AdapterError::invalid_input));
        return;
    }
    native->message = soup_message_new(request.method == HttpMethod::get ? SOUP_METHOD_GET : SOUP_METHOD_POST,
        request.url.c_str());
    if (!native->message) {
        finish_transport(native, transport_error(AdapterError::invalid_input));
        return;
    }
    soup_message_add_flags(native->message,
        static_cast<SoupMessageFlags>(SOUP_MESSAGE_NO_REDIRECT | SOUP_MESSAGE_DO_NOT_USE_AUTH_CACHE));
    std::string content_type;
    for (const auto &header : request.headers) {
        if (equal_ascii_case_insensitive(header.name, "Content-Type")) content_type = header.value;
        else soup_message_headers_append(soup_message_get_request_headers(native->message), header.name.c_str(), header.value.c_str());
    }
    if (request.method == HttpMethod::post) {
        GBytes *body = g_bytes_new(request.body.empty() ? nullptr : request.body.data(), request.body.size());
        soup_message_set_request_body_from_bytes(native->message,
            content_type.empty() ? nullptr : content_type.c_str(), body);
        g_bytes_unref(body);
    }
    native->cancellable = g_cancellable_new();
    const std::uint32_t timeout_ms = request.timeout_ms == 0 ? state->options.default_timeout_ms : request.timeout_ms;
    native->timeout = g_timeout_source_new(std::max<std::uint32_t>(1, timeout_ms));
    g_source_set_callback(native->timeout, transport_timeout, new WeakRequestCallback(native),
        destroy_weak_request_callback);
    g_source_attach(native->timeout, state->context);
    SoupSession *session = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (request_is_current_locked(*state, native) && state->session) {
            session = SOUP_SESSION(g_object_ref(state->session));
        }
    }
    if (!session) return;
    soup_session_send_async(session, native->message, G_PRIORITY_DEFAULT, native->cancellable,
        transport_send_ready, new WeakRequestCallback(native));
    g_object_unref(session);
}

void SoupByteTransport::cancel(OperationId operation) {
    const auto state = state_;
    if (!state) return;
    std::vector<std::shared_ptr<State::Request>> canceled;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return;
        for (auto entry = state->requests.begin(); entry != state->requests.end();) {
            if (entry->second->operation != operation) {
                ++entry;
                continue;
            }
            canceled.push_back(entry->second);
            entry = state->requests.erase(entry);
        }
    }
    for (const auto &request : canceled) abandon_transport_request(*request);
}

void SoupByteTransport::shutdown() {
    const auto state = state_;
    if (!state) return;
    std::vector<std::shared_ptr<State::Request>> canceled;
    SoupSession *session = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return;
        state->closed = true;
        session = std::exchange(state->session, nullptr);
        for (const auto &entry : state->requests) canceled.push_back(entry.second);
        state->requests.clear();
    }
    for (const auto &request : canceled) abandon_transport_request(*request);
    if (session) {
        soup_session_abort(session);
        g_object_unref(session);
    }
}

struct ManualCopyInsertionAdapter::State {
    explicit State(GMainContext *target) : context(g_main_context_ref(target ? target : g_main_context_default())) {}
    ~State() { g_main_context_unref(context); }

    mutable std::mutex mutex;
    GMainContext *context = nullptr;
    bool closed = false;
    std::uint64_t next = 1;
    std::map<OperationId, std::uint64_t> pending;
};

namespace {

template <typename Callback>
void defer_manual(const std::shared_ptr<ManualCopyInsertionAdapter::State> &state, OperationId operation,
    Callback callback) {
    std::uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return;
        token = state->next++;
        state->pending[operation] = token;
    }
    defer_to(state->context, [state, operation, token, callback = std::move(callback)]() mutable {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto found = state->pending.find(operation);
            if (state->closed || found == state->pending.end() || found->second != token) return;
            state->pending.erase(found);
        }
        callback();
    });
}

} // namespace

ManualCopyInsertionAdapter::ManualCopyInsertionAdapter(GMainContext *context)
    : state_(std::make_shared<State>(context)) {}

ManualCopyInsertionAdapter::ManualCopyInsertionAdapter(const GlibExecutor &executor)
    : ManualCopyInsertionAdapter(executor.context()) {}

ManualCopyInsertionAdapter::~ManualCopyInsertionAdapter() {
    shutdown();
}

void ManualCopyInsertionAdapter::capture_original(OperationId operation, TargetCompletion completion) {
    const auto state = state_;
    if (!state) return;
    defer_manual(state, operation, [operation, completion = std::move(completion)]() mutable {
        completion(operation, {AdapterError::insertion_unavailable, {}});
    });
}

void ManualCopyInsertionAdapter::inspect(OperationId operation, TargetToken, TargetCompletion completion) {
    const auto state = state_;
    if (!state) return;
    defer_manual(state, operation, [operation, completion = std::move(completion)]() mutable {
        completion(operation, {AdapterError::insertion_unavailable, {}});
    });
}

void ManualCopyInsertionAdapter::insert(OperationId operation, TargetToken, std::string,
    VoidCompletion completion) {
    const auto state = state_;
    if (!state) return;
    defer_manual(state, operation, [operation, completion = std::move(completion)]() mutable {
        completion(operation, {AdapterError::insertion_unavailable});
    });
}

void ManualCopyInsertionAdapter::cancel(OperationId operation) {
    const auto state = state_;
    if (!state) return;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->pending.erase(operation);
}

void ManualCopyInsertionAdapter::shutdown() {
    const auto state = state_;
    if (!state) return;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->closed = true;
    state->pending.clear();
}

} // namespace lemonvox::ubuntu
