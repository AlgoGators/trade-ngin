#include "trade_ngin/apps/live_portfolio_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_fast.hpp"
#include "trade_ngin/strategy/trend_following_slow.hpp"

namespace trade_ngin {

namespace {
std::string report_date_string(const Timestamp& date) {
    const auto time = std::chrono::system_clock::to_time_t(date);
    std::tm tm{};
    core::safe_gmtime(&time, &tm);
    std::ostringstream text;
    text << std::put_time(&tm, "%Y-%m-%d");
    return text.str();
}
}

Result<void> seed_qt_report_positions(
    PostgresDatabase& db, const std::string& strategy_id,
    const std::vector<std::string>& strategy_names, const std::string& portfolio_id,
    const Timestamp& report_date) {
    const auto date = report_date_string(report_date);
    for (const auto& name : strategy_names) {
        auto result = db.seed_qt_positions_from_system(strategy_id, name, portfolio_id, date);
        if (result.is_error()) {
            const auto scope = nlohmann::json{{"portfolio_id", portfolio_id},
                {"strategy_id", strategy_id}, {"strategy_name", name},
                {"portfolio_type", "qt"}, {"report_date", date}}.dump();
            return make_error<void>(result.error()->code(),
                "Failed to seed QT report positions " + scope + ": " + result.error()->what(),
                "seed_qt_report_positions");
        }
    }
    return Result<void>();
}

Result<void> seed_qt_proposal_positions(
    PostgresDatabase& db, const std::string& strategy_id,
    const std::vector<std::string>& strategy_names, const std::string& portfolio_id,
    const Timestamp& report_date) {
    const auto date = report_date_string(report_date);
    if (strategy_names.empty()) {
        const auto scope = nlohmann::json{{"portfolio_id", portfolio_id},
            {"strategy_id", strategy_id}, {"report_date", date}}.dump();
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "No enabled components for QT proposal seed " + scope,
            "seed_qt_proposal_positions");
    }
    for (const auto& name : strategy_names) {
        auto result = db.seed_qt_proposal_positions_from_system(
            strategy_id, name, portfolio_id, date);
        if (result.is_error()) {
            const auto scope = nlohmann::json{{"portfolio_id", portfolio_id},
                {"strategy_id", strategy_id}, {"strategy_name", name},
                {"report_date", date}}.dump();
            return make_error<void>(result.error()->code(),
                "Failed to seed QT proposal positions " + scope + ": " + result.error()->what(),
                "seed_qt_proposal_positions");
        }
    }
    return Result<void>();
}

Result<ReportPositionSnapshot> load_qt_report_position_snapshot(
    PostgresDatabase& db, const std::string& strategy_id,
    const std::vector<std::string>& strategy_names, const std::string& portfolio_id,
    const Timestamp& report_date, const StrategyPositionRows& system_rows) {
    constexpr double kQuantityEpsilon = 1e-10;
    StrategyPositionRows by_strategy;
    std::unordered_map<std::string, Position> combined;
    std::unordered_map<std::string, size_t> evidence_counts;
    const auto scope = nlohmann::json{{"portfolio_id", portfolio_id},
        {"strategy_id", strategy_id}, {"strategy_names", strategy_names},
        {"portfolio_type", "qt"}, {"report_date", report_date_string(report_date)}}.dump();
    auto qt_result = db.load_report_positions_by_date(
        strategy_id, strategy_names, portfolio_id, report_date, "qt");
    if (qt_result.is_error()) {
        return make_error<ReportPositionSnapshot>(qt_result.error()->code(),
            "Failed to load QT report positions " + scope + ": " + qt_result.error()->what(),
            "load_qt_report_position_snapshot");
    }

    const std::unordered_map<std::string, Position> empty_rows;
    for (const auto& strategy_name : strategy_names) {
        const auto qt_it = qt_result.value().find(strategy_name);
        const auto& raw_qt_rows = qt_it == qt_result.value().end() ? empty_rows : qt_it->second;
        evidence_counts[strategy_name] = raw_qt_rows.size();
        auto system_it = system_rows.find(strategy_name);
        if (system_it != system_rows.end()) {
            for (const auto& [symbol, system_position] : system_it->second) {
                if (std::abs(system_position.quantity.as_double()) > kQuantityEpsilon &&
                    raw_qt_rows.find(symbol) == raw_qt_rows.end()) {
                    return make_error<ReportPositionSnapshot>(
                        ErrorCode::INVALID_DATA,
                        "Missing QT report position " + scope +
                            ", strategy " + strategy_name + ", symbol " + symbol,
                        "load_qt_report_position_snapshot");
                }
            }
        }

        auto& strategy_positions = by_strategy[strategy_name];
        for (const auto& [symbol, qt_position] : raw_qt_rows) {
            if (std::abs(qt_position.quantity.as_double()) <= kQuantityEpsilon) {
                continue;
            }

            strategy_positions[symbol] = qt_position;
            auto combined_it = combined.find(symbol);
            if (combined_it == combined.end()) {
                combined[symbol] = qt_position;
            } else {
                combined_it->second.quantity += qt_position.quantity;
            }
        }
    }

    return Result<ReportPositionSnapshot>(ReportPositionSnapshot{
        std::move(by_strategy), std::move(combined), portfolio_id, strategy_id,
        strategy_names, "qt", report_date, std::move(evidence_counts)});
}

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

bool runtime_control_enabled(const char* value) {
    return value != nullptr && std::string(value) == "true";
}

Result<nlohmann::json> build_runtime_trading_snapshot(const AppConfig& config) {
    nlohmann::json snapshot = {
        {"snapshot_version", 1}, {"portfolio_id", config.portfolio_id},
        {"initial_capital", config.initial_capital},
        {"reserve_capital_pct", config.reserve_capital_pct},
        {"benchmark_mode", config.benchmark_mode}, {"execution", config.execution.to_json()},
        {"optimization", config.opt_config.to_json()}, {"risk", config.risk_config.to_json()},
        {"max_drawdown", config.max_drawdown}, {"max_leverage", config.max_leverage},
        {"backtest", config.backtest.to_json()}, {"live", config.live.to_json()},
        {"strategy_defaults", config.strategy_defaults.to_json()},
        {"strategies", config.strategies_config}};
    const auto has_secret_key = [](const auto& self, const nlohmann::json& value) -> bool {
        if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                std::string key = it.key();
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (key.find("password") != std::string::npos ||
                    key.find("secret") != std::string::npos ||
                    key.find("token") != std::string::npos ||
                    key.find("credential") != std::string::npos || key.rfind("smtp", 0) == 0 ||
                    self(self, it.value())) return true;
            }
        } else if (value.is_array()) {
            for (const auto& entry : value) if (self(self, entry)) return true;
        }
        return false;
    };
    if (has_secret_key(has_secret_key, snapshot))
        return make_error<nlohmann::json>(ErrorCode::INVALID_ARGUMENT,
                                         "runtime_snapshot_contains_secret_key");
    return snapshot;
}

namespace {
Result<StrategySelection> select_enabled_live_strategies_inner(
    const nlohmann::json& strategies_config, SelectionConsumption* observation);
}

Result<StrategySelection> select_controlled_live_strategies(
    const nlohmann::json& config, SelectionConsumption* observation) {
    if (observation) *observation = {};
    try {
        if (!config.is_object())
            return make_error<StrategySelection>(ErrorCode::INVALID_ARGUMENT, "runtime_config_invalid");
        double sum = 0.0;
        for (const auto& [name, definition] : config.items()) {
            SelectionRead* read = nullptr;
            if (observation) {
                observation->controlled_validation.push_back({});
                read = &observation->controlled_validation.back();
                read->name = name;
                read->enabled_live_read = true;
            }
            const bool enabled = definition.value("enabled_live", false);
            if (read) {
                read->enabled_live_present = definition.contains("enabled_live");
                read->enabled_live_defaulted = !*read->enabled_live_present;
                read->enabled_live_value = enabled;
            }
            if (!enabled) continue;
            if (read) read->allocation_read = true;
            double weight = definition.at("default_allocation").get<double>();
            if (read) {
                read->allocation_defaulted = false;
                read->allocation_value = weight;
            }
            if (!std::isfinite(weight) || weight <= 0.0 || weight > 1.0)
                return make_error<StrategySelection>(ErrorCode::INVALID_ARGUMENT, "runtime_allocation_unsupported");
            sum += weight;
            if (observation) observation->controlled_sum = sum;
        }
        if (std::abs(sum - 1.0) > 1e-9)
            return make_error<StrategySelection>(ErrorCode::INVALID_ARGUMENT, "runtime_allocation_unsupported");
        auto result = select_enabled_live_strategies_inner(config, observation);
        if (result.is_ok()) {
            auto selection = result.value();
            // Preserve exact configured weights instead of any floating-point normalization.
            for (auto& [name, weight] : selection.allocations) {
                weight = config.at(name).at("default_allocation").get<double>();
                if (observation) {
                    for (auto& read : observation->controlled_validation) {
                        if (read.name == name) {
                            read.effective_allocation = weight;
                            break;
                        }
                    }
                }
            }
            return selection;
        }
        return result;
    } catch (const std::exception&) {
        return make_error<StrategySelection>(ErrorCode::INVALID_ARGUMENT, "runtime_config_invalid");
    }
}

namespace {
Result<StrategySelection> select_enabled_live_strategies_inner(
    const nlohmann::json& strategies_config, SelectionConsumption* observation) {
    if (strategies_config.is_null() || !strategies_config.is_object()) {
        return make_error<StrategySelection>(ErrorCode::INVALID_ARGUMENT,
                                             "No strategies section found in loaded configuration",
                                             "select_enabled_live_strategies");
    }

    StrategySelection selection;
    for (const auto& [strategy_id, strategy_def] : strategies_config.items()) {
        SelectionRead* read = nullptr;
        if (observation) {
            observation->ordinary_selection.push_back({});
            read = &observation->ordinary_selection.back();
            read->name = strategy_id;
        }
        // Use enabled_live flag for live portfolio (mirrors enabled_backtest's
        // role for backtests -- the same config file drives both).
        const bool enabled_present = strategy_def.contains("enabled_live");
        if (read) read->enabled_live_present = enabled_present;
        bool enabled = false;
        if (enabled_present) {
            if (read) read->enabled_live_read = true;
            enabled = strategy_def["enabled_live"].get<bool>();
            if (read) read->enabled_live_value = enabled;
        }
        if (enabled_present && enabled) {
            if (read) read->allocation_read = true;
            double default_allocation = strategy_def.value("default_allocation", 0.5);
            if (read) {
                read->allocation_defaulted = !strategy_def.contains("default_allocation");
                read->allocation_value = default_allocation;
            }
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
        if (observation) observation->ordinary_sum = selection.allocation_sum_before_normalization;
    }
    if (observation) observation->ordinary_normalized =
        selection.allocation_sum_before_normalization > 0.0;
    if (selection.allocation_sum_before_normalization > 0.0) {
        for (auto& [_, alloc] : selection.allocations) {
            alloc /= selection.allocation_sum_before_normalization;
        }
    }

    // Sort strategy names for deterministic combined ID (Tier 2).
    std::sort(selection.names.begin(), selection.names.end());

    INFO("Total strategies enabled: " + std::to_string(selection.names.size()));
    for (const auto& [name, alloc] : selection.allocations) {
        if (observation) {
            for (auto& read : observation->ordinary_selection) {
                if (read.name == name) {
                    read.effective_allocation = alloc;
                    break;
                }
            }
        }
        INFO("Strategy " + name + " normalized allocation: " + std::to_string(alloc * 100.0) + "%");
    }

    return Result<StrategySelection>(selection);
}
}

Result<StrategySelection> select_enabled_live_strategies(
    const nlohmann::json& strategies_config, SelectionConsumption* observation) {
    if (observation) *observation = {};
    return select_enabled_live_strategies_inner(strategies_config, observation);
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

FactoryTrendConfig resolve_factory_trend_config(
    const std::string& strategy_type, const nlohmann::json& strategy_def,
    const StrategyDefaultsConfig& strategy_defaults,
    std::optional<double> slow_max_symbol_concentration_override) {
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
        return trend_config;
    }
    if (strategy_type == "TrendFollowingFastStrategy") {
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
        return trend_config;
    }
    if (strategy_type == "TrendFollowingSlowStrategy") {
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
        return trend_config;
    }
    return std::monostate{};
}

namespace {
constexpr std::size_t kInspectionMaxBytes = 2u * 1024u * 1024u;

nlohmann::json unavailable_inspection_capture(const char* reason) {
    return {{"status", "unavailable"}, {"reason", reason},
            {"supplied", nullptr}, {"selected_trend", nullptr}};
}

bool inspection_strategy_id(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char ch) {
            return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                   (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        });
}

template <typename Config>
nlohmann::json inspection_trend_members(const Config& config) {
    const double scalars[] = {config.weight, config.risk_target, config.fx_rate,
        config.idm, config.max_symbol_concentration, config.carver_buffer_floor,
        config.carver_buffer_position_factor};
    for (double value : scalars) {
        if (!std::isfinite(value)) throw std::invalid_argument("inspection_stage_unavailable");
    }
    nlohmann::json ema = nlohmann::json::array();
    for (const auto& [short_window, long_window] : config.ema_windows)
        ema.push_back(nlohmann::json::array({short_window, long_window}));
    nlohmann::json fdm = nlohmann::json::array();
    for (const auto& [count, multiplier] : config.fdm) {
        if (!std::isfinite(multiplier))
            throw std::invalid_argument("inspection_stage_unavailable");
        fdm.push_back(nlohmann::json::array({count, multiplier}));
    }
    static_assert(std::numeric_limits<std::size_t>::digits <= 64);
    return {{"weight", config.weight}, {"risk_target", config.risk_target},
            {"fx_rate", config.fx_rate}, {"idm", config.idm},
            {"max_symbol_concentration", config.max_symbol_concentration},
            {"use_position_buffering", config.use_position_buffering},
            {"carver_buffer_floor", config.carver_buffer_floor},
            {"carver_buffer_position_factor", config.carver_buffer_position_factor},
            {"ema_windows", std::move(ema)},
            {"vol_lookback_short", config.vol_lookback_short},
            {"vol_lookback_long", config.vol_lookback_long},
            {"max_history_size", static_cast<std::uint64_t>(config.max_history_size)},
            {"fdm", std::move(fdm)}};
}

template <typename Config>
nlohmann::json inspection_trend_stages(const Config& factory) {
    const int short_window = factory.vol_lookback_short > 0 ?
        factory.vol_lookback_short :
        (std::is_same_v<Config, TrendFollowingConfig> ? 22 :
         std::is_same_v<Config, TrendFollowingFastConfig> ? 16 : 64);
    if (factory.vol_lookback_long <= short_window &&
        short_window > std::numeric_limits<int>::max() / 4)
        throw std::invalid_argument("inspection_stage_unavailable");
    auto normalized = factory;
    normalize_constructor_trend_config(normalized);
    return {{"factory_resolved", inspection_trend_members(factory)},
            {"constructor_normalized", inspection_trend_members(normalized)}};
}
}

nlohmann::json build_live_config_inspection_capture(
    const AppConfig& config, const StrategySelection& selection,
    std::optional<double> slow_max_symbol_concentration_override) {
    const auto supplied = project_live_config_fields(config);
    if (supplied.is_error()) return unavailable_inspection_capture("projection_invalid");
    try {
        if (slow_max_symbol_concentration_override &&
            !std::isfinite(*slow_max_symbol_concentration_override))
            return unavailable_inspection_capture("selected_stage_unavailable");
        if (selection.names.size() != selection.allocations.size() ||
            selection.names.size() != selection.configs.size())
            return unavailable_inspection_capture("selected_stage_unavailable");
        std::set<std::string> seen;
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& id : selection.names) {
            if (!inspection_strategy_id(id) || !seen.insert(id).second)
                return unavailable_inspection_capture("selected_stage_unavailable");
            const auto allocation = selection.allocations.at(id);
            const auto& definition = selection.configs.at(id);
            if (!std::isfinite(allocation) || !config.strategies_config.contains(id) ||
                definition != config.strategies_config.at(id) || !definition.is_object())
                return unavailable_inspection_capture("selected_stage_unavailable");
            const std::string type = definition.value("type", "TrendFollowingStrategy");
            const auto factory = resolve_factory_trend_config(type, definition,
                config.strategy_defaults, slow_max_symbol_concentration_override);
            if (std::holds_alternative<std::monostate>(factory))
                return unavailable_inspection_capture("selected_stage_unavailable");
            nlohmann::json stages;
            std::visit([&](const auto& typed) {
                using T = std::decay_t<decltype(typed)>;
                if constexpr (!std::is_same_v<T, std::monostate>)
                    stages = inspection_trend_stages(typed);
            }, factory);
            rows.push_back({{"strategy_id", id}, {"strategy_type", type},
                            {"selected_allocation", allocation},
                            {"factory_resolved", std::move(stages["factory_resolved"])},
                            {"constructor_normalized", std::move(stages["constructor_normalized"])}});
        }
        nlohmann::json override = {{"state", "absent"}};
        if (slow_max_symbol_concentration_override)
            override = {{"state", "present"},
                        {"value", *slow_max_symbol_concentration_override}};
        nlohmann::json result = {
            {"status", "available"}, {"reason", "none"}, {"supplied", supplied.value()},
            {"selected_trend", {{"schema_version", 1},
                 {"provenance", "shared_resolver_same_inputs"},
                 {"slow_concentration_override", std::move(override)},
                 {"strategies", std::move(rows)}}}};
        if (result.dump().size() > kInspectionMaxBytes)
            return unavailable_inspection_capture("capture_failed");
        return result;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        return unavailable_inspection_capture("selected_stage_unavailable");
    }
}

std::vector<std::shared_ptr<StrategyInterface>> build_strategy_instances(
    const StrategySelection& selection, const StrategyConfig& base_strategy_config,
    double initial_capital, const StrategyDefaultsConfig& strategy_defaults,
    std::optional<double> slow_max_symbol_concentration_override,
    std::shared_ptr<PostgresDatabase> db, std::shared_ptr<InstrumentRegistry> registry_ptr,
    FactoryConsumption* observation) {
    if (observation) *observation = {};
    std::vector<std::shared_ptr<StrategyInterface>> strategies;

    for (const auto& strategy_name : selection.names) {
        FactoryRead* read = nullptr;
        if (observation) {
            observation->entries.push_back({});
            read = &observation->entries.back();
            read->name = strategy_name;
        }
        const auto& strategy_def = selection.configs.at(strategy_name);
        std::string strategy_type = strategy_def.value("type", "TrendFollowingStrategy");
        if (read) read->type_defaulted = !strategy_def.contains("type");
        double allocation = selection.allocations.at(strategy_name);
        if (read) {
            read->effective_allocation = allocation;
            read->initial_capital_argument = initial_capital;
        }

        StrategyConfig strategy_config = base_strategy_config;
        strategy_config.capital_allocation = initial_capital * allocation;
        if (read) read->allocated_capital = strategy_config.capital_allocation;

        INFO("Creating strategy: " + strategy_name + " (type: " + strategy_type +
             ", allocation: " + std::to_string(allocation * 100.0) + "%)");

        std::shared_ptr<StrategyInterface> strategy;
        auto factory_config = resolve_factory_trend_config(
            strategy_type, strategy_def, strategy_defaults,
            slow_max_symbol_concentration_override);
        if (read) {
            if (std::holds_alternative<TrendFollowingConfig>(factory_config))
                read->profile = FactoryProfile::Standard;
            else if (std::holds_alternative<TrendFollowingFastConfig>(factory_config))
                read->profile = FactoryProfile::Fast;
            else if (std::holds_alternative<TrendFollowingSlowConfig>(factory_config))
                read->profile = FactoryProfile::Slow;
            else
                read->profile = FactoryProfile::Unsupported;
        }

        if (auto* trend_config = std::get_if<TrendFollowingConfig>(&factory_config)) {
            if (read) read->construction = SetupStage::Attempted;
            strategy = std::make_shared<TrendFollowingStrategy>(strategy_name, strategy_config,
                                                                 *trend_config, db, registry_ptr);
            if (read) read->construction = SetupStage::Succeeded;

        } else if (auto* trend_config = std::get_if<TrendFollowingFastConfig>(&factory_config)) {
            if (read) read->construction = SetupStage::Attempted;
            strategy = std::make_shared<TrendFollowingFastStrategy>(strategy_name, strategy_config,
                                                                     *trend_config, db, registry_ptr);
            if (read) read->construction = SetupStage::Succeeded;

        } else if (auto* trend_config = std::get_if<TrendFollowingSlowConfig>(&factory_config)) {
            if (read) read->construction = SetupStage::Attempted;
            strategy = std::make_shared<TrendFollowingSlowStrategy>(strategy_name, strategy_config,
                                                                     *trend_config, db, registry_ptr);
            if (read) read->construction = SetupStage::Succeeded;

        } else {
            ERROR("Unknown strategy type: " + strategy_type + " for strategy: " + strategy_name);
            throw std::runtime_error("Unknown strategy type: " + strategy_type +
                                     " for strategy: " + strategy_name);
        }

        if (read) read->initialize = SetupStage::Attempted;
        auto init_result = strategy->initialize();
        if (init_result.is_error()) {
            if (read) {
                read->initialize = SetupStage::Failed;
                read->initialize_error = init_result.error()->code();
            }
            ERROR("Failed to initialize strategy " + strategy_name + ": " +
                  init_result.error()->what());
            throw std::runtime_error("Failed to initialize strategy " + strategy_name + ": " +
                                     std::string(init_result.error()->what()));
        }
        if (read) read->initialize = SetupStage::Succeeded;
        INFO("Strategy " + strategy_name + " initialization successful");

        if (read) read->start = SetupStage::Attempted;
        auto start_result = strategy->start();
        if (start_result.is_error()) {
            if (read) {
                read->start = SetupStage::Failed;
                read->start_error = start_result.error()->code();
            }
            ERROR("Failed to start strategy " + strategy_name + ": " +
                  start_result.error()->what());
            throw std::runtime_error("Failed to start strategy " + strategy_name + ": " +
                                     std::string(start_result.error()->what()));
        }
        if (read) read->start = SetupStage::Succeeded;
        INFO("Strategy " + strategy_name + " started successfully");

        strategies.push_back(strategy);
    }

    return strategies;
}

std::vector<RunInputsShaBatch> group_into_sha_batches(
    const std::vector<std::pair<std::string, std::string>>& date_sha_pairs) {
    std::vector<RunInputsShaBatch> batches;
    for (const auto& [date, sha] : date_sha_pairs) {
        if (!batches.empty() && batches.back().sha == sha) {
            batches.back().through_date = date;
        } else {
            batches.push_back({sha, date, date});
        }
    }
    return batches;
}

}  // namespace trade_ngin
