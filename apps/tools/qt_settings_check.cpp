// apps/tools/qt_settings_check.cpp
//
// HD D-17: would a live run accept these desk settings? The desk service runs this before
// AlgoLens saves a strategy_config version, so a change the engine refuses is never saved (and
// never stops the desk run, the 09:30 send and the 10:00 fallback until reverted).
//
//   qt_settings_check --portfolio-config <dir under config/portfolios> --overrides-file <json>
//
// It reads the files under ./config like the live runners, applies the overrides as the active
// row would be applied (check_live_settings_overlay), touches no database and books nothing.
// Prints one JSON line: {"ok": true} or {"ok": false, "error": "<the reason a run would give>"}.
// Exit 0 when accepted, 3 when refused, 4 when the config files themselves do not load, 2 on
// bad arguments or an unreadable overrides file.

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/live/live_settings.hpp"

namespace {

int usage(const std::string& why) {
    std::cout << nlohmann::json{{"ok", false}, {"error", why}}.dump() << std::endl;
    std::cerr << "usage: qt_settings_check --portfolio-config <dir> --overrides-file <file>"
              << std::endl;
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string portfolio_dir;
    std::string overrides_file;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--portfolio-config" || arg == "--overrides-file") && i + 1 < argc) {
            (arg == "--portfolio-config" ? portfolio_dir : overrides_file) = argv[++i];
        } else {
            return usage("unknown argument " + arg);
        }
    }
    if (portfolio_dir.empty() || overrides_file.empty()) {
        return usage("--portfolio-config and --overrides-file are required");
    }
    if (portfolio_dir.find('/') != std::string::npos || portfolio_dir.find("..") != std::string::npos) {
        return usage("--portfolio-config names a directory under config/portfolios");
    }
    std::ifstream in(overrides_file);
    if (!in) return usage("cannot read " + overrides_file);
    std::stringstream text;
    text << in.rdbuf();
    nlohmann::json overrides;
    try {
        overrides = nlohmann::json::parse(text.str());
    } catch (const std::exception& exc) {
        return usage(std::string("the overrides are not JSON: ") + exc.what());
    }
    // The files themselves must load: if they do not, the engine is misconfigured and the change
    // cannot be judged (exit 4, which the desk service reports as unavailable), not refused.
    if (auto files = trade_ngin::ConfigLoader::load("./config", portfolio_dir); files.is_error()) {
        std::cout << nlohmann::json{{"ok", false},
                                    {"error", std::string("the config files do not load: ") +
                                                  files.error()->what()}}
                         .dump()
                  << std::endl;
        return 4;
    }
    auto checked = trade_ngin::check_live_settings_overlay("./config", portfolio_dir, overrides);
    if (checked.is_error()) {
        std::cout << nlohmann::json{{"ok", false}, {"error", checked.error()->what()}}.dump()
                  << std::endl;
        return 3;
    }
    std::cout << nlohmann::json{{"ok", true}}.dump() << std::endl;
    return 0;
}
