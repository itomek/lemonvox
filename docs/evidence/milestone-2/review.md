# Independent product review

Verdict: **PASS**, including the final Settings routing labels and Ubuntu CI
changes. This was an independent read-only agent review of the Ubuntu host,
adapters, Settings, diagnostics, GNOME extension, D-Bus contract and build/
packaging configuration against the existing shared-core and product contracts.

Two concrete lifetime findings were fixed before delivery:

- Transport State and live requests retained one another when shutdown stopped
  the GLib context before cancellation dispatch. Weak callback capsules now let
  shutdown clear owned responses, client closures, sources and native references
  without a context drain or dangling callback userdata.
- Stop queued WAV bytes in an idle closure. Pending WAV bytes and completion now
  belong to capture State; cancel/shutdown zero and release them immediately.
  The idle thunk retains only weak State and operation metadata.

No-drain weak-State/closure lifetime regressions, late-callback tests, the full
configured CTest gate and focused AddressSanitizer/UBSan checks passed. Native
library cancellation metadata can outlive the adapter until its own callback
is dispatched; this does not retain host-owned recordings or response state.

The review also checked the regenerated Settings evidence, native semantic
appearance bridge, configurable active-only cancellation, strict manual Copy
policy, remote-provider labels retaining raw model IDs, and pinned hosted CI.
No remaining concrete product-code blocker was found. Package reproducibility,
installation lifecycle and public CI outcomes are separate recorded gates.
