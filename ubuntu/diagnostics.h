#pragma once
#include "adapters.h"
#include <string>
namespace lemonvox::ubuntu {
class PrivateDiagnostics final : public DiagnosticsSink {
public:
    explicit PrivateDiagnostics(std::string path = {});
    void record(DiagnosticEntry) override;
    void set_debug(bool value) { debug_ = value; }
    const std::string &path() const { return path_; }
private: std::string path_; bool debug_ = false;
};
}
