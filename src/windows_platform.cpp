#include "laso/platform.hpp"

#include <Windows.h>
#include <wincrypt.h>
#include <Shellapi.h>
#include <Wincodec.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "Crypt32.lib")

namespace laso {
namespace {
using Microsoft::WRL::ComPtr;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) { reset(); h_ = std::exchange(other.h_, nullptr); } return *this; }
    [[nodiscard]] HANDLE get() const { return h_; }
    [[nodiscard]] explicit operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = nullptr) { if (*this) CloseHandle(h_); h_ = value; }
private:
    HANDLE h_{nullptr};
};

struct BrowserWindowSummary {
    std::size_t visible_window_count{0};
    bool browser_visible{false};
    bool active_browser_visible{false};
    HWND foreground{nullptr};
};

bool is_browser_window(HWND window) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == 0) return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
    if (!process) return false;
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, image.data(), &length) || length == 0) return false;
    image.resize(length);
    auto executable = std::filesystem::path(image).filename().wstring();
    std::transform(executable.begin(), executable.end(), executable.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    static constexpr std::array<std::wstring_view, 6> browsers{
        L"msedge.exe", L"chrome.exe", L"firefox.exe", L"brave.exe", L"opera.exe", L"vivaldi.exe"};
    return std::find(browsers.begin(), browsers.end(), executable) != browsers.end();
}

bool is_chatgpt_window(HWND window) {
    if (!window || !is_browser_window(window)) return false;
    const int length = GetWindowTextLengthW(window);
    if (length <= 0 || length > 4096) return false;
    std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, title.data(), length + 1);
    if (copied <= 0) return false;
    title.resize(static_cast<std::size_t>(copied));
    std::transform(title.begin(), title.end(), title.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return title.find(L"chatgpt") != std::wstring::npos;
}

BrowserWindowSummary browser_window_summary() {
    BrowserWindowSummary summary;
    summary.foreground = GetForegroundWindow();
    if (!EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            if (!IsWindowVisible(window)) return TRUE;
            auto& state = *reinterpret_cast<BrowserWindowSummary*>(parameter);
            ++state.visible_window_count;
            if (is_browser_window(window)) {
                state.browser_visible = true;
                if (window == state.foreground) state.active_browser_visible = true;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&summary)))
        throw std::runtime_error("browser status unavailable");
    return summary;
}

class GdiObject {
public:
    explicit GdiObject(HGDIOBJ object = nullptr) : object_(object) {}
    ~GdiObject() { if (object_) DeleteObject(object_); }
    GdiObject(const GdiObject&) = delete;
    GdiObject& operator=(const GdiObject&) = delete;
    [[nodiscard]] HGDIOBJ get() const { return object_; }
private:
    HGDIOBJ object_;
};

class Dc {
public:
    explicit Dc(HDC dc = nullptr) : dc_(dc) {}
    ~Dc() { if (dc_) DeleteDC(dc_); }
    Dc(const Dc&) = delete;
    Dc& operator=(const Dc&) = delete;
    [[nodiscard]] HDC get() const { return dc_; }
private:
    HDC dc_;
};

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("invalid UTF-8 argument");
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), count) != count)
        throw std::runtime_error("invalid UTF-8 argument");
    return out;
}

std::string narrow(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("invalid Unicode result");
    std::string out(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), count, nullptr, nullptr) != count)
        throw std::runtime_error("invalid Unicode result");
    return out;
}

int integer_arg(const nlohmann::json& args, const char* name, int min, int max) {
    if (!args.contains(name) || !args[name].is_number_integer()) throw std::runtime_error("invalid capability request");
    const auto value = args[name].get<long long>();
    if (value < min || value > max) throw std::runtime_error("invalid capability request");
    return static_cast<int>(value);
}

void require_cancelled(const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) throw std::runtime_error("cancelled");
}

[[noreturn]] void invalid_capability_request() {
    throw std::runtime_error("invalid capability request");
}

void validate_object_keys(const nlohmann::json& args,
                          std::initializer_list<std::string_view> allowed,
                          std::initializer_list<std::string_view> required = {}) {
    if (!args.is_object()) invalid_capability_request();
    for (const auto key : required)
        if (!args.contains(std::string(key))) invalid_capability_request();
    for (auto it = args.begin(); it != args.end(); ++it) {
        const auto key = std::string_view(it.key());
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) invalid_capability_request();
    }
}

void validate_string_argument(const nlohmann::json& args, const char* key, std::size_t max_bytes,
                              bool required = false) {
    if (!args.contains(key)) {
        if (required) invalid_capability_request();
        return;
    }
    if (!args.at(key).is_string() || args.at(key).get_ref<const std::string&>().empty() ||
        args.at(key).get_ref<const std::string&>().size() > max_bytes) invalid_capability_request();
}

void validate_chat_orchestrator_arguments(const std::string& capability, const nlohmann::json& args) {
    if (capability == "browser.status" || capability == "window.list") {
        validate_object_keys(args, {});
    } else if (capability == "window.focus") {
        validate_object_keys(args, {"window_id"}, {"window_id"});
        validate_string_argument(args, "window_id", 128, true);
    } else if (capability == "ui.inspect") {
        validate_object_keys(args, {"window_id"}, {"window_id"});
        validate_string_argument(args, "window_id", 128, true);
    } else if (capability == "ui.focus") {
        validate_object_keys(args, {"window_id", "target"}, {"window_id", "target"});
        validate_string_argument(args, "window_id", 128, true);
        validate_string_argument(args, "target", 512, true);
    } else if (capability == "ui.invoke") {
        validate_object_keys(args, {"window_id", "target", "action", "value"}, {"window_id", "target", "action"});
        validate_string_argument(args, "window_id", 128, true);
        validate_string_argument(args, "target", 512, true);
        validate_string_argument(args, "value", 4096);
        if (!args.at("action").is_string() ||
            (args.at("action") != "invoke" && args.at("action") != "set_value")) invalid_capability_request();
        const auto action = args.at("action").get<std::string>();
        if ((action == "set_value") != args.contains("value")) invalid_capability_request();
    } else if (capability == "keyboard.type") {
        validate_object_keys(args, {"text"}, {"text"});
        if (!args.at("text").is_string() || args.at("text").get_ref<const std::string&>().size() > 1024)
            invalid_capability_request();
    } else if (capability == "keyboard.key") {
        validate_object_keys(args, {"key"}, {"key"});
        validate_string_argument(args, "key", 16, true);
        static const std::set<std::string> core_key_allowlist{
            "ENTER", "ESC", "TAB", "SPACE", "BACKSPACE", "DELETE", "UP", "DOWN", "LEFT", "RIGHT", "HOME", "END"};
        if (!core_key_allowlist.contains(args.at("key").get<std::string>())) invalid_capability_request();
    }
}

std::string base64(const BYTE* data, DWORD size) {
    DWORD needed = 0;
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &needed))
        throw std::runtime_error("image encoding failed");
    std::string text(needed, '\0');
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, text.data(), &needed))
        throw std::runtime_error("image encoding failed");
    if (!text.empty() && text.back() == '\0') text.pop_back();
    return text;
}

nlohmann::json capture(const std::function<bool()>& cancelled) {
    require_cancelled(cancelled);
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) throw std::runtime_error("screen capture unavailable");
    HDC source = GetDC(nullptr);
    if (!source) throw std::runtime_error("screen capture unavailable");
    Dc source_guard(nullptr);
    HDC memory = CreateCompatibleDC(source);
    if (!memory) { ReleaseDC(nullptr, source); throw std::runtime_error("screen capture unavailable"); }
    Dc memory_guard(memory);
    HBITMAP bitmap = CreateCompatibleBitmap(source, width, height);
    if (!bitmap) { ReleaseDC(nullptr, source); throw std::runtime_error("screen capture unavailable"); }
    GdiObject bitmap_guard(bitmap);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL copied = BitBlt(memory, 0, 0, width, height, source, 0, 0, SRCCOPY | CAPTUREBLT);
    if (previous) SelectObject(memory, previous);
    ReleaseDC(nullptr, source);
    if (!copied) throw std::runtime_error("screen capture failed");
    require_cancelled(cancelled);

    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) throw std::runtime_error("screen encoder unavailable");
    struct ComCleanup { bool active; ~ComCleanup() { if (active) CoUninitialize(); } } com_cleanup{uninitialize};
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> wic_bitmap;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, &wic_bitmap)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder))) {
        throw std::runtime_error("screen encoder unavailable");
    }
    constexpr UINT max_capture_dimension = 1280;
    const double scale = std::min(1.0, static_cast<double>(max_capture_dimension) / std::max(width, height));
    const UINT output_width = static_cast<UINT>(std::max(1.0, std::round(width * scale)));
    const UINT output_height = static_cast<UINT>(std::max(1.0, std::round(height * scale)));
    IWICBitmapSource* image_source = wic_bitmap.Get();
    if (output_width != static_cast<UINT>(width) || output_height != static_cast<UINT>(height)) {
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(wic_bitmap.Get(), output_width, output_height, WICBitmapInterpolationModeFant)))
            throw std::runtime_error("screen encoder unavailable");
        image_source = scaler.Get();
    }
    IStream* raw_stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &raw_stream))) throw std::runtime_error("screen encoder unavailable");
    ComPtr<IStream> output_stream; output_stream.Attach(raw_stream);
    if (FAILED(encoder->Initialize(output_stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &properties)) || FAILED(frame->Initialize(properties.Get())))
        throw std::runtime_error("screen encoder unavailable");
    PROPBAG2 option{}; option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT quality; VariantInit(&quality); quality.vt = VT_R4; quality.fltVal = 0.5F;
    if (properties) properties->Write(1, &option, &quality);
    if (FAILED(frame->SetSize(output_width, output_height))) throw std::runtime_error("screen encoder failed");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format)) || FAILED(frame->WriteSource(image_source, nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) throw std::runtime_error("screen encoder failed");
    HGLOBAL output_memory = nullptr;
    if (FAILED(GetHGlobalFromStream(output_stream.Get(), &output_memory))) throw std::runtime_error("screen encoder failed");
    STATSTG stream_stats{};
    if (FAILED(output_stream->Stat(&stream_stats, STATFLAG_NONAME))) throw std::runtime_error("screen encoder failed");
    const auto size = stream_stats.cbSize.QuadPart;
    if (size == 0 || size > 600U * 1024U) throw std::runtime_error("image exceeds bounded output size");
    const auto* bytes = static_cast<const BYTE*>(GlobalLock(output_memory));
    if (!bytes) throw std::runtime_error("screen encoder failed");
    const auto data = base64(bytes, static_cast<DWORD>(size));
    GlobalUnlock(output_memory);
    return {{"width", output_width}, {"height", output_height}, {"display", "primary"}, {"format", "jpeg"},
            {"data_base64", data}, {"persisted", false}};
}

class ComApartment {
public:
    ComApartment() {
        const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE) throw std::runtime_error("Windows automation unavailable");
        active_ = SUCCEEDED(result);
    }
    ~ComApartment() { if (active_) CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
private:
    bool active_{false};
};

std::pair<std::string, bool> bounded_bstr_utf8(BSTR value, std::size_t max_chars) {
    if (!value) return {"", false};
    const auto length = static_cast<std::size_t>(SysStringLen(value));
    auto keep = std::min(length, max_chars);
    if (keep > 0 && keep < length && value[keep - 1] >= 0xd800 && value[keep - 1] <= 0xdbff) --keep;
    try {
        auto result = narrow(std::wstring_view(value, keep));
        SysFreeString(value);
        return {std::move(result), length > keep};
    } catch (...) {
        SysFreeString(value);
        throw;
    }
}

HWND window_handle(const nlohmann::json& args) {
    if (!args.contains("window_id") || !args["window_id"].is_string()) throw std::runtime_error("invalid capability request");
    const auto text = widen(args["window_id"].get<std::string>());
    wchar_t* end = nullptr;
    const auto value = wcstoull(text.c_str(), &end, 16);
    if (!end || *end != L'\0' || value == 0) throw std::runtime_error("invalid capability request");
    const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
    if (!IsWindow(window)) throw std::runtime_error("window not found");
    return window;
}

nlohmann::json inspect_ui(const nlohmann::json& args, const std::function<bool()>& cancelled) {
    ComApartment apartment;
    const auto window = window_handle(args);
    if (!is_chatgpt_window(window) || GetForegroundWindow() != window)
        throw std::runtime_error("window is not the foreground ChatGPT browser window");
    constexpr int max_nodes = 32;
    constexpr int max_depth = 8;
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationElement> root;
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) ||
        FAILED(automation->ElementFromHandle(window, &root)) || FAILED(automation->get_ControlViewWalker(&walker)))
        throw std::runtime_error("Windows automation unavailable");
    struct Node { ComPtr<IUIAutomationElement> element; int depth; };
    std::vector<Node> pending{{root, 0}};
    nlohmann::json elements = nlohmann::json::array();
    bool truncated = false;
    while (!pending.empty() && elements.size() < static_cast<std::size_t>(max_nodes)) {
        require_cancelled(cancelled);
        if (GetForegroundWindow() != window) throw std::runtime_error("ChatGPT window lost foreground focus during inspection");
        auto current = std::move(pending.back());
        pending.pop_back();
        BSTR name = nullptr, automation_id = nullptr;
        CONTROLTYPEID control_type{};
        BOOL enabled = FALSE, offscreen = FALSE;
        current.element->get_CurrentName(&name);
        current.element->get_CurrentAutomationId(&automation_id);
        current.element->get_CurrentControlType(&control_type);
        current.element->get_CurrentIsEnabled(&enabled);
        current.element->get_CurrentIsOffscreen(&offscreen);
        const auto [element_name, name_truncated] = bounded_bstr_utf8(name, 256);
        const auto [element_automation_id, automation_id_truncated] = bounded_bstr_utf8(automation_id, 128);
        elements.push_back({{"name", element_name}, {"name_truncated", name_truncated},
                            {"automation_id", element_automation_id},
                            {"automation_id_truncated", automation_id_truncated},
                            {"control_type", control_type}, {"enabled", enabled != FALSE}, {"offscreen", offscreen != FALSE},
                            {"depth", current.depth}});
        if (current.depth >= max_depth) continue;
        ComPtr<IUIAutomationElement> child;
        if (FAILED(walker->GetFirstChildElement(current.element.Get(), &child)) || !child) continue;
        std::vector<ComPtr<IUIAutomationElement>> siblings;
        while (child && siblings.size() < 257) {
            siblings.push_back(child);
            ComPtr<IUIAutomationElement> next;
            if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) break;
            child = std::move(next);
        }
        for (auto it = siblings.rbegin(); it != siblings.rend(); ++it) pending.push_back({*it, current.depth + 1});
    }
    truncated = truncated || !pending.empty();
    return {{"elements", std::move(elements)}, {"truncated", truncated}};
}

nlohmann::json invoke_ui(const nlohmann::json& args, const std::function<bool()>& cancelled) {
    ComApartment apartment;
    require_cancelled(cancelled);
    const auto window = window_handle(args);
    if (!is_chatgpt_window(window) || GetForegroundWindow() != window)
        throw std::runtime_error("window is not the foreground ChatGPT browser window");
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) ||
        FAILED(automation->ElementFromHandle(window, &root))) throw std::runtime_error("Windows automation unavailable");
    const auto target_name = widen(args.at("target").get<std::string>());
    ComPtr<IUIAutomationElementArray> matches;
    auto find_matches = [&](PROPERTYID property) {
        VARIANT value; VariantInit(&value);
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocStringLen(target_name.data(), static_cast<UINT>(target_name.size()));
        ComPtr<IUIAutomationCondition> condition;
        if (!value.bstrVal || FAILED(automation->CreatePropertyCondition(property, value, &condition))) {
            VariantClear(&value);
            throw std::runtime_error("Windows automation unavailable");
        }
        VariantClear(&value);
        matches.Reset();
        if (FAILED(root->FindAll(TreeScope_Subtree, condition.Get(), &matches)) || !matches)
            throw std::runtime_error("Windows automation target not found");
        int found = 0;
        if (FAILED(matches->get_Length(&found))) throw std::runtime_error("Windows automation target not found");
        return found;
    };
    int count = find_matches(UIA_AutomationIdPropertyId);
    if (count == 0) count = find_matches(UIA_NamePropertyId);
    if (count != 1) throw std::runtime_error(count == 0 ? "Windows automation target not found" : "Windows automation target is ambiguous");
    if (GetForegroundWindow() != window) throw std::runtime_error("ChatGPT window lost foreground focus");
    ComPtr<IUIAutomationElement> target;
    if (FAILED(matches->GetElement(0, &target)) || !target) throw std::runtime_error("Windows automation target not found");
    const auto action = args.value("action", std::string("invoke"));
    if (action == "focus") {
        if (FAILED(target->SetFocus())) throw std::runtime_error("Windows control focus failed");
    } else if (action == "set_value") {
        if (!args.contains("value") || !args["value"].is_string() || args["value"].get_ref<const std::string&>().size() > 4096)
            throw std::runtime_error("invalid capability request");
        ComPtr<IUIAutomationValuePattern> pattern;
        if (FAILED(target->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pattern))) || !pattern)
            throw std::runtime_error("Windows control does not support setting a value");
        const auto text = widen(args["value"].get<std::string>());
        BSTR bstr = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        if (!bstr) throw std::runtime_error("Windows control value update failed");
        const auto set_result = pattern->SetValue(bstr);
        SysFreeString(bstr);
        if (FAILED(set_result)) throw std::runtime_error("Windows control value update failed");
    } else if (action == "invoke") {
        ComPtr<IUIAutomationInvokePattern> pattern;
        if (FAILED(target->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&pattern))) || !pattern)
            throw std::runtime_error("Windows control does not support invoke");
        if (FAILED(pattern->Invoke())) throw std::runtime_error("Windows control invocation failed");
    } else throw std::runtime_error("invalid capability request");
    return {{"performed", action}};
}

bool send_input(std::vector<INPUT>& inputs) {
    if (inputs.empty()) return true;
    return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
}

WORD key_code(const std::string& name) {
    static const std::map<std::string, WORD> keys{
        {"ENTER", static_cast<WORD>(VK_RETURN)}, {"TAB", static_cast<WORD>(VK_TAB)},
        {"ESC", static_cast<WORD>(VK_ESCAPE)}, {"SPACE", static_cast<WORD>(VK_SPACE)},
        {"BACKSPACE", static_cast<WORD>(VK_BACK)}, {"DELETE", static_cast<WORD>(VK_DELETE)},
        {"UP", static_cast<WORD>(VK_UP)}, {"DOWN", static_cast<WORD>(VK_DOWN)},
        {"LEFT", static_cast<WORD>(VK_LEFT)}, {"RIGHT", static_cast<WORD>(VK_RIGHT)},
        {"HOME", static_cast<WORD>(VK_HOME)}, {"END", static_cast<WORD>(VK_END)},
        {"PAGEUP", static_cast<WORD>(VK_PRIOR)}, {"PAGEDOWN", static_cast<WORD>(VK_NEXT)},
        {"INSERT", static_cast<WORD>(VK_INSERT)}};
    const auto it = keys.find(name);
    if (it != keys.end()) return it->second;
    if (name.size() >= 2 && name[0] == 'F') {
        try { const int n = std::stoi(name.substr(1)); if (n >= 1 && n <= 24) return static_cast<WORD>(VK_F1 + n - 1); }
        catch (...) {}
    }
    if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0]))) return static_cast<WORD>(std::toupper(static_cast<unsigned char>(name[0])));
    throw std::runtime_error("unsupported key");
}

void append_key(std::vector<INPUT>& out, WORD vk, DWORD flags) {
    INPUT input{}; input.type = INPUT_KEYBOARD; input.ki.wVk = vk; input.ki.dwFlags = flags; out.push_back(input);
}

std::wstring quote_arg(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(arg);
    std::wstring out(1, L'\"');
    std::size_t slashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') { out.append(slashes * 2 + 1, L'\\'); out.push_back(L'\"'); slashes = 0; continue; }
        out.append(slashes, L'\\'); slashes = 0; out.push_back(ch);
    }
    out.append(slashes * 2, L'\\'); out.push_back(L'\"');
    return out;
}

std::wstring strip_extended_prefix(std::wstring value) {
    if (value.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + value.substr(8);
    if (value.starts_with(L"\\\\?\\")) value.erase(0, 4);
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

void drain_pipe(HANDLE pipe, std::string& output, bool& truncated) {
    std::array<char, 8192> buffer{};
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
        DWORD read = 0;
        const DWORD request = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(pipe, buffer.data(), request, &read, nullptr) || read == 0) break;
        const auto room = output.size() < 65536 ? 65536 - output.size() : 0;
        const auto copy = std::min<std::size_t>(room, read);
        output.append(buffer.data(), copy);
        if (copy < read) truncated = true;
    }
}

nlohmann::json run_process(const Config& config, const nlohmann::json& args,
                           const std::function<bool()>& cancelled) {
    if (!args.contains("executable") || !args["executable"].is_string() ||
        !args.contains("arguments") || !args["arguments"].is_array()) throw std::runtime_error("invalid capability request");
    const auto executable_utf8 = args["executable"].get<std::string>();
    if (executable_utf8.size() > 4096 || args["arguments"].size() > 128) throw std::runtime_error("invalid capability request");
    const auto executable = widen(executable_utf8);
    if (!std::filesystem::path(executable).is_absolute() || std::filesystem::path(executable).lexically_normal() != executable)
        throw std::runtime_error("invalid capability request");
    if (executable.starts_with(L"\\\\")) throw std::runtime_error("capability denied");
    if (std::find(config.allowed_executables.begin(), config.allowed_executables.end(), executable) == config.allowed_executables.end())
        throw std::runtime_error("capability denied");
    Handle image(CreateFileW(executable.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!image) throw std::runtime_error("capability unavailable");
    BY_HANDLE_FILE_INFORMATION file_info{};
    if (!GetFileInformationByHandle(image.get(), &file_info) || (file_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (file_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) throw std::runtime_error("capability denied");
    std::wstring resolved(32768, L'\0');
    const auto resolved_size = GetFinalPathNameByHandleW(image.get(), resolved.data(), static_cast<DWORD>(resolved.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!resolved_size || resolved_size >= resolved.size()) throw std::runtime_error("capability denied");
    resolved.resize(resolved_size);
    std::wstring configured(32768, L'\0');
    const auto configured_size = GetFullPathNameW(executable.c_str(), static_cast<DWORD>(configured.size()), configured.data(), nullptr);
    if (!configured_size || configured_size >= configured.size()) throw std::runtime_error("capability denied");
    configured.resize(configured_size);
    if (strip_extended_prefix(std::move(resolved)) != strip_extended_prefix(std::move(configured)))
        throw std::runtime_error("capability denied");

    std::vector<std::wstring> argv; argv.push_back(executable);
    std::size_t total = 0;
    for (const auto& item : args["arguments"]) {
        if (!item.is_string()) throw std::runtime_error("invalid capability request");
        auto text = item.get<std::string>();
        if (text.size() > 8192 || text.find('\0') != std::string::npos || (total += text.size()) > 32768)
            throw std::runtime_error("invalid capability request");
        argv.push_back(widen(text));
    }
    std::wstring command;
    for (const auto& arg : argv) { if (!command.empty()) command.push_back(L' '); command += quote_arg(arg); }
    if (command.size() > 32767) throw std::runtime_error("invalid capability request");
    unsigned timeout = 30000;
    if (args.contains("timeout_ms")) {
        if (!args["timeout_ms"].is_number_integer()) throw std::runtime_error("invalid capability request");
        const auto raw = args["timeout_ms"].get<long long>();
        if (raw < 100 || raw > 300000) throw std::runtime_error("invalid capability request");
        timeout = static_cast<unsigned>(raw);
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_read_raw = nullptr, out_write_raw = nullptr, err_read_raw = nullptr, err_write_raw = nullptr;
    if (!CreatePipe(&out_read_raw, &out_write_raw, &sa, 0) || !CreatePipe(&err_read_raw, &err_write_raw, &sa, 0)) {
        if (out_read_raw) CloseHandle(out_read_raw); if (out_write_raw) CloseHandle(out_write_raw);
        if (err_read_raw) CloseHandle(err_read_raw); if (err_write_raw) CloseHandle(err_write_raw);
        throw std::runtime_error("process setup failed");
    }
    Handle out_read(out_read_raw), out_write(out_write_raw), err_read(err_read_raw), err_write(err_write_raw);
    SetHandleInformation(out_read.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read.get(), HANDLE_FLAG_INHERIT, 0);
    SECURITY_ATTRIBUTES input_attributes{sizeof(input_attributes), nullptr, TRUE};
    Handle null_input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &input_attributes,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!null_input) throw std::runtime_error("process setup failed");

    std::vector<std::wstring> environment;
    for (const auto& name : config.environment_allowlist) {
        const auto length = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
        if (!length) continue;
        if (length > 8192) throw std::runtime_error("configured environment value too large");
        std::wstring value(length, L'\0');
        const auto written = GetEnvironmentVariableW(name.c_str(), value.data(), length);
        if (written >= length) throw std::runtime_error("configured environment value too large");
        value.resize(written);
        environment.push_back(name + L"=" + value);
    }
    std::sort(environment.begin(), environment.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> env_block;
    for (const auto& entry : environment) { env_block.insert(env_block.end(), entry.begin(), entry.end()); env_block.push_back(L'\0'); }
    if (env_block.empty()) env_block.push_back(L'\0');
    env_block.push_back(L'\0');

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::byte> attr_storage(attr_size);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attr_size)) throw std::runtime_error("process setup failed");
    struct AttributeCleanup { PPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeCleanup() { DeleteProcThreadAttributeList(value); } } cleanup{attributes};
    HANDLE inherited[]{out_write.get(), err_write.get(), null_input.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
        throw std::runtime_error("process setup failed");
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = out_write.get(); startup.StartupInfo.hStdError = err_write.get(); startup.StartupInfo.hStdInput = null_input.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
    std::wstring temp(MAX_PATH + 1, L'\0');
    const auto temp_len = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    if (!temp_len || temp_len >= temp.size()) throw std::runtime_error("temporary working directory unavailable");
    temp.resize(temp_len);
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                        env_block.data(), temp.c_str(), &startup.StartupInfo, &process))
        throw std::runtime_error("process launch failed");
    Handle process_handle(process.hProcess), thread_handle(process.hThread);
    out_write.reset(); err_write.reset(); null_input.reset();
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) { TerminateProcess(process_handle.get(), 1); throw std::runtime_error("process isolation failed"); }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job.get(), process_handle.get())) { TerminateProcess(process_handle.get(), 1); throw std::runtime_error("process isolation failed"); }

    std::string stdout_text, stderr_text; bool stdout_truncated = false, stderr_truncated = false;
    ResumeThread(thread_handle.get());
    const auto started = GetTickCount64();
    bool timed_out = false, was_cancelled = false;
    for (;;) {
        drain_pipe(out_read.get(), stdout_text, stdout_truncated);
        drain_pipe(err_read.get(), stderr_text, stderr_truncated);
        const DWORD wait = WaitForSingleObject(process_handle.get(), 50);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) { TerminateJobObject(job.get(), 1); break; }
        if (cancelled && cancelled()) { was_cancelled = true; TerminateJobObject(job.get(), 1); break; }
        if (GetTickCount64() - started >= timeout) { timed_out = true; TerminateJobObject(job.get(), 1); break; }
    }
    WaitForSingleObject(process_handle.get(), 2000);
    drain_pipe(out_read.get(), stdout_text, stdout_truncated);
    drain_pipe(err_read.get(), stderr_text, stderr_truncated);
    out_read.reset(); err_read.reset();
    if (was_cancelled) throw std::runtime_error("cancelled");
    if (timed_out) throw std::runtime_error("timed out");
    DWORD exit_code = 1; GetExitCodeProcess(process_handle.get(), &exit_code);
    return {{"exit_code", exit_code}, {"stdout", stdout_text}, {"stderr", stderr_text},
            {"output_truncated", stdout_truncated || stderr_truncated}};
}

} // namespace

WindowsPlatform::WindowsPlatform(Config config) : config_(std::move(config)) {}

PluginIdentity WindowsPlatform::identity() const {
    return {"windows.native", "1.0.0", "Native Win32 and Windows SDK capabilities", true};
}

std::vector<CapabilityDescriptor> WindowsPlatform::capabilities() const {
    using Json = nlohmann::json;
    const auto schema = [](Json properties, std::initializer_list<const char*> required = {}) {
        Json result{{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
        if (required.size() != 0) result["required"] = required;
        return result;
    };
    std::vector<CapabilityDescriptor> items{
        {"screen.capture", "Capture the primary display without persisting the image", "sensitive_read", schema(Json::object()), true, {}},
        {"pointer.move", "Move the pointer in primary-display pixel coordinates", "interaction",
            schema(Json{{"x", Json{{"type", "integer"}}}, {"y", Json{{"type", "integer"}}},
                        {"coordinate_space", Json{{"const", "primary_display_pixels"}}}}, {"x", "y", "coordinate_space"}), true, {}},
        {"pointer.click", "Click in primary-display pixel coordinates", "interaction",
            schema(Json{{"x", Json{{"type", "integer"}}}, {"y", Json{{"type", "integer"}}},
                        {"button", Json{{"enum", Json{"left", "right", "middle"}}}},
                        {"count", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 2}}},
                        {"coordinate_space", Json{{"const", "primary_display_pixels"}}}},
                   {"x", "y", "button", "count", "coordinate_space"}), true, {}},
        {"keyboard.type", "Type literal text into the focused ChatGPT browser control", "sensitive_write",
            schema(Json{{"text", Json{{"type", "string"}, {"maxLength", 1024}}}}, {"text"}), true, {}},
        {"keyboard.key", "Press one supported key in the foreground ChatGPT browser", "interaction",
            schema(Json{{"key", Json{{"enum", Json{"ENTER", "ESC", "TAB", "SPACE", "BACKSPACE", "DELETE",
                                                        "UP", "DOWN", "LEFT", "RIGHT", "HOME", "END"}}}}, {"key"}), true, {}},
        {"clipboard.read", "Read plain-text clipboard data", "sensitive_read", schema(Json::object()), true, {}},
        {"clipboard.write", "Replace plain-text clipboard data", "sensitive_write",
            schema(Json{{"text", Json{{"type", "string"}, {"maxLength", 1048576}}}}, {"text"}), true, {}},
        {"browser.status", "Report visible-window count and browser visibility without exposing titles", "sensitive_read",
            schema(Json::object()), true, {}},
        {"window.list", "List visible ChatGPT browser windows with generic titles", "sensitive_read", schema(Json::object()), true, {}},
        {"window.focus", "Focus a visible ChatGPT browser window by its local id", "interaction",
            schema(Json{{"window_id", Json{{"type", "string"}, {"maxLength", 128}}}}, {"window_id"}), true, {}},
        {"ui.focus", "Focus one UI Automation control by target", "interaction",
            schema(Json{{"window_id", Json{{"type", "string"}, {"maxLength", 128}}},
                        {"target", Json{{"type", "string"}, {"maxLength", 512}}}}, {"window_id", "target"}), true, {}},
        {"ui.inspect", "Inspect a bounded foreground ChatGPT UI Automation control tree", "sensitive_read",
            schema(Json{{"window_id", Json{{"type", "string"}, {"maxLength", 128}}}}, {"window_id"}), true, {}},
        {"ui.invoke", "Invoke a uniquely matched UI Automation control or set its value", "interaction",
            schema(Json{{"window_id", Json{{"type", "string"}, {"maxLength", 128}}},
                        {"target", Json{{"type", "string"}, {"maxLength", 512}}},
                        {"action", Json{{"enum", Json{"invoke", "set_value"}}}},
                        {"value", Json{{"type", "string"}, {"maxLength", 4096}}}}, {"window_id", "target", "action"}), true, {}},
        {"shell.execute", "Run an allowlisted executable with explicit arguments", "high",
            schema(Json{{"executable", Json{{"type", "string"}, {"maxLength", 4096}}},
                        {"arguments", Json{{"type", "array"}, {"maxItems", 128}}},
                        {"timeout_ms", Json{{"type", "integer"}, {"minimum", 100}, {"maximum", 300000}}}},
                   {"executable", "arguments"}),
            !config_.allowed_executables.empty(), {}}
    };
    return items;
}

nlohmann::json WindowsPlatform::invoke(const InvocationContext& invocation) {
    return invoke(invocation.capability, invocation.arguments, invocation.cancelled);
}

bool WindowsPlatform::health() const { return true; }

void WindowsPlatform::shutdown() noexcept {}

bool WindowsPlatform::available(const std::string& capability) const {
    if (capability == "shell.execute") return !config_.allowed_executables.empty();
    static constexpr std::array<std::string_view, 13> native_capabilities{
        "screen.capture", "pointer.move", "pointer.click", "keyboard.type", "keyboard.key",
        "clipboard.read", "clipboard.write", "browser.status", "window.list", "window.focus", "ui.focus", "ui.inspect", "ui.invoke"};
    return std::find(native_capabilities.begin(), native_capabilities.end(), capability) != native_capabilities.end();
}

nlohmann::json WindowsPlatform::invoke(const std::string& capability, const nlohmann::json& args,
                                       const std::function<bool()>& cancelled) {
    require_cancelled(cancelled);
    validate_chat_orchestrator_arguments(capability, args);
    if (capability == "screen.capture") return capture(cancelled);
    if (capability == "ui.focus") {
        auto focus_args = args;
        focus_args["action"] = "focus";
        return invoke_ui(focus_args, cancelled);
    }
    if (capability == "ui.inspect") return inspect_ui(args, cancelled);
    if (capability == "ui.invoke") return invoke_ui(args, cancelled);
    if (capability == "browser.status") {
        const auto summary = browser_window_summary();
        return {{"window_count", summary.visible_window_count},
                {"browser_status", {{"browser_visible", summary.browser_visible},
                                    {"active_browser_visible", summary.active_browser_visible}}}};
    }
    if (capability == "pointer.move" || capability == "pointer.click") {
        if (args.value("coordinate_space", std::string{}) != "primary_display_pixels") throw std::runtime_error("invalid capability request");
        const int x = integer_arg(args, "x", 0, GetSystemMetrics(SM_CXSCREEN) - 1);
        const int y = integer_arg(args, "y", 0, GetSystemMetrics(SM_CYSCREEN) - 1);
        if (!SetCursorPos(x, y)) throw std::runtime_error("pointer action failed");
        if (capability == "pointer.move") return {{"moved", true}};
        const auto button = args.value("button", std::string{});
        const int count = args.value("count", 0);
        if (count < 1 || count > 2) throw std::runtime_error("invalid capability request");
        DWORD down = 0, up = 0;
        if (button == "left") { down = MOUSEEVENTF_LEFTDOWN; up = MOUSEEVENTF_LEFTUP; }
        else if (button == "right") { down = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP; }
        else if (button == "middle") { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }
        else throw std::runtime_error("invalid capability request");
        std::vector<INPUT> inputs;
        for (int i = 0; i < count; ++i) { INPUT a{}; a.type = INPUT_MOUSE; a.mi.dwFlags = down; inputs.push_back(a); INPUT b{}; b.type = INPUT_MOUSE; b.mi.dwFlags = up; inputs.push_back(b); }
        if (!send_input(inputs)) throw std::runtime_error("pointer action failed");
        return {{"clicked", true}, {"count", count}};
    }
    if (capability == "keyboard.type") {
        const HWND target_window = GetForegroundWindow();
        if (!is_chatgpt_window(target_window)) throw std::runtime_error("foreground window is not ChatGPT");
        const auto& text = args["text"].get_ref<const std::string&>();
        if (text.size() > 1024) throw std::runtime_error("keyboard input exceeds the per-call limit");
        const auto wide = widen(text);
        std::vector<INPUT> inputs; inputs.reserve(wide.size() * 2);
        for (wchar_t ch : wide) { INPUT down{}; down.type = INPUT_KEYBOARD; down.ki.wScan = static_cast<WORD>(ch); down.ki.dwFlags = KEYEVENTF_UNICODE; inputs.push_back(down); INPUT up = down; up.ki.dwFlags |= KEYEVENTF_KEYUP; inputs.push_back(up); }
        if (GetForegroundWindow() != target_window || !is_chatgpt_window(target_window))
            throw std::runtime_error("foreground ChatGPT window changed before keyboard input");
        if (!send_input(inputs)) throw std::runtime_error("keyboard input failed");
        return {{"typed_chars", wide.size()}, {"sensitive_payload_redacted", true}};
    }
    if (capability == "keyboard.key") {
        const HWND target_window = GetForegroundWindow();
        if (!is_chatgpt_window(target_window)) throw std::runtime_error("foreground window is not ChatGPT");
        const auto key = args["key"].get<std::string>();
        if (key.empty() || key.size() > 16) throw std::runtime_error("invalid capability request");
        const auto vk = key_code(key);
        std::vector<INPUT> inputs;
        append_key(inputs, vk, 0); append_key(inputs, vk, KEYEVENTF_KEYUP);
        if (GetForegroundWindow() != target_window || !is_chatgpt_window(target_window))
            throw std::runtime_error("foreground ChatGPT window changed before keyboard input");
        if (!send_input(inputs)) throw std::runtime_error("keyboard input failed");
        return {{"pressed", true}};
    }
    if (capability == "clipboard.read") {
        if (!OpenClipboard(nullptr)) throw std::runtime_error("clipboard unavailable");
        struct Close { ~Close() { CloseClipboard(); } } close;
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (!data) return {{"text", ""}, {"chars", 0}};
        const auto size = GlobalSize(data);
        if (!size || size > 1U * 1024U * 1024U) throw std::runtime_error("clipboard data exceeds bounded size");
        const auto* value = static_cast<const wchar_t*>(GlobalLock(data));
        if (!value) throw std::runtime_error("clipboard unavailable");
        const auto chars = size / sizeof(wchar_t);
        const auto end = std::find(value, value + chars, L'\0');
        if (end == value + chars) { GlobalUnlock(data); throw std::runtime_error("clipboard data is incomplete"); }
        const std::wstring_view text(value, static_cast<std::size_t>(end - value));
        const auto result = narrow(text);
        GlobalUnlock(data);
        if (result.size() > 1U * 1024U * 1024U) throw std::runtime_error("clipboard data exceeds bounded size");
        return {{"text", result}, {"chars", text.size()}};
    }
    if (capability == "clipboard.write") {
        if (!args.contains("text") || !args["text"].is_string()) throw std::runtime_error("invalid capability request");
        const auto& text = args["text"].get_ref<const std::string&>();
        if (text.size() > 1U * 1024U * 1024U) throw std::runtime_error("invalid capability request");
        const auto wide = widen(text);
        if (!OpenClipboard(nullptr)) throw std::runtime_error("clipboard unavailable");
        struct Close { ~Close() { CloseClipboard(); } } close;
        if (!EmptyClipboard()) throw std::runtime_error("clipboard write failed");
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
        if (!memory) throw std::runtime_error("clipboard write failed");
        auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
        if (!destination) { GlobalFree(memory); throw std::runtime_error("clipboard write failed"); }
        std::copy(wide.begin(), wide.end(), destination); destination[wide.size()] = L'\0'; GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) { GlobalFree(memory); throw std::runtime_error("clipboard write failed"); }
        return {{"written_chars", wide.size()}, {"sensitive_payload_redacted", true}};
    }
    if (capability == "window.list") {
        nlohmann::json windows = nlohmann::json::array(); bool truncated = false;
        std::pair<nlohmann::json*, bool*> state{&windows, &truncated};
        EnumWindows([](HWND window, LPARAM data) -> BOOL {
            auto& state = *reinterpret_cast<std::pair<nlohmann::json*, bool*>*>(data);
            if (!IsWindowVisible(window) || !is_chatgpt_window(window)) return TRUE;
            const int length = GetWindowTextLengthW(window);
            if (length <= 0 || length > 4096) return TRUE;
            if (state.first->size() >= 32) { *state.second = true; return FALSE; }
            wchar_t id[32]{}; swprintf_s(id, L"%p", static_cast<void*>(window));
            state.first->push_back({{"window_id", narrow(id)}, {"title", "ChatGPT"},
                                    {"active", GetForegroundWindow() == window}});
            return TRUE;
        }, reinterpret_cast<LPARAM>(&state));
        return {{"windows", windows}, {"windows_truncated", truncated}};
    }
    if (capability == "window.focus") {
        if (!args.contains("window_id") || !args["window_id"].is_string()) throw std::runtime_error("invalid capability request");
        const auto text = widen(args["window_id"].get<std::string>());
        wchar_t* end = nullptr; const auto value = wcstoull(text.c_str(), &end, 16);
        if (!end || *end != L'\0' || value == 0) throw std::runtime_error("invalid capability request");
        HWND hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
        if (!IsWindow(hwnd) || !is_chatgpt_window(hwnd) || !SetForegroundWindow(hwnd)) throw std::runtime_error("window focus failed");
        return {{"focused", true}};
    }
    if (capability == "shell.execute") return run_process(config_, args, cancelled);
    throw std::runtime_error("unknown capability");
}

std::string random_id() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("identity generation failed");
    wchar_t buffer[40]{};
    if (StringFromGUID2(id, buffer, 40) == 0) throw std::runtime_error("identity generation failed");
    std::wstring raw(buffer);
    raw.erase(std::remove(raw.begin(), raw.end(), L'{'), raw.end());
    raw.erase(std::remove(raw.begin(), raw.end(), L'}'), raw.end());
    raw.erase(std::remove(raw.begin(), raw.end(), L'-'), raw.end());
    std::transform(raw.begin(), raw.end(), raw.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return narrow(raw);
}

std::string client_id() {
    static const std::string id = [] {
        try {
            const auto path = Config::default_path().parent_path() / L"client-id";
            std::filesystem::create_directories(path.parent_path());
            std::ifstream in(path, std::ios::binary);
            std::string existing;
            if (in && std::getline(in, existing) && std::regex_match(existing, std::regex("^[a-f0-9]{32}$"))) return existing;
            const auto created = random_id();
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0; WriteFile(file, created.data(), static_cast<DWORD>(created.size()), &written, nullptr);
                WriteFile(file, "\n", 1, &written, nullptr); CloseHandle(file);
            }
            return created;
        } catch (...) { return random_id(); }
    }();
    return id;
}

} // namespace laso
