#include "laso/broker.hpp"

#include <algorithm>
#include <string>
#include <stdexcept>

namespace laso::broker {
namespace {

using Json = nlohmann::json;

Json response(std::string request_id, bool ok, Json payload) {
    Json result{{"version", protocol_version}, {"request_id", std::move(request_id)}, {"ok", ok}};
    if (ok) result["result"] = std::move(payload);
    else result["error"] = std::move(payload);
    return result;
}

std::string serialize_bounded(const Json& value) {
    auto result = value.dump();
    if (result.size() <= max_message_bytes) return result;
    return R"({"version":1,"request_id":"","ok":false,"error":"response_too_large"})";
}

} // namespace

const std::vector<std::string>& supported_operations() {
    static const std::vector<std::string> operations{
        "query_status",
        "query_appcontainer_sid",
        "query_legacy_loopback",
        "cleanup_legacy_loopback",
    };
    return operations;
}

bool valid_request_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    });
}

bool caller_is_authorized(const CallerIdentity& caller, std::string_view authorized_user_sid) noexcept {
    return caller.session_id != 0 && !caller.sid.empty() && caller.sid == authorized_user_sid;
}

Dispatcher::Dispatcher(PrivilegedState& state, AuditSink& audit) noexcept : state_(state), audit_(audit) {}

std::string Dispatcher::handle(std::string_view request, const CallerIdentity& caller) noexcept {
    std::string request_id;
    std::string operation = "invalid_request";
    std::string result_code = "invalid_request";
    bool request_logged = false;
    const auto append_audit = [&](std::string_view audited_operation, std::string_view phase,
                                  bool success, std::string_view code) noexcept {
        try { return audit_.append(audited_operation, caller.sid, phase, success, code); }
        catch (...) { return false; }
    };

    try {
        if (request.size() > max_message_bytes) {
            operation = "oversized_request";
            append_audit(operation, "request", false, "frame_limit");
            return serialize_bounded(response({}, false, "request_too_large"));
        }

        const auto parsed = Json::parse(request.begin(), request.end(), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            append_audit(operation, "request", false, "malformed_json");
            return serialize_bounded(response({}, false, "invalid_request"));
        }

        if (parsed.contains("request_id") && parsed.at("request_id").is_string())
            request_id = parsed.at("request_id").get<std::string>();
        if (!parsed.contains("version") || !parsed.at("version").is_number_integer() ||
            parsed.at("version").get<std::int64_t>() != protocol_version ||
            !parsed.contains("request_id") || !parsed.at("request_id").is_string() ||
            !valid_request_id(request_id) || !parsed.contains("operation") ||
            !parsed.at("operation").is_string() || parsed.size() != 3) {
            append_audit(operation, "request", false, "invalid_schema");
            return serialize_bounded(response(valid_request_id(request_id) ? request_id : "", false, "invalid_request"));
        }

        operation = parsed.at("operation").get<std::string>();
        const auto& operations = supported_operations();
        if (std::find(operations.begin(), operations.end(), operation) == operations.end()) {
            append_audit("unsupported_operation", "request", false, "unsupported_operation");
            return serialize_bounded(response(request_id, false, "unsupported_operation"));
        }

        if (!append_audit(operation, "request", true, "accepted"))
            return serialize_bounded(response(request_id, false, "audit_unavailable"));
        request_logged = true;

        Json result = Json::object();
        if (operation == "query_status") {
            result = {{"broker_version", protocol_version}, {"operations", operations}};
        } else if (operation == "query_appcontainer_sid") {
            result = {{"sid", state_.appcontainer_sid()}};
        } else if (operation == "query_legacy_loopback") {
            const auto state = state_.query_legacy_loopback();
            result = {{"enabled", state.enabled}, {"configured_sid_count", state.configured_sid_count}};
        } else if (operation == "cleanup_legacy_loopback") {
            const bool removed = state_.remove_legacy_loopback();
            const auto after = state_.query_legacy_loopback();
            result = {{"removed", removed}, {"still_enabled", after.enabled},
                      {"preserved_sid_count", after.configured_sid_count}};
            if (after.enabled) throw std::runtime_error("cleanup_verification_failed");
        }

        if (!append_audit(operation, "complete", true, "ok"))
            return serialize_bounded(response(request_id, false, "audit_completion_unavailable"));
        return serialize_bounded(response(request_id, true, std::move(result)));
    } catch (const std::exception& error) {
        result_code = error.what();
    } catch (...) {
        result_code = "internal_error";
    }

    if (request_logged) append_audit(operation, "complete", false, result_code);
    else append_audit("internal_error", "request", false, result_code);
    if (result_code == "cleanup_verification_failed")
        return serialize_bounded(response(request_id, false, "cleanup_verification_failed"));
    return serialize_bounded(response(request_id, false, "operation_failed"));
}

} // namespace laso::broker
