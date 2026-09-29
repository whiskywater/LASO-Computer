#include "laso/plugin.hpp"

#include <algorithm>
#include <stdexcept>

namespace laso {

void CapabilityRegistry::register_provider(std::shared_ptr<CapabilityProvider> provider) {
    if (!provider) throw std::runtime_error("plugin provider is null");
    const auto identity = provider->identity();
    if (identity.id.empty() || identity.id.size() > 128 || identity.version.empty() || identity.version.size() > 64)
        throw std::runtime_error("plugin identity is invalid");
    if (std::any_of(providers_.begin(), providers_.end(), [&](const auto& entry) { return entry.first == identity.id; }))
        throw std::runtime_error("plugin identity already registered");
    auto items = provider->capabilities();
    if (items.empty() || items.size() > 128) throw std::runtime_error("plugin capability list is invalid");
    for (auto& item : items) {
        if (item.name.empty() || item.name.size() > 128 || item.name.find('.') == std::string::npos ||
            !item.argument_schema.is_object() || item.argument_schema.dump().size() > 16384 ||
            descriptors_.contains(item.name)) throw std::runtime_error("plugin capability descriptor is invalid");
        item.provider_id = identity.id;
    }
    providers_.emplace(identity.id, provider);
    for (auto& item : items) descriptors_.emplace(item.name, std::move(item));
}

bool CapabilityRegistry::available(const std::string& capability) const {
    const auto descriptor = descriptors_.find(capability);
    if (descriptor == descriptors_.end() || !descriptor->second.available) return false;
    const auto provider = providers_.find(descriptor->second.provider_id);
    return provider != providers_.end() && provider->second->health();
}

std::vector<CapabilityDescriptor> CapabilityRegistry::capabilities() const {
    std::vector<CapabilityDescriptor> out;
    out.reserve(descriptors_.size());
    for (const auto& [name, item] : descriptors_) {
        auto copy = item;
        const auto provider = providers_.find(item.provider_id);
        copy.available = copy.available && provider != providers_.end() && provider->second->health();
        out.push_back(std::move(copy));
    }
    return out;
}

nlohmann::json CapabilityRegistry::invoke(const InvocationContext& invocation) const {
    const auto descriptor = descriptors_.find(invocation.capability);
    if (descriptor == descriptors_.end() || !descriptor->second.available)
        throw std::runtime_error("capability unavailable");
    const auto provider = providers_.find(descriptor->second.provider_id);
    if (provider == providers_.end() || !provider->second->health()) throw std::runtime_error("capability unavailable");
    return provider->second->invoke(invocation);
}

std::vector<PluginIdentity> CapabilityRegistry::plugins() const {
    std::vector<PluginIdentity> out;
    out.reserve(providers_.size());
    for (const auto& [id, provider] : providers_) { (void)id; out.push_back(provider->identity()); }
    return out;
}

void CapabilityRegistry::shutdown() noexcept {
    for (auto& [id, provider] : providers_) { (void)id; provider->shutdown(); }
}

} // namespace laso
