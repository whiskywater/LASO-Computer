#include "laso/config.hpp"
#include "laso/protocol.hpp"
#include "laso/platform.hpp"
#include "laso/core_worker_protocol.hpp"

#include <chrono>
#include <Windows.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
class LargeResultProvider final : public laso::CapabilityProvider {
public:
    [[nodiscard]] laso::PluginIdentity identity() const override { return {"test.large", "1", "test provider", false}; }
    [[nodiscard]] std::vector<laso::CapabilityDescriptor> capabilities() const override {
        return {{"test.large", "test-only oversized result", "test", nlohmann::json{{"type", "object"}}, true, {}}};
    }
    [[nodiscard]] nlohmann::json invoke(const laso::InvocationContext&) override {
        return {{"data", std::string(laso::max_frame_bytes, 'x')}};
    }
    [[nodiscard]] bool health() const override { return true; }
    void shutdown() noexcept override {}
};

class SuccessProvider final : public laso::CapabilityProvider {
public:
    [[nodiscard]] laso::PluginIdentity identity() const override { return {"test.success", "1", "test provider", false}; }
    [[nodiscard]] std::vector<laso::CapabilityDescriptor> capabilities() const override {
        return {{"test.success", "test successful job", "test", nlohmann::json{{"type", "object"}}, true, {}}};
    }
    [[nodiscard]] nlohmann::json invoke(const laso::InvocationContext&) override { return {{"success", true}}; }
    [[nodiscard]] bool health() const override { return true; }
    void shutdown() noexcept override {}
};
class FailingProvider final : public laso::CapabilityProvider {
public:
    [[nodiscard]] laso::PluginIdentity identity() const override { return {"test.failure", "1", "test provider", false}; }
    [[nodiscard]] std::vector<laso::CapabilityDescriptor> capabilities() const override {
        return {{"test.failure", "test failure state", "test", nlohmann::json{{"type", "object"}}, true, {}}};
    }
    [[nodiscard]] nlohmann::json invoke(const laso::InvocationContext&) override { throw std::runtime_error("expected action failure"); }
    [[nodiscard]] bool health() const override { return true; }
    void shutdown() noexcept override {}
};

class CancellableProvider final : public laso::CapabilityProvider {
public:
    [[nodiscard]] laso::PluginIdentity identity() const override { return {"test.cancellable", "1", "test provider", false}; }
    [[nodiscard]] std::vector<laso::CapabilityDescriptor> capabilities() const override {
        return {{"test.cancellable", "test cancellation", "test", nlohmann::json{{"type", "object"}}, true, {}}};
    }
    [[nodiscard]] nlohmann::json invoke(const laso::InvocationContext& context) override {
        for (int i = 0; i < 1000; ++i) {
            if (context.cancelled && context.cancelled()) throw std::runtime_error("cancelled");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return {{"unexpected", true}};
    }
    [[nodiscard]] bool health() const override { return true; }
    void shutdown() noexcept override {}
};
}

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

nlohmann::json request(const std::string& id, const std::string& operation) {
    return {{"protocol_version", 1}, {"request_id", id}, {"operation", operation}};
}

void config_tests() {
    laso::Config config;
    require(config.default_decision == laso::Decision::deny, "default policy must deny");
    require(config.decision_for("screen.capture") == laso::Decision::deny, "capability must inherit deny");
    config.capabilities["screen.capture"] = laso::Decision::allow;
    require(config.decision_for("screen.capture") == laso::Decision::allow, "explicit allow must be effective");
    std::string error;
    require(config.validate(error), "valid config should pass");
    config.capabilities["browser.status"] = laso::Decision::allow;
    require(config.validate(error), "browser.status should be a recognized capability");
    config.capabilities["unknown"] = laso::Decision::allow;
    require(!config.validate(error), "unknown capability should fail config validation");
}

void protocol_tests() {
    laso::Config config;
    laso::CapabilityRegistry registry;
    registry.register_provider(std::make_shared<laso::WindowsPlatform>(config));
    laso::WorkerProtocol worker(config, std::move(registry), laso::AuditLog{});
    auto hello = worker.handle(request("hello-1", "hello"));
    require(hello.value("ok", false), "hello should succeed");
    require(hello.value("protocol_version", 0) == 1, "protocol version should be v1");
    require(hello["payload"]["capabilities"].is_array(), "hello capabilities should be listed");
    bool browser_status_advertised = false;
    for (const auto& item : hello["payload"]["capabilities"])
        if (item.value("name", std::string{}) == "browser.status") browser_status_advertised = true;
    require(browser_status_advertised, "hello should advertise browser.status");

    auto submit = request("submit-1", "submit");
    submit["job_id"] = "job-1";
    submit["payload"] = {{"capability", "screen.capture"}, {"arguments", nlohmann::json::object()},
                         {"durable_session", true}, {"durable_session_id", "session-1"},
                         {"continuation", "continue"}, {"session_context", {{"source", "test"}}}};
    submit["durable_session"] = true;
    submit["durable_session_id"] = "session-1";
    submit["continuation"] = "continue";
    submit["session_context"] = {{"source", "test"}};
    auto accepted = worker.handle(submit);
    require(accepted.value("ok", false), "submit with optional session fields should be accepted");
    const auto external_id = accepted.value("external_job_id", std::string{});
    require(!external_id.empty(), "submit should allocate external job id");

    nlohmann::json result;
    for (int i = 0; i < 100; ++i) {
        auto poll = request("result-" + std::to_string(i), "result"); poll["external_job_id"] = external_id;
        result = worker.handle(poll);
        if (result.value("state", std::string{}) != "Running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(result.value("ok", false), "failed worker action remains valid protocol response");
    require(result.value("state", std::string{}) == "Failed", "default deny should fail the job");
    require(result.value("error", std::string{}) == "capability denied", "failed job should carry bounded error");

    auto duplicate = worker.handle(submit);
    require(!duplicate.value("ok", true), "duplicate request id should be rejected");
    auto malformed = worker.handle(nlohmann::json::object());
    require(!malformed.value("ok", true), "malformed request should fail protocol");
}

void failed_job_semantics_test() {
    laso::Config config; config.capabilities["test.failure"] = laso::Decision::allow;
    laso::CapabilityRegistry registry; registry.register_provider(std::make_shared<FailingProvider>());
    laso::WorkerProtocol worker(config, std::move(registry), laso::AuditLog{});
    auto submit = request("failure-submit", "submit"); submit["job_id"] = "failure-job";
    submit["payload"] = {{"capability", "test.failure"}, {"arguments", nlohmann::json::object()}};
    const auto accepted = worker.handle(submit);
    const auto external_id = accepted.value("external_job_id", std::string{});
    nlohmann::json result;
    for (int i = 0; i < 100; ++i) {
        auto poll = request("failure-result-" + std::to_string(i), "result"); poll["external_job_id"] = external_id;
        result = worker.handle(poll);
        if (result.value("state", std::string{}) != "Running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(result.value("ok", false), "a failed capability action must not become a transport failure");
    require(result.value("state", std::string{}) == "Failed", "failed capability action should report Failed state");
    require(result.value("error", std::string{}) == "expected action failure",
            "failed capability action should preserve bounded provider diagnostics");
}

void cancellation_test() {
    laso::Config config; config.capabilities["test.cancellable"] = laso::Decision::allow;
    laso::CapabilityRegistry registry; registry.register_provider(std::make_shared<CancellableProvider>());
    laso::WorkerProtocol worker(config, std::move(registry), laso::AuditLog{});
    auto submit = request("cancel-submit", "submit"); submit["job_id"] = "cancel-job";
    submit["payload"] = {{"capability", "test.cancellable"}, {"arguments", nlohmann::json::object()}};
    const auto accepted = worker.handle(submit);
    const auto external_id = accepted.value("external_job_id", std::string{});
    auto cancel = request("cancel-request", "cancel"); cancel["external_job_id"] = external_id;
    require(worker.handle(cancel).value("ok", false), "cancel request should be accepted");
    nlohmann::json result;
    for (int i = 0; i < 100; ++i) {
        auto poll = request("cancel-result-" + std::to_string(i), "result"); poll["external_job_id"] = external_id;
        result = worker.handle(poll);
        if (result.value("state", std::string{}) != "Running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(result.value("ok", false), "cancelled action remains a valid protocol result");
    require(result.value("state", std::string{}) == "Cancelled", "cancelled action should report Cancelled state");
}

void process_tests() {
    wchar_t system_directory[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(system_directory, MAX_PATH);
    require(length > 0 && length < MAX_PATH, "system directory unavailable for process test");
    const std::wstring whoami = std::wstring(system_directory) + L"\\whoami.exe";
    const std::wstring ping = std::wstring(system_directory) + L"\\ping.exe";
    laso::Config denied; denied.capabilities["shell.execute"] = laso::Decision::allow;
    auto denied_registry = laso::CapabilityRegistry{};
    denied_registry.register_provider(std::make_shared<laso::WindowsPlatform>(denied));
    laso::WorkerProtocol denied_worker(denied, std::move(denied_registry), laso::AuditLog{});
    auto denied_submit = request("process-denied-submit", "submit"); denied_submit["job_id"] = "process-denied-job";
    denied_submit["payload"] = {{"capability", "shell.execute"}, {"arguments", nlohmann::json::object()}};
    // Supply a clean UTF-8 path without involving a shell.
    const int utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, whoami.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string whoami_utf8(static_cast<std::size_t>(utf8_size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, whoami.c_str(), -1, whoami_utf8.data(), utf8_size, nullptr, nullptr);
    whoami_utf8.resize(static_cast<std::size_t>(utf8_size - 1));
    denied_submit["payload"]["arguments"] = {{"executable", whoami_utf8}, {"arguments", nlohmann::json::array()}};
    const auto unavailable = denied_worker.handle(denied_submit);
    const auto unavailable_id = unavailable.value("external_job_id", std::string{});
    require(!unavailable_id.empty(), "restricted process request should be accepted as a job");
    nlohmann::json unavailable_result;
    for (int i = 0; i < 100; ++i) {
        auto poll = request("process-denied-result-" + std::to_string(i), "result"); poll["external_job_id"] = unavailable_id;
        unavailable_result = denied_worker.handle(poll);
        if (unavailable_result.value("state", std::string{}) != "Running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(unavailable_result.value("ok", false) && unavailable_result.value("state", std::string{}) == "Failed",
            "process execution without an exact executable allowlist must fail as a job");

    laso::Config allowed; allowed.capabilities["shell.execute"] = laso::Decision::allow;
    allowed.allowed_executables.push_back(whoami);
    const auto direct = laso::WindowsPlatform(allowed).invoke("shell.execute", {{"executable", whoami_utf8}, {"arguments", nlohmann::json::array()}});
    require(direct.value("exit_code", 1) == 0 && !direct.value("stdout", std::string{}).empty(), "direct allowlisted process primitive failed");
    laso::CapabilityRegistry registry; registry.register_provider(std::make_shared<laso::WindowsPlatform>(allowed));
    laso::WorkerProtocol worker(allowed, std::move(registry), laso::AuditLog{});
    auto submit = request("process-allow-submit", "submit"); submit["job_id"] = "process-allow-job";
    submit["payload"] = {{"capability", "shell.execute"}, {"arguments", {{"executable", whoami_utf8}, {"arguments", nlohmann::json::array()}}}};
    const auto accepted = worker.handle(submit);
    const auto external_id = accepted.value("external_job_id", std::string{});
    nlohmann::json result;
    for (int i = 0; i < 200; ++i) {
        auto poll = request("process-allow-result-" + std::to_string(i), "result"); poll["external_job_id"] = external_id;
        result = worker.handle(poll);
        if (result.value("state", std::string{}) != "Running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(result.value("ok", false) && result.value("state", std::string{}) == "Completed", "allowlisted process should complete directly");
    require(result["payload"].value("exit_code", 1) == 0, "allowlisted process exit code should be zero");
    require(!result["payload"].value("stdout", std::string{}).empty(), "allowlisted process output should be returned");

    laso::Config timed; timed.capabilities["shell.execute"] = laso::Decision::allow; timed.allowed_executables.push_back(ping);
    laso::CapabilityRegistry timed_registry; timed_registry.register_provider(std::make_shared<laso::WindowsPlatform>(timed));
    laso::WorkerProtocol timed_worker(timed, std::move(timed_registry), laso::AuditLog{});
    std::string ping_utf8(static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, ping.c_str(), -1, nullptr, 0, nullptr, nullptr)), '\0');
    const int ping_len = WideCharToMultiByte(CP_UTF8, 0, ping.c_str(), -1, ping_utf8.data(), static_cast<int>(ping_utf8.size()), nullptr, nullptr);
    require(ping_len > 1, "ping path conversion failed"); ping_utf8.resize(static_cast<std::size_t>(ping_len - 1));
    auto timed_submit = request("process-timeout-submit", "submit"); timed_submit["job_id"] = "process-timeout-job";
    timed_submit["payload"] = {{"capability", "shell.execute"}, {"arguments", {{"executable", ping_utf8},
        {"arguments", nlohmann::json::array({"-n", "10", "127.0.0.1"})}, {"timeout_ms", 100}}}};
    const auto timed_accepted = timed_worker.handle(timed_submit);
    const auto timed_id = timed_accepted.value("external_job_id", std::string{});
    nlohmann::json timed_result;
    for (int i = 0; i < 300; ++i) {
        auto poll = request("process-timeout-result-" + std::to_string(i), "result"); poll["external_job_id"] = timed_id;
        timed_result = timed_worker.handle(poll);
        if (timed_result.value("state", std::string{}) != "Running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(timed_result.value("ok", false) && timed_result.value("state", std::string{}) == "TimedOut",
            "allowlisted process timeout should be a valid TimedOut job result");
}

void lifetime_bound_tests() {
    // Request replay protection is a rolling window, not a lifetime request cap.
    laso::CapabilityRegistry replay_registry;
    replay_registry.register_provider(std::make_shared<laso::WindowsPlatform>());
    laso::WorkerProtocol replay_worker(laso::Config{}, std::move(replay_registry), laso::AuditLog{});
    for (int i = 0; i < 4100; ++i) {
        const auto response = replay_worker.handle(request("replay-" + std::to_string(i), "hello"));
        require(response.value("ok", false), "request replay cache must evict old entries instead of bricking the worker");
    }
    require(replay_worker.handle(request("replay-0", "hello")).value("ok", false),
            "oldest request id should be reusable after bounded replay eviction");
    require(!replay_worker.handle(request("replay-4099", "hello")).value("ok", true),
            "recent request id must still be rejected as a duplicate");

    // Completed jobs must not count forever against the 64-active-job limit.
    laso::Config config;
    config.capabilities["test.success"] = laso::Decision::allow;
    laso::CapabilityRegistry registry;
    registry.register_provider(std::make_shared<SuccessProvider>());
    laso::WorkerProtocol worker(config, std::move(registry), laso::AuditLog{});
    std::string first_external_id;
    for (int i = 0; i < 300; ++i) {
        auto submit = request("lifetime-submit-" + std::to_string(i), "submit");
        submit["job_id"] = "lifetime-job-" + std::to_string(i);
        submit["payload"] = {{"capability", "test.success"}, {"arguments", nlohmann::json::object()}};
        const auto accepted = worker.handle(submit);
        require(accepted.value("ok", false), "completed jobs must not permanently exhaust the active-job limit");
        const auto external_id = accepted.value("external_job_id", std::string{});
        require(!external_id.empty(), "lifetime test should receive an external job id");
        if (i == 0) first_external_id = external_id;

        nlohmann::json result;
        for (int poll = 0; poll < 100; ++poll) {
            auto fetch = request("lifetime-result-" + std::to_string(i) + "-" + std::to_string(poll), "result");
            fetch["external_job_id"] = external_id;
            result = worker.handle(fetch);
            if (result.value("state", std::string{}) != "Running") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(result.value("ok", false) && result.value("state", std::string{}) == "Completed",
                "lifetime test job should complete");
    }
    auto old_result = request("lifetime-old-result", "result");
    old_result["external_job_id"] = first_external_id;
    const auto evicted = worker.handle(old_result);
    require(!evicted.value("ok", true) && evicted.value("error", std::string{}) == "job not found",
            "old completed jobs should be evicted from bounded result retention");
}
void frame_tests() {
    laso::CapabilityRegistry registry;
    registry.register_provider(std::make_shared<laso::WindowsPlatform>());
    laso::WorkerProtocol worker(laso::Config{}, std::move(registry), laso::AuditLog{});
    std::istringstream input(std::string(laso::max_frame_bytes + 1, 'x') + "\n");
    std::ostringstream output, diagnostics;
    const int status = worker.serve(input, output, diagnostics);
    require(status != 0, "oversized frame should fail cleanly");
    const auto response = nlohmann::json::parse(output.str());
    require(!response.value("ok", true), "oversized frame should produce protocol error");
    require(output.str().size() <= laso::max_frame_bytes + 1, "error frame must remain bounded");

    laso::CapabilityRegistry malformed_registry;
    malformed_registry.register_provider(std::make_shared<laso::WindowsPlatform>());
    laso::WorkerProtocol malformed_worker(laso::Config{}, std::move(malformed_registry), laso::AuditLog{});
    std::istringstream malformed_input("{not json}\n");
    std::ostringstream malformed_output, malformed_diagnostics;
    require(malformed_worker.serve(malformed_input, malformed_output, malformed_diagnostics) == 0,
            "malformed JSON should fail the frame without crashing the worker");
    const auto malformed_response = nlohmann::json::parse(malformed_output.str());
    require(!malformed_response.value("ok", true), "malformed JSON must produce a protocol error");

    laso::CapabilityRegistry large_registry;
    large_registry.register_provider(std::make_shared<LargeResultProvider>());
    laso::Config allow_large; allow_large.capabilities["test.large"] = laso::Decision::allow;
    laso::WorkerProtocol large_worker(allow_large, std::move(large_registry), laso::AuditLog{});
    auto submit = request("large-submit", "submit");
    submit["job_id"] = "large-job";
    submit["payload"] = {{"capability", "test.large"}, {"arguments", nlohmann::json::object()}};
    auto direct = large_worker.handle(submit);
    const auto id = direct.value("external_job_id", std::string{});
    require(!id.empty(), "large-result test job should start");
    nlohmann::json large_response;
    for (int i = 0; i < 100; ++i) {
        auto oversized = request("large-fetch-" + std::to_string(i), "result");
        oversized["external_job_id"] = id;
        large_response = large_worker.handle(oversized);
        if (large_response.value("state", std::string{}) != "Running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(large_response.value("ok", false), "large job completion remains a valid worker result");
    if (!large_response.contains("payload") || !large_response["payload"].is_object() || !large_response["payload"].contains("data"))
        throw std::runtime_error("large provider response missing payload: " + large_response.dump().substr(0, 300));
    require(large_response["payload"]["data"].get<std::string>().size() >= laso::max_frame_bytes,
            "test provider must generate an oversized response payload");
    const auto safe_frame = laso::encode_response_frame(large_response);
    require(safe_frame.size() <= laso::max_frame_bytes + 1, "oversized result must be converted to a bounded protocol frame");
    const auto safe_response = nlohmann::json::parse(safe_frame);
    require(!safe_response.value("ok", true) && safe_response.value("error", std::string{}) == "response exceeds frame size limit",
            "oversized result must become a controlled protocol error");
}

void core_worker_protocol_tests() {
    namespace protocol = laso::core_worker_protocol;
    const std::string valid =
        "{\"protocol_version\":1,\"request_id\":\"req-7\",\"operation\":\"submit\","
        "\"job_id\":\"job-7\",\"external_job_id\":\"\",\"payload\":{\"input\":{}}}\n";
    const auto request = protocol::parse_request_frame(valid);
    require(request.request_id == "req-7" && request.operation == "submit" && request.job_id == "job-7",
            "Core worker request fields should parse");
    protocol::Response result;
    result.request_id = request.request_id;
    result.state = "Completed";
    result.external_job_id = "external-7";
    result.payload = {{"answer", 42}};
    const auto encoded = protocol::serialize_response_frame(result);
    require(!encoded.empty() && encoded.back() == '\n', "Core worker response should be newline framed");
    const auto response = nlohmann::json::parse(encoded);
    require(response.at("protocol_version") == 1 && response.at("request_id") == request.request_id &&
                response.at("ok") == true && response.at("state") == "Completed" &&
                response.at("external_job_id") == "external-7" && response.at("payload").at("answer") == 42,
            "Core worker response should preserve correlation and result fields");

    auto rejects = [](const std::string& frame) {
        try { (void)protocol::parse_request_frame(frame); } catch (const std::exception&) { return true; }
        return false;
    };
    require(rejects("{not-json}\n"), "malformed JSON frame should be rejected");
    require(rejects(valid.substr(0, valid.size() - 1)), "truncated frame should be rejected");
    require(rejects(std::string(protocol::max_frame_bytes + 1, 'x') + "\n"), "oversized frame should be rejected");
    require(rejects("{\"protocol_version\":1,\"operation\":\"submit\",\"job_id\":\"j\",\"external_job_id\":\"\",\"payload\":{}}\n"),
            "request without correlation id should be rejected");
    require(rejects("{\"protocol_version\":1,\"request_id\":\"r\",\"operation\":\"exec\",\"job_id\":\"j\",\"external_job_id\":\"\",\"payload\":{}}\n"),
            "unknown worker operation should be rejected");
    require(rejects("{\"protocol_version\":1,\"request_id\":\"r\",\"request_id\":\"x\",\"operation\":\"submit\",\"job_id\":\"j\",\"external_job_id\":\"\",\"payload\":{}}\n"),
            "duplicate JSON members should be rejected");
    require(rejects("{\"protocol_version\":1,\"request_id\":\"r\",\"operation\":\"submit\",\"job_id\":\"j\",\"external_job_id\":\"\",\"payload\":{\"nested\":{\"a\":1,\"a\":2}}}\n"),
            "nested duplicate JSON members should be rejected");
    const auto status = protocol::parse_request_frame(
        "{\"protocol_version\":1,\"request_id\":\"req-status\",\"operation\":\"status\",\"job_id\":\"\",\"external_job_id\":\"external-7\",\"payload\":{}}\n");
    const auto result_poll = protocol::parse_request_frame(
        "{\"protocol_version\":1,\"request_id\":\"req-result\",\"operation\":\"result\",\"job_id\":\"\",\"external_job_id\":\"external-7\",\"payload\":{}}\n");
    const auto cancellation = protocol::parse_request_frame(
        "{\"protocol_version\":1,\"request_id\":\"req-cancel\",\"operation\":\"cancel\",\"job_id\":\"\",\"external_job_id\":\"external-7\",\"payload\":{}}\n");
    require(status.operation == "status" && status.job_id.empty() && status.external_job_id == "external-7",
            "Core status should target an external job without a job identifier");
    require(result_poll.operation == "result" && result_poll.job_id.empty() && result_poll.external_job_id == "external-7",
            "Core result polling should target an external job without a job identifier");
    require(cancellation.operation == "cancel" && cancellation.job_id.empty() && cancellation.external_job_id == "external-7",
            "Core cancellation should target the correlated external job");
    require(rejects("{\"protocol_version\":1,\"request_id\":\"r\",\"operation\":\"cancel\",\"job_id\":\"j\",\"external_job_id\":\"\",\"payload\":{}}\n"),
            "cancel request without external job id should be rejected");
    protocol::Response failed;
    failed.request_id = "req-fail";
    failed.ok = false;
    failed.error = "protocol failure";
    require(nlohmann::json::parse(protocol::serialize_response_frame(failed)).at("error") == "protocol failure",
            "Core worker error response should preserve its error text");
}
}

int main() {
    try {
        config_tests();
        protocol_tests();
        failed_job_semantics_test();
        cancellation_test();
        process_tests();
        lifetime_bound_tests();
        frame_tests();
        core_worker_protocol_tests();
        std::cout << "All LASO-Computer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
