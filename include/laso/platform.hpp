#pragma once

#include "laso/config.hpp"
#include "laso/plugin.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>

namespace laso {

class WindowsPlatform final : public CapabilityProvider {
public:
    explicit WindowsPlatform(Config config = {});
    [[nodiscard]] PluginIdentity identity() const override;
    [[nodiscard]] std::vector<CapabilityDescriptor> capabilities() const override;
    [[nodiscard]] nlohmann::json invoke(const InvocationContext& invocation) override;
    [[nodiscard]] bool health() const override;
    void shutdown() noexcept override;
    [[nodiscard]] nlohmann::json invoke(const std::string& capability,
                                        const nlohmann::json& arguments,
                                        const std::function<bool()>& cancelled = {});
    [[nodiscard]] bool available(const std::string& capability) const;
private:
    Config config_;
};

[[nodiscard]] std::string random_id();
[[nodiscard]] std::string client_id();

} // namespace laso
