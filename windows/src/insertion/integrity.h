#pragma once

#include <windows.h>

#include <cstdint>

namespace lemonvox::win {

// Mandatory integrity levels as SECURITY_MANDATORY_*_RID values.
enum class IntegrityDecision : std::uint8_t {
    // The target runs at or below LemonVox's integrity; synthesized input is delivered.
    allowed,
    // User Interface Privilege Isolation (UIPI) silently drops SendInput to a
    // higher-integrity window, for example an elevated terminal.
    target_higher,
    // The target's token cannot be read (protected or elevated process).
    // Treated as higher: LemonVox never assumes it may type into it.
    unknown,
};

// Pure comparison used by the adapter; exposed for tests.
IntegrityDecision compare_integrity(DWORD source_rid, DWORD target_rid);

// Reads a process token's integrity RID. Returns false when unreadable.
bool process_integrity(HANDLE process, DWORD &rid);
bool current_process_integrity(DWORD &rid);
bool process_id_integrity(DWORD process_id, DWORD &rid);

// Whether LemonVox may synthesize input into the given process.
IntegrityDecision input_integrity(DWORD target_process_id);

} // namespace lemonvox::win
