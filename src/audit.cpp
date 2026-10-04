#include "laso/audit.hpp"
#include "laso/config.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <fstream>

namespace laso {
namespace {
std::filesystem::path audit_path() {
    const auto config = laso::Config::default_path();
    return config.parent_path() / L"audit.jsonl";
}
}

AuditLog::AuditLog(std::filesystem::path path) : path_(std::move(path)) {
    if (path_.empty()) {
        try { path_ = audit_path(); } catch (...) { path_.clear(); }
    }
}

AuditLog::AuditLog(AuditLog&& other) noexcept : path_(std::move(other.path_)) {}

AuditLog& AuditLog::operator=(AuditLog&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        path_ = std::move(other.path_);
    }
    return *this;
}

void AuditLog::event(const std::string& request_id, const std::string& capability,
                     const std::string& decision, const std::string& outcome) {
    if (path_.empty()) return;
    std::scoped_lock lock(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) return;
    const auto entry = nlohmann::json{{"request_id", request_id}, {"capability", capability},
                                      {"decision", decision}, {"outcome", outcome}}.dump();
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (out) out << entry << '\n';
}

} // namespace laso
