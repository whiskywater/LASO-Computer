#include "laso/playwright.hpp"
#include "laso/protocol.hpp"

#include <Windows.h>
#include <Aclapi.h>
#include <sddl.h>
#include <userenv.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace laso {
namespace {

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) { reset(); value_ = std::exchange(other.value_, nullptr); } return *this; }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE next = nullptr) { if (*this) CloseHandle(value_); value_ = next; }
private:
    HANDLE value_{nullptr};
};

std::wstring quote(std::wstring_view value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(value);
    std::wstring out(1, L'"');
    std::size_t slashes = 0;
    for (const auto ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'"') { out.append(slashes * 2 + 1, L'\\'); out.push_back(ch); slashes = 0; continue; }
        out.append(slashes, L'\\'); slashes = 0; out.push_back(ch);
    }
    out.append(slashes * 2, L'\\'); out.push_back(L'"');
    return out;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("plugin configuration is invalid");
    std::string out(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr) != size)
        throw std::runtime_error("plugin configuration is invalid");
    return out;
}

bool safe_path(const std::filesystem::path& path, bool directory) {
    if (path.empty() || !path.is_absolute() || path.lexically_normal() != path) return false;
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    if (directory ? (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 : (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    for (auto ancestor = path.parent_path(); !ancestor.empty(); ancestor = ancestor.parent_path()) {
        const auto parent_attributes = GetFileAttributesW(ancestor.c_str());
        if (parent_attributes == INVALID_FILE_ATTRIBUTES || (parent_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
        if (ancestor == ancestor.root_path()) break;
    }
    return true;
}

void grant_appcontainer_access(const std::filesystem::path& path, PSID sid, DWORD mask) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL old_acl = nullptr;
    const auto get_result = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &old_acl, nullptr, &descriptor);
    if (get_result != ERROR_SUCCESS) throw std::runtime_error("Playwright plugin isolation setup failed");
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = mask;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    PACL updated_acl = nullptr;
    const auto acl_result = SetEntriesInAclW(1, &access, old_acl, &updated_acl);
    if (acl_result != ERROR_SUCCESS) {
        LocalFree(descriptor);
        throw std::runtime_error("Playwright plugin isolation setup failed");
    }
    const auto set_result = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, updated_acl, nullptr);
    LocalFree(updated_acl);
    LocalFree(descriptor);
    if (set_result != ERROR_SUCCESS) throw std::runtime_error("Playwright plugin isolation setup failed");
}

void revoke_appcontainer_access(const std::filesystem::path& path, PSID sid) noexcept {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL old_acl = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &old_acl, nullptr, &descriptor) != ERROR_SUCCESS) return;
    EXPLICIT_ACCESSW access{};
    access.grfAccessMode = REVOKE_ACCESS;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    PACL updated_acl = nullptr;
    if (SetEntriesInAclW(1, &access, old_acl, &updated_acl) == ERROR_SUCCESS) {
        SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, updated_acl, nullptr);
        LocalFree(updated_acl);
    }
    LocalFree(descriptor);
}

std::wstring environment_value(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed || needed > 32768) return {};
    std::wstring result(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, result.data(), needed);
    if (!written || written >= needed) return {};
    result.resize(written);
    return result;
}

std::vector<wchar_t> safe_environment(const std::filesystem::path& temp_path) {
    std::vector<std::wstring> entries;
    for (const auto* variable : {L"SystemRoot", L"WINDIR", L"LOCALAPPDATA", L"ProgramFiles", L"ProgramFiles(x86)"}) {
        const auto value = environment_value(variable);
        if (!value.empty()) entries.push_back(std::wstring(variable) + L"=" + value);
    }
    entries.push_back(L"TEMP=" + temp_path.wstring());
    entries.push_back(L"TMP=" + temp_path.wstring());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& entry : entries) { block.insert(block.end(), entry.begin(), entry.end()); block.push_back(L'\0'); }
    block.push_back(L'\0');
    return block;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

} // namespace

struct PlaywrightMcpProvider::Impl {
    explicit Impl(Config::Playwright settings) : config(std::move(settings)) {
        try { start(); healthy = true; }
        catch (const std::exception& error) { failure = error.what(); stop(); healthy = false; }
        catch (...) { failure = "unknown plugin startup failure"; stop(); healthy = false; }
    }

    Config::Playwright config;
    Handle process, job, input, output, stderr_output;
    std::vector<std::byte> attributes_storage;
    PPROC_THREAD_ATTRIBUTE_LIST attributes{};
    PSID appcontainer_sid{};
    PSID internet_sid{};
    std::filesystem::path plugin_temp;
    std::vector<std::filesystem::path> acl_paths;
    std::mutex lock;
    std::mutex diagnostics_lock;
    std::thread stderr_reader;
    std::string startup_diagnostics;
    bool healthy{false};
    std::string failure;
    unsigned long long next_id{1};
    std::string buffered;

    ~Impl() { stop(); }

    void start() {
        const std::filesystem::path node(config.node_executable), server(config.server_entry);
        if (!safe_path(node, false) || !safe_path(server, false)) throw std::runtime_error("configured Playwright runtime paths must be existing non-reparse paths");
        const auto local = environment_value(L"LOCALAPPDATA");
        if (local.empty()) throw std::runtime_error("Playwright plugin unavailable");
        plugin_temp = std::filesystem::path(local) / L"LASO-Computer" / L"plugin-temp";
        std::error_code ec;
        std::filesystem::create_directories(plugin_temp, ec);
        if (ec || !safe_path(plugin_temp, true)) throw std::runtime_error("Playwright plugin unavailable");

        PSID app_sid_raw = nullptr;
        const auto profile = CreateAppContainerProfile(L"LASOComputer.Playwright", L"LASO-Computer Playwright", L"Isolated LASO-Computer browser adapter", nullptr, 0, &app_sid_raw);
        if (SUCCEEDED(profile)) appcontainer_sid = app_sid_raw;
        else if (profile == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
            if (FAILED(DeriveAppContainerSidFromAppContainerName(L"LASOComputer.Playwright", &appcontainer_sid)))
                throw std::runtime_error("Playwright plugin isolation setup failed");
        } else throw std::runtime_error("Playwright AppContainer profile unavailable");
        if (!ConvertStringSidToSidW(L"S-1-15-3-1", &internet_sid)) throw std::runtime_error("Playwright internet capability unavailable");

        const auto node_directory = node.parent_path();
        auto package_directory = server.parent_path();
        for (auto parent = package_directory; !parent.empty(); parent = parent.parent_path()) {
            if (_wcsicmp(parent.filename().c_str(), L"node_modules") == 0) { package_directory = parent; break; }
            if (parent == parent.root_path()) break;
        }
        if (!safe_path(node_directory, true) || !safe_path(package_directory, true))
            throw std::runtime_error("Playwright plugin directories must be existing non-reparse directories");
        grant_appcontainer_access(node_directory, appcontainer_sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE | FILE_LIST_DIRECTORY | FILE_TRAVERSE);
        acl_paths.push_back(node_directory);
        if (package_directory != node_directory)
            { grant_appcontainer_access(package_directory, appcontainer_sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE | FILE_LIST_DIRECTORY | FILE_TRAVERSE); acl_paths.push_back(package_directory); }
        grant_appcontainer_access(plugin_temp, appcontainer_sid, FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | FILE_LIST_DIRECTORY | FILE_TRAVERSE | DELETE);
        acl_paths.push_back(plugin_temp);

        SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
        HANDLE child_input_raw = nullptr, parent_input_raw = nullptr, parent_output_raw = nullptr, child_output_raw = nullptr;
        HANDLE parent_stderr_raw = nullptr, child_stderr_raw = nullptr;
        if (!CreatePipe(&child_input_raw, &parent_input_raw, &inherited, 0) ||
            !CreatePipe(&parent_output_raw, &child_output_raw, &inherited, 0) ||
            !CreatePipe(&parent_stderr_raw, &child_stderr_raw, &inherited, 0)) {
            if (child_input_raw) CloseHandle(child_input_raw); if (parent_input_raw) CloseHandle(parent_input_raw);
            if (parent_output_raw) CloseHandle(parent_output_raw); if (child_output_raw) CloseHandle(child_output_raw);
            if (parent_stderr_raw) CloseHandle(parent_stderr_raw); if (child_stderr_raw) CloseHandle(child_stderr_raw);
            throw std::runtime_error("Playwright plugin process setup failed");
        }
        Handle child_input(child_input_raw), parent_input(parent_input_raw), parent_output(parent_output_raw), child_output(child_output_raw);
        Handle parent_stderr(parent_stderr_raw), child_stderr(child_stderr_raw);
        SetHandleInformation(parent_input.get(), HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(parent_output.get(), HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(parent_stderr.get(), HANDLE_FLAG_INHERIT, 0);

        SIZE_T attribute_bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &attribute_bytes);
        attributes_storage.resize(attribute_bytes);
        attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes_storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 2, 0, &attribute_bytes)) throw std::runtime_error("Playwright plugin process setup failed");
        HANDLE inherited_handles[]{child_input.get(), child_output.get(), child_stderr.get()};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles, sizeof(inherited_handles), nullptr, nullptr))
            throw std::runtime_error("Playwright plugin process setup failed");
        SID_AND_ATTRIBUTES capability{internet_sid, SE_GROUP_ENABLED};
        SECURITY_CAPABILITIES security{appcontainer_sid, &capability, 1, 0};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &security, sizeof(security), nullptr, nullptr))
            throw std::runtime_error("Playwright AppContainer permission setup failed");

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = child_input.get();
        startup.StartupInfo.hStdOutput = child_output.get();
        startup.StartupInfo.hStdError = child_stderr.get();
        startup.lpAttributeList = attributes;
        // Node's default realpath pass walks the volume root while resolving
        // modules. Keeping resolved module paths avoids that broad traversal;
        // package and executable paths are separately checked above.
        std::wstring command = quote(node.native()) + L" --preserve-symlinks " + quote(server.native()) +
            L" --isolated --browser msedge --headless --no-webmcp --output-dir " + quote(plugin_temp.native()) +
            L" --timeout-action 5000 --timeout-navigation 20000 --output-max-size 1048576";
        std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
        auto environment = safe_environment(plugin_temp);
        PROCESS_INFORMATION info{};
        const auto cwd = node_directory.wstring();
        if (!CreateProcessW(node.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                environment.data(), cwd.c_str(), &startup.StartupInfo, &info))
            throw std::runtime_error("Playwright plugin process could not start");
        process.reset(info.hProcess);
        Handle thread(info.hThread);
        child_input.reset(); child_output.reset(); child_stderr.reset();
        job.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job) { TerminateProcess(process.get(), 1); throw std::runtime_error("Playwright plugin isolation failed"); }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 16;
        limits.ProcessMemoryLimit = 512U * 1024U * 1024U;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job.get(), process.get())) {
            TerminateProcess(process.get(), 1); throw std::runtime_error("Playwright plugin isolation failed");
        }
        input = std::move(parent_input); output = std::move(parent_output); stderr_output = std::move(parent_stderr);
        stderr_reader = std::thread([this] {
            std::array<char, 2048> bytes{};
            for (;;) {
                DWORD count = 0;
                if (!ReadFile(stderr_output.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count == 0) break;
                std::scoped_lock guard(diagnostics_lock);
                if (startup_diagnostics.size() < 8192) {
                    const auto keep = std::min<std::size_t>(count, 8192 - startup_diagnostics.size());
                    startup_diagnostics.append(bytes.data(), keep);
                }
            }
        });
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) throw std::runtime_error("Playwright plugin process could not start");
        initialize();
    }

    void send(const nlohmann::json& value) {
        const auto payload = value.dump();
        if (payload.size() > max_frame_bytes) throw std::runtime_error("Playwright request exceeds size limit");
        const auto header = std::string("Content-Length: ") + std::to_string(payload.size()) + "\r\n\r\n";
        DWORD written = 0;
        if (!WriteFile(input.get(), header.data(), static_cast<DWORD>(header.size()), &written, nullptr) || written != header.size() ||
            !WriteFile(input.get(), payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) || written != payload.size())
            throw std::runtime_error("Playwright provider transport failed");
    }

    std::string receive(const std::function<bool()>& cancelled, ULONGLONG deadline) {
        for (;;) {
            if (cancelled && cancelled()) throw std::runtime_error("cancelled");
            if (GetTickCount64() >= deadline) throw std::runtime_error("timed out");
            DWORD available = 0;
            if (!PeekNamedPipe(output.get(), nullptr, 0, nullptr, &available, nullptr)) {
                DWORD exit_code = STILL_ACTIVE;
                if (process) GetExitCodeProcess(process.get(), &exit_code);
                if (exit_code != STILL_ACTIVE) throw std::runtime_error("Playwright provider stopped (exit " + std::to_string(exit_code) + ")");
                throw std::runtime_error("Playwright provider transport failed");
            }
            if (available) {
                std::array<char, 8192> bytes{};
                DWORD read = 0;
                if (!ReadFile(output.get(), bytes.data(), std::min<DWORD>(available, static_cast<DWORD>(bytes.size())), &read, nullptr) || !read)
                    throw std::runtime_error("Playwright provider transport failed");
                buffered.append(bytes.data(), read);
                if (buffered.size() > max_frame_bytes + 4096) throw std::runtime_error("Playwright response exceeds size limit");
                const auto header_end = buffered.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    std::size_t size = 0;
                    bool found = false;
                    std::size_t line = 0;
                    while (line < header_end) {
                        const auto end = buffered.find("\r\n", line);
                        const auto actual_end = end == std::string::npos || end > header_end ? header_end : end;
                        const auto content = std::string_view(buffered).substr(line, actual_end - line);
                        constexpr std::string_view key = "content-length:";
                        if (content.size() >= key.size() && lower(std::string(content.substr(0, key.size()))) == key) {
                            const auto first = content.find_first_not_of(" \t", key.size());
                            if (first == std::string_view::npos) throw std::runtime_error("invalid Playwright protocol frame");
                            const auto [ptr, error] = std::from_chars(content.data() + first, content.data() + content.size(), size);
                            if (error != std::errc{} || ptr != content.data() + content.size()) throw std::runtime_error("invalid Playwright protocol frame");
                            found = true;
                        }
                        line = actual_end + 2;
                    }
                    if (!found || size == 0 || size > max_frame_bytes) throw std::runtime_error("invalid Playwright protocol frame");
                    if (buffered.size() >= header_end + 4 + size) {
                        std::string body = buffered.substr(header_end + 4, size);
                        buffered.erase(0, header_end + 4 + size);
                        return body;
                    }
                }
            } else {
                if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) throw std::runtime_error("Playwright provider stopped");
                Sleep(5);
            }
        }
    }

    nlohmann::json receive_response(unsigned long long expected, const std::function<bool()>& cancelled, ULONGLONG deadline) {
        for (;;) {
            const auto response = nlohmann::json::parse(receive(cancelled, deadline));
            if (response.is_object() && response.value("id", nlohmann::json{}) == expected) return response;
        }
    }

    void initialize() {
        const auto id = next_id++;
        send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "initialize"},
              {"params", {{"protocolVersion", "2025-03-26"}, {"capabilities", nlohmann::json::object()},
                           {"clientInfo", {{"name", "laso-computer"}, {"version", "1.0.0"}}}}}});
        const auto response = receive_response(id, {}, GetTickCount64() + 30000);
        if (response.contains("error") || !response.contains("result")) throw std::runtime_error("Playwright provider initialization failed");
        send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    }

    nlohmann::json call(std::string tool, nlohmann::json args, const InvocationContext& invocation) {
        std::scoped_lock guard(lock);
        if (!healthy || !process || WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
            healthy = false;
            throw std::runtime_error("capability unavailable");
        }
        const auto id = next_id++;
        send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
              {"params", {{"name", std::move(tool)}, {"arguments", std::move(args)}}}});
        const auto response = receive_response(id, invocation.cancelled, GetTickCount64() + std::clamp(invocation.timeout_ms, 100U, 300000U));
        if (response.contains("error") || !response.contains("result")) throw std::runtime_error("Playwright provider request failed");
        const auto& result = response.at("result");
        if (result.value("isError", false)) throw std::runtime_error("Playwright browser action failed");
        if (result.contains("content") && result["content"].dump().size() > max_frame_bytes - 4096)
            throw std::runtime_error("Playwright result exceeds bounded output size");
        return {{"plugin", "playwright.mcp"}, {"content", result.value("content", nlohmann::json::array())}};
    }

    void stop() noexcept {
        if (input) input.reset();
        if (job) TerminateJobObject(job.get(), 0);
        else if (process) TerminateProcess(process.get(), 0);
        if (process) WaitForSingleObject(process.get(), 2000);
        job.reset(); process.reset(); output.reset(); stderr_output.reset();
        if (stderr_reader.joinable()) stderr_reader.join();
        if (appcontainer_sid) {
            for (const auto& path : acl_paths) revoke_appcontainer_access(path, appcontainer_sid);
        }
        acl_paths.clear();
        if (attributes) { DeleteProcThreadAttributeList(attributes); attributes = nullptr; }
        attributes_storage.clear();
        if (internet_sid) { LocalFree(internet_sid); internet_sid = nullptr; }
        if (appcontainer_sid) { FreeSid(appcontainer_sid); appcontainer_sid = nullptr; }
        healthy = false;
    }
};

PlaywrightMcpProvider::PlaywrightMcpProvider(Config::Playwright config) : impl_(std::make_unique<Impl>(std::move(config))) {}
PlaywrightMcpProvider::~PlaywrightMcpProvider() = default;

PluginIdentity PlaywrightMcpProvider::identity() const {
    return {"playwright.mcp", "MCP", "Out-of-process Playwright MCP adapter in a Windows AppContainer", false};
}

std::vector<CapabilityDescriptor> PlaywrightMcpProvider::capabilities() const {
    using Json = nlohmann::json;
    const auto schema = [](Json properties, std::initializer_list<const char*> required = {}) {
        Json result{{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
        if (required.size()) result["required"] = required;
        return result;
    };
    const auto string = [](int maximum = 4096) { return Json{{"type", "string"}, {"maxLength", maximum}}; };
    const bool running = impl_ && impl_->healthy;
    return {
        {"browser.navigate", "Navigate the isolated Playwright browser to an HTTP(S) URL", "network_write",
            schema(Json{{"url", string(8192)}}, {"url"}), running, {}},
        {"browser.snapshot", "Read a bounded browser accessibility snapshot", "sensitive_read",
            schema(Json{{"depth", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 12}}}}), running, {}},
        {"browser.query", "Find text in the browser accessibility snapshot", "sensitive_read",
            schema(Json{{"text", string(2048)}}, {"text"}), running, {}},
        {"browser.click", "Click an element identified by a Playwright snapshot ref", "interaction",
            schema(Json{{"target", string(1024)}, {"element", string(512)}}, {"target"}), running, {}},
        {"browser.fill", "Fill an editable element identified by a Playwright snapshot ref", "sensitive_write",
            schema(Json{{"target", string(1024)}, {"element", string(512)}, {"text", string(8192)}}, {"target", "text"}), running, {}},
        {"browser.select", "Select an option in an element identified by a snapshot ref", "interaction",
            schema(Json{{"target", string(1024)}, {"element", string(512)}, {"value", string(1024)}}, {"target", "value"}), running, {}},
        {"browser.tabs", "List or select a tab in the isolated browser", "interaction",
            schema(Json{{"action", Json{{"enum", Json{"list", "select"}}}},
                        {"index", Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 64}}}}, {"action"}), running, {}},
        {"browser.back", "Navigate the isolated browser back", "network_write", schema(Json::object()), running, {}},
        {"browser.screenshot", "Capture the current isolated browser page", "sensitive_read", schema(Json::object()), running, {}}
    };
}

nlohmann::json PlaywrightMcpProvider::invoke(const InvocationContext& invocation) {
    const auto& args = invocation.arguments;
    std::string tool;
    nlohmann::json mapped = nlohmann::json::object();
    if (invocation.capability == "browser.navigate") {
        if (!args.contains("url") || !args["url"].is_string() || args["url"].get_ref<const std::string&>().size() > 8192)
            throw std::runtime_error("invalid capability request");
        const auto url = args["url"].get<std::string>();
        const auto lowered = lower(url);
        const auto scheme_size = lowered.starts_with("https://") ? 8U : lowered.starts_with("http://") ? 7U : 0U;
        const auto authority_end = url.find_first_of("/?#", scheme_size);
        const auto at = url.find('@');
        if (scheme_size == 0 || (at != std::string::npos && (authority_end == std::string::npos || at < authority_end)))
            throw std::runtime_error("invalid browser URL");
        tool = "browser_navigate"; mapped["url"] = url;
    } else if (invocation.capability == "browser.snapshot") {
        tool = "browser_snapshot";
        if (args.contains("depth")) mapped["depth"] = args.at("depth");
    } else if (invocation.capability == "browser.query") {
        if (!args.contains("text") || !args["text"].is_string() || args["text"].get_ref<const std::string&>().empty() || args["text"].get_ref<const std::string&>().size() > 2048)
            throw std::runtime_error("invalid capability request");
        tool = "browser_find"; mapped["text"] = args.at("text");
    } else if (invocation.capability == "browser.click" || invocation.capability == "browser.fill" || invocation.capability == "browser.select") {
        if (!args.contains("target") || !args["target"].is_string() || args["target"].get_ref<const std::string&>().empty() || args["target"].get_ref<const std::string&>().size() > 1024)
            throw std::runtime_error("invalid capability request");
        mapped["target"] = args.at("target");
        if (args.contains("element")) mapped["element"] = args.at("element");
        if (invocation.capability == "browser.click") tool = "browser_click";
        else if (invocation.capability == "browser.fill") {
            if (!args.contains("text") || !args["text"].is_string() || args["text"].get_ref<const std::string&>().size() > 8192)
                throw std::runtime_error("invalid capability request");
            tool = "browser_type"; mapped["text"] = args.at("text");
        } else {
            if (!args.contains("value") || !args["value"].is_string() || args["value"].get_ref<const std::string&>().size() > 1024)
                throw std::runtime_error("invalid capability request");
            tool = "browser_select_option"; mapped["values"] = nlohmann::json::array({args.at("value")});
        }
    } else if (invocation.capability == "browser.tabs") {
        if (!args.contains("action") || !args["action"].is_string()) throw std::runtime_error("invalid capability request");
        const auto action = args.at("action").get<std::string>();
        if (action != "list" && action != "select") throw std::runtime_error("invalid capability request");
        if (action == "select" && (!args.contains("index") || !args["index"].is_number_integer() || args.at("index").get<int>() < 0 || args.at("index").get<int>() > 64))
            throw std::runtime_error("invalid capability request");
        tool = "browser_tabs"; mapped["action"] = action;
        if (args.contains("index")) mapped["index"] = args.at("index");
    } else if (invocation.capability == "browser.back") tool = "browser_navigate_back";
    else if (invocation.capability == "browser.screenshot") tool = "browser_take_screenshot";
    else throw std::runtime_error("capability unavailable");
    return impl_->call(std::move(tool), std::move(mapped), invocation);
}

bool PlaywrightMcpProvider::health() const { return impl_ && impl_->healthy && impl_->process && WaitForSingleObject(impl_->process.get(), 0) == WAIT_TIMEOUT; }
std::string PlaywrightMcpProvider::status_detail() const {
    if (!impl_ || impl_->healthy) return "ready";
    std::scoped_lock guard(impl_->diagnostics_lock);
    if (impl_->startup_diagnostics.find("EPERM") != std::string::npos)
        return "AppContainer denied Node.js runtime path resolution; provider remains disabled";
    return impl_->failure;
}
void PlaywrightMcpProvider::shutdown() noexcept { if (impl_) impl_->stop(); }

} // namespace laso
