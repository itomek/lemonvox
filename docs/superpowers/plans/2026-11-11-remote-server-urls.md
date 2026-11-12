# Remote Server URLs Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for this work.

**Goal:** Configure remote server URLs directly, with no local forward.
**Architecture:** Remove the shared loopback policy; retain the bridge fact as
informational. Reuse the Windows canonical parser in Settings and transport.
**Tech Stack:** C++17, Win32, Foundation, libsoup, CMake/CTest, GitHub CI.
**Spec:** ../specs/2026-11-11-remote-server-urls-design.md

## Constraints and review focus

Preserve URL scheme/host/port/path validation, credential/query/fragment rejection,
TLS and no redirects. Do not broaden a server listener or change the server.
Cover LAN, HTTPS DNS and IPv6, a missing host, invalid port, and API-prefix paths.

## Task 1: Endpoint policy and tests

Files: core/protocol.{h,cpp}, windows/{CMakeLists.txt,src/net/CMakeLists.txt,
src/ui/settings_layout.cpp}, macos/LemonadeClient.swift, ubuntu/adapters.cpp,
tests/{protocol_test.cpp,pipeline_test.swift}, windows/tests/{net_test.cpp,
settings_unit_test.cpp}, ubuntu/tests/adapters_test.cpp.
- [x] Add remote acceptance regressions and run CI against old policy.
- [ ] Confirm failures are the remote acceptance assertions.
- [ ] Ignore host_loopback for shared eligibility, reuse Windows parser, remove Ubuntu transport host gate, validate Mac host presence.
- [ ] Update existing tests and current wording/documentation for remote URLs.
- [ ] Run complete platform CI suites and inspect failures.

## Task 2: Direct Windows trial

- [ ] Download tested PR artifact and launch with exact Strix Halo URL.
- [ ] Verify discovery and selected models with temporary forwarding stopped.
- [ ] Exercise synthetic full dictation in a test-owned field; restore live capture for the user.
- [ ] Review final diff and report evidence and limitations.
