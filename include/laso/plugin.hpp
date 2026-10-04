#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace laso {

struct PluginIdentity {
    std::string id;
    std::string version;
    std::string description;
    bool trusted_by_default{false};
};

struct CapabilityDescriptor {
    std::string name;
    std::string description;
    std::string risk;
    nlohmann::json argument_schema;
    bool available{false};
    std::string provider_id;
    // This capability can request a Core interaction while it runs. The
    // process adapter keeps submit open until execution completes so requests
    // cannot arrive after Core has received its submit response.
    bool requires_synchronous_interactions{false};
};

struct InvocationContext {
    std::string request_id;
    std::string job_id;
    std::string external_job_id;
    std::string capability;
    nlohmann::json arguments;
    unsigned timeout_ms{30000};
    std::function<bool()> cancelled;
    // Present only for a capability whose descriptor opts into synchronous
    // interactions. Call before a dependent action and require "approved"
    // for permission or "answered" for question before proceeding.
    std::function<nlohmann::json(const std::string&, const nlohmann::json&)> request_interaction;
};

class CapabilityProvider {
public:
    virtual ~CapabilityProvider() = default;
    [[nodiscard]] virtual PluginIdentity identity() const = 0;
    [[nodiscard]] virtual std::vector<CapabilityDescriptor> capabilities() const = 0;
    [[nodiscard]] virtual nlohmann::json invoke(const InvocationContext& invocation) = 0;
    [[nodiscard]] virtual bool health() const = 0;
    virtual void shutdown() noexcept = 0;
};

class CapabilityRegistry {
public:
    void register_provider(std::shared_ptr<CapabilityProvider> provider);
    [[nodiscard]] bool available(const std::string& capability) const;
    [[nodiscard]] bool requires_synchronous_interactions(const std::string& capability) const;
    [[nodiscard]] std::vector<CapabilityDescriptor> capabilities() const;
    [[nodiscard]] nlohmann::json invoke(const InvocationContext& invocation) const;
    [[nodiscard]] std::vector<PluginIdentity> plugins() const;
    void shutdown() noexcept;

private:
    std::map<std::string, std::shared_ptr<CapabilityProvider>> providers_;
    std::map<std::string, CapabilityDescriptor> descriptors_;
};

} // namespace laso
