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

class InteractiveProvider final : public laso::CapabilityProvider {
public:
    explicit InteractiveProvider(std::string interaction_type) : interaction_type_(std::move(interaction_type)) {}
    [[nodiscard]] laso::PluginIdentity identity() const override {
        return {"test.interactive", "1", "test-only interactive provider", false};
    }
    [[nodiscard]] std::vector<laso::CapabilityDescriptor> capabilities() const override {
        laso::CapabilityDescriptor descriptor{"test.interactive", "test interaction", "low",
                                             Json{{"type", "object"}}, true, {}};
        descriptor.requires_synchronous_interactions = true;
        return {descriptor};
    }
    [[nodiscard]] Json invoke(const laso::InvocationContext& context) override {
        if (!context.request_interaction) throw std::runtime_error("interaction callback unavailable");
        const auto request_payload = interaction_type_ == "permission"
                                         ? Json{{"resource", "test.read"}}
                                         : Json{{"questions", Json::array({"Continue?"})}};
        const auto response = context.request_interaction(interaction_type_, request_payload);
        const auto decision = response.value("decision", std::string{});
        const auto payload = response.value("payload", Json::object());
        if (interaction_type_ == "permission" && decision != "approved")
            throw std::runtime_error("permission denied or unavailable");
        if (interaction_type_ == "question" && decision != "answered")
            throw std::runtime_error("question was not answered");
        ++executions;
        return {{"allowed", true}, {"decision", decision}, {"response", payload}};
    }
    [[nodiscard]] bool health() const override { return true; }
    void shutdown() noexcept override {}
    std::atomic_int executions{0};

private:
    std::string interaction_type_;
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

void permission_and_question_interaction_frames() {
    auto run = [](const std::string& type, const std::string& decision, const Json& response_payload) {
        laso::Config config;
        laso::CapabilityRegistry registry;
        registry.register_provider(std::make_shared<EchoProvider>());
        laso::WorkerProtocol dispatcher(config, std::move(registry), laso::AuditLog{});
        laso::CoreWorkerAdapter adapter(dispatcher, 200, [] { return std::string("interaction-test-id"); });
        laso::PolicyInteraction interaction;
        interaction.worker_id = "worker-test";
        interaction.worker_job_id = "job-test";
        interaction.external_job_id = "external-test";
        interaction.session_id = "session-test";
        interaction.capability = "test.echo";
        interaction.type = type;
        interaction.title = type == "permission" ? "Permission request" : "Question";
        interaction.summary = "Generic interaction test.";
        interaction.payload = type == "permission" ? Json{{"resource", "test.read"}}
                                                    : Json{{"questions", Json::array({"Continue?"})}};
        interaction.risk = type == "permission" ? "medium" : "low";
        interaction.category = "capability." + type;
        std::istringstream input("{\"protocol_version\":1,\"message_type\":\"worker_response\",\"request_id\":\"interaction-interaction-test-id\",\"decision\":\"" +
                                 decision + "\",\"payload\":" + response_payload.dump() +
                                 ",\"reason\":\"test response\"}\n");
        std::ostringstream output;
        const auto response = adapter.exchange_interaction(interaction, input, output);
        std::istringstream frames(output.str());
        std::string frame;
        require(static_cast<bool>(std::getline(frames, frame)), "interaction request frame should be emitted");
        const auto request = Json::parse(frame);
        require(request.value("message_type", std::string{}) == "worker_request" &&
                    request.value("request_type", std::string{}) == type &&
                    request.value("request_id", std::string{}) == "interaction-interaction-test-id" &&
                    request.value("payload", Json::object()) == interaction.payload,
                "interaction request must preserve type, correlation, and bounded payload");
        require(response.decision == decision && response.payload == response_payload,
                "interaction response must preserve decision and payload");
    };
    run("permission", "approved", Json::object());
    run("permission", "denied", Json::object());
    run("question", "answered", Json{{"answers", {{"continue", "yes"}}}});
}

void question_rejects_permission_decisions() {
    laso::Config config;
    laso::CapabilityRegistry registry;
    registry.register_provider(std::make_shared<EchoProvider>());
    laso::WorkerProtocol dispatcher(config, std::move(registry), laso::AuditLog{});
    laso::CoreWorkerAdapter adapter(dispatcher, 200, [] { return std::string("invalid-decision-id"); });
    laso::PolicyInteraction interaction;
    interaction.worker_id = "worker-test";
    interaction.worker_job_id = "job-test";
    interaction.external_job_id = "external-test";
    interaction.session_id = "session-test";
    interaction.type = "question";
    std::istringstream input(
        "{\"protocol_version\":1,\"message_type\":\"worker_response\",\"request_id\":\"interaction-invalid-decision-id\",\"decision\":\"approved\",\"payload\":{},\"reason\":\"\"}\n");
    std::ostringstream output;
    const auto result = adapter.exchange_interaction(interaction, input, output);
    require(result.decision == "cancelled", "Core-invalid question decision must fail closed");
}

void provider_interactions_stay_inside_submit_exchange() {
    auto run = [](const std::string& type, const std::string& decision, const Json& response_payload,
                  bool require_approval) {
        laso::Config config;
        config.capabilities["test.interactive"] = require_approval ? laso::Decision::require_approval
                                                                     : laso::Decision::allow;
        auto provider = std::make_shared<InteractiveProvider>(type);
        laso::CapabilityRegistry registry;
        registry.register_provider(provider);
        laso::WorkerProtocol dispatcher(config, std::move(registry), laso::AuditLog{});
        int sequence = 0;
        laso::CoreWorkerAdapter adapter(dispatcher, 500, [&sequence] {
            return "provider-interaction-" + std::to_string(++sequence);
        });
        std::string input_frame =
            "{\"protocol_version\":1,\"request_id\":\"interactive-submit\",\"operation\":\"submit\",\"job_id\":\"interactive-job\",\"external_job_id\":\"\",\"payload\":{\"worker_id\":\"worker-test\",\"durable_session_id\":\"session-test\",\"capability\":\"test.interactive\",\"input\":{}}}\n";
        if (require_approval)
            input_frame += "{\"protocol_version\":1,\"message_type\":\"worker_response\",\"request_id\":\"interaction-provider-interaction-1\",\"decision\":\"approved\",\"payload\":{},\"reason\":\"approved\"}\n";
        const auto provider_id = require_approval ? "provider-interaction-2" : "provider-interaction-1";
        input_frame += "{\"protocol_version\":1,\"message_type\":\"worker_response\",\"request_id\":\"interaction-" +
                       provider_id + "\",\"decision\":\"" + decision + "\",\"payload\":" + response_payload.dump() +
                       ",\"reason\":\"test decision\"}\n";
        std::istringstream input(input_frame);
        std::ostringstream output, diagnostics;
        require(adapter.serve(input, output, diagnostics) == 0,
                "interactive provider exchange should preserve worker lifecycle");
        std::istringstream frames(output.str());
        std::vector<std::string> output_frames;
        std::string line;
        while (std::getline(frames, line)) output_frames.push_back(line);
        const std::size_t expected = require_approval ? 3 : 2;
        require(output_frames.size() == expected, "each interaction and operation should produce one frame");
        std::size_t index = 0;
        if (require_approval) {
            require(Json::parse(output_frames[index++]).value("request_type", std::string{}) == "approval",
                    "local require_approval must be resolved before provider execution");
        }
        const auto worker_request = Json::parse(output_frames[index++]);
        require(worker_request.value("message_type", std::string{}) == "worker_request" &&
                    worker_request.value("request_type", std::string{}) == type,
                "provider interaction must use the matching Core request type");
        const auto submit_response = Json::parse(output_frames[index++]);
        require(submit_response.value("request_id", std::string{}) == "interactive-submit",
                "submit response must follow all provider interactions");
        auto result_request = request("interactive-result", "result");
        result_request.external_job_id = submit_response.value("external_job_id", std::string{});
        const auto final_result = adapter.handle(result_request);
        require(provider->executions.load() ==
                    ((type == "question" && decision == "answered") ||
                             (type == "permission" && decision == "approved")
                         ? 1
                         : 0),
                "provider must execute its test action only after a valid interaction response");
        if (type == "question" && decision == "answered")
            require(final_result.at("payload").at("response") == response_payload,
                    "question answer payload must reach the provider result unchanged");
        if (type == "permission" && decision == "denied")
            require(final_result.value("state", std::string{}) == "Failed" &&
                        final_result.value("error", std::string{}) == "permission denied or unavailable",
                    "denied permission must yield a deterministic failed result");
        if (decision == "cancelled")
            require(final_result.value("state", std::string{}) == "Cancelled" &&
                        final_result.value("error", std::string{}) == "cancelled",
                    "cancelled interactions must preserve the cancelled job state");
        if (decision == "expired")
            require(final_result.value("state", std::string{}) == "TimedOut" &&
                        final_result.value("error", std::string{}) == "timed out",
                    "expired interactions must preserve the timeout job state");
        std::istringstream shutdown_input(
            "{\"protocol_version\":1,\"request_id\":\"interactive-shutdown\",\"operation\":\"shutdown\",\"job_id\":\"\",\"external_job_id\":\"\",\"payload\":{}}\n");
        std::ostringstream shutdown_output, shutdown_diagnostics;
        require(adapter.serve(shutdown_input, shutdown_output, shutdown_diagnostics) == 0 &&
                    Json::parse(shutdown_output.str()).value("request_id", std::string{}) == "interactive-shutdown",
                "worker should remain healthy for shutdown after interaction and result retrieval");
    };

    run("permission", "approved", Json::object(), true);
    run("permission", "denied", Json::object(), false);
    run("permission", "cancelled", Json::object(), false);
    run("permission", "expired", Json::object(), false);
    run("question", "answered", Json{{"answers", {{"continue", "yes"}}}}, false);
    run("question", "cancelled", Json::object(), false);
    run("question", "expired", Json::object(), false);
}

void endpoint_deny_precedes_provider_interactions() {
    laso::Config config;
    config.capabilities["test.interactive"] = laso::Decision::deny;
    auto provider = std::make_shared<InteractiveProvider>("permission");
    laso::CapabilityRegistry registry;
    registry.register_provider(provider);
    laso::WorkerProtocol dispatcher(config, std::move(registry), laso::AuditLog{});
    laso::CoreWorkerAdapter adapter(dispatcher);
    std::istringstream input(
        "{\"protocol_version\":1,\"request_id\":\"deny-submit\",\"operation\":\"submit\",\"job_id\":\"deny-job\",\"external_job_id\":\"\",\"payload\":{\"worker_id\":\"worker-test\",\"capability\":\"test.interactive\",\"input\":{}}}\n"
        "{\"protocol_version\":1,\"request_id\":\"deny-shutdown\",\"operation\":\"shutdown\",\"job_id\":\"\",\"external_job_id\":\"\",\"payload\":{}}\n");
    std::ostringstream output, diagnostics;
    require(adapter.serve(input, output, diagnostics) == 0 && provider->executions.load() == 0,
            "endpoint deny must prevent provider interaction and execution");
    require(output.str().find("worker_request") == std::string::npos,
            "endpoint deny must not ask Core to override local policy");
}

void missing_provider_permission_response_fails_closed() {
    laso::Config config;
    config.capabilities["test.interactive"] = laso::Decision::allow;
    auto provider = std::make_shared<InteractiveProvider>("permission");
    laso::CapabilityRegistry registry;
    registry.register_provider(provider);
    laso::WorkerProtocol dispatcher(config, std::move(registry), laso::AuditLog{});
    laso::CoreWorkerAdapter adapter(dispatcher, 10, [] { return std::string("missing-permission-response"); });
    std::istringstream input(
        "{\"protocol_version\":1,\"request_id\":\"missing-permission-submit\",\"operation\":\"submit\",\"job_id\":\"missing-permission-job\",\"external_job_id\":\"\",\"payload\":{\"worker_id\":\"worker-test\",\"capability\":\"test.interactive\",\"input\":{}}}\n");
    std::ostringstream output, diagnostics;
    require(adapter.serve(input, output, diagnostics) == 2 && provider->executions.load() == 0,
            "unavailable permission interaction must not execute the dependent provider action");
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
        permission_and_question_interaction_frames();
        question_rejects_permission_decisions();
        provider_interactions_stay_inside_submit_exchange();
        endpoint_deny_precedes_provider_interactions();
        missing_provider_permission_response_fails_closed();
        return 0;
    } catch (...) {
        return 1;
    }
}
