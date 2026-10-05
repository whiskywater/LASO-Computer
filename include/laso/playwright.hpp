#pragma once

#include "laso/config.hpp"
#include "laso/plugin.hpp"

#include <memory>

namespace laso {

class PlaywrightMcpProvider final : public CapabilityProvider {
public:
    explicit PlaywrightMcpProvider(Config::Playwright config);
    ~PlaywrightMcpProvider() override;
    PlaywrightMcpProvider(const PlaywrightMcpProvider&) = delete;
    PlaywrightMcpProvider& operator=(const PlaywrightMcpProvider&) = delete;

    [[nodiscard]] PluginIdentity identity() const override;
    [[nodiscard]] std::vector<CapabilityDescriptor> capabilities() const override;
    [[nodiscard]] nlohmann::json invoke(const InvocationContext& invocation) override;
    [[nodiscard]] bool health() const override;
    [[nodiscard]] std::string status_detail() const;
    void shutdown() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace laso
