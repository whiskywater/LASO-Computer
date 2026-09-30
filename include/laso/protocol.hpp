#pragma once

#include "laso/audit.hpp"
#include "laso/config.hpp"
#include "laso/platform.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <istream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <ostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace laso {

inline constexpr std::size_t max_frame_bytes = 1U << 20;
[[nodiscard]] std::string encode_response_frame(const nlohmann::json& response);

class WorkerProtocol {
public:
    WorkerProtocol(Config config, WindowsPlatform platform, AuditLog audit);
    WorkerProtocol(Config config, CapabilityRegistry registry, AuditLog audit);
    ~WorkerProtocol();
    int serve(std::istream& input, std::ostream& output, std::ostream& diagnostics);
    [[nodiscard]] nlohmann::json handle(const nlohmann::json& request);

private:
    struct Job {
        std::string id;
        std::string laso_id;
        std::uint64_t sequence{};
        std::string state{"Running"};
        nlohmann::json payload;
        std::string error;
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        bool done{false};
        std::jthread worker;
    };

    [[nodiscard]] nlohmann::json submit(const nlohmann::json& request);
    [[nodiscard]] nlohmann::json inspect(const nlohmann::json& request, bool result);
    [[nodiscard]] nlohmann::json fail(const std::string& request_id,
                                      const std::string& reason) const;
    [[nodiscard]] nlohmann::json success(const std::string& request_id,
                                         const std::string& state,
                                         nlohmann::json payload = nullptr) const;

    Config config_;
    CapabilityRegistry registry_;
    AuditLog audit_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs_;
    std::unordered_map<std::string, std::string> jobs_by_laso_id_;
    std::deque<std::string> seen_request_order_;
    std::unordered_set<std::string> seen_request_ids_;
    std::deque<std::string> seen_job_order_;
    std::unordered_set<std::string> seen_job_ids_;
    std::uint64_t next_job_sequence_{1};
    std::atomic_bool shutting_down_{false};
};

} // namespace laso
