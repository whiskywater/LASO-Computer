#include "laso/broker.hpp"

#include <windows.h>
#include <sddl.h>
#include <userenv.h>
#include <netfw.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using laso::broker::AuditSink;
using laso::broker::CallerIdentity;
using laso::broker::Dispatcher;
using laso::broker::LoopbackStatus;
using laso::broker::PrivilegedState;
using Json = nlohmann::json;

constexpr wchar_t audit_root_name[] = L"LASO-Computer\\Broker";
constexpr wchar_t audit_file_name[] = L"audit.jsonl";
std::atomic<HANDLE> active_pipe{INVALID_HANDLE_VALUE};
HANDLE stop_event = nullptr;
SERVICE_STATUS_HANDLE service_status_handle = nullptr;
SERVICE_STATUS service_status{};
std::mutex service_status_mutex;
std::wstring authorized_sid_text;

struct LocalFreeDeleter { void operator()(void* value) const noexcept { if (value) LocalFree(value); } };
struct SidDeleter { void operator()(void* value) const noexcept { if (value) FreeSid(value); } };

using GetLoopbackConfig = DWORD(WINAPI*)(DWORD*, PSID_AND_ATTRIBUTES*);
using SetLoopbackConfig = DWORD(WINAPI*)(DWORD, PSID_AND_ATTRIBUTES);
GetLoopbackConfig get_loopback_config();
SetLoopbackConfig set_loopback_config();

class AppContainerList final {
public:
    AppContainerList() {
        const DWORD result = get_loopback_config()(&count_, &entries_);
        if (result != ERROR_SUCCESS) throw std::runtime_error("network_isolation_query_failed");
    }
    ~AppContainerList() {
        if (!entries_) return;
        for (DWORD i = 0; i < count_; ++i) if (entries_[i].Sid) HeapFree(GetProcessHeap(), 0, entries_[i].Sid);
        HeapFree(GetProcessHeap(), 0, entries_);
    }
    AppContainerList(const AppContainerList&) = delete;
    AppContainerList& operator=(const AppContainerList&) = delete;
    [[nodiscard]] DWORD count() const noexcept { return count_; }
    [[nodiscard]] PSID_AND_ATTRIBUTES data() const noexcept { return entries_; }
private:
    DWORD count_{};
    PSID_AND_ATTRIBUTES entries_{};
};

HMODULE network_isolation_module() {
    static HMODULE module = LoadLibraryExW(L"FirewallAPI.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) throw std::runtime_error("network_isolation_api_unavailable");
    return module;
}

GetLoopbackConfig get_loopback_config() {
    const auto function = reinterpret_cast<GetLoopbackConfig>(
        GetProcAddress(network_isolation_module(), "NetworkIsolationGetAppContainerConfig"));
    if (!function) throw std::runtime_error("network_isolation_api_unavailable");
    return function;
}

SetLoopbackConfig set_loopback_config() {
    const auto function = reinterpret_cast<SetLoopbackConfig>(
        GetProcAddress(network_isolation_module(), "NetworkIsolationSetAppContainerConfig"));
    if (!function) throw std::runtime_error("network_isolation_api_unavailable");
    return function;
}

std::wstring appcontainer_sid() {
    PSID sid_raw = nullptr;
    if (FAILED(DeriveAppContainerSidFromAppContainerName(laso::broker::appcontainer_name, &sid_raw)))
        throw std::runtime_error("appcontainer_sid_unavailable");
    std::unique_ptr<void, SidDeleter> sid(sid_raw);
    LPWSTR text_raw = nullptr;
    if (!ConvertSidToStringSidW(sid.get(), &text_raw)) throw std::runtime_error("appcontainer_sid_unavailable");
    std::unique_ptr<wchar_t, LocalFreeDeleter> text(text_raw);
    return text.get();
}

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) throw std::runtime_error("invalid_utf8");
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), required) != required)
        throw std::runtime_error("invalid_utf8");
    return result;
}

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::runtime_error("sid_conversion_failed");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), required, nullptr, nullptr) != required)
        throw std::runtime_error("sid_conversion_failed");
    return result;
}

class WindowsPrivilegedState final : public PrivilegedState {
public:
    std::string appcontainer_sid() override { return wide_to_utf8(::appcontainer_sid()); }

    LoopbackStatus query_legacy_loopback() override {
        const auto target_text = ::appcontainer_sid();
        PSID target_raw = nullptr;
        if (!ConvertStringSidToSidW(target_text.c_str(), &target_raw)) throw std::runtime_error("appcontainer_sid_unavailable");
        std::unique_ptr<void, LocalFreeDeleter> target(target_raw);
        AppContainerList configured;
        bool enabled = false;
        for (DWORD i = 0; i < configured.count(); ++i)
            if (configured.data()[i].Sid && EqualSid(configured.data()[i].Sid, target.get())) enabled = true;
        return {wide_to_utf8(target_text), enabled, configured.count()};
    }

    bool remove_legacy_loopback() override {
        const auto target_text = ::appcontainer_sid();
        PSID target_raw = nullptr;
        if (!ConvertStringSidToSidW(target_text.c_str(), &target_raw)) throw std::runtime_error("appcontainer_sid_unavailable");
        std::unique_ptr<void, LocalFreeDeleter> target(target_raw);
        AppContainerList configured;
        std::vector<SID_AND_ATTRIBUTES> retained;
        retained.reserve(configured.count());
        bool found = false;
        for (DWORD i = 0; i < configured.count(); ++i) {
            const auto& entry = configured.data()[i];
            if (entry.Sid && EqualSid(entry.Sid, target.get())) found = true;
            else retained.push_back(entry);
        }
        if (!found) return false;
        const DWORD result = set_loopback_config()(static_cast<DWORD>(retained.size()), retained.data());
        if (result != ERROR_SUCCESS) throw std::runtime_error("network_isolation_update_failed");
        return true;
    }
};

bool path_is_plain_directory(const std::filesystem::path& path, bool create) {
    if (create && !CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

class FileAudit final : public AuditSink {
public:
    FileAudit() {
        PWSTR raw_root = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw_root)))
            throw std::runtime_error("audit_directory_unavailable");
        std::unique_ptr<wchar_t, LocalFreeDeleter> root(raw_root);
        const auto program_data = std::filesystem::path(root.get());
        const auto product_root = program_data / L"LASO-Computer";
        const auto broker_root = product_root / L"Broker";
        if (!path_is_plain_directory(product_root, true) || !path_is_plain_directory(broker_root, true))
            throw std::runtime_error("audit_directory_unavailable");
        path_ = broker_root / audit_file_name;
    }

    bool append(std::string_view operation, std::string_view caller_sid, std::string_view phase,
                bool success, std::string_view result_code) override {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        char timestamp[32]{};
        sprintf_s(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        const Json event{{"timestamp_utc", timestamp}, {"operation", operation}, {"caller_sid", caller_sid},
                         {"phase", phase}, {"success", success}, {"result_code", result_code}};
        const auto line = event.dump() + "\n";
        HANDLE file = CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        const bool valid = GetFileInformationByHandle(file, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        DWORD written{};
        const bool ok = valid && line.size() <= std::numeric_limits<DWORD>::max() &&
            WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) &&
            static_cast<std::size_t>(written) == line.size();
        CloseHandle(file);
        return ok;
    }
private:
    std::filesystem::path path_;
};

void report_status(DWORD state, DWORD win32_exit = NO_ERROR, DWORD wait_hint = 0) {
    std::scoped_lock guard(service_status_mutex);
    service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    service_status.dwCurrentState = state;
    service_status.dwWin32ExitCode = win32_exit;
    service_status.dwWaitHint = wait_hint;
    service_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    service_status.dwCheckPoint = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING ? service_status.dwCheckPoint + 1 : 0;
    if (service_status_handle) SetServiceStatus(service_status_handle, &service_status);
}

DWORD WINAPI control_handler(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        report_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        if (stop_event) SetEvent(stop_event);
        const HANDLE pipe = active_pipe.load();
        if (pipe != INVALID_HANDLE_VALUE) CancelIoEx(pipe, nullptr);
    }
    return NO_ERROR;
}

bool read_client_identity(HANDLE pipe, CallerIdentity& identity) {
    ULONG pid{};
    if (!GetNamedPipeClientProcessId(pipe, &pid) || pid == 0) return false;
    DWORD session{};
    if (!ProcessIdToSessionId(pid, &session) || session == 0) return false;
    if (!ImpersonateNamedPipeClient(pipe)) return false;
    HANDLE raw_token{};
    const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token) != FALSE;
    const bool reverted = RevertToSelf() != FALSE;
    if (!reverted) {
        if (raw_token) CloseHandle(raw_token);
        if (stop_event) SetEvent(stop_event);
        return false;
    }
    if (!opened) return false;
    struct HandleCloser { void operator()(void* h) const noexcept { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
    std::unique_ptr<void, HandleCloser> token(raw_token);
    DWORD bytes{};
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (bytes == 0) return false;
    std::vector<std::byte> buffer(bytes);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), bytes, &bytes)) return false;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR raw_sid{};
    if (!ConvertSidToStringSidW(user->User.Sid, &raw_sid)) return false;
    std::unique_ptr<wchar_t, LocalFreeDeleter> sid(raw_sid);
    identity = {wide_to_utf8(sid.get()), session};
    return true;
}

std::string pipe_security_descriptor() {
    // Client rights: read/write message data, basic metadata/control, and
    // synchronization. Do not grant FILE_CREATE_PIPE_INSTANCE to the caller.
    return "D:P(A;;GA;;;SY)(A;;0x00120183;;;" + wide_to_utf8(authorized_sid_text) + ")";
}

struct EventCloser { void operator()(void* value) const noexcept { if (value) CloseHandle(value); } };
using Event = std::unique_ptr<void, EventCloser>;

bool complete_overlapped(HANDLE pipe, OVERLAPPED& operation, DWORD timeout_ms,
                         DWORD& transferred, DWORD& result_error) {
    const DWORD wait = WaitForSingleObject(operation.hEvent, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        WaitForSingleObject(operation.hEvent, INFINITE);
        GetOverlappedResult(pipe, &operation, &transferred, FALSE);
        result_error = wait == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
        return false;
    }
    if (GetOverlappedResult(pipe, &operation, &transferred, FALSE)) return true;
    result_error = GetLastError();
    return false;
}

bool connect_client(HANDLE pipe) {
    Event event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &operation)) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    DWORD transferred{};
    DWORD result_error{};
    return complete_overlapped(pipe, operation, 5000, transferred, result_error);
}

bool read_request(HANDLE pipe, std::vector<char>& buffer, DWORD& received, DWORD& error) {
    Event event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) { error = GetLastError(); return false; }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    if (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &received, &operation)) return true;
    error = GetLastError();
    if (error == ERROR_IO_PENDING) return complete_overlapped(pipe, operation, 5000, received, error);
    return false;
}

bool send_json(HANDLE pipe, const std::string& value) {
    if (value.size() > laso::broker::max_message_bytes) return false;
    Event event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD written{};
    if (WriteFile(pipe, value.data(), static_cast<DWORD>(value.size()), &written, &operation))
        return written == value.size();
    DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING) return false;
    return complete_overlapped(pipe, operation, 5000, written, error) && written == value.size();
}

void wait_response_read(HANDLE pipe) {
    std::vector<char> acknowledgement(1);
    DWORD received{};
    DWORD error{};
    // The client sends a one-byte transport ACK after reading its bounded JSON
    // response, avoiding an unbounded FlushFileBuffers wait on the service.
    (void)read_request(pipe, acknowledgement, received, error);
}

void serve_one(HANDLE pipe, Dispatcher& dispatcher) {
    std::vector<char> buffer(laso::broker::max_message_bytes + 1);
    DWORD received{};
    DWORD read_error{};
    const bool read = read_request(pipe, buffer, received, read_error);
    if (!read && read_error == ERROR_MORE_DATA) {
        send_json(pipe, R"({"version":1,"request_id":"","ok":false,"error":"request_too_large"})");
        return;
    }
    if (!read || received > laso::broker::max_message_bytes) {
        send_json(pipe, R"({"version":1,"request_id":"","ok":false,"error":"invalid_request"})");
        return;
    }
    // Impersonation is performed after reading the client message, as required
    // for named-pipe client impersonation on Windows.
    CallerIdentity caller;
    if (!read_client_identity(pipe, caller) || !laso::broker::caller_is_authorized(caller, wide_to_utf8(authorized_sid_text))) {
        send_json(pipe, R"({"version":1,"request_id":"","ok":false,"error":"unauthorized"})");
        return;
    }
    if (send_json(pipe, dispatcher.handle(std::string_view(buffer.data(), received), caller)))
        wait_response_read(pipe);
}

void run_service() {
    service_status_handle = RegisterServiceCtrlHandlerExW(L"LASOComputerBroker", control_handler, nullptr);
    if (!service_status_handle) return;
    report_status(SERVICE_START_PENDING, NO_ERROR, 5000);
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event) { report_status(SERVICE_STOPPED, GetLastError()); return; }
    try {
        WindowsPrivilegedState state;
        FileAudit audit;
        Dispatcher dispatcher(state, audit);
        const std::wstring sddl = utf8_to_wide(pipe_security_descriptor());
        PSECURITY_DESCRIPTOR descriptor_raw{};
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_raw, nullptr))
            throw std::runtime_error("pipe_acl_unavailable");
        std::unique_ptr<void, LocalFreeDeleter> descriptor(descriptor_raw);
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.get(), FALSE};
        report_status(SERVICE_RUNNING);

        while (WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT) {
            HANDLE pipe = CreateNamedPipeW(laso::broker::pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1, static_cast<DWORD>(laso::broker::max_message_bytes), static_cast<DWORD>(laso::broker::max_message_bytes),
                2000, &attributes);
            if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("pipe_creation_failed");
            active_pipe.store(pipe);
            if (connect_client(pipe)) {
                serve_one(pipe, dispatcher);
                DisconnectNamedPipe(pipe);
            }
            active_pipe.store(INVALID_HANDLE_VALUE);
            CloseHandle(pipe);
        }
        report_status(SERVICE_STOPPED);
    } catch (...) {
        report_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    }
    if (stop_event) { CloseHandle(stop_event); stop_event = nullptr; }
}

void WINAPI service_main(DWORD, LPWSTR*) { run_service(); }

bool parse_service_args(int argc, wchar_t** argv) {
    bool service = false;
    bool sid_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--service" && !service) service = true;
        else if (arg.starts_with(L"--authorized-sid=") && !sid_seen) {
            authorized_sid_text = arg.substr(17);
            sid_seen = true;
        }
        else return false;
    }
    if (!service || !sid_seen || authorized_sid_text.empty()) return false;
    PSID sid{};
    if (!ConvertStringSidToSidW(authorized_sid_text.c_str(), &sid)) return false;
    LocalFree(sid);
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (!parse_service_args(argc, argv)) return 2;
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(L"LASOComputerBroker"), service_main},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) return static_cast<int>(GetLastError());
    return 0;
}
