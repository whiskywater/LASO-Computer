#include "laso/broker.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class FakeState final : public laso::broker::PrivilegedState {
public:
    std::string appcontainer_sid() override { return "S-1-15-2-test"; }
    laso::broker::LoopbackStatus query_legacy_loopback() override {
        return {appcontainer_sid(), enabled, 2};
    }
    bool remove_legacy_loopback() override {
        const bool was_enabled = enabled;
        enabled = false;
        return was_enabled;
    }
    bool enabled{true};
};

class FakeAudit final : public laso::broker::AuditSink {
public:
    bool append(std::string_view, std::string_view, std::string_view,
                bool, std::string_view) override {
        ++events;
        return available;
    }
    bool available{true};
    unsigned events{};
};

Json call(laso::broker::Dispatcher& dispatcher, std::string operation,
          Json extra = Json::object(), std::string request_id = "test-1") {
    Json request{{"version", laso::broker::protocol_version},
                 {"request_id", std::move(request_id)},
                 {"operation", std::move(operation)}};
    for (auto it = extra.begin(); it != extra.end(); ++it) request[it.key()] = it.value();
    const auto response = dispatcher.handle(request.dump(), {"S-1-5-21-user", 1});
    return Json::parse(response);
}

void run() {
    FakeState state;
    FakeAudit audit;
    laso::broker::Dispatcher dispatcher(state, audit);

    const auto status = call(dispatcher, "query_status");
    require(status.value("ok", false), "status should succeed");
    const auto operations = status.at("result").at("operations");
    require(operations == Json{"query_status", "query_appcontainer_sid",
                               "query_legacy_loopback", "cleanup_legacy_loopback"},
            "only the reviewed typed operation set should be exposed");

    require(call(dispatcher, "query_appcontainer_sid").at("result").at("sid") == "S-1-15-2-test",
            "AppContainer SID query should use an independent nonmutating path");
    require(call(dispatcher, "query_legacy_loopback").at("result").at("enabled"),
            "legacy state should be queryable");
    const auto cleaned = call(dispatcher, "cleanup_legacy_loopback");
    require(cleaned.value("ok", false) && !state.enabled, "cleanup should remove only the managed legacy state");

    require(!call(dispatcher, "run_command").value("ok", true), "unknown command must be rejected");
    require(!call(dispatcher, "query_status", {{"command", "whoami"}}).value("ok", true),
            "shell command fields must be rejected");
    require(!call(dispatcher, "query_status", {{"path", "..\\Windows\\System32"}}).value("ok", true),
            "arbitrary path fields must be rejected");

    auto wrong_version = Json{{"version", 99}, {"request_id", "v1"}, {"operation", "query_status"}};
    require(!Json::parse(dispatcher.handle(wrong_version.dump(), {"S-1-5-21-user", 1})).value("ok", true),
            "unsupported protocol version must be rejected");
    require(!Json::parse(dispatcher.handle("{broken", {"S-1-5-21-user", 1})).value("ok", true),
            "malformed JSON must be rejected");
    require(!Json::parse(dispatcher.handle(std::string(laso::broker::max_message_bytes + 1, 'x'),
                                          {"S-1-5-21-user", 1})).value("ok", true),
            "oversized request must be rejected");

    require(laso::broker::caller_is_authorized({"S-1-5-21-user", 1}, "S-1-5-21-user"),
            "authorized interactive caller should pass identity policy");
    require(!laso::broker::caller_is_authorized({"S-1-5-21-other", 1}, "S-1-5-21-user"),
            "different user should fail identity policy");
    require(!laso::broker::caller_is_authorized({"S-1-5-21-user", 0}, "S-1-5-21-user"),
            "noninteractive session should fail identity policy");

    FakeState audit_blocked_state;
    FakeAudit unavailable_audit;
    unavailable_audit.available = false;
    laso::broker::Dispatcher audit_blocked(audit_blocked_state, unavailable_audit);
    require(!call(audit_blocked, "cleanup_legacy_loopback", Json::object(), "audit-blocked").value("ok", true) &&
            audit_blocked_state.enabled,
            "privileged cleanup must fail closed when the audit sink is unavailable");
    require(audit.events >= 8, "requests should be audited");
}
} // namespace

int main() {
    try {
        run();
        std::cout << "Broker protocol tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
