#pragma once

#include "laso/plugin.hpp"

namespace laso {

// Compiled only into the explicit acceptance worker target. This provider has
// no access to the filesystem, network, process APIs, or interactive desktop.
class AcceptanceTestProvider final : public CapabilityProvider {
public:
    [[nodiscard]] PluginIdentity identity() const override;
    [[nodiscard]] std::vector<CapabilityDescriptor> capabilities() const override;
    [[nodiscard]] nlohmann::json invoke(const InvocationContext& invocation) override;
    [[nodiscard]] bool health() const override;
    void shutdown() noexcept override;
};

} // namespace laso
