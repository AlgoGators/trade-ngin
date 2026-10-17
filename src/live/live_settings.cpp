// src/live/live_settings.cpp
//
// QT plan E2: a live run's settings are its config files with the portfolio's active
// trading.strategy_config row merged in, and a record of exactly what was used.

#include "trade_ngin/live/live_settings.hpp"

#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

std::string live_settings_refusal_line(const std::string& portfolio_id, const std::string& why) {
    return "STRATEGY_CONFIG refused for " + portfolio_id + ": " + why +
           ". Refusing to run: a saved settings change that cannot be applied is never replaced "
           "by the file values (ruling 24).";
}

Result<LiveSettings> resolve_live_settings(
    const std::filesystem::path& config_base_path, const std::string& portfolio_name,
    const std::string& portfolio_id,
    const Result<std::optional<StrategyConfigRow>>& active_row) {
    if (active_row.is_error()) {
        return make_error<LiveSettings>(
            ErrorCode::DATABASE_ERROR,
            "the active strategy_config row could not be read: " +
                std::string(active_row.error()->what()),
            "LiveSettings");
    }
    const std::optional<StrategyConfigRow>& row = active_row.value();
    if (row && !row->portfolio_id.empty() && row->portfolio_id != portfolio_id) {
        return make_error<LiveSettings>(ErrorCode::INVALID_DATA,
                                        "the strategy_config row read is for " +
                                            row->portfolio_id + ", not " + portfolio_id,
                                        "LiveSettings");
    }

    nlohmann::json snapshot;
    auto loaded = ConfigLoader::load(config_base_path, portfolio_name,
                                     row ? &row->overrides : nullptr, &snapshot);
    if (loaded.is_error()) {
        return make_error<LiveSettings>(
            loaded.error()->code(),
            (row ? "strategy_config version " + std::to_string(row->version) + ": " : std::string()) +
                loaded.error()->what(),
            "LiveSettings");
    }
    if (loaded.value().portfolio_id != portfolio_id) {
        return make_error<LiveSettings>(ErrorCode::INVALID_DATA,
                                        "the config files now name portfolio " +
                                            loaded.value().portfolio_id + ", not " + portfolio_id,
                                        "LiveSettings");
    }

    LiveSettings out;
    out.config = loaded.value();
    if (row) out.strategy_config_version = row->version;
    out.settings_used = {
        {"strategy_config_version",
         row ? nlohmann::json(row->version) : nlohmann::json(nullptr)},
        {"config", std::move(snapshot)}};
    if (row) {
        INFO("STRATEGY_CONFIG: " + portfolio_id + " runs on its config files with strategy_config "
             "version " + std::to_string(row->version) + " (by " + row->created_by + ": " +
             row->reason + ") merged in");
    } else {
        INFO("STRATEGY_CONFIG: " + portfolio_id +
             " has no active strategy_config row; it runs on its config files unchanged");
    }
    return out;
}

Result<void> check_live_settings_overlay(const std::filesystem::path& config_base_path,
                                         const std::string& portfolio_name,
                                         const nlohmann::json& overrides) {
    auto files = ConfigLoader::load(config_base_path, portfolio_name);
    if (files.is_error()) {
        return make_error<void>(files.error()->code(),
                                std::string("the config files do not load: ") +
                                    files.error()->what(),
                                "LiveSettings");
    }
    const std::string portfolio_id = files.value().portfolio_id;
    StrategyConfigRow row;
    row.portfolio_id = portfolio_id;
    row.version = 0;
    row.overrides = overrides;
    row.reason = "checked before saving (D-17)";
    row.created_by = "qt_settings_check";
    const Result<std::optional<StrategyConfigRow>> active(
        std::optional<StrategyConfigRow>(std::move(row)));
    auto resolved = resolve_live_settings(config_base_path, portfolio_name, portfolio_id, active);
    if (resolved.is_error()) {
        std::string why = resolved.error()->what();
        const std::string prefix = "strategy_config version 0: ";
        if (why.rfind(prefix, 0) == 0) why = why.substr(prefix.size());
        return make_error<void>(resolved.error()->code(), why, "LiveSettings");
    }
    if (auto loop_keys = ConfigLoader::require_loop_keys(resolved.value().config);
        loop_keys.is_error()) {
        return make_error<void>(loop_keys.error()->code(), loop_keys.error()->what(),
                                "LiveSettings");
    }
    return Result<void>();
}

}  // namespace trade_ngin
