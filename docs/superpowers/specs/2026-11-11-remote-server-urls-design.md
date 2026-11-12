# Remote server URLs

The user explicitly requires using a server on another machine, including the
Strix Halo at `http://192.168.1.231:13305/api/v1`, directly from the Windows VM.
Loopback-only validation defeats this workflow. Remove that shared policy and
the Windows Settings and Ubuntu transport gates. HTTP and HTTPS endpoints may
use LAN IPs, DNS names or IPv6; preserve API prefixes ending in `/v1`.

Keep malformed URL, embedded-credential, query/fragment and non-HTTP scheme
rejection, normal TLS verification, and existing redirect/proxy behavior.
The existing host_loopback fact remains informational for bridge compatibility.
Do not pretend a remote host is loopback. Windows Settings should reuse the
transport's pure URL parser instead of maintaining a weaker second parser.
Update all platforms' current wording and regressions because validation is
shared. Authentication UI is outside this request.

Verify failing remote acceptance regressions first, then the complete CI suites.
Build the PR artifact, configure the exact remote URL, stop the temporary relay,
and exercise synthetic transcription/cleanup before handing back live recording.
