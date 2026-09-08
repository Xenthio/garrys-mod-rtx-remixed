#include "startup_assets.h"
#include <iostream>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) {
    try {
        std::filesystem::path root;
        std::optional<std::vector<std::filesystem::path>> sources;
        astra::startup_assets::WorkshopOptions workshop;
        for (int i = 1; i < argc; ++i) {
            const std::wstring option = argv[i];
            if (option == L"--game-root" && i + 1 < argc) root = argv[++i];
            else if (option == L"--steam-root" && i + 1 < argc) workshop.steamRoot = argv[++i];
            else if (option == L"--steam-user" && i + 1 < argc) {
                const std::wstring account = argv[++i];
                if (account.empty() || account.size() > 10 || account.find_first_not_of(L"0123456789") != std::wstring::npos)
                    throw std::runtime_error("--steam-user requires a decimal Steam account ID");
                std::string digits;
                for (const auto digit : account) digits += static_cast<char>(digit);
                workshop.accountId = std::move(digits);
            }
            else if (option == L"--no-workshop") workshop.enabled = false;
            else if (option == L"--source" && i + 1 < argc) {
                if (!sources) sources.emplace();
                sources->emplace_back(argv[++i]);
            } else if (option == L"--no-addons") {
                if (!sources) sources.emplace();
            } else throw std::runtime_error("Usage: astra_rtx_prepare_assets --game-root PATH [--source PATH ... | --no-addons] [--no-workshop] [--steam-root PATH --steam-user ACCOUNT_ID]");
        }
        if (root.empty()) throw std::runtime_error("--game-root is required");
        const auto report = astra::startup_assets::Prepare(root, sources, workshop);
        std::cout << report.dump(2) << '\n';
        return report.value("ready", false) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Astra startup preparation failed: " << error.what() << '\n';
        return 2;
    }
}
