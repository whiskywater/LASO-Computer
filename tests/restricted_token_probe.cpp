#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
class Handle {
public:
    explicit Handle(HANDLE h = nullptr) : h_(h) {}
    ~Handle() { if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) { if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); h_ = std::exchange(other.h_, nullptr); } return *this; }
    HANDLE get() const { return h_; }
private: HANDLE h_{};
};

struct TempFilesCleanup {
    std::filesystem::path marker;
    ~TempFilesCleanup() {
        DeleteFileW((marker.wstring() + L".write").c_str());
        DeleteFileW(marker.c_str());
    }
};

std::wstring quote(const std::wstring& value) {
    std::wstring out = L"\""; std::size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') { out.append(slashes * 2 + 1, L'\\'); out.push_back(c); slashes = 0; continue; }
        out.append(slashes, L'\\'); slashes = 0; out.push_back(c);
    }
    out.append(slashes * 2, L'\\'); out.push_back(L'\"'); return out;
}

Handle privilege_stripped_token() {
    HANDLE base_raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY |
            TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_PRIVILEGES, &base_raw)) throw GetLastError();
    Handle base(base_raw); HANDLE child_raw = nullptr;
    if (!CreateRestrictedToken(base.get(), DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &child_raw))
        throw GetLastError();
    return Handle(child_raw);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::cerr << "usage: laso-restricted-token-probe <node.exe>\n"; return 2; }
    try {
        const std::filesystem::path node(argv[1]);
        std::array<wchar_t, MAX_PATH> temp{};
        const DWORD temp_len = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
        if (!temp_len || temp_len >= temp.size()) throw GetLastError();
        std::array<wchar_t, MAX_PATH> marker_path{};
        if (!GetTempFileNameW(temp.data(), L"lcp", 0, marker_path.data())) throw GetLastError();
        const auto outside = std::filesystem::path(marker_path.data());
        TempFilesCleanup cleanup{outside};
        { std::ofstream file(outside, std::ios::binary | std::ios::trunc); file << "LASO-TEST-ONLY-OUTSIDE-MARKER"; }

        auto token = privilege_stripped_token();
        BOOL is_restricted = IsTokenRestricted(token.get());
        DWORD token_bytes = 0; GetTokenInformation(token.get(), TokenIntegrityLevel, nullptr, 0, &token_bytes);
        std::vector<std::byte> token_info(token_bytes);
        if (!GetTokenInformation(token.get(), TokenIntegrityLevel, token_info.data(), token_bytes, &token_bytes)) throw GetLastError();
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(token_info.data());
        const DWORD integrity = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);

        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE}; HANDLE read_raw = nullptr, write_raw = nullptr;
        if (!CreatePipe(&read_raw, &write_raw, &inherit, 0)) throw GetLastError();
        Handle read_end(read_raw), write_end(write_raw); SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0);
        HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        if (null_input == INVALID_HANDLE_VALUE) throw GetLastError(); Handle input(null_input);

        SIZE_T attr_bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_bytes);
        std::vector<std::byte> attr_storage(attr_bytes);
        auto attrs = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_bytes)) throw GetLastError();
        HANDLE inherited[]{input.get(), write_end.get()};
        if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) throw GetLastError();

        const std::wstring outside_arg = outside.wstring();
        DWORD privilege_bytes = 0; GetTokenInformation(token.get(), TokenPrivileges, nullptr, 0, &privilege_bytes);
        std::vector<std::byte> privilege_data(privilege_bytes);
        if (!GetTokenInformation(token.get(), TokenPrivileges, privilege_data.data(), privilege_bytes, &privilege_bytes)) throw GetLastError();
        auto* privileges = reinterpret_cast<TOKEN_PRIVILEGES*>(privilege_data.data());
        unsigned enabled_privileges = 0; std::string enabled_names;
        for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
            if ((privileges->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) == 0) continue;
            ++enabled_privileges; std::array<wchar_t, 128> name{}; DWORD name_size = static_cast<DWORD>(name.size());
            if (LookupPrivilegeNameW(nullptr, &privileges->Privileges[i].Luid, name.data(), &name_size)) {
                if (!enabled_names.empty()) enabled_names += ",";
                for (DWORD c = 0; c < name_size; ++c) enabled_names.push_back(static_cast<char>(name[c]));
            }
        }
        const std::wstring js = L"const fs=require('fs'),cp=require('child_process');let r,w;try{r=fs.readFileSync(process.argv[1],'utf8')==='LASO-TEST-ONLY-OUTSIDE-MARKER'?'ALLOWED':'WRONG'}catch(e){r='DENIED:'+e.code}try{fs.writeFileSync(process.argv[1]+'.write','x');w='ALLOWED'}catch(e){w='DENIED:'+e.code}const p=cp.spawnSync(process.execPath,['-e','process.exit(0)']);console.log(JSON.stringify({outsideRead:r,outsideWrite:w,childProcessBlocked:!!p.error,childProcessError:p.error?String(p.error.code||'UNKNOWN')+'; errno='+String(p.error.errno||'n/a'):'',childProcessStatus:p.status,runtimeRead:fs.existsSync(process.execPath)}));";
        std::wstring command = quote(node.wstring()) + L" -e " + quote(js) + L" " + quote(outside_arg);
        std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
        std::vector<std::wstring> env_entries;
        std::array<wchar_t, MAX_PATH> system_root{}; const UINT system_root_len = GetWindowsDirectoryW(system_root.data(), static_cast<UINT>(system_root.size()));
        if (!system_root_len || system_root_len >= system_root.size()) throw GetLastError();
        env_entries.push_back(L"SystemRoot=" + std::wstring(system_root.data(), system_root_len));
        env_entries.push_back(L"TEMP=" + std::wstring(temp.data()));
        env_entries.push_back(L"TMP=" + std::wstring(temp.data()));
        env_entries.push_back(L"PATH=" + node.parent_path().wstring());
        std::sort(env_entries.begin(), env_entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        std::vector<wchar_t> environment;
        for (const auto& entry : env_entries) { environment.insert(environment.end(), entry.begin(), entry.end()); environment.push_back(L'\0'); }
        environment.push_back(L'\0');
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.get(); startup.StartupInfo.hStdOutput = write_end.get(); startup.StartupInfo.hStdError = write_end.get(); startup.lpAttributeList = attrs;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessAsUserW(token.get(), node.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                environment.data(), node.parent_path().c_str(), &startup.StartupInfo, &pi)) throw GetLastError();
        Handle process(pi.hProcess), thread(pi.hThread); input = Handle{}; write_end = Handle{};
        HANDLE job_raw = CreateJobObjectW(nullptr, nullptr); if (!job_raw) throw GetLastError(); Handle job(job_raw);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.BasicLimitInformation.ActiveProcessLimit = 1; limits.ProcessMemoryLimit = 512U * 1024U * 1024U;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) || !AssignProcessToJobObject(job.get(), process.get())) throw GetLastError();
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) throw GetLastError();
        if (WaitForSingleObject(process.get(), 15000) != WAIT_OBJECT_0) { TerminateJobObject(job.get(), 1); throw std::runtime_error("probe timeout"); }
        std::array<char, 4096> output{}; DWORD count = 0; ReadFile(read_end.get(), output.data(), static_cast<DWORD>(output.size() - 1), &count, nullptr);
        std::cout << "IsTokenRestricted=" << (is_restricted ? "true" : "false") << "\nIntegrityRID=" << integrity
                  << " (8192=Medium, 4096=Low)\nEnabledPrivileges=" << enabled_privileges << "\nEnabledPrivilegeNames="
                  << enabled_names << "\n" << std::string(output.data(), count);
        DeleteProcThreadAttributeList(attrs);
        return 0;
    } catch (DWORD error) { std::cerr << "Win32 error=" << error << "\n"; return 1; }
      catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
}
