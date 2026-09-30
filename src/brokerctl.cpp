#include "laso/broker.hpp"

#include <windows.h>

#include <memory>
#include <stdexcept>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Json = nlohmann::json;

std::string new_request_id() {
    return std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
}

Json transact(std::string_view operation) {
    const auto request_id = new_request_id();
    const Json request{{"version", laso::broker::protocol_version}, {"request_id", request_id}, {"operation", operation}};
    const auto bytes = request.dump();
    if (bytes.size() > laso::broker::max_message_bytes) throw std::runtime_error("request too large");
    if (!WaitNamedPipeW(laso::broker::pipe_name, 1500)) throw std::runtime_error("broker unavailable");
    constexpr DWORD pipe_data_and_metadata = 0x00120183;
    HANDLE pipe = CreateFileW(laso::broker::pipe_name, pipe_data_and_metadata, 0, nullptr, OPEN_EXISTING,
                              SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("broker unavailable");
    struct Closer { void operator()(void* value) const noexcept { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
    std::unique_ptr<void, Closer> handle(pipe);
    DWORD mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) throw std::runtime_error("broker protocol error");
    DWORD written{};
    if (!WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size())
        throw std::runtime_error("broker transport error");
    FlushFileBuffers(pipe);
    std::vector<char> response(laso::broker::max_message_bytes + 1);
    DWORD received{};
    const ULONGLONG deadline = GetTickCount64() + 5000;
    bool read = false;
    while (GetTickCount64() < deadline) {
        DWORD available{};
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            throw std::runtime_error("broker transport error");
        if (available) {
            read = ReadFile(pipe, response.data(), static_cast<DWORD>(response.size()), &received, nullptr) != FALSE;
            if (!read && GetLastError() == ERROR_MORE_DATA) throw std::runtime_error("broker response exceeds limit");
            break;
        }
        Sleep(10);
    }
    if (!read && GetTickCount64() >= deadline) throw std::runtime_error("broker response timed out");
    if (!read || received > laso::broker::max_message_bytes) throw std::runtime_error("broker transport error");
    const char acknowledgement = 'A';
    DWORD acknowledgement_written{};
    if (!WriteFile(pipe, &acknowledgement, 1, &acknowledgement_written, nullptr) || acknowledgement_written != 1)
        throw std::runtime_error("broker acknowledgement failed");
    const auto parsed = Json::parse(response.data(), response.data() + received, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || parsed.value("version", 0) != laso::broker::protocol_version ||
        parsed.value("request_id", std::string{}) != request_id || !parsed.contains("ok"))
        throw std::runtime_error("broker response invalid");
    if (!parsed.value("ok", false)) {
        const auto error = parsed.value("error", std::string("broker operation failed"));
        throw std::runtime_error(error);
    }
    return parsed.at("result");
}

int print_preflight(bool require_cleanup) {
    std::cout << "LASO-Computer unattended preflight\n\n";
    try {
        const auto status = transact("query_status");
        const Json expected_operations{"query_status", "query_appcontainer_sid",
                                       "query_legacy_loopback", "cleanup_legacy_loopback"};
        if (status.value("broker_version", 0) != laso::broker::protocol_version ||
            status.value("operations", Json::array()) != expected_operations)
            throw std::runtime_error("broker version mismatch");
        std::cout << "Broker: OK\nProtocol: v" << status.at("broker_version").get<unsigned>() << "\nService: running\n";
        const auto loopback = transact("query_legacy_loopback");
        const bool stale = loopback.value("enabled", false);
        std::cout << "Required provisioning: " << (stale ? "legacy cleanup available" : "OK") << "\n"
                  << "Playwright privileged setup: not required\n"
                  << "Stale loopback exemption: " << (stale ? "present (cleanup_legacy_loopback is available)" : "none") << "\n"
                  << "Interactive UAC expected: NO\n";
        if (require_cleanup && stale) {
            std::cout << "Unattended test readiness: BLOCKED (run the approved broker cleanup operation)\n";
            return 2;
        }
        std::cout << "Unattended test readiness: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "Broker: unavailable (" << error.what() << ")\n"
                  << "Protocol: unknown\nService: unavailable\n"
                  << "Playwright privileged setup: not required\n"
                  << "Interactive UAC expected: NO\n";
        if (require_cleanup) {
            std::cout << "Unattended test readiness: BLOCKED (required broker operation unavailable)\n";
            return 2;
        }
        std::cout << "Unattended test readiness: PASS for non-privileged build/unit/browser lanes; privileged operations are unavailable\n";
        return 0;
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: LASOComputerBrokerCtl.exe <status|appcontainer-sid|legacy-loopback|cleanup-legacy-loopback|preflight> [--require-cleanup]\n";
        return 2;
    }
    const std::wstring command(argv[1]);
    try {
        if (command == L"preflight") {
            const bool require_cleanup = argc == 3 && std::wstring_view(argv[2]) == L"--require-cleanup";
            if (argc == 3 && !require_cleanup) return 2;
            return print_preflight(require_cleanup);
        }
        if (argc != 2) return 2;
        std::string operation;
        if (command == L"status") operation = "query_status";
        else if (command == L"appcontainer-sid") operation = "query_appcontainer_sid";
        else if (command == L"legacy-loopback") operation = "query_legacy_loopback";
        else if (command == L"cleanup-legacy-loopback") operation = "cleanup_legacy_loopback";
        else return 2;
        std::cout << transact(operation).dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}
