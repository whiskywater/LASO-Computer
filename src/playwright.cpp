#include "laso/playwright.hpp"
#include "laso/protocol.hpp"

#include <WinSock2.h>
#include <Windows.h>
#include <Aclapi.h>
#include <sddl.h>
#include <userenv.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
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

void grant_appcontainer_access(const std::filesystem::path& path, PSID sid, DWORD mask, DWORD inheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL old_acl = nullptr;
    const auto get_result = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &old_acl, nullptr, &descriptor);
    if (get_result != ERROR_SUCCESS) throw std::runtime_error("Playwright plugin isolation setup failed");
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = mask;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = inheritance;
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
    access.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
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
    for (const auto* variable : {L"SystemRoot", L"WINDIR", L"ProgramFiles", L"ProgramFiles(x86)"}) {
        const auto value = environment_value(variable);
        if (!value.empty()) entries.push_back(std::wstring(variable) + L"=" + value);
    }
    const auto local_app_data = environment_value(L"LOCALAPPDATA");
    if (!local_app_data.empty()) entries.push_back(L"LOCALAPPDATA=" + local_app_data);
    entries.push_back(L"TEMP=" + temp_path.wstring());
    entries.push_back(L"TMP=" + temp_path.wstring());
    entries.push_back(L"PLAYWRIGHT_BROWSERS_PATH=" + temp_path.wstring());
    // Playwright 1.64 names its Windows browser IPC pipe from the browser
    // guid and this optional namespace salt. A per-run salt prevents another
    // local Playwright host from occupying the same predictable pipe name.
    entries.push_back(L"PWTEST_SOCKETS_DIR=" + temp_path.wstring());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& entry : entries) { block.insert(block.end(), entry.begin(), entry.end()); block.push_back(L'\0'); }
    block.push_back(L'\0');
    return block;
}

std::vector<wchar_t> browser_environment(const std::filesystem::path& run_path) {
    std::vector<std::wstring> entries;
    for (const auto* variable : {L"SystemRoot", L"WINDIR", L"LOCALAPPDATA", L"ProgramFiles", L"ProgramFiles(x86)", L"USERPROFILE", L"APPDATA"}) {
        const auto value = environment_value(variable);
        if (!value.empty()) entries.push_back(std::wstring(variable) + L"=" + value);
    }
    const auto system_root = environment_value(L"SystemRoot");
    if (!system_root.empty()) entries.push_back(L"PATH=" + system_root + L"\\System32;" + system_root);
    entries.push_back(L"TEMP=" + run_path.wstring());
    entries.push_back(L"TMP=" + run_path.wstring());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& entry : entries) { block.insert(block.end(), entry.begin(), entry.end()); block.push_back(L'\0'); }
    block.push_back(L'\0');
    return block;
}

unsigned short allocate_loopback_port() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("browser endpoint setup failed");
    SOCKET socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket == INVALID_SOCKET) { WSACleanup(); throw std::runtime_error("browser endpoint setup failed"); }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    const bool bound = bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    int length = sizeof(address);
    if (!bound || getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        closesocket(socket); WSACleanup(); throw std::runtime_error("browser endpoint setup failed");
    }
    const auto port = ntohs(address.sin_port);
    closesocket(socket);
    WSACleanup();
    return port;
}

std::string http_get_localhost(unsigned short port, const wchar_t* path) {
    HINTERNET session = WinHttpOpen(L"LASO-Computer/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) throw std::runtime_error("browser endpoint health check failed");
    HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    DWORD timeout = 1000;
    if (request) {
        WinHttpSetOption(request, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(request, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    }
    const bool sent = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    std::string result;
    if (sent) {
        std::array<char, 4096> buffer{};
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            if (available > 65536U - result.size()) { result.clear(); break; }
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available)), &read)) { result.clear(); break; }
            result.append(buffer.data(), read);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

bool loopback_listener_owned_by(unsigned short port, DWORD pid) {
    ULONG bytes = 0;
    if (GetExtendedTcpTable(nullptr, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
        return false;
    std::vector<std::byte> storage(bytes);
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.data());
    if (GetExtendedTcpTable(storage.data(), &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        return false;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (row.dwLocalAddr == htonl(INADDR_LOOPBACK) && ntohs(static_cast<u_short>(row.dwLocalPort)) == port && row.dwOwningPid == pid)
            return true;
    }
    return false;
}

class BrowserProcess {
public:
    BrowserProcess(const std::filesystem::path& executable, const std::filesystem::path& run_path)
        : run_path_(run_path), profile_(run_path / L"profile"), port_(allocate_loopback_port()) {
        std::error_code error;
        if (!std::filesystem::create_directory(profile_, error) || error || !safe_path(profile_, true))
            throw std::runtime_error("isolated browser profile could not be created");
        const auto log_file = run_path_ / L"edge.log";
        const auto command = quote(executable.native()) + L" --headless=new --no-first-run --no-default-browser-check --disable-extensions --disable-sync --disable-features=msEdgeSidebarV2 " +
            L"--enable-logging --log-file=" + quote(log_file.native()) + L" --user-data-dir=" + quote(profile_.native()) +
            L" --remote-debugging-address=127.0.0.1 --remote-debugging-port=" + std::to_wstring(port_) + L" about:blank";
        std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
        auto environment = browser_environment(run_path_);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                environment.data(), run_path_.c_str(), &startup, &info))
            throw std::runtime_error("managed Edge process could not start");
        process_.reset(info.hProcess);
        Handle thread(info.hThread);
        job_.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job_) { TerminateProcess(process_.get(), 1); throw std::runtime_error("browser process isolation failed"); }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 32;
        limits.ProcessMemoryLimit = 2ULL * 1024ULL * 1024ULL * 1024ULL;
        if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job_.get(), process_.get())) {
            TerminateProcess(process_.get(), 1); throw std::runtime_error("browser process isolation failed");
        }
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) throw std::runtime_error("managed Edge process could not start");
        wait_for_ready();
    }
    ~BrowserProcess() { stop(); }
    BrowserProcess(const BrowserProcess&) = delete;
    BrowserProcess& operator=(const BrowserProcess&) = delete;
    [[nodiscard]] unsigned short port() const noexcept { return port_; }
    [[nodiscard]] const std::filesystem::path& profile() const noexcept { return profile_; }
    [[nodiscard]] bool healthy() const noexcept { return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT; }
    void stop() noexcept {
        if (job_) TerminateJobObject(job_.get(), 0);
        else if (process_) TerminateProcess(process_.get(), 0);
        if (process_) WaitForSingleObject(process_.get(), 2000);
        job_.reset(); process_.reset();
    }
private:
    void wait_for_ready() {
        const auto deadline = GetTickCount64() + 30000;
        while (GetTickCount64() < deadline) {
            if (!healthy()) throw std::runtime_error("managed Edge exited during startup");
            const auto body = http_get_localhost(port_, L"/json/version");
            const auto version = nlohmann::json::parse(body, nullptr, false);
            const auto websocket = version.is_object() ? version.value("webSocketDebuggerUrl", std::string{}) : std::string{};
            if (version.is_object() &&
                version.value("Browser", std::string{}).starts_with("Edg/") &&
                websocket.starts_with("ws://127.0.0.1:" + std::to_string(port_) + "/devtools/browser/") &&
                loopback_listener_owned_by(port_, GetProcessId(process_.get()))) return;
            Sleep(100);
        }
        std::string detail = "managed Edge CDP endpoint did not become ready (port=" + std::to_string(port_) +
            ", owned-listener=" + (loopback_listener_owned_by(port_, GetProcessId(process_.get())) ? "yes" : "no") + ")";
        std::ifstream log(log_file_, std::ios::binary);
        if (log) {
            std::string contents((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
            if (contents.size() > 4096) contents.erase(0, contents.size() - 4096);
            if (!contents.empty()) detail += ": " + contents;
        }
        throw std::runtime_error(detail);
    }
    std::filesystem::path run_path_, profile_, log_file_{run_path_ / L"edge.log"};
    unsigned short port_{};
    Handle process_, job_;
};

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
    std::unique_ptr<BrowserProcess> browser;
    std::vector<std::byte> attributes_storage;
    PPROC_THREAD_ATTRIBUTE_LIST attributes{};
    PSID appcontainer_sid{};
    PSID internet_sid{};
    std::filesystem::path plugin_temp;
    std::filesystem::path appcontainer_local;
    std::filesystem::path browser_run;
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
        const std::filesystem::path node(config.node_executable), server(config.server_entry), browser_executable(config.browser_executable);
        if (!safe_path(node, false) || !safe_path(server, false) || !safe_path(browser_executable, false))
            throw std::runtime_error("configured Playwright runtime paths must be existing non-reparse paths");
        const auto package_manifest = server.parent_path() / L"package.json";
        if (!safe_path(package_manifest, false)) throw std::runtime_error("configured Playwright package is invalid");
        std::ifstream manifest_stream(package_manifest, std::ios::binary);
        const std::string manifest_text((std::istreambuf_iterator<char>(manifest_stream)), std::istreambuf_iterator<char>());
        if (!manifest_stream.good() && !manifest_stream.eof()) throw std::runtime_error("configured Playwright package is invalid");
        if (manifest_text.size() > 65536) throw std::runtime_error("configured Playwright package is invalid");
        const auto manifest = nlohmann::json::parse(manifest_text, nullptr, false);
        if (!manifest.is_object() || manifest.value("name", "") != "@playwright/mcp" || manifest.value("version", "") != "0.0.83")
            throw std::runtime_error("Playwright provider package version is not the tested 0.0.83 release");
        const auto local = environment_value(L"LOCALAPPDATA");
        if (local.empty()) throw std::runtime_error("Playwright plugin unavailable");
        PSID app_sid_raw = nullptr;
        const auto profile = CreateAppContainerProfile(L"LASOComputer.Playwright", L"LASO-Computer Playwright", L"Isolated LASO-Computer browser adapter", nullptr, 0, &app_sid_raw);
        if (SUCCEEDED(profile)) appcontainer_sid = app_sid_raw;
        else if (profile == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
            if (FAILED(DeriveAppContainerSidFromAppContainerName(L"LASOComputer.Playwright", &appcontainer_sid)))
                throw std::runtime_error("Playwright plugin isolation setup failed");
        } else throw std::runtime_error("Playwright AppContainer profile unavailable");
        LPWSTR appcontainer_sid_text = nullptr;
        PWSTR appcontainer_folder = nullptr;
        if (!ConvertSidToStringSidW(appcontainer_sid, &appcontainer_sid_text) ||
            FAILED(GetAppContainerFolderPath(appcontainer_sid_text, &appcontainer_folder))) {
            if (appcontainer_sid_text) LocalFree(appcontainer_sid_text);
            throw std::runtime_error("Playwright AppContainer data directory unavailable");
        }
        appcontainer_local = appcontainer_folder;
        LocalFree(appcontainer_sid_text);
        CoTaskMemFree(appcontainer_folder);
        const auto managed_root = std::filesystem::path(local) / L"LASO-Computer";
        // Playwright's Windows server registry uses LOCALAPPDATA\ms-playwright\b
        // even when attaching through CDP. For an AppContainer, Windows maps
        // LOCALAPPDATA to this package's AC directory. Provision that cache
        // path and keep all provider output beneath the same managed subtree.
        const auto playwright_root = appcontainer_local / L"ms-playwright";
        const auto plugin_root = playwright_root / L"LASO-Computer" / L"plugin-temp";
        const auto browser_root = managed_root / L"browser-runs";
        std::error_code ec;
        std::filesystem::create_directories(playwright_root / L"b", ec);
        if (ec || !safe_path(playwright_root / L"b", true)) throw std::runtime_error("Playwright plugin unavailable");
        std::filesystem::create_directories(plugin_root, ec);
        if (ec || !safe_path(plugin_root, true)) throw std::runtime_error("Playwright plugin unavailable");
        std::filesystem::create_directories(browser_root, ec);
        if (ec || !safe_path(browser_root, true)) throw std::runtime_error("Playwright browser runtime unavailable");
        GUID temp_id{};
        wchar_t temp_name[40]{};
        if (FAILED(CoCreateGuid(&temp_id)) || StringFromGUID2(temp_id, temp_name, static_cast<int>(std::size(temp_name))) == 0)
            throw std::runtime_error("Playwright plugin unavailable");
        plugin_temp = plugin_root / temp_name;
        if (!std::filesystem::create_directory(plugin_temp, ec) || ec || !safe_path(plugin_temp, true))
            throw std::runtime_error("Playwright plugin unavailable");
        GUID browser_id{};
        wchar_t browser_name[40]{};
        if (FAILED(CoCreateGuid(&browser_id)) || StringFromGUID2(browser_id, browser_name, static_cast<int>(std::size(browser_name))) == 0)
            throw std::runtime_error("Playwright browser runtime unavailable");
        browser_run = browser_root / browser_name;
        if (!std::filesystem::create_directory(browser_run, ec) || ec || !safe_path(browser_run, true))
            throw std::runtime_error("Playwright browser runtime unavailable");
        browser = std::make_unique<BrowserProcess>(browser_executable, browser_run);
        const auto provider_config = plugin_temp / L"provider-config.json";
        {
            std::ofstream config_stream(provider_config, std::ios::binary | std::ios::trunc);
            const auto config_json = nlohmann::json{
                {"browser", {{"cdpEndpoint", "http://127.0.0.1:" + std::to_string(browser->port())}, {"cdpTimeout", 10000}}}
            }.dump();
            config_stream.write(config_json.data(), static_cast<std::streamsize>(config_json.size()));
            config_stream.flush();
            if (!config_stream) throw std::runtime_error("Playwright plugin configuration failed");
        }
        const auto node_directory = node.parent_path();
        auto package_directory = server.parent_path();
        for (auto parent = package_directory; !parent.empty(); parent = parent.parent_path()) {
            if (_wcsicmp(parent.filename().c_str(), L"node_modules") == 0) { package_directory = parent; break; }
            if (parent == parent.root_path()) break;
        }
        if (!safe_path(node_directory, true) || !safe_path(package_directory, true))
            throw std::runtime_error("Playwright plugin directories must be existing non-reparse directories");
        const auto read_execute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE | FILE_LIST_DIRECTORY | FILE_TRAVERSE;
        const auto grant_once = [this](const std::filesystem::path& path, DWORD mask, DWORD inheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT) {
            if (std::find(acl_paths.begin(), acl_paths.end(), path) != acl_paths.end()) return;
            grant_appcontainer_access(path, appcontainer_sid, mask, inheritance);
            acl_paths.push_back(path);
        };
        grant_once(node_directory, read_execute);
        grant_once(package_directory, read_execute);
        grant_once(server.parent_path(), read_execute);
        // Installed Edge under Program Files already grants read/execute to
        // ALL APPLICATION PACKAGES. Do not alter its ACL from the endpoint.
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
        SECURITY_CAPABILITIES security{appcontainer_sid, nullptr, 0, 0};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &security, sizeof(security), nullptr, nullptr))
            throw std::runtime_error("Playwright AppContainer permission setup failed");

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = child_input.get();
        startup.StartupInfo.hStdOutput = child_output.get();
        startup.StartupInfo.hStdError = child_stderr.get();
        startup.lpAttributeList = attributes;
        // In Node/libuv on Windows AppContainer, realpath's GetFinalPathNameByHandleW
        // DOS-volume translation is denied even for AppContainer-owned output paths.
        // MCP 0.0.83's documented flag skips that workspace/file canonicalization
        // guard. The OS AppContainer ACL, one-process Job Object, and this adapter's
        // fixed typed tools remain the enforcement boundary; do not grant C:\ access.
        std::wstring command = quote(node.native()) + L" --preserve-symlinks --preserve-symlinks-main " + quote(server.native()) +
            L" --config " + quote(provider_config.native()) + L" --no-webmcp --output-dir " + quote(plugin_temp.native()) +
            L" --allow-unrestricted-file-access" +
            L" --timeout-action 5000 --timeout-navigation 20000 --output-max-size 1048576";
        std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
        auto environment = safe_environment(plugin_temp);
        PROCESS_INFORMATION info{};
        const auto cwd = node_directory.wstring();
        if (!CreateProcessW(node.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                environment.data(), cwd.c_str(), &startup.StartupInfo, &info))
            throw std::runtime_error("Playwright plugin process could not start (win32=" + std::to_string(GetLastError()) + ")");
        process.reset(info.hProcess);
        Handle thread(info.hThread);
        child_input.reset(); child_output.reset(); child_stderr.reset();
        job.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job) { TerminateProcess(process.get(), 1); throw std::runtime_error("Playwright plugin isolation failed"); }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        // The attached MCP provider must not create its own browser or any
        // other child process. The C++ runtime owns Edge in a separate job.
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
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
        auto payload = value.dump();
        if (payload.size() + 1 > max_frame_bytes) throw std::runtime_error("Playwright request exceeds size limit");
        payload.push_back('\n');
        std::size_t offset = 0;
        while (offset < payload.size()) {
            DWORD written = 0;
            const auto remaining = payload.size() - offset;
            const auto amount = static_cast<DWORD>(std::min<std::size_t>(remaining, 16U * 1024U));
            if (!WriteFile(input.get(), payload.data() + offset, amount, &written, nullptr) || written == 0)
                throw std::runtime_error("Playwright provider transport failed");
            offset += written;
        }
    }

    std::string receive(const std::function<bool()>& cancelled, ULONGLONG deadline) {
        for (;;) {
            if (cancelled && cancelled()) throw std::runtime_error("cancelled");
            if (GetTickCount64() >= deadline) throw std::runtime_error("timed out");
            const auto newline = buffered.find('\n');
            if (newline != std::string::npos) {
                if (newline + 1 > max_frame_bytes) throw std::runtime_error("Playwright response exceeds size limit");
                std::string body = buffered.substr(0, newline);
                buffered.erase(0, newline + 1);
                if (!body.empty() && body.back() == '\r') body.pop_back();
                if (body.empty()) throw std::runtime_error("invalid Playwright protocol frame");
                return body;
            }
            if (buffered.size() >= max_frame_bytes) throw std::runtime_error("Playwright response exceeds size limit");
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
            } else {
                if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) throw std::runtime_error("Playwright provider stopped");
                Sleep(5);
            }
        }
    }

    nlohmann::json receive_response(unsigned long long expected, const std::function<bool()>& cancelled, ULONGLONG deadline) {
        for (;;) {
            nlohmann::json response;
            try { response = nlohmann::json::parse(receive(cancelled, deadline)); }
            catch (const nlohmann::json::exception&) { throw std::runtime_error("invalid Playwright protocol message"); }
            if (!response.is_object() || !response.contains("jsonrpc") || !response.at("jsonrpc").is_string() ||
                response.at("jsonrpc").get_ref<const std::string&>() != "2.0")
                throw std::runtime_error("invalid Playwright protocol message");
            if (!response.contains("id")) {
                if (!response.contains("method") || !response.at("method").is_string())
                    throw std::runtime_error("invalid Playwright protocol notification");
                continue;
            }
            if (!response.at("id").is_number_integer() && !response.at("id").is_string())
                throw std::runtime_error("invalid Playwright protocol request id");
            if (response.at("id") == expected && !response.contains("method") &&
                (response.contains("result") || response.contains("error"))) return response;
            if (!response.contains("method")) {
                if (response.contains("result") || response.contains("error")) continue;
                throw std::runtime_error("invalid Playwright protocol response");
            }
            if (!response.at("method").is_string()) throw std::runtime_error("invalid Playwright protocol request");
            const auto method = response.value("method", "");
            if (method == "ping") {
                send({{"jsonrpc", "2.0"}, {"id", response.at("id")}, {"result", nlohmann::json::object()}});
            } else {
                send({{"jsonrpc", "2.0"}, {"id", response.at("id")},
                      {"error", {{"code", -32601}, {"message", "Method not found"}}}});
            }
        }
    }

    void initialize() {
        const auto id = next_id++;
        send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "initialize"},
              {"params", {{"protocolVersion", "2025-03-26"}, {"capabilities", nlohmann::json::object()},
                           {"clientInfo", {{"name", "laso-computer"}, {"version", "1.0.0"}}}}}});
        const auto response = receive_response(id, {}, GetTickCount64() + 30000);
        if (response.contains("error") || !response.contains("result")) throw std::runtime_error("Playwright provider initialization failed");
        const auto& result = response.at("result");
        if (!result.is_object() || result.value("protocolVersion", "") != "2025-03-26" ||
            !result.contains("serverInfo") || !result.at("serverInfo").is_object() ||
            result.at("serverInfo").value("name", "") != "Playwright" ||
            result.at("serverInfo").value("version", "") != "1.64.0-alpha-1790635538000")
            throw std::runtime_error("Playwright MCP handshake did not match the pinned provider build");
        send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});

        const auto tools_id = next_id++;
        send({{"jsonrpc", "2.0"}, {"id", tools_id}, {"method", "tools/list"}, {"params", nlohmann::json::object()}});
        const auto tools_response = receive_response(tools_id, {}, GetTickCount64() + 30000);
        if (tools_response.contains("error") || !tools_response.contains("result") ||
            !tools_response.at("result").is_object() || !tools_response.at("result").contains("tools") ||
            !tools_response.at("result").at("tools").is_array())
            throw std::runtime_error("Playwright MCP tools handshake failed");
        constexpr std::array<std::string_view, 9> required_tools{
            "browser_navigate", "browser_snapshot", "browser_find", "browser_click", "browser_type",
            "browser_select_option", "browser_tabs", "browser_navigate_back", "browser_take_screenshot"};
        const auto& available = tools_response.at("result").at("tools");
        for (const auto tool : required_tools) {
            if (std::none_of(available.begin(), available.end(), [tool](const auto& item) {
                    return item.is_object() && item.value("name", "") == tool;
                }))
                throw std::runtime_error("Playwright MCP required tool is unavailable");
        }

        // MCP initialization alone does not launch the browser. Probe an
        // actual browser operation before advertising capabilities so a
        // provider with a broken browser subprocess fails closed at startup.
        const auto browser_id = next_id++;
        send({{"jsonrpc", "2.0"}, {"id", browser_id}, {"method", "tools/call"},
              {"params", {{"name", "browser_tabs"}, {"arguments", {{"action", "list"}}}}}});
        const auto browser_response = receive_response(browser_id, {}, GetTickCount64() + 10000);
        if (browser_response.contains("error") || !browser_response.contains("result") ||
            browser_response.at("result").value("isError", false))
            throw std::runtime_error("Playwright MCP could not attach to the managed browser");
    }

    nlohmann::json call(std::string tool, nlohmann::json args, const InvocationContext& invocation) {
        std::scoped_lock guard(lock);
        if (!healthy || !process || WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0 || !browser || !browser->healthy()) {
            healthy = false;
            stop();
            throw std::runtime_error("capability unavailable");
        }
        const auto id = next_id++;
        nlohmann::json response;
        try {
            send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
                  {"params", {{"name", std::move(tool)}, {"arguments", std::move(args)}}}});
            response = receive_response(id, invocation.cancelled,
                GetTickCount64() + std::clamp(invocation.timeout_ms, 100U, 300000U));
        } catch (...) {
            // A timeout/cancellation or broken transport desynchronizes the
            // single-flight MCP channel. Tear down the whole job immediately.
            stop();
            throw;
        }
        if (response.contains("error") || !response.contains("result")) throw std::runtime_error("Playwright provider request failed");
        const auto& result = response.at("result");
        if (result.value("isError", false)) {
            if (!browser || !browser->healthy()) {
                healthy = false;
                stop();
                throw std::runtime_error("managed browser exited during browser action");
            }
            throw std::runtime_error("Playwright browser action failed: " + result.dump().substr(0, 1024));
        }
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
        if (browser) { browser->stop(); browser.reset(); }
        if (appcontainer_sid) {
            for (const auto& path : acl_paths) revoke_appcontainer_access(path, appcontainer_sid);
        }
        acl_paths.clear();
        if (!plugin_temp.empty() && safe_path(plugin_temp, true)) {
            std::error_code cleanup_error;
            std::filesystem::remove_all(plugin_temp, cleanup_error);
        }
        plugin_temp.clear();
        if (!browser_run.empty() && safe_path(browser_run, true)) {
            std::error_code cleanup_error;
            std::filesystem::remove_all(browser_run, cleanup_error);
        }
        browser_run.clear();
        if (attributes) { DeleteProcThreadAttributeList(attributes); attributes = nullptr; }
        attributes_storage.clear();
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
    const bool running = health();
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
    else if (invocation.capability == "browser.screenshot") { tool = "browser_take_screenshot"; mapped["scale"] = "css"; }
    else throw std::runtime_error("capability unavailable");
    return impl_->call(std::move(tool), std::move(mapped), invocation);
}

bool PlaywrightMcpProvider::health() const {
    if (!impl_) return false;
    std::scoped_lock guard(impl_->lock);
    const bool running = impl_->healthy && impl_->process && WaitForSingleObject(impl_->process.get(), 0) == WAIT_TIMEOUT &&
        impl_->browser && impl_->browser->healthy();
    if (!running) {
        impl_->healthy = false;
        impl_->stop();
    }
    return running;
}
std::string PlaywrightMcpProvider::status_detail() const {
    if (!impl_ || impl_->healthy) return "ready";
    std::scoped_lock guard(impl_->diagnostics_lock);
    if (impl_->startup_diagnostics.find("EPERM") != std::string::npos)
        return "AppContainer denied Node.js runtime path resolution; provider remains disabled";
    return impl_->failure;
}
void PlaywrightMcpProvider::shutdown() noexcept { if (impl_) impl_->stop(); }

} // namespace laso
