#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace laso::broker {

inline constexpr unsigned protocol_version = 1;
inline constexpr std::size_t max_message_bytes = 4096;
inline constexpr wchar_t pipe_name[] = L"\\\\.\\pipe\\LASOComputerBroker";
inline constexpr wchar_t appcontainer_name[] = L"LASOComputer.Playwright";
inline constexpr char service_name[] = "LASOComputerBroker";

struct CallerIdentity {
    std::string sid;
    std::uint32_t session_id{};
};

struct LoopbackStatus {
    std::string appcontainer_sid;
    bool enabled{};
    std::uint32_t configured_sid_count{};
};

class PrivilegedState {
public:
    virtual ~PrivilegedState() = default;
    virtual std::string appcontainer_sid() = 0;
    virtual LoopbackStatus query_legacy_loopback() = 0;
    // Removes only the compiled-in LASO-Computer Playwright SID, preserving
    // every other AppContainer SID in the Windows loopback-exemption list.
    virtual bool remove_legacy_loopback() = 0;
};

class AuditSink {
public:
    virtual ~AuditSink() = default;
    virtual bool append(std::string_view operation, std::string_view caller_sid,
                        std::string_view phase, bool success,
                        std::string_view result_code) = 0;
};

class Dispatcher final {
public:
    Dispatcher(PrivilegedState& state, AuditSink& audit) noexcept;
    [[nodiscard]] std::string handle(std::string_view request, const CallerIdentity& caller) noexcept;

private:
    PrivilegedState& state_;
    AuditSink& audit_;
};

[[nodiscard]] const std::vector<std::string>& supported_operations();
[[nodiscard]] bool valid_request_id(std::string_view request_id) noexcept;
[[nodiscard]] bool caller_is_authorized(const CallerIdentity& caller,
                                        std::string_view authorized_user_sid) noexcept;

} // namespace laso::broker
