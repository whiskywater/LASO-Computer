#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace laso {

enum class Decision { deny, allow, require_approval };

struct Config {
    int version{1};
    Decision default_decision{Decision::deny};
    std::map<std::string, Decision> capabilities;
    std::vector<std::wstring> allowed_executables;
    std::vector<std::wstring> environment_allowlist;
    unsigned approval_timeout_ms{60000};
    struct Playwright {
        bool enabled{false};
        std::wstring node_executable;
        std::wstring server_entry;
    } playwright;

    [[nodiscard]] Decision decision_for(const std::string& capability) const;
    [[nodiscard]] bool validate(std::string& error) const;
    [[nodiscard]] static Config load(const std::filesystem::path& path);
    [[nodiscard]] static std::filesystem::path default_path();
    void save_example(const std::filesystem::path& path) const;
};

[[nodiscard]] std::string to_string(Decision decision);
[[nodiscard]] Decision decision_from_string(const std::string& value);
[[nodiscard]] const std::vector<std::string>& capability_names();

} // namespace laso
