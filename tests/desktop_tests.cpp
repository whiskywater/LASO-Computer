#include "laso/audit.hpp"
#include "laso/platform.hpp"
#include "laso/protocol.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
constexpr int edit_id = 2101;
constexpr int button_id = 2102;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

LRESULT CALLBACK test_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_COMMAND && LOWORD(wparam) == button_id) SetWindowTextW(window, L"LASO Computer Desktop Fixture — clicked");
    return DefWindowProcW(window, message, wparam, lparam);
}

void pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

nlohmann::json run_job(laso::WorkerProtocol& worker, const std::string& capability, nlohmann::json arguments) {
    static unsigned counter = 0;
    const auto suffix = std::to_string(++counter);
    nlohmann::json submit{{"protocol_version", 1}, {"request_id", "desktop-submit-" + suffix},
        {"operation", "submit"}, {"job_id", "desktop-job-" + suffix},
        {"payload", {{"capability", capability}, {"arguments", std::move(arguments)}}}};
    const auto accepted = worker.handle(submit);
    require(accepted.value("ok", false), "desktop job submission failed");
    const auto external_id = accepted.value("external_job_id", std::string{});
    require(!external_id.empty(), "desktop job id missing");
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
        pump_messages();
        nlohmann::json result{{"protocol_version", 1}, {"request_id", "desktop-result-" + suffix + "-" + std::to_string(attempt)},
            {"operation", "result"}, {"external_job_id", external_id}};
        const auto response = worker.handle(result);
        if (response.value("state", std::string{}) != "Running") {
            require(response.value("ok", false), "worker result operation failed");
            return response;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("desktop job timed out");
}

nlohmann::json run_capability(laso::WorkerProtocol& worker, const std::string& capability, nlohmann::json arguments) {
    const auto response = run_job(worker, capability, std::move(arguments));
    if (response.value("state", std::string{}) != "Completed")
        throw std::runtime_error(capability + " action failed: " + response.value("error", std::string("job failed")));
    return response.value("payload", nlohmann::json::object());
}

std::string handle_id(HWND window) {
    wchar_t value[32]{};
    swprintf_s(value, L"%p", static_cast<void*>(window));
    const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(count - 1), '\0');
    std::string terminated(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, terminated.data(), count, nullptr, nullptr);
    terminated.resize(static_cast<std::size_t>(count - 1));
    text = std::move(terminated);
    return text;
}

struct ClipboardSnapshot {
    bool safe_to_write{false};
    bool originally_empty{false};
    bool has_unicode_text{false};
    std::wstring original_text;

    ClipboardSnapshot() {
        if (!OpenClipboard(nullptr)) return;
        UINT format = 0;
        unsigned count = 0;
        bool only_unicode_text = true;
        while ((format = EnumClipboardFormats(format)) != 0) {
            ++count;
            if (format != CF_UNICODETEXT) only_unicode_text = false;
        }
        has_unicode_text = IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
        originally_empty = count == 0;
        if (originally_empty) safe_to_write = true;
        else if (count == 1 && only_unicode_text && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            const auto bytes = data ? GlobalSize(data) : 0;
            const auto* text = data && bytes >= sizeof(wchar_t) ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
            if (text) {
                const auto maximum = bytes / sizeof(wchar_t);
                const auto end = std::find(text, text + maximum, L'\0');
                if (end != text + maximum) { original_text.assign(text, end); safe_to_write = true; }
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
    }

    ~ClipboardSnapshot() {
        if (!safe_to_write || !OpenClipboard(nullptr)) return;
        EmptyClipboard();
        if (!originally_empty) {
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (original_text.size() + 1) * sizeof(wchar_t));
            if (memory) {
                auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
                if (destination) {
                    std::copy(original_text.begin(), original_text.end(), destination);
                    destination[original_text.size()] = L'\0';
                    GlobalUnlock(memory);
                    if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
                } else GlobalFree(memory);
            }
        }
        CloseClipboard();
    }
};

void run_tests() {
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = test_window_proc;
    cls.lpszClassName = L"LASOComputerDesktopFixture";
    RegisterClassW(&cls);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"LASO Computer Desktop Fixture",
        WS_OVERLAPPEDWINDOW, 120, 120, 640, 360, nullptr, nullptr, cls.hInstance, nullptr);
    require(window != nullptr, "could not create disposable desktop fixture window");
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        24, 24, 360, 32, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(edit_id)), cls.hInstance, nullptr);
    HWND button = CreateWindowExW(0, L"BUTTON", L"Fixture button", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        24, 72, 180, 40, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(button_id)), cls.hInstance, nullptr);
    require(edit && button, "could not create desktop fixture controls");
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    pump_messages();

    POINT original_cursor{};
    GetCursorPos(&original_cursor);
    const HWND previous_foreground = GetForegroundWindow();
    struct RestoreDesktop {
        HWND window; HWND previous; POINT cursor;
        ~RestoreDesktop() {
            if (window) DestroyWindow(window);
            SetCursorPos(cursor.x, cursor.y);
            if (previous && IsWindow(previous)) SetForegroundWindow(previous);
        }
    } restore{window, previous_foreground, original_cursor};

    ClipboardSnapshot clipboard_snapshot;

    laso::Config config;
    for (const auto* capability : {"screen.capture", "pointer.move", "pointer.click", "keyboard.type",
            "clipboard.read", "clipboard.write", "browser.status", "window.list", "window.focus", "ui.focus", "ui.inspect", "ui.invoke"})
        config.capabilities[capability] = laso::Decision::allow;
    laso::CapabilityRegistry registry;
    registry.register_provider(std::make_shared<laso::WindowsPlatform>(config));
    const auto audit_path = std::filesystem::temp_directory_path() / L"laso-desktop-test-audit.jsonl";
    std::error_code ignored; std::filesystem::remove(audit_path, ignored);
    laso::WorkerProtocol worker(config, std::move(registry), laso::AuditLog(audit_path));

    const auto window_id = handle_id(window);
    const auto list = run_capability(worker, "window.list", nlohmann::json::object());
    bool found = false;
    for (const auto& item : list.at("windows")) if (item.value("window_id", "") == window_id) found = true;
    require(found, "window enumeration did not find the purpose-built fixture");

    const auto browser_status = run_capability(worker, "browser.status", nlohmann::json::object());
    require(browser_status.contains("window_count") && browser_status["window_count"].is_number(),
            "browser status should include a visible window count");
    require(browser_status.contains("browser_status") && browser_status["browser_status"].is_object() &&
                browser_status["browser_status"].contains("browser_visible") &&
                browser_status["browser_status"]["browser_visible"].is_boolean() &&
                browser_status["browser_status"].contains("active_browser_visible") &&
                browser_status["browser_status"]["active_browser_visible"].is_boolean(),
            "browser status should expose visibility booleans without window titles");

    const auto inspected = run_capability(worker, "ui.inspect", {{"window_id", window_id}, {"max_nodes", 64}, {"max_depth", 4}});
    require(inspected.contains("elements") && inspected["elements"].is_array(), "UI Automation inspection failed");
    std::string edit_automation_id;
    for (const auto& item : inspected["elements"]) {
        if (item.value("automation_id", "") == std::to_string(edit_id)) edit_automation_id = item.value("automation_id", "");
    }
    if (!edit_automation_id.empty()) {
        const auto changed = run_capability(worker, "ui.invoke", {{"window_id", window_id}, {"automation_id", edit_automation_id},
            {"action", "set_value"}, {"value", "set by UI Automation"}});
        require(changed.value("performed", "") == "set_value", "UI Automation value provider did not run");
        wchar_t value[128]{}; GetWindowTextW(edit, value, static_cast<int>(std::size(value)));
        require(std::wstring(value) == L"set by UI Automation", "UI Automation did not update the fixture edit control");
    } else {
        throw std::runtime_error("UI Automation did not expose the fixture edit control automation id");
    }

    const auto focus_result = run_job(worker, "window.focus", {{"window_id", window_id}});
    require(focus_result.value("ok", false), "window focus result was a protocol error");
    require(focus_result.value("state", std::string{}) == "Completed" ||
            focus_result.value("state", std::string{}) == "Failed",
            "window focus returned an invalid job state");
    if (focus_result.value("state", std::string{}) == "Completed")
        require(focus_result.value("payload", nlohmann::json::object()).value("focused", false),
                "window focus reported completion without focusing the fixture");

    const auto control_focus = run_capability(worker, "ui.focus", {{"window_id", window_id}, {"automation_id", edit_automation_id}});
    require(control_focus.value("performed", "") == "focus", "UI Automation control focus failed");
    SetWindowTextW(edit, L"");
    for (int i = 0; i < 30 && (GetForegroundWindow() != window || GetFocus() != edit); ++i) {
        pump_messages();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const bool fixture_has_focus = GetForegroundWindow() == window && GetFocus() == edit;
    if (fixture_has_focus) {
        run_capability(worker, "keyboard.type", {{"text", "typed safely into fixture"}});
        for (int i = 0; i < 20; ++i) { pump_messages(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        wchar_t typed_value[128]{}; GetWindowTextW(edit, typed_value, static_cast<int>(std::size(typed_value)));
        require(std::wstring(typed_value) == L"typed safely into fixture", "keyboard input did not reach the fixture edit control");
    } else {
        std::cout << "Keyboard and click tests skipped: Windows did not grant foreground focus to the disposable fixture\n";
    }

    RECT button_rect{}; GetWindowRect(button, &button_rect);
    const int click_x = (button_rect.left + button_rect.right) / 2;
    const int click_y = (button_rect.top + button_rect.bottom) / 2;
    const auto moved = run_capability(worker, "pointer.move", {{"x", click_x}, {"y", click_y}, {"coordinate_space", "primary_display_pixels"}});
    (void)moved;
    if (fixture_has_focus) {
        run_capability(worker, "pointer.click", {{"x", click_x}, {"y", click_y}, {"button", "left"}, {"count", 1}, {"coordinate_space", "primary_display_pixels"}});
        pump_messages();
        wchar_t title[128]{}; GetWindowTextW(window, title, static_cast<int>(std::size(title)));
        require(std::wstring(title).find(L"clicked") != std::wstring::npos, "mouse click did not activate the fixture button");
    }

    const auto capture = run_capability(worker, "screen.capture", nlohmann::json::object());
    require(capture.value("format", "") == "jpeg" && !capture.value("data_base64", std::string{}).empty(), "screen capture failed");

    if (clipboard_snapshot.safe_to_write) {
        const auto write = run_capability(worker, "clipboard.write", {{"text", "LASO fixture clipboard value"}});
        require(write.value("sensitive_payload_redacted", false), "clipboard write did not return redacted metadata");
        const auto round_trip = run_capability(worker, "clipboard.read", nlohmann::json::object());
        require(round_trip.value("text", "") == "LASO fixture clipboard value", "clipboard round-trip failed");
    } else if (clipboard_snapshot.has_unicode_text) {
        const auto read = run_capability(worker, "clipboard.read", nlohmann::json::object());
        require(read.contains("text"), "clipboard read failed for text clipboard data");
    } else {
        std::cout << "Clipboard read/write tests skipped: clipboard is unavailable or contains non-text data\n";
    }
    std::ifstream audit(audit_path, std::ios::binary);
    const std::string audit_text((std::istreambuf_iterator<char>(audit)), std::istreambuf_iterator<char>());
    require(audit_text.find("LASO fixture clipboard value") == std::string::npos, "audit log exposed clipboard contents");
    std::filesystem::remove(audit_path, ignored);
}
}

int main() {
    try {
        run_tests();
        std::cout << "Windows desktop fixture tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Desktop fixture test failure: " << error.what() << '\n';
        return 1;
    }
}
