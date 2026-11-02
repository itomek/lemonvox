#include "insertion/integrity.h"

#include <vector>

namespace lemonvox::win {

IntegrityDecision compare_integrity(DWORD source_rid, DWORD target_rid) {
    return target_rid <= source_rid ? IntegrityDecision::allowed : IntegrityDecision::target_higher;
}

bool process_integrity(HANDLE process, DWORD &rid) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    bool ok = false;
    if (size != 0) {
        std::vector<unsigned char> buffer(size);
        if (GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
            const auto *label = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(buffer.data());
            const PUCHAR count = GetSidSubAuthorityCount(label->Label.Sid);
            if (count && *count > 0) {
                rid = *GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(*count - 1));
                ok = true;
            }
        }
    }
    CloseHandle(token);
    return ok;
}

bool current_process_integrity(DWORD &rid) {
    return process_integrity(GetCurrentProcess(), rid);
}

bool process_id_integrity(DWORD process_id, DWORD &rid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return false;
    const bool ok = process_integrity(process, rid);
    CloseHandle(process);
    return ok;
}

IntegrityDecision input_integrity(DWORD target_process_id) {
    DWORD source = 0;
    DWORD target = 0;
    if (!current_process_integrity(source) || !process_id_integrity(target_process_id, target)) {
        return IntegrityDecision::unknown;
    }
    return compare_integrity(source, target);
}

} // namespace lemonvox::win
