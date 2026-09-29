#include "laso/config.hpp"

#include "laso/platform.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace laso {
namespace {

constexpr std::size_t config_limit = 1U << 20;

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("invalid UTF-8 configuration string");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count) != count)
        throw std::runtime_error("invalid UTF-8 configuration string");
    return result;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("invalid Unicode configuration string");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) != count)
        throw std::runtime_error("invalid Unicode configuration string");
    return result;
}

void require_keys(const nlohmann::json& object,
                  const std::initializer_list<const char*>& required,
                  const std::initializer_list<const char*>& optional = {}) {
    if (!object.is_object()) throw std::runtime_error("configuration sections must be objects");
    for (const auto* key : required)
        if (!object.contains(key)) throw std::runtime_error(std::string("missing config field: ") + key);
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const auto* key : required) known = known || it.key() == key;
        for (const auto* key : optional) known = known || it.key() == key;
        if (!known) throw std::runtime_error("unknown configuration field");
    }
}

} // namespace

std::string to_string(Decision decision) {
    switch (decision) {
    case Decision::allow: return "allow";
    case Decision::require_approval: return "require_approval";
    default: return "deny";
    }
}

Decision decision_from_string(const std::string& value) {
    if (value == "allow") return Decision::allow;
    if (value == "require_approval") return Decision::require_approval;
    if (value == "deny") return Decision::deny;
    throw std::runtime_error("invalid policy decision");
}

const std::vector<std::string>& capability_names() {
    static const std::vector<std::string> values{
        "screen.capture", "pointer.move", "pointer.click", "keyboard.type", "keyboard.key",
        "clipboard.read", "clipboard.write", "window.list", "window.focus", "ui.focus",
        "ui.inspect", "ui.invoke", "browser.navigate", "browser.snapshot", "browser.query",
        "browser.click", "browser.fill", "browser.select", "browser.tabs", "browser.back",
        "browser.screenshot", "shell.execute"};
    return values;
}

Decision Config::decision_for(const std::string& capability) const {
    const auto it = capabilities.find(capability);
    return it == capabilities.end() ? default_decision : it->second;
}

bool Config::validate(std::string& error) const {
    if (version != 1) { error = "unsupported config version"; return false; }
    if (approval_timeout_ms < 1000 || approval_timeout_ms > 600000) {
        error = "approval timeout must be between 1000 and 600000 milliseconds"; return false;
    }
    for (const auto& [name, value] : capabilities) {
        if (std::find(capability_names().begin(), capability_names().end(), name) == capability_names().end()) {
            error = "unknown capability in config"; return false;
        }
        if (value != Decision::allow && value != Decision::deny && value != Decision::require_approval) {
            error = "invalid capability decision"; return false;
        }
    }
    std::set<std::wstring> unique;
    for (const auto& path : allowed_executables) {
        if (path.empty() || path.size() > 2048 || path.find(L'\0') != std::wstring::npos ||
            !std::filesystem::path(path).is_absolute() || std::filesystem::path(path).lexically_normal() != path ||
            !unique.insert(path).second) {
            error = "executable allowlist entries must be unique clean absolute paths"; return false;
        }
    }
    unique.clear();
    for (const auto& name : environment_allowlist) {
        if (name.empty() || name.size() > 128 || !unique.insert(name).second ||
            !std::all_of(name.begin(), name.end(), [](wchar_t ch) {
                return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') || ch == L'_' ||
                       (ch >= L'0' && ch <= L'9');
            }) || (name.front() >= L'0' && name.front() <= L'9')) {
            error = "environment allowlist names must be unique identifiers"; return false;
        }
    }
    if (playwright.enabled) {
        for (const auto* path : {&playwright.node_executable, &playwright.server_entry, &playwright.browser_executable}) {
            const std::filesystem::path candidate(*path);
            if (path->empty() || path->size() > 4096 || !candidate.is_absolute() || candidate.lexically_normal() != candidate) {
                error = "enabled Playwright plugin paths must be clean absolute paths"; return false;
            }
        }
    }
    return true;
}

Config Config::load(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return Config{};
    if (ec) throw std::runtime_error("configuration file could not be accessed");
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > config_limit) throw std::runtime_error("configuration exceeds size limit");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("configuration file could not be opened");
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto json = nlohmann::json::parse(text);
    require_keys(json, {"version", "default_decision", "capabilities", "shell", "approval_timeout_ms"}, {"plugins"});
    Config cfg;
    cfg.version = json.at("version").get<int>();
    cfg.default_decision = decision_from_string(json.at("default_decision").get<std::string>());
    if (!json.at("capabilities").is_object()) throw std::runtime_error("capabilities must be an object");
    for (auto it = json.at("capabilities").begin(); it != json.at("capabilities").end(); ++it)
        cfg.capabilities.emplace(it.key(), decision_from_string(it.value().get<std::string>()));
    const auto& shell = json.at("shell");
    require_keys(shell, {"allowed_executables", "environment_allowlist"});
    for (const auto& entry : shell.at("allowed_executables")) cfg.allowed_executables.push_back(widen(entry.get<std::string>()));
    for (const auto& entry : shell.at("environment_allowlist")) cfg.environment_allowlist.push_back(widen(entry.get<std::string>()));
    cfg.approval_timeout_ms = json.at("approval_timeout_ms").get<unsigned>();
    if (json.contains("plugins")) {
        const auto& plugins = json.at("plugins");
        require_keys(plugins, {}, {"playwright"});
        if (plugins.contains("playwright")) {
            const auto& playwright = plugins.at("playwright");
            require_keys(playwright, {"enabled", "node_executable", "server_entry"}, {"browser_executable"});
            cfg.playwright.enabled = playwright.at("enabled").get<bool>();
            cfg.playwright.node_executable = widen(playwright.at("node_executable").get<std::string>());
            cfg.playwright.server_entry = widen(playwright.at("server_entry").get<std::string>());
            if (playwright.contains("browser_executable"))
                cfg.playwright.browser_executable = widen(playwright.at("browser_executable").get<std::string>());
            if (cfg.playwright.enabled && cfg.playwright.browser_executable.empty())
                throw std::runtime_error("enabled Playwright plugin requires an explicit browser executable");
        }
    }
    std::string error;
    if (!cfg.validate(error)) throw std::runtime_error(error);
    return cfg;
}

std::filesystem::path Config::default_path() {
    wchar_t* local = nullptr;
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!length || length > 32768) throw std::runtime_error("LOCALAPPDATA is unavailable");
    std::wstring value(length, L'\0');
    const auto written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), length);
    if (!written || written >= length) throw std::runtime_error("LOCALAPPDATA is unavailable");
    value.resize(written);
    (void)local;
    return std::filesystem::path(value) / L"LASO-Computer" / L"config.json";
}

void Config::save_example(const std::filesystem::path& path) const {
    std::filesystem::create_directories(path.parent_path());
    const nlohmann::json output{
        {"version", 1}, {"default_decision", "deny"}, {"capabilities", nlohmann::json::object()},
        {"shell", {{"allowed_executables", nlohmann::json::array()}, {"environment_allowlist", nlohmann::json::array()}}},
        {"plugins", {{"playwright", {{"enabled", false}, {"node_executable", ""}, {"server_entry", ""},
                                       {"browser_executable", ""}}}}},
        {"approval_timeout_ms", 60000}};
    const auto data = output.dump(2) + "\n";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("configuration already exists or cannot be created");
    DWORD written = 0;
    const bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    CloseHandle(file);
    if (!ok) { DeleteFileW(path.c_str()); throw std::runtime_error("configuration could not be written"); }
}

} // namespace laso
