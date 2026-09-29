#include "laso/config.hpp"
#include "laso/platform.hpp"
#include "laso/protocol.hpp"
#include "laso/audit.hpp"
#include "laso/playwright.hpp"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void usage() {
    std::cerr << "LASO-Computer (Windows native worker)\n"
              << "  laso-computer --status | --capabilities | --init-config | --check-config [--config PATH]\n"
              << "  laso-computer [--config PATH]  # worker protocol v1 on stdin/stdout\n";
}

std::wstring get_arg(int argc, wchar_t** argv, int& i) {
    if (i + 1 >= argc) throw std::runtime_error("--config requires a path");
    return argv[++i];
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        std::filesystem::path config_path = laso::Config::default_path();
        std::wstring command;
        for (int i = 1; i < argc; ++i) {
            if (std::wstring_view(argv[i]) == L"--config") config_path = get_arg(argc, argv, i);
            else if (std::wstring_view(argv[i]) == L"--status" || std::wstring_view(argv[i]) == L"--capabilities" ||
                     std::wstring_view(argv[i]) == L"--init-config" || std::wstring_view(argv[i]) == L"--check-config") {
                if (!command.empty()) { usage(); return 2; }
                command = argv[i];
            } else { usage(); return 2; }
        }
        if (command == L"--init-config") {
            laso::Config{}.save_example(config_path);
            std::wcout << L"Created deny-by-default config at " << config_path.wstring() << L"\n";
            return 0;
        }
        const auto config = laso::Config::load(config_path);
        std::string validation_error;
        if (!config.validate(validation_error)) {
            std::cerr << "configuration invalid: " << validation_error << '\n';
            return 2;
        }
        auto platform = std::make_shared<laso::WindowsPlatform>(config);
        if (command == L"--check-config") {
            std::cout << "configuration_valid: true\n"
                      << "default_decision: " << laso::to_string(config.default_decision) << '\n'
                      << "enabled_capabilities:\n";
            for (const auto& capability : config.capabilities) {
                if (capability.second != laso::Decision::deny)
                    std::cout << "  " << capability.first << ": " << laso::to_string(capability.second) << '\n';
            }
            std::cout << "process_allowlist_count: " << config.allowed_executables.size() << '\n';
            std::cout << "playwright_configured: " << (config.playwright.enabled ? "true" : "false") << '\n';
            if (config.playwright.enabled) {
                std::wcout << L"  node_executable: " << config.playwright.node_executable << L"\n"
                           << L"  server_entry: " << config.playwright.server_entry << L"\n"
                           << L"  browser_executable: " << config.playwright.browser_executable << L"\n";
            }
            if (std::any_of(config.capabilities.begin(), config.capabilities.end(), [](const auto& item) { return item.second == laso::Decision::allow; }))
                std::cout << "warning: one or more capabilities are explicitly allowed\n";
            if (config.allowed_executables.empty()) std::cout << "warning: shell.execute is unavailable without an executable allowlist\n";
            return 0;
        }
        if (command == L"--status" || command == L"--capabilities") {
            for (const auto& name : laso::capability_names()) {
                std::cout << name << " available=" << (platform->available(name) ? "true" : "false")
                          << " decision=" << laso::to_string(config.decision_for(name)) << '\n';
            }
            std::cout << "client_id=" << laso::client_id() << "\n";
            return 0;
        }
        laso::CapabilityRegistry registry;
        registry.register_provider(platform);
        if (config.playwright.enabled) {
            auto playwright = std::make_shared<laso::PlaywrightMcpProvider>(config.playwright);
            if (!playwright->health()) std::cerr << "playwright plugin unavailable: " << playwright->status_detail() << '\n';
            registry.register_provider(std::move(playwright));
        }
        laso::WorkerProtocol worker(config, std::move(registry), laso::AuditLog{});
        return worker.serve(std::cin, std::cout, std::cerr);
    } catch (const std::exception& error) {
        std::cerr << "laso-computer: " << error.what() << '\n';
        return 2;
    }
}
