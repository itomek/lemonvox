#include "net/endpoint_facts.h"

#include <ws2tcpip.h>

#include <cstddef>
#include <string_view>

namespace lemonvox::win {

namespace {

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool is_unreserved(char c) { return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~'; }
bool is_sub_delim(char c) {
    switch (c) {
    case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+': case ',': case ';': case '=':
        return true;
    default:
        return false;
    }
}

// Validates RFC 3986 characters with well-formed percent escapes.
bool valid_component(std::string_view text, std::string_view extra) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '%') {
            if (text.size() - index < 3) return false;
            if (!is_hex(text[index + 1]) || !is_hex(text[index + 2])) return false;
            index += 2;
            continue;
        }
        if (is_unreserved(c) || is_sub_delim(c) || extra.find(c) != std::string_view::npos) continue;
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
            const char c = host[index];
            if (c == ':') colon = true;
            else if (!is_hex(c) && c != '.') return false;
        }
        if (!colon) return false;
        IN6_ADDR address{};
        const std::string literal(host.substr(1, host.size() - 2));
        if (InetPtonA(AF_INET6, literal.c_str(), &address) != 1) return false;
        for (char c : host) canonical.push_back(lower(c));
        return true;
    }
    // A registered name or IPv4 literal: dot-separated non-empty labels.
    std::size_t label = 0;
    for (char c : host) {
        if (c == '.') {
            if (label == 0) return false;
            label = 0;
        } else if (is_alpha(c) || is_digit(c) || c == '-') {
            if (++label > 63) return false;
        } else {
            return false;
        }
        canonical.push_back(lower(c));
    }
    return label != 0 && canonical.size() <= 253;
}

bool parse_port(std::string_view port, std::string &canonical) {
    if (port.empty() || port.size() > 5 || port.front() == '0') return false;
    unsigned value = 0;
    for (char c : port) {
        if (!is_digit(c)) return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value == 0 || value > 65535) return false;
    canonical = std::to_string(value);
    return true;
}

} // namespace

bool endpoint_facts(const std::string &endpoint_utf8, lemonvox::ProtocolEndpointFacts &facts) {
    facts = {};
    const std::string_view text(endpoint_utf8);
    if (text.empty() || text.size() > 2048) return false;
    for (char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        // Rejects whitespace, controls, DEL, non-ASCII, and characters RFC 3986 never allows.
        if (byte <= 0x20 || byte >= 0x7F || c == '\\' || c == '"' || c == '<' || c == '>' || c == '^' ||
            c == '`' || c == '{' || c == '|' || c == '}') {
            return false;
        }
    }

    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || !is_alpha(text[0])) return false;
    std::string scheme;
    for (char c : text.substr(0, colon)) {
        if (!is_alpha(c) && !is_digit(c) && c != '+' && c != '-' && c != '.') return false;
        scheme.push_back(lower(c));
    }
    std::string_view rest = text.substr(colon + 1);
    if (rest.substr(0, 2) != "//") return false;
    rest.remove_prefix(2);

    const std::size_t authority_end = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, authority_end);
    std::string_view tail = authority_end == std::string_view::npos ? std::string_view() : rest.substr(authority_end);

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
    if (!parse_host(host_text, host)) return false;
    if (has_port && !parse_port(port_text, port)) return false;

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
            if (segment.empty() || segment == "." || segment == ".." || !valid_component(segment, ":@")) {
                return false;
            }
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

bool endpoint_is_valid(const std::string &endpoint_utf8, lemonvox::ProtocolEndpointFacts *facts) {
    lemonvox::ProtocolEndpointFacts parsed;
    const bool valid = endpoint_facts(endpoint_utf8, parsed) &&
        lemonvox::Protocol::validate_endpoint(parsed) == LV_PROTOCOL_OK;
    if (facts) *facts = std::move(parsed);
    return valid;
}

} // namespace lemonvox::win
