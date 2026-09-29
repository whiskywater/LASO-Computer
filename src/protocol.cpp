#include "laso/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <regex>
#include <stdexcept>

namespace laso {
namespace {
constexpr int protocol_version = 1;
constexpr std::size_t max_requests = 4096;
constexpr std::size_t max_jobs = 64;
const std::regex valid_id("^[A-Za-z0-9._:/-]{1,128}$");

bool bounded_string(const nlohmann::json& value, std::size_t limit) {
    return value.is_string() && value.get_ref<const std::string&>().size() <= limit;
}

} // namespace

std::string encode_response_frame(const nlohmann::json& response) {
    auto frame = response.dump();
    if (frame.size() > max_frame_bytes) {
        frame = nlohmann::json{{"protocol_version", protocol_version},
                               {"request_id", response.is_object() ? response.value("request_id", std::string{}) : std::string{}},
                               {"ok", false}, {"state", "Failed"},
                               {"error", "response exceeds frame size limit"}}.dump();
    }
    if (frame.size() > max_frame_bytes) throw std::runtime_error("protocol error response exceeds frame size limit");
    frame.push_back('\n');
    return frame;
}

WorkerProtocol::WorkerProtocol(Config config, WindowsPlatform platform, AuditLog audit)
    : config_(std::move(config)), audit_(std::move(audit)) {
    registry_.register_provider(std::make_shared<WindowsPlatform>(std::move(platform)));
}

WorkerProtocol::WorkerProtocol(Config config, CapabilityRegistry registry, AuditLog audit)
    : config_(std::move(config)), registry_(std::move(registry)), audit_(std::move(audit)) {}

WorkerProtocol::~WorkerProtocol() {
    std::vector<std::shared_ptr<Job>> jobs;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& [id, job] : jobs_) { (void)id; jobs.push_back(job); }
    }
    for (const auto& job : jobs) {
        job->cancelled.store(true);
        job->worker.request_stop();
    }
    for (const auto& job : jobs) if (job->worker.joinable()) job->worker.join();
    registry_.shutdown();
}

nlohmann::json WorkerProtocol::fail(const std::string& request_id, const std::string& reason) const {
    return {{"protocol_version", protocol_version}, {"request_id", request_id},
            {"ok", false}, {"state", "Failed"}, {"error", reason}};
}

nlohmann::json WorkerProtocol::success(const std::string& request_id, const std::string& state,
                                       nlohmann::json payload) const {
    nlohmann::json value{{"protocol_version", protocol_version}, {"request_id", request_id},
                         {"ok", true}, {"state", state}};
    if (!payload.is_null()) value["payload"] = std::move(payload);
    return value;
}

nlohmann::json WorkerProtocol::handle(const nlohmann::json& request) {
    if (!request.is_object() || !request.contains("protocol_version") ||
        !request["protocol_version"].is_number_integer() || request["protocol_version"] != protocol_version ||
        !request.contains("request_id") || !bounded_string(request["request_id"], 128) ||
        !std::regex_match(request["request_id"].get<std::string>(), valid_id) ||
        !request.contains("operation") || !bounded_string(request["operation"], 32))
        return fail("", "malformed or unsupported request");

    const auto request_id = request["request_id"].get<std::string>();
    {
        std::scoped_lock lock(mutex_);
        if (seen_request_ids_.contains(request_id)) return fail(request_id, "request id already used");
        if (seen_request_ids_.size() >= max_requests) return fail(request_id, "request replay cache full");
        seen_request_ids_.insert(request_id);
    }
    const auto operation = request["operation"].get<std::string>();
    if (shutting_down_ && operation != "shutdown") return fail(request_id, "worker is shutting down");

    if (operation == "hello") {
        nlohmann::json capabilities = nlohmann::json::array();
        for (const auto& item : registry_.capabilities()) {
            capabilities.push_back({{"name", item.name}, {"description", item.description}, {"risk", item.risk},
                                    {"available", item.available}, {"decision", to_string(config_.decision_for(item.name))},
                                    {"argument_schema", item.argument_schema}, {"provider_id", item.provider_id}});
        }
        nlohmann::json plugins = nlohmann::json::array();
        for (const auto& plugin : registry_.plugins()) plugins.push_back({{"id", plugin.id}, {"version", plugin.version},
            {"description", plugin.description}, {"trusted_by_default", plugin.trusted_by_default}});
        return success(request_id, "Completed", {{"client_id", client_id()}, {"os", "windows"},
                       {"architecture", "x64"}, {"capabilities", std::move(capabilities)}, {"plugins", std::move(plugins)}});
    }
    if (operation == "submit") return submit(request);
    if (operation == "status") return inspect(request, false);
    if (operation == "result") return inspect(request, true);
    if (operation == "cancel") {
        if (!request.contains("external_job_id") || !bounded_string(request["external_job_id"], 128))
            return fail(request_id, "invalid external job id");
        std::shared_ptr<Job> job;
        {
            std::scoped_lock lock(mutex_);
            const auto it = jobs_.find(request["external_job_id"].get<std::string>());
            if (it == jobs_.end()) return fail(request_id, "job not found");
            job = it->second;
        }
        job->cancelled.store(true);
        job->worker.request_stop();
        return success(request_id, "Running", {{"cancellation_requested", true}});
    }
    if (operation == "shutdown") {
        std::vector<std::shared_ptr<Job>> jobs;
        {
            std::scoped_lock lock(mutex_);
            shutting_down_ = true;
            for (const auto& [id, job] : jobs_) { (void)id; job->cancelled.store(true); job->worker.request_stop(); jobs.push_back(job); }
        }
        for (auto& job : jobs) if (job->worker.joinable()) job->worker.join();
        return success(request_id, "Completed", {{"stopped", true}});
    }
    return fail(request_id, "unknown operation");
}

nlohmann::json WorkerProtocol::submit(const nlohmann::json& request) {
    const auto request_id = request.value("request_id", std::string{});
    if (!request.contains("job_id") || !bounded_string(request["job_id"], 128) ||
        !std::regex_match(request["job_id"].get<std::string>(), valid_id) ||
        !request.contains("payload") || !request["payload"].is_object())
        return fail(request_id, "invalid capability request");
    const auto& payload = request["payload"];
    if (!payload.contains("capability") || !bounded_string(payload["capability"], 128) ||
        !payload.contains("arguments") || !payload["arguments"].is_object())
        return fail(request_id, "invalid capability request");
    const auto capability = payload["capability"].get<std::string>();
    const auto job_id = request["job_id"].get<std::string>();
    if (payload["arguments"].dump().size() > max_frame_bytes) return fail(request_id, "request exceeds size limit");

    std::scoped_lock lock(mutex_);
    if (shutting_down_) return fail(request_id, "worker is shutting down");
    if (jobs_by_laso_id_.contains(job_id)) return fail(request_id, "job id already submitted");
    if (jobs_.size() >= max_jobs) return fail(request_id, "active or retained job limit reached");
    const auto external_id = random_id();
    auto job = std::make_shared<Job>();
    job->id = external_id;
    jobs_[external_id] = job;
    jobs_by_laso_id_[job_id] = external_id;
    job->worker = std::jthread([this, job, request_id, job_id, capability, args = payload["arguments"]](std::stop_token stop) {
        std::string outcome = "success";
        std::string decision = to_string(config_.decision_for(capability));
        try {
            const auto policy = config_.decision_for(capability);
            if (!registry_.available(capability)) throw std::runtime_error("capability unavailable");
            if (policy == Decision::deny) throw std::runtime_error("capability denied");
            if (policy == Decision::require_approval) throw std::runtime_error("approval unavailable");
            if (stop.stop_requested() || job->cancelled.load()) throw std::runtime_error("cancelled");
            InvocationContext invocation{request_id, job_id, job->id, capability, args, 30000,
                [job, stop] { return stop.stop_requested() || job->cancelled.load(); }};
            auto result = registry_.invoke(invocation);
            std::scoped_lock job_lock(job->mutex);
            if (stop.stop_requested() || job->cancelled.load()) { job->state = "Cancelled"; outcome = "cancelled"; }
            else { job->state = "Completed"; job->payload = std::move(result); }
        } catch (const std::exception& error) {
            std::scoped_lock job_lock(job->mutex);
            if (job->cancelled.load() || std::string(error.what()) == "cancelled") {
                job->state = "Cancelled"; job->error = "cancelled"; outcome = "cancelled";
            } else if (std::string(error.what()) == "approval unavailable") {
                job->state = "Failed"; job->error = "approval denied or unavailable"; outcome = "denied";
            } else if (std::string(error.what()) == "capability denied") {
                job->state = "Failed"; job->error = "capability denied"; outcome = "denied";
            } else if (std::string(error.what()) == "timed out") {
                job->state = "TimedOut"; job->error = "timed out"; outcome = "timed_out";
            } else if (std::string(error.what()) == "capability unavailable") {
                job->state = "Failed"; job->error = "capability unavailable"; outcome = "failed";
            } else {
                job->state = "Failed"; job->error = "capability failed"; outcome = "failed";
            }
        }
        { std::scoped_lock job_lock(job->mutex); job->done = true; }
        audit_.event(request_id, capability, decision, outcome);
    });
    return {{"protocol_version", protocol_version}, {"request_id", request_id}, {"ok", true},
            {"state", "Running"}, {"external_job_id", external_id}, {"metadata", {{"job_id", job_id}}}};
}

nlohmann::json WorkerProtocol::inspect(const nlohmann::json& request, bool result) {
    const auto request_id = request.value("request_id", std::string{});
    if (!request.contains("external_job_id") || !bounded_string(request["external_job_id"], 128))
        return fail(request_id, "invalid external job id");
    std::shared_ptr<Job> job;
    {
        std::scoped_lock lock(mutex_);
        const auto it = jobs_.find(request["external_job_id"].get<std::string>());
        if (it == jobs_.end()) return fail(request_id, "job not found");
        job = it->second;
    }
    std::scoped_lock lock(job->mutex);
    nlohmann::json response{{"protocol_version", protocol_version}, {"request_id", request_id},
                            {"ok", true}, {"state", job->state}, {"external_job_id", job->id}};
    if (result && job->done && !job->payload.is_null()) response["payload"] = job->payload;
    if (result && job->done && !job->error.empty()) response["error"] = job->error;
    return response;
}

int WorkerProtocol::serve(std::istream& input, std::ostream& output, std::ostream& diagnostics) {
    auto write = [&](const nlohmann::json& response) {
        const auto frame = encode_response_frame(response);
        output.write(frame.data(), static_cast<std::streamsize>(frame.size()));
        output.flush();
        return output.good();
    };
    for (;;) {
        std::string line;
        bool too_large = false;
        char ch = 0;
        bool got_any = false;
        while (input.get(ch)) {
            got_any = true;
            if (ch == '\n') break;
            if (line.size() < max_frame_bytes) line.push_back(ch);
            else too_large = true;
        }
        if (!got_any && input.eof()) break;
        if (too_large) {
            diagnostics << "input frame exceeded protocol limit\n";
            if (!write(fail("", "request exceeds frame size limit"))) return 2;
            return 2;
        }
        if (line.empty()) {
            if (input.eof()) break;
            if (!write(fail("", "empty protocol frame"))) return 2;
            continue;
        }
        try {
            const auto request = nlohmann::json::parse(line);
            const bool shutdown = request.is_object() && request.value("operation", std::string{}) == "shutdown";
            if (!write(handle(request))) return 2;
            if (shutdown) break;
        } catch (const nlohmann::json::exception&) {
            if (!write(fail("", "malformed or unsupported request"))) return 2;
        }
    }
    registry_.shutdown();
    return 0;
}

} // namespace laso
