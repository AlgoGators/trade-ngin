#include "trade_ngin/apps/live_portfolio_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <stdexcept>

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_fast.hpp"
#include "trade_ngin/strategy/trend_following_slow.hpp"

namespace trade_ngin {

std::unordered_map<std::string, Bar> latest_bar_by_symbol(const std::vector<Bar>& all_bars) {
    std::unordered_map<std::string, Bar> latest;
    for (const auto& bar : all_bars) {
        latest[bar.symbol] = bar;  // last occurrence per symbol wins (chronological order)
    }
    return latest;
}

double compute_mark_to_market_equity(
    const std::unordered_map<std::string, Position>& positions,
    const std::unordered_map<std::string, Bar>& latest_bars) {
    double total_equity = 0.0;
    for (const auto& [sym, position] : positions) {
        auto bars_it = latest_bars.find(sym);
        if (bars_it != latest_bars.end()) {
            double close_price = bars_it->second.close.as_double();
            double qty = position.quantity.as_double();
            total_equity += qty * close_price;
        }
    }
    return total_equity;
}

std::string hash_bars(const std::vector<Bar>& bars) {
    std::hash<std::string> hasher;
    size_t running_hash = 0;
    for (const auto& bar : bars) {
        std::ostringstream bar_repr;
        bar_repr << bar.symbol << std::chrono::system_clock::to_time_t(bar.timestamp)
                  << bar.close.as_double();
        running_hash ^=
            hasher(bar_repr.str()) + 0x9e3779b9 + (running_hash << 6) + (running_hash >> 2);
    }
    std::ostringstream hash_hex;
    hash_hex << std::hex << running_hash;
    return hash_hex.str();
}

nlohmann::json build_run_inputs_row(const std::string& trade_ngin_sha,
                                     const nlohmann::json& config_snapshot,
                                     const std::vector<std::string>& universe,
                                     const std::vector<Bar>& all_bars,
                                     const std::string& benchmark_mode,
                                     std::optional<Timestamp> start_date,
                                     std::optional<Timestamp> end_date) {
    nlohmann::json row;
    row["trade_ngin_sha"] = trade_ngin_sha;
    row["config_snapshot"] = config_snapshot;
    row["universe"] = universe;

    nlohmann::json data_window;
    data_window["schema"] = "trading";
    data_window["table"] = "bar";
    data_window["start"] =
        start_date.has_value() ? std::chrono::system_clock::to_time_t(*start_date) : 0;
    data_window["end"] =
        end_date.has_value() ? std::chrono::system_clock::to_time_t(*end_date) : 0;
    data_window["row_count"] = all_bars.size();
    data_window["content_hash"] = hash_bars(all_bars);
    row["data_window"] = data_window;

    row["risk_limits_id"] = nlohmann::json::value_t::null;

    nlohmann::json engine_flags;
    engine_flags["benchmark_mode"] = benchmark_mode;
    engine_flags["rng_seed"] = nlohmann::json::value_t::null;
    row["engine_flags"] = engine_flags;

    return row;
}

Result<StrategySelection> select_enabled_live_strategies(const nlohmann::json& strategies_config) {
    if (strategies_config.is_null() || !strategies_config.is_object()) {
        return make_error<StrategySelection>(ErrorCode::INVALID_ARGUMENT,
                                             "No strategies section found in loaded configuration",
                                             "select_enabled_live_strategies");
    }

    StrategySelection selection;
    for (const auto& [strategy_id, strategy_def] : strategies_config.items()) {
        // Use enabled_live flag for live portfolio (mirrors enabled_backtest's
        // role for backtests -- the same config file drives both).
        if (strategy_def.contains("enabled_live") && strategy_def["enabled_live"].get<bool>()) {
            double default_allocation = strategy_def.value("default_allocation", 0.5);
            selection.allocations[strategy_id] = default_allocation;
            selection.configs[strategy_id] = strategy_def;
            selection.names.push_back(strategy_id);
            INFO("Loaded strategy: " + strategy_id +
                 " with allocation: " + std::to_string(default_allocation * 100.0) + "%");
        }
    }

    if (selection.names.empty()) {
        ERROR("No enabled_live strategies found in loaded configuration");
        return make_error<StrategySelection>(ErrorCode::INVALID_ARGUMENT,
                                             "No enabled_live strategies found in loaded configuration",
                                             "select_enabled_live_strategies");
    }

    // Normalize allocations to sum to 1.0. If configured allocations sum
    // to <1.0 (e.g. 0.6 + 0.3, expecting 10% idle), this loop silently
    // rescales -- partial deployment is not supported. The caller is
    // expected to WARN using allocation_sum_before_normalization when it
    // is not ~1.0.
    for (const auto& [_, alloc] : selection.allocations) {
        selection.allocation_sum_before_normalization += alloc;
    }
    if (selection.allocation_sum_before_normalization > 0.0) {
        for (auto& [_, alloc] : selection.allocations) {
            alloc /= selection.allocation_sum_before_normalization;
        }
    }

    // Sort strategy names for deterministic combined ID (Tier 2).
    std::sort(selection.names.begin(), selection.names.end());

    INFO("Total strategies enabled: " + std::to_string(selection.names.size()));
    for (const auto& [name, alloc] : selection.allocations) {
        INFO("Strategy " + name + " normalized allocation: " + std::to_string(alloc * 100.0) + "%");
    }

    return Result<StrategySelection>(selection);
}

std::string build_combined_strategy_id(const std::vector<std::string>& sorted_strategy_names) {
    // Generate combined strategy_id: LIVE_<sorted_names_joined_by_&> (Tier 2).
    // Callers pass selection.names, which select_enabled_live_strategies
    // already returns sorted -- kept as a separate function (rather than
    // folded into selection) because benchmark_replay derives this same id
    // from a run_inputs row's recorded universe/config, not from a fresh
    // selection call.
    std::string combined_strategy_id = "LIVE_";
    for (size_t i = 0; i < sorted_strategy_names.size(); ++i) {
        if (i > 0)
            combined_strategy_id += "_";
        combined_strategy_id += sorted_strategy_names[i];
    }
    return combined_strategy_id;
}

std::vector<std::shared_ptr<StrategyInterface>> build_strategy_instances(
    const StrategySelection& selection, const StrategyConfig& base_strategy_config,
    double initial_capital, const StrategyDefaultsConfig& strategy_defaults,
    std::optional<double> slow_max_symbol_concentration_override,
    std::shared_ptr<PostgresDatabase> db, std::shared_ptr<InstrumentRegistry> registry_ptr) {
    std::vector<std::shared_ptr<StrategyInterface>> strategies;

    for (const auto& strategy_name : selection.names) {
        const auto& strategy_def = selection.configs.at(strategy_name);
        std::string strategy_type = strategy_def.value("type", "TrendFollowingStrategy");
        double allocation = selection.allocations.at(strategy_name);

        StrategyConfig strategy_config = base_strategy_config;
        strategy_config.capital_allocation = initial_capital * allocation;

        INFO("Creating strategy: " + strategy_name + " (type: " + strategy_type +
             ", allocation: " + std::to_string(allocation * 100.0) + "%)");

        std::shared_ptr<StrategyInterface> strategy;

        if (strategy_type == "TrendFollowingStrategy") {
            TrendFollowingConfig trend_config;
            if (strategy_def.contains("config")) {
                const auto& cfg = strategy_def["config"];
                trend_config.weight = cfg.value("weight", 0.03);
                trend_config.risk_target = cfg.value("risk_target", 0.2);
                trend_config.idm = cfg.value("idm", 2.5);
                trend_config.max_symbol_concentration = cfg.value("max_symbol_concentration", 0.15);
                trend_config.use_position_buffering = cfg.value("use_position_buffering", true);
                trend_config.carver_buffer_floor =
                    cfg.value("carver_buffer_floor", strategy_defaults.carver_buffer_floor);
                trend_config.carver_buffer_position_factor = cfg.value(
                    "carver_buffer_position_factor", strategy_defaults.carver_buffer_position_factor);
                if (cfg.contains("ema_windows")) {
                    trend_config.ema_windows.clear();
                    for (const auto& window : cfg["ema_windows"]) {
                        trend_config.ema_windows.push_back(
                            {window[0].get<int>(), window[1].get<int>()});
                    }
                }
                trend_config.vol_lookback_short = cfg.value("vol_lookback_short", 32);
                trend_config.vol_lookback_long = cfg.value("vol_lookback_long", 252);
            }
            if (trend_config.fdm.empty()) {
                trend_config.fdm = strategy_defaults.fdm;
            }

            strategy = std::make_shared<TrendFollowingStrategy>(strategy_name, strategy_config,
                                                                 trend_config, db, registry_ptr);

        } else if (strategy_type == "TrendFollowingFastStrategy") {
            TrendFollowingFastConfig trend_config;
            if (strategy_def.contains("config")) {
                const auto& cfg = strategy_def["config"];
                trend_config.weight = cfg.value("weight", 0.03);
                trend_config.risk_target = cfg.value("risk_target", 0.25);
                trend_config.idm = cfg.value("idm", 2.5);
                trend_config.max_symbol_concentration = cfg.value("max_symbol_concentration", 0.15);
                trend_config.use_position_buffering = cfg.value("use_position_buffering", false);
                trend_config.carver_buffer_floor =
                    cfg.value("carver_buffer_floor", strategy_defaults.carver_buffer_floor);
                trend_config.carver_buffer_position_factor = cfg.value(
                    "carver_buffer_position_factor", strategy_defaults.carver_buffer_position_factor);
                if (cfg.contains("ema_windows")) {
                    trend_config.ema_windows.clear();
                    for (const auto& window : cfg["ema_windows"]) {
                        trend_config.ema_windows.push_back(
                            {window[0].get<int>(), window[1].get<int>()});
                    }
                }
                trend_config.vol_lookback_short = cfg.value("vol_lookback_short", 16);
                trend_config.vol_lookback_long = cfg.value("vol_lookback_long", 252);
            }
            if (trend_config.fdm.empty()) {
                trend_config.fdm = strategy_defaults.fdm;
            }

            strategy = std::make_shared<TrendFollowingFastStrategy>(strategy_name, strategy_config,
                                                                     trend_config, db, registry_ptr);

        } else if (strategy_type == "TrendFollowingSlowStrategy") {
            TrendFollowingSlowConfig trend_config;
            if (strategy_def.contains("config")) {
                const auto& cfg = strategy_def["config"];
                trend_config.weight = cfg.value("weight", 0.03);
                trend_config.risk_target = cfg.value("risk_target", 0.15);
                trend_config.idm = cfg.value("idm", 2.5);
                trend_config.max_symbol_concentration = cfg.value("max_symbol_concentration", 0.15);
                trend_config.use_position_buffering = cfg.value("use_position_buffering", true);
                trend_config.carver_buffer_floor =
                    cfg.value("carver_buffer_floor", strategy_defaults.carver_buffer_floor);
                trend_config.carver_buffer_position_factor = cfg.value(
                    "carver_buffer_position_factor", strategy_defaults.carver_buffer_position_factor);
                if (cfg.contains("ema_windows")) {
                    trend_config.ema_windows.clear();
                    for (const auto& window : cfg["ema_windows"]) {
                        trend_config.ema_windows.push_back(
                            {window[0].get<int>(), window[1].get<int>()});
                    }
                }
                trend_config.vol_lookback_short = cfg.value("vol_lookback_short", 64);
                trend_config.vol_lookback_long = cfg.value("vol_lookback_long", 252);
            } else {
                trend_config.weight = 0.03;
                trend_config.risk_target = 0.15;
                // Only the base portfolio pinned this in the fallback branch; the
                // conservative one left the struct default in place. Preserved as a
                // parameter so neither portfolio's behaviour changes.
                if (slow_max_symbol_concentration_override.has_value()) {
                    trend_config.max_symbol_concentration = *slow_max_symbol_concentration_override;
                }
                trend_config.idm = 2.5;
                trend_config.use_position_buffering = true;
                trend_config.ema_windows = {{4, 16},   {8, 32},   {16, 64},
                                            {32, 128}, {64, 256}, {128, 512}};
                trend_config.vol_lookback_short = 64;
                trend_config.vol_lookback_long = 252;
            }
            if (trend_config.fdm.empty()) {
                trend_config.fdm = strategy_defaults.fdm;
            }

            strategy = std::make_shared<TrendFollowingSlowStrategy>(strategy_name, strategy_config,
                                                                     trend_config, db, registry_ptr);

        } else {
            ERROR("Unknown strategy type: " + strategy_type + " for strategy: " + strategy_name);
            throw std::runtime_error("Unknown strategy type: " + strategy_type +
                                     " for strategy: " + strategy_name);
        }

        auto init_result = strategy->initialize();
        if (init_result.is_error()) {
            ERROR("Failed to initialize strategy " + strategy_name + ": " +
                  init_result.error()->what());
            throw std::runtime_error("Failed to initialize strategy " + strategy_name + ": " +
                                     std::string(init_result.error()->what()));
        }
        INFO("Strategy " + strategy_name + " initialization successful");

        auto start_result = strategy->start();
        if (start_result.is_error()) {
            ERROR("Failed to start strategy " + strategy_name + ": " +
                  start_result.error()->what());
            throw std::runtime_error("Failed to start strategy " + strategy_name + ": " +
                                     std::string(start_result.error()->what()));
        }
        INFO("Strategy " + strategy_name + " started successfully");

        strategies.push_back(strategy);
    }

    return strategies;
}

}  // namespace trade_ngin
