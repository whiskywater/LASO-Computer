#include "laso/audit.hpp"
#include "laso/core_worker_adapter.hpp"
#include "laso/platform.hpp"
#include "laso/protocol.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

nlohmann::json run_core_job(laso::CoreWorkerAdapter& worker, const std::string& capability,
                            nlohmann::json arguments, std::string suffix) {
    using Request = laso::core_worker_protocol::Request;
    Request submit;
    submit.request_id = "core-desktop-submit-" + suffix;
    submit.operation = "submit";
    submit.job_id = "core-desktop-job-" + suffix;
    submit.payload = {{"capability", capability}, {"input", std::move(arguments)},
                      {"worker_id", "lane4-desktop-fixture"}};
    const auto accepted = worker.handle(submit);
    require(accepted.value("ok", false), "Core child job submission failed");
    const auto external_id = accepted.value("external_job_id", std::string{});
    require(!external_id.empty(), "Core child job id missing");
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
        pump_messages();
        Request result;
        result.request_id = "core-desktop-result-" + suffix + "-" + std::to_string(attempt);
        result.operation = "result";
        result.external_job_id = external_id;
        const auto response = worker.handle(result);
        if (response.value("state", std::string{}) != "Running") return response;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Core child job timed out");
}

nlohmann::json run_core_capability(laso::CoreWorkerAdapter& worker, const std::string& capability,
                                   nlohmann::json arguments, std::string suffix) {
    const auto response = run_core_job(worker, capability, std::move(arguments), std::move(suffix));
    if (response.value("state", std::string{}) != "Completed")
        throw std::runtime_error(capability + " Core child job failed: " + response.value("error", std::string("job failed")));
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

std::filesystem::path find_test_browser() {
    std::vector<std::filesystem::path> candidates;
    if (const auto* program_files_x86 = _wgetenv(L"ProgramFiles(x86)"))
        candidates.emplace_back(std::filesystem::path(program_files_x86) / LR"(Microsoft\Edge\Application\msedge.exe)");
    if (const auto* program_files = _wgetenv(L"ProgramFiles")) {
        candidates.emplace_back(std::filesystem::path(program_files) / LR"(Microsoft\Edge\Application\msedge.exe)");
        candidates.emplace_back(std::filesystem::path(program_files) / LR"(Google\Chrome\Application\chrome.exe)");
    }
    if (const auto* local_app_data = _wgetenv(L"LOCALAPPDATA")) {
        candidates.emplace_back(std::filesystem::path(local_app_data) / LR"(Microsoft\Edge\Application\msedge.exe)");
        candidates.emplace_back(std::filesystem::path(local_app_data) / LR"(Google\Chrome\Application\chrome.exe)");
    }
    for (const auto& candidate : candidates) if (std::filesystem::is_regular_file(candidate)) return candidate;
    return {};
}

std::wstring local_file_url(const std::filesystem::path& path) {
    const auto raw = path.wstring();
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()),
                                           nullptr, 0, nullptr, nullptr);
    require(bytes > 0, "could not encode the local browser fixture path");
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()),
                                utf8.data(), bytes, nullptr, nullptr) == bytes,
            "could not encode the local browser fixture path");
    std::string url = "file:///";
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char byte : utf8) {
        if (byte == '\\') { url.push_back('/'); continue; }
        const bool safe = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                          (byte >= '0' && byte <= '9') || byte == '/' || byte == ':' || byte == '-' ||
                          byte == '.' || byte == '_' || byte == '~';
        if (safe) url.push_back(static_cast<char>(byte));
        else { url.push_back('%'); url.push_back(hex[byte >> 4]); url.push_back(hex[byte & 0x0f]); }
    }
    return std::wstring(url.begin(), url.end());
}

class BrowserFixture {
public:
    BrowserFixture() {
        try {
        const auto executable = find_test_browser();
        if (executable.empty()) return;
        directory_ = std::filesystem::temp_directory_path() /
            (L"LASO-Lane4-BrowserFixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory_);
        const auto page = directory_ / L"fixture.html";
        std::ofstream html(page, std::ios::binary);
        html << "<!doctype html><html><head><meta charset='utf-8'><title>ChatGPT - LASO Lane 4 UIA Fixture</title></head>"
                "<body><label for='chat'>Message</label>"
                "<textarea id='chat' aria-label='Lane 4 fixture message'></textarea>"
                "<button id='send' aria-label='Send fixture'>Send fixture</button>"
                "<div id='status' aria-live='polite'>LANE4-READY</div>"
                "<script>const c=document.getElementById('chat');const s=document.getElementById('status');"
                "c.addEventListener('input',()=>s.textContent=c.value);"
                "c.addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();s.textContent='LANE4-KEY-DISPATCH-OK';}});"
                "document.getElementById('send').addEventListener('click',()=>s.textContent='LANE4-INVOKE-OK');</script>"
                "</body></html>";
        html.close();
        require(static_cast<bool>(html), "could not create the disposable browser fixture page");

        job_ = CreateJobObjectW(nullptr, nullptr);
        require(job_ != nullptr, "could not create an isolated browser fixture job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE,
                "could not constrain the browser fixture process lifetime");

        const auto command_line = L"\"" + executable.wstring() + L"\" --user-data-dir=\"" + directory_.wstring() +
            L"\\profile\" --no-first-run --no-default-browser-check --disable-sync --disable-extensions --app=\"" +
            local_file_url(page) + L"\"";
        std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, nullptr, directory_.c_str(), &startup, &process))
            throw std::runtime_error("could not start the isolated browser fixture");
        process_ = process.hProcess;
        const auto thread = process.hThread;
        if (!AssignProcessToJobObject(job_, process_)) {
            TerminateProcess(process_, 1);
            CloseHandle(thread);
            throw std::runtime_error("could not constrain the isolated browser fixture process");
        }
        if (ResumeThread(thread) == static_cast<DWORD>(-1)) {
            TerminateProcess(process_, 1);
            CloseHandle(thread);
            throw std::runtime_error("could not start the isolated browser fixture");
        }
        CloseHandle(thread);
        for (int attempt = 0; attempt < 300; ++attempt) {
            if (find_window(process.dwProcessId)) { ready_ = true; return; }
            if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        throw std::runtime_error("isolated browser fixture did not create a visible window");
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~BrowserFixture() { cleanup(); }

    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] HWND window() const { return window_; }

private:
    void cleanup() noexcept {
        if (job_) {
            TerminateJobObject(job_, 0);
            for (int attempt = 0; attempt < 100; ++attempt) {
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                if (QueryInformationJobObject(job_, JobObjectBasicAccountingInformation,
                                              &accounting, sizeof(accounting), nullptr) && accounting.ActiveProcesses == 0)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            CloseHandle(job_); // The job contains only the dedicated temporary-profile browser.
            job_ = nullptr;
        }
        if (process_) { WaitForSingleObject(process_, 5000); CloseHandle(process_); process_ = nullptr; }
        std::error_code ignored;
        if (!directory_.empty()) std::filesystem::remove_all(directory_, ignored);
    }

    struct Search { DWORD process_id; HWND window; };
    static BOOL CALLBACK find_window_proc(HWND hwnd, LPARAM data) {
        auto& search = *reinterpret_cast<Search*>(data);
        DWORD process_id = 0;
        GetWindowThreadProcessId(hwnd, &process_id);
        if (process_id == search.process_id && IsWindowVisible(hwnd)) {
            search.window = hwnd;
            return FALSE;
        }
        return TRUE;
    }
    HWND find_window(DWORD process_id) {
        Search search{process_id, nullptr};
        EnumWindows(find_window_proc, reinterpret_cast<LPARAM>(&search));
        window_ = search.window;
        return window_;
    }

    std::filesystem::path directory_;
    HANDLE job_{nullptr};
    HANDLE process_{nullptr};
    HWND window_{nullptr};
    bool ready_{false};
};

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
    BrowserFixture browser_fixture;
    if (!browser_fixture.ready())
        std::cout << "Browser UI Automation fixture skipped: Edge or Chrome was not found\n";

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
    laso::CoreWorkerAdapter core_worker(worker);

    const auto window_id = handle_id(window);
    const auto list = run_core_capability(core_worker, "window.list", nlohmann::json::object(), "window-list");
    require(list.at("windows").size() <= 32, "browser discovery exceeded its window limit");
    bool found = false;
    for (const auto& item : list.at("windows")) if (item.value("window_id", "") == window_id) found = true;
    require(!found, "browser discovery must not expose unrelated non-browser windows");
    if (browser_fixture.ready()) {
        const auto browser_id = handle_id(browser_fixture.window());
        bool browser_found = false;
        auto current_windows = list.at("windows");
        for (int attempt = 0; attempt < 40 && !browser_found; ++attempt) {
            for (const auto& item : current_windows) {
                if (item.value("window_id", std::string{}) != browser_id) continue;
                browser_found = true;
                require(item.value("title", std::string{}) == "ChatGPT" && item.contains("active"),
                        "browser discovery must redact conversation titles and expose only active state");
            }
            if (!browser_found) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                current_windows = run_core_capability(core_worker, "window.list", nlohmann::json::object(),
                    "window-list-browser-ready-" + std::to_string(attempt + 1)).at("windows");
            }
        }
        require(browser_found, "browser discovery did not expose the isolated supported-browser fixture");
    }
    const auto unrelated_focus = run_core_job(core_worker, "window.focus", {{"window_id", window_id}}, "window-focus-unrelated-window");
    require(unrelated_focus.value("state", std::string{}) == "Failed",
            "window.focus must not target a non-ChatGPT window");

    const auto browser_status = run_core_capability(core_worker, "browser.status", nlohmann::json::object(), "browser-status");
    require(browser_status.contains("window_count") && browser_status["window_count"].is_number(),
            "browser status should include a visible window count");
    require(browser_status.contains("browser_status") && browser_status["browser_status"].is_object() &&
                browser_status["browser_status"].contains("browser_visible") &&
                browser_status["browser_status"]["browser_visible"].is_boolean() &&
                browser_status["browser_status"].contains("active_browser_visible") &&
                browser_status["browser_status"]["active_browser_visible"].is_boolean(),
            "browser status should expose visibility booleans without window titles");

    const auto unrelated_inspection = run_core_job(core_worker, "ui.inspect",
        {{"window_id", window_id}}, "ui-inspect-unrelated-window");
    require(unrelated_inspection.value("state", std::string{}) == "Failed",
            "UI Automation must not inspect a non-ChatGPT window");
    const auto unrelated_control_focus = run_core_job(core_worker, "ui.focus",
        {{"window_id", window_id}, {"target", std::to_string(edit_id)}}, "ui-focus-unrelated-window");
    require(unrelated_control_focus.value("state", std::string{}) == "Failed",
            "UI Automation focus must not target a non-ChatGPT window");
    const auto unrelated_control_invoke = run_core_job(core_worker, "ui.invoke",
        {{"window_id", window_id}, {"target", std::to_string(button_id)}, {"action", "invoke"}},
        "ui-invoke-unrelated-window");
    require(unrelated_control_invoke.value("state", std::string{}) == "Failed",
            "UI Automation invoke must not target a non-ChatGPT window");

    const auto malformed_inspect = run_core_job(core_worker, "ui.inspect",
        {{"window_id", window_id}, {"max_nodes", 65}}, "ui-inspect-oversized-limit");
    require(malformed_inspect.value("state", std::string{}) == "Failed",
            "UI Automation must reject arguments outside its fixed bounded inspection schema");
    const auto malformed_keyboard = run_core_job(core_worker, "keyboard.type",
        {{"text", std::string(4097, 'x')}}, "keyboard-type-oversized");
    require(malformed_keyboard.value("state", std::string{}) == "Failed",
            "keyboard typing must reject oversized text before input dispatch");
    const auto oversized_keyboard_dispatch = run_core_job(core_worker, "keyboard.type",
        {{"text", std::string(1025, 'x')}}, "keyboard-type-dispatch-bound");
    require(oversized_keyboard_dispatch.value("state", std::string{}) == "Failed",
            "keyboard typing must enforce the smaller per-call SendInput bound");
    const auto malformed_extra = run_core_job(core_worker, "window.list",
        {{"include_all_windows", true}}, "window-list-extra-argument");
    require(malformed_extra.value("state", std::string{}) == "Failed",
            "browser discovery must reject arguments outside its empty schema");
    const auto malformed_window_id = run_core_job(core_worker, "window.focus",
        {{"window_id", std::string(129, 'A')}}, "window-focus-oversized-id");
    require(malformed_window_id.value("state", std::string{}) == "Failed",
            "window.focus must reject an oversized window id");
    const auto malformed_control_id = run_core_job(core_worker, "ui.focus",
        {{"window_id", window_id}, {"target", std::string(513, 'A')}}, "ui-focus-oversized-id");
    require(malformed_control_id.value("state", std::string{}) == "Failed",
            "ui.focus must reject an oversized automation id");
    const auto malformed_invoke = run_core_job(core_worker, "ui.invoke",
        {{"window_id", window_id}, {"target", "send"}, {"action", "execute_script"}},
        "ui-invoke-invalid-action");
    require(malformed_invoke.value("state", std::string{}) == "Failed",
            "ui.invoke must reject actions outside its fixed enum");
    const auto unsupported_value_set = run_core_job(core_worker, "ui.invoke",
        {{"window_id", window_id}, {"target", "send"}, {"action", "set_value"}},
        "ui-invoke-value-not-in-core-contract");
    require(unsupported_value_set.value("state", std::string{}) == "Failed",
            "ui.invoke set_value must fail closed without a bounded value argument");
    const auto malformed_key = run_core_job(core_worker, "keyboard.key",
        {{"key", "ENTER"}, {"modifiers", nlohmann::json::array({"ctrl", "shift", "alt", "win"})}},
        "keyboard-key-too-many-modifiers");
    require(malformed_key.value("state", std::string{}) == "Failed",
            "keyboard.key must reject oversized modifier lists");

    {
        laso::Config denied_config;
        laso::CapabilityRegistry denied_registry;
        denied_registry.register_provider(std::make_shared<laso::WindowsPlatform>(denied_config));
        laso::WorkerProtocol denied_worker(denied_config, std::move(denied_registry), laso::AuditLog{});
        laso::CoreWorkerAdapter denied_core(denied_worker);
        const auto denied = run_core_job(denied_core, "clipboard.read", nlohmann::json::object(), "clipboard-default-deny");
        require(denied.value("state", std::string{}) == "Failed" &&
                    denied.value("error", std::string{}) == "capability denied",
                "default-deny endpoint policy must reject a capability with valid arguments");
    }
    {
        laso::Config approval_config;
        approval_config.capabilities["keyboard.type"] = laso::Decision::require_approval;
        laso::CapabilityRegistry approval_registry;
        approval_registry.register_provider(std::make_shared<laso::WindowsPlatform>(approval_config));
        laso::WorkerProtocol approval_worker(approval_config, std::move(approval_registry), laso::AuditLog{});
        laso::CoreWorkerAdapter approval_core(approval_worker);
        SetWindowTextW(edit, L"");
        const auto unapproved = run_core_job(approval_core, "keyboard.type", {{"text", "must not type"}}, "keyboard-require-approval");
        require(unapproved.value("state", std::string{}) == "Failed" &&
                    unapproved.value("error", std::string{}) == "approval denied or unavailable",
                "require_approval must fail closed without a Core approval exchange");
        wchar_t value[64]{}; GetWindowTextW(edit, value, static_cast<int>(std::size(value)));
        require(std::wstring(value).empty(), "unapproved keyboard input changed the fixture");
    }

    RECT button_rect{}; GetWindowRect(button, &button_rect);
    SetForegroundWindow(window);
    pump_messages();
    const bool native_fixture_foreground = GetForegroundWindow() == window;
    if (native_fixture_foreground) {
        SetWindowTextW(edit, L"");
        const auto unrelated_type = run_core_job(core_worker, "keyboard.type", {{"text", "must not type"}}, "keyboard-type-unrelated-window");
        require(unrelated_type.value("state", std::string{}) == "Failed",
                "keyboard.type must fail closed when a non-ChatGPT window is foreground");
        wchar_t untouched[64]{}; GetWindowTextW(edit, untouched, static_cast<int>(std::size(untouched)));
        require(std::wstring(untouched).empty(), "keyboard.type changed the unrelated fixture window");
    }
    const int click_x = (button_rect.left + button_rect.right) / 2;
    const int click_y = (button_rect.top + button_rect.bottom) / 2;
    const auto moved = run_capability(worker, "pointer.move", {{"x", click_x}, {"y", click_y}, {"coordinate_space", "primary_display_pixels"}});
    (void)moved;
    if (native_fixture_foreground) {
        run_capability(worker, "pointer.click", {{"x", click_x}, {"y", click_y}, {"button", "left"}, {"count", 1}, {"coordinate_space", "primary_display_pixels"}});
        pump_messages();
        wchar_t title[128]{}; GetWindowTextW(window, title, static_cast<int>(std::size(title)));
        require(std::wstring(title).find(L"clicked") != std::wstring::npos, "mouse click did not activate the fixture button");
    } else std::cout << "Pointer click test skipped: Windows did not grant foreground focus to the disposable fixture\n";

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

    if (browser_fixture.ready()) {
        const auto browser_id = handle_id(browser_fixture.window());
        const auto focused_window = run_core_capability(core_worker, "window.focus",
            {{"window_id", browser_id}}, "browser-window-focus");
        require(focused_window.value("focused", false), "Core child window.focus did not focus the local browser fixture");

        std::string edit_id_from_browser;
        std::string button_id_from_browser;
        bool named_edit = false;
        bool named_button = false;
        nlohmann::json browser_tree;
        for (int attempt = 0; attempt < 40; ++attempt) {
            browser_tree = run_core_capability(core_worker, "ui.inspect",
                {{"window_id", browser_id}},
                "browser-ui-inspect-" + std::to_string(attempt));
            require(browser_tree.at("elements").size() <= 32, "browser UI Automation exceeded its node bound");
            for (const auto& item : browser_tree.at("elements")) {
                require(item.value("name", std::string{}).size() <= 4 * 256 &&
                            item.value("automation_id", std::string{}).size() <= 4 * 128,
                        "browser UI Automation returned an unbounded property");
                if (item.value("automation_id", std::string{}) == "chat") edit_id_from_browser = "chat";
                if (item.value("automation_id", std::string{}) == "send") button_id_from_browser = "send";
                if (item.value("name", std::string{}) == "Lane 4 fixture message") named_edit = true;
                if (item.value("name", std::string{}) == "Send fixture") named_button = true;
            }
            if ((!edit_id_from_browser.empty() || named_edit) && (!button_id_from_browser.empty() || named_button)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        require(!edit_id_from_browser.empty() || named_edit,
                "UI Automation did not expose the local browser fixture's message control");
        const std::string edit_target = !edit_id_from_browser.empty() ? edit_id_from_browser : std::string("Lane 4 fixture message");
        nlohmann::json focus_args{{"window_id", browser_id}, {"target", edit_target}};
        const auto control = run_core_capability(core_worker, "ui.focus", focus_args, "browser-control-focus");
        require(control.value("performed", "") == "focus", "UI Automation did not focus the local browser control");
        run_core_capability(core_worker, "keyboard.type", {{"text", "LANE4-TYPED-OK"}}, "browser-keyboard-type");

        bool typed_text_observed = false;
        for (int attempt = 0; attempt < 40; ++attempt) {
            browser_tree = run_core_capability(core_worker, "ui.inspect",
                {{"window_id", browser_id}},
                "browser-ui-inspect-typed-" + std::to_string(attempt));
            if (browser_tree.dump().find("LANE4-TYPED-OK") != std::string::npos) { typed_text_observed = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        require(typed_text_observed, "keyboard.type did not reach the focused browser fixture control");

        run_core_capability(core_worker, "keyboard.key", {{"key", "ENTER"}}, "browser-keyboard-enter");
        bool key_observed = false;
        for (int attempt = 0; attempt < 40; ++attempt) {
            browser_tree = run_core_capability(core_worker, "ui.inspect",
                {{"window_id", browser_id}},
                "browser-ui-inspect-key-" + std::to_string(attempt));
            if (browser_tree.dump().find("LANE4-KEY-DISPATCH-OK") != std::string::npos) { key_observed = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        require(key_observed, "keyboard.key did not reach the focused browser fixture control");

        const std::string button_target = !button_id_from_browser.empty() ? button_id_from_browser : std::string("Send fixture");
        nlohmann::json invoke_args{{"window_id", browser_id}, {"target", button_target}, {"action", "invoke"}};
        require(!button_id_from_browser.empty() || named_button,
                "UI Automation did not expose the local browser fixture's send control");
        const auto invoked = run_core_capability(core_worker, "ui.invoke", invoke_args, "browser-ui-invoke");
        require(invoked.value("performed", "") == "invoke", "UI Automation did not invoke the local browser fixture control");
        bool invoke_observed = false;
        for (int attempt = 0; attempt < 40; ++attempt) {
            browser_tree = run_core_capability(core_worker, "ui.inspect",
                {{"window_id", browser_id}},
                "browser-ui-inspect-invoke-" + std::to_string(attempt));
            if (browser_tree.dump().find("LANE4-INVOKE-OK") != std::string::npos) { invoke_observed = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        require(invoke_observed, "ui.invoke did not reach the local browser fixture control");
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
