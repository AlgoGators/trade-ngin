#include "trade_ngin/regime_detection/runner_config.hpp"

#include "trade_ngin/core/config_loader.hpp"

namespace trade_ngin {
namespace regime {

namespace {

bool is_placeholder(const std::string& value) {
    return value.empty() || value.rfind("YOUR_", 0) == 0;
}

}  // namespace

std::vector<std::filesystem::path> default_runner_config_dirs() {
    return {"./config", "../config"};
}

Result<std::string> load_runner_db_connection(
    const std::vector<std::filesystem::path>& search_dirs) {
    std::string tried;
    for (const auto& dir : search_dirs) {
        if (!std::filesystem::exists(dir / "defaults.json")) {
            tried += " " + dir.string();
            continue;
        }
        auto cfg = ConfigLoader::load(dir, "base");
        if (cfg.is_error()) {
            return make_error<std::string>(cfg.error()->code(),
                                           "Config in " + dir.string() + " did not load: " +
                                               std::string(cfg.error()->what()),
                                           "RegimeRunnerConfig");
        }
        const auto& db = cfg.value().database;
        if (is_placeholder(db.host) || is_placeholder(db.username) || is_placeholder(db.name)) {
            return make_error<std::string>(ErrorCode::INVALID_ARGUMENT,
                                           "Database block in " + dir.string() +
                                               "/defaults.json is empty or still has the "
                                               "config_template placeholders",
                                           "RegimeRunnerConfig");
        }
        return Result<std::string>(db.get_connection_string());
    }
    return make_error<std::string>(ErrorCode::FILE_NOT_FOUND,
                                   "No defaults.json found in:" + tried +
                                       ". Copy config_template/ to config/ and fill in the "
                                       "database block",
                                   "RegimeRunnerConfig");
}

}  // namespace regime
}  // namespace trade_ngin
