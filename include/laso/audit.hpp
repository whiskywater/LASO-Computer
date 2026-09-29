#pragma once

#include <filesystem>
#include <mutex>
#include <string>

namespace laso {

class AuditLog {
public:
    explicit AuditLog(std::filesystem::path path = {});
    AuditLog(AuditLog&& other) noexcept;
    AuditLog& operator=(AuditLog&& other) noexcept;
    AuditLog(const AuditLog&) = delete;
    AuditLog& operator=(const AuditLog&) = delete;
    void event(const std::string& request_id, const std::string& capability,
               const std::string& decision, const std::string& outcome);

private:
    std::filesystem::path path_;
    std::mutex mutex_;
};

} // namespace laso
