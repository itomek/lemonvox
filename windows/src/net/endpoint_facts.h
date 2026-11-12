#pragma once

#include "protocol.h"

#include <string>

namespace lemonvox::win {

// Parses a user-entered Lemonade endpoint the way the Mac shell's
// URLComponents bridge does, but strictly: only printable ASCII, an explicit
// "scheme://authority" form, a well-formed host, an optional decimal port
// 1-65535, and a path of RFC 3986 characters without empty, "." or ".."
// segments. Returns false (facts reset) when the text is not such a URL.
//
// On success the facts describe the URL; the caller must still run
// lemonvox::Protocol::validate_endpoint, which rejects non-http(s) schemes,
// userinfo, query/fragment and paths not ending in "v1".
// canonical_base_url is "<scheme>://<host>[:<port>]<path>" with the scheme and
// host lowercased and trailing slashes removed, e.g. "http://127.0.0.1:13305/v1";
// the core appends "/models" etc. to it.
bool endpoint_facts(const std::string &endpoint_utf8, lemonvox::ProtocolEndpointFacts &facts);

// endpoint_facts() followed by Protocol::validate_endpoint() == LV_PROTOCOL_OK.
bool endpoint_is_valid(const std::string &endpoint_utf8, lemonvox::ProtocolEndpointFacts *facts = nullptr);

} // namespace lemonvox::win
