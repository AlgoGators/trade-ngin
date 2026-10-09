// include/trade_ngin/live/live_settings.hpp
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/strategy_config_row.hpp"

namespace trade_ngin {

/**
 * @brief A live run's effective settings (QT plan E2, rulings 2, 3, 4 and 24).
 *
 * config         the file config with the portfolio's active strategy_config row merged in
 * settings_used  {"strategy_config_version": n or null, "config": the credential-free merged
 *                config}, written to trading.live_run_metadata.settings_used by the runner
 */
struct LiveSettings {
    AppConfig config;
    nlohmann::json settings_used;
    std::optional<int> strategy_config_version;
};

/**
 * @brief Resolve a LIVE run's settings from its config files and the result of the
 *        strategy_config lookup (PostgresDatabase::get_active_strategy_config).
 *
 * - lookup error: refused. A failed lookup never means "use the files" (ruling 24).
 * - no active row: the files unchanged, strategy_config_version null. Not a failure.
 * - an active row: merged over the files (ConfigLoader::load with the overlay); a row that
 *   cannot be applied is refused, never dropped (ruling 24).
 * - the row's portfolio must be the files' portfolio.
 *
 * Backtests never call this (ruling 4): they load their files with the two-argument
 * ConfigLoader::load and nothing else.
 */
Result<LiveSettings> resolve_live_settings(
    const std::filesystem::path& config_base_path, const std::string& portfolio_name,
    const std::string& portfolio_id,
    const Result<std::optional<StrategyConfigRow>>& active_row);

/// The one log line a runner writes when resolve_live_settings refuses, so the watchdog and a
/// reader of the log see the same words on every runner.
std::string live_settings_refusal_line(const std::string& portfolio_id, const std::string& why);

}  // namespace trade_ngin
