#include "laso/acceptance_test_provider.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace laso {
namespace {
using Json = nlohmann::json;

CapabilityDescriptor descriptor(std::string name, std::string description, bool synchronous) {
    CapabilityDescriptor value{std::move(name), std::move(description), "low", Json{{"type", "object"}}, true,
                               "acceptance.test"};
    value.requires_synchronous_interactions = synchronous;
    return value;
}
} // namespace

PluginIdentity AcceptanceTestProvider::identity() const {
    return {"acceptance.test", "1", "Isolated protocol acceptance provider", false};
}

std::vector<CapabilityDescriptor> AcceptanceTestProvider::capabilities() const {
    return {descriptor("acceptance.permission", "Request a correlated permission decision", true),
            descriptor("acceptance.question", "Request a bounded correlated answer", true),
            descriptor("acceptance.wait", "Wait cooperatively for cancellation", false)};
}

Json AcceptanceTestProvider::invoke(const InvocationContext& invocation) {
    if (invocation.capability == "acceptance.permission") {
        if (!invocation.request_interaction) throw std::runtime_error("permission interaction unavailable");
        const auto response = invocation.request_interaction(
            "permission", Json{{"resource", "acceptance.permission"}, {"operation", "acceptance-only check"}});
        const auto decision = response.value("decision", std::string{});
        if (decision != "approved") throw std::runtime_error("permission denied or unavailable");
        return Json{{"decision", decision}, {"continued", true}};
    }
    if (invocation.capability == "acceptance.question") {
        if (!invocation.request_interaction) throw std::runtime_error("question interaction unavailable");
        const auto response = invocation.request_interaction(
            "question", Json{{"questions", Json::array({"Provide the acceptance answer."})}});
        if (response.value("decision", std::string{}) != "answered")
            throw std::runtime_error("question was not answered");
        const auto payload = response.value("payload", Json::object());
        const auto answer = payload.value("answer", std::string{});
        if (answer.empty() || answer.size() > 256) throw std::runtime_error("invalid acceptance answer");
        return Json{{"answer", answer}, {"answered", true}};
    }
    if (invocation.capability == "acceptance.wait") {
        if (!invocation.cancelled) throw std::runtime_error("cancellation callback unavailable");
        while (!invocation.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        throw std::runtime_error("cancelled");
    }
    throw std::runtime_error("acceptance capability unavailable");
}

bool AcceptanceTestProvider::health() const { return true; }
void AcceptanceTestProvider::shutdown() noexcept {}

} // namespace laso
