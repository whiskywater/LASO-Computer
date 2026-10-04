#include "laso/core_worker_adapter.hpp"

#include <atomic>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class EchoProvider final : public laso::CapabilityProvider {
public:
    [[nodiscard]] laso::PluginIdentity identity() const override { return {"test.echo", "1", "test provider", false}; }
    [[nodiscard]] std::vector<laso::CapabilityDescriptor> capabilities() const override {
        return {{"test.echo", "test echo", "low", Json{{"type", "object"}}, true, {}}};
    }
    [[nodiscard]] Json invoke(const laso::InvocationContext& context) override {
        ++calls;
        return {{"arguments", context.arguments}, {"request_id", context.request_id},
                {"job_id", context.job_id}};
    }
    [[nodiscard]] bool health() const override { return true; }
    void shutdown() noexcept override {}
    std::atomic_int calls{0};
};

class SlowProvider final : public laso::CapabilityProvider {
public:
    [[nodiscard]] laso::PluginIdentity identity() const override { return {"test.slow", "1", "test provider", false}; }
    [[nodiscard]] std::vector<laso::CapabilityDescriptor> capabilities() const override {
        return {{"test.slow", "test cancellable", "low", Json{{"type", "object"}}, true, {}}};
    }
    [[nodiscard]] Json invoke(const laso::InvocationContext& context) override {
        ++calls;
        for (int i = 0; i < 500; ++i) {
            if (context.cancelled && context.cancelled()) throw std::runtime_error("cancelled");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return {{"unexpected", true}};
    }
    [[nodiscard]] bool health() const override { return true; }
    void shutdown() noexcept override {}
    std::atomic_int calls{0};
};

laso::core_worker_protocol::Request request(std::string id, std::string operation) {
    laso::core_worker_protocol::Request value;
    value.request_id = std::move(id);
    value.operation = std::move(operation);
    return value;
}

Json wait_for_result(laso::CoreWorkerAdapter& adapter, const std::string& external_id) {
    Json response;
    for (int i = 0; i < 200; ++i) {
        auto poll = request("poll-" + std::to_string(i) + "-" + external_id, "result");
        poll.external_job_id = external_id;
        response = adapter.handle(poll);
        if (response.value("state", std::string{}) != "Running") return response;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return response;
}

void handshake_and_dispatch() {
    laso::Config config;
    config.capabilities["test.echo"] = laso::Decision::allow;
    auto provider = std::make_shared<EchoProvider>();
    laso::CapabilityRegistry registry;
    registry.register_provider(provider);
    laso::WorkerProtocol dispatcher(config, std::move(registry), laso::AuditLog{});
    laso::CoreWorkerAdapter adapter(dispatcher);

    auto hello_request = request("hello-1", "hello");
    const auto hello = adapter.handle(hello_request);
    require(hello.value("request_id", std::string{}) == "hello-1" && hello.value("ok", false),
            "hello should preserve Core correlation");
    const auto metadata = hello.at("metadata");
    require(metadata.value("id", std::string{}) == "laso-computer" && metadata.value("healthy", false) &&
                metadata.value("supports_status", false) && metadata.contains("capabilities"),
            "hello should return generic worker metadata");
    require(!metadata.contains("client_id") && !metadata.contains("hostname") &&
                !metadata.contains("computer_name"),
            "hello must not expose machine identity");

    auto submit = request("submit-1", "submit");
    submit.job_id = "core-job-1";
    submit.payload = {{"capability", "test.echo"}, {"input", {{"value", 17}}}};
    const auto accepted = adapter.handle(submit);
    require(accepted.value("request_id", std::string{}) == "submit-1" &&
                accepted.value("state", std::string{}) == "Running" &&
                !accepted.value("external_job_id", std::string{}).empty(),
            "submit should return a Core external job id");
    const auto external_id = accepted.at("external_job_id").get<std::string>();

    auto status = request("status-1", "status");
    status.external_job_id = external_id;
    require(adapter.handle(status).value("external_job_id", std::string{}) == external_id,
            "status should preserve the external job id");
    const auto completed = wait_for_result(adapter, external_id);
    require(completed.value("state", std::string{}) == "Completed" &&
                completed.value("request_id", std::string{}) != "submit-1" &&
                completed.at("payload").at("arguments").at("value") == 17 &&
                completed.at("payload").at("request_id") == "submit-1" &&
                completed.at("payload").at("job_id") == "core-job-1",
            "Core input should reach the local dispatcher with correlation intact");
    require(provider->calls.load() == 1, "one Core submit should invoke exactly once");
}

void approval_fails_closed() {
    laso::Config config;
    config.capabilities["test.echo"] = laso::Decision::require_approval;
    auto provider = std::make_shared<EchoProvider>();
    laso::CapabilityRegistry registry;
    registry.register_provider(provider);
    laso::WorkerProtocol dispatcher(config, std::move(registry), laso::AuditLog{});
    laso::CoreWorkerAdapter adapter(dispatcher);
    auto submit = request("approval-submit", "submit");
    submit.job_id = "approval-job";
    submit.payload = {{"capability", "test.echo"}, {"input", Json::object()}};
    const auto accepted = adapter.handle(submit);
    const auto result = wait_for_result(adapter, accepted.value("external_job_id", std::string{}));
    require(result.value("state", std::string{}) == "Failed" &&
                result.value("error", std::string{}) == "approval denied or unavailable" &&
                provider->calls.load() == 0,
            "require_approval must fail closed without an interaction channel");
}

void cancel_and_framed_serve() {
    laso::Config config;
    config.capabilities["test.slow"] = laso::Decision::allow;
    auto provider = std::make_shared<SlowProvider>();
    laso::CapabilityRegistry registry;
    registry.register_provider(provider);
    laso::WorkerProtocol dispatcher(config, std::move(registry), laso::AuditLog{});
    laso::CoreWorkerAdapter adapter(dispatcher);
    auto submit = request("slow-submit", "submit");
    submit.job_id = "slow-job";
    submit.payload = {{"capability", "test.slow"}, {"input", Json::object()}};
    const auto accepted = adapter.handle(submit);
    auto cancel = request("cancel-1", "cancel");
    cancel.external_job_id = accepted.value("external_job_id", std::string{});
    const auto cancelled = adapter.handle(cancel);
    require(cancelled.value("acknowledged", false) &&
                cancelled.value("external_job_id", std::string{}) == cancel.external_job_id,
            "cancel should acknowledge the target Core job");
    require(wait_for_result(adapter, cancel.external_job_id).value("state", std::string{}) == "Cancelled",
            "cancellation should reach the local capability");

    laso::CapabilityRegistry serve_registry;
    serve_registry.register_provider(std::make_shared<EchoProvider>());
    laso::WorkerProtocol serve_dispatcher(laso::Config{}, std::move(serve_registry), laso::AuditLog{});
    laso::CoreWorkerAdapter serve_adapter(serve_dispatcher);
    std::istringstream input(
        "{\"protocol_version\":1,\"request_id\":\"serve-hello\",\"operation\":\"hello\",\"job_id\":\"\",\"external_job_id\":\"\",\"payload\":{}}\n"
        "{\"protocol_version\":1,\"request_id\":\"serve-stop\",\"operation\":\"shutdown\",\"job_id\":\"\",\"external_job_id\":\"\",\"payload\":{}}\n");
    std::ostringstream output, diagnostics;
    require(serve_adapter.serve(input, output, diagnostics) == 0, "framed worker serve should shut down cleanly");
    std::istringstream frames(output.str());
    std::string first, second;
    require(static_cast<bool>(std::getline(frames, first)) && static_cast<bool>(std::getline(frames, second)),
            "framed worker serve should emit one response per request");
    require(Json::parse(first).value("request_id", std::string{}) == "serve-hello" &&
                Json::parse(second).value("request_id", std::string{}) == "serve-stop",
            "framed worker responses should preserve request correlation");
}

void approval_round_trip_and_denial() {
    auto run = [](const std::string& decision, int expected_calls) {
        laso::Config config;
        config.capabilities["test.echo"] = laso::Decision::require_approval;
        auto provider = std::make_shared<EchoProvider>();
        laso::CapabilityRegistry registry;
        registry.register_provider(provider);
        laso::WorkerProtocol dispatcher(config, std::move(registry), laso::AuditLog{});
        laso::CoreWorkerAdapter adapter(dispatcher, 200, [] { return std::string("fixture-id"); });
        std::istringstream input(
            std::string("{\"protocol_version\":1,\"request_id\":\"approval-submit\",\"operation\":\"submit\",\"job_id\":\"approval-job\",\"external_job_id\":\"\",\"payload\":{\"worker_id\":\"worker-test\",\"durable_session_id\":\"session-test\",\"capability\":\"test.echo\",\"input\":{}}}\n") +
            "{\"protocol_version\":1,\"message_type\":\"worker_response\",\"request_id\":\"interaction-fixture-id\",\"decision\":\"" + decision + "\",\"payload\":{},\"reason\":\"test decision\"}\n" +
            "{\"protocol_version\":1,\"request_id\":\"approval-shutdown\",\"operation\":\"shutdown\",\"job_id\":\"\",\"external_job_id\":\"\",\"payload\":{}}\n");
        std::ostringstream output, diagnostics;
        require(adapter.serve(input, output, diagnostics) == 0, "approval exchange should preserve worker transport");
        std::istringstream frames(output.str());
        std::string interaction_frame, submit_frame, shutdown_frame;
        require(static_cast<bool>(std::getline(frames, interaction_frame)) &&
                    static_cast<bool>(std::getline(frames, submit_frame)) &&
                    static_cast<bool>(std::getline(frames, shutdown_frame)),
                "approval exchange should emit interaction and operation responses");
        const auto interaction = Json::parse(interaction_frame);
        require(interaction.value("message_type", std::string{}) == "worker_request" &&
                    interaction.value("request_type", std::string{}) == "approval" &&
                    interaction.value("worker_id", std::string{}) == "worker-test" &&
                    interaction.value("worker_job_id", std::string{}) == "approval-job" &&
                    interaction.value("session_id", std::string{}) == "session-test" &&
                    interaction.value("request_id", std::string{}) == "interaction-fixture-id",
                "approval request should be correlated and use Core job/session identity");
        require(Json::parse(submit_frame).value("request_id", std::string{}) == "approval-submit",
                "submit response should follow the approval exchange");
        require(provider->calls.load() == expected_calls,
                "only an approved decision may invoke the capability");
    };
    run("approved", 1);
    run("denied", 0);
}

void malformed_approval_fails_closed() {
    laso::Config config;
    config.capabilities["test.echo"] = laso::Decision::require_approval;
    auto provider = std::make_shared<EchoProvider>();
    laso::CapabilityRegistry registry;
    registry.register_provider(provider);
    laso::WorkerProtocol dispatcher(config, std::move(registry), laso::AuditLog{});
    laso::CoreWorkerAdapter adapter(dispatcher, 100, [] { return std::string("bad-fixture-id"); });
    std::istringstream input(
        "{\"protocol_version\":1,\"request_id\":\"bad-submit\",\"operation\":\"submit\",\"job_id\":\"bad-job\",\"external_job_id\":\"\",\"payload\":{\"capability\":\"test.echo\",\"input\":{}}}\n"
        "{\"protocol_version\":1,\"message_type\":\"worker_response\",\"request_id\":\"wrong-id\",\"request_id\":\"interaction-bad-fixture-id\",\"decision\":\"approved\",\"payload\":{},\"reason\":\"\"}\n");
    std::ostringstream output, diagnostics;
    require(adapter.serve(input, output, diagnostics) == 2 && provider->calls.load() == 0,
            "malformed or mismatched interaction must terminate without executing");
}

void missing_approval_response_times_out_closed() {
    laso::Config config;
    config.capabilities["test.echo"] = laso::Decision::require_approval;
    auto provider = std::make_shared<EchoProvider>();
    laso::CapabilityRegistry registry;
    registry.register_provider(provider);
    laso::WorkerProtocol dispatcher(config, std::move(registry), laso::AuditLog{});
    laso::CoreWorkerAdapter adapter(dispatcher, 10, [] { return std::string("timeout-fixture"); });
    std::istringstream input(
        "{\"protocol_version\":1,\"request_id\":\"timeout-submit\",\"operation\":\"submit\",\"job_id\":\"timeout-job\",\"external_job_id\":\"\",\"payload\":{\"capability\":\"test.echo\",\"input\":{}}}\n");
    std::ostringstream output, diagnostics;
    require(adapter.serve(input, output, diagnostics) == 2 && provider->calls.load() == 0,
            "missing approval response must time out without invoking");
}

void cancel_completed_job_is_not_acknowledged() {
    laso::Config config;
    config.capabilities["test.echo"] = laso::Decision::allow;
    auto provider = std::make_shared<EchoProvider>();
    laso::CapabilityRegistry registry;
    registry.register_provider(provider);
    laso::WorkerProtocol dispatcher(config, std::move(registry), laso::AuditLog{});
    laso::CoreWorkerAdapter adapter(dispatcher);
    auto submit = request("done-submit", "submit");
    submit.job_id = "done-job";
    submit.payload = {{"capability", "test.echo"}, {"input", Json::object()}};
    const auto accepted = adapter.handle(submit);
    const auto completed = wait_for_result(adapter, accepted.value("external_job_id", std::string{}));
    require(completed.value("state", std::string{}) == "Completed", "test job should complete before cancellation");
    auto cancel = request("done-cancel", "cancel");
    cancel.external_job_id = accepted.at("external_job_id").get<std::string>();
    require(!adapter.handle(cancel).value("acknowledged", true),
            "cancel after terminal completion must not be acknowledged");
}
} // namespace

int main() {
    try {
        handshake_and_dispatch();
        approval_fails_closed();
        cancel_and_framed_serve();
        approval_round_trip_and_denial();
        malformed_approval_fails_closed();
        missing_approval_response_times_out_closed();
        cancel_completed_job_is_not_acknowledged();
        return 0;
    } catch (...) {
        return 1;
    }
}
