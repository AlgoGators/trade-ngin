// Pure, testable helpers shared by the live-portfolio runner's benchmark
// pass, its replay-contract logging, and benchmark_replay (ADR-005).
// Extracted out of apps/strategies/live_portfolio_runner.cpp specifically
// so they can be unit-tested and reused: apps/ binaries aren't linked into
// trade_ngin_tests, but anything declared here and implemented against the
// trade_ngin library is -- and any construction logic here is guaranteed
// identical between the live path and a deferred replay, which is exactly
// what the ADR-005 7 parity gate needs to compare.
#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"

namespace trade_ngin {

class PostgresDatabase;
class InstrumentRegistry;

// Latest bar per symbol from a flat, chronologically-loaded bar vector
// (DataConversionUtils::arrow_table_to_bars' real return type). Built once
// by the caller and reused, rather than each consumer re-scanning the
// vector per position.
std::unordered_map<std::string, Bar> latest_bar_by_symbol(const std::vector<Bar>& all_bars);

// Mark-to-market equity: sum(quantity * latest_close) per position.
// Deliberately not derived from the fills/PnL machinery (ADR-005 D-4) --
// positions with no matching entry in latest_bars are silently skipped
// (same behavior as the original inline implementation this replaces).
double compute_mark_to_market_equity(
    const std::unordered_map<std::string, Position>& positions,
    const std::unordered_map<std::string, Bar>& latest_bars);

// Builds the replay-contract row for trading.run_inputs (ADR-005 5.2).
// content_hash is a non-cryptographic hash over every bar's
// (symbol, timestamp, close) -- sufficient to detect "the loaded bars
// changed" (e.g. a futures back-adjustment restatement) between when this
// row was recorded and a later replay; it does not need to be
// collision-resistant against an adversary.
// The exact hash build_run_inputs_row uses for data_window.content_hash,
// exposed so a replay tool can recompute it against freshly-loaded bars and
// compare, per ADR-005 5.3 step 2b (a mismatch means a data restatement --
// e.g. a futures back-adjustment -- happened between recording and replay).
std::string hash_bars(const std::vector<Bar>& bars);

// start_date/end_date populate data_window.start/end (epoch seconds) when
// given; omitted, they stay 0 -- the placeholder value earlier callers
// wrote (data_window.content_hash remains the load-bearing field either
// way, per ADR-005 5.2). Optional and defaulted so existing callers/tests
// that only cared about content_hash keep compiling unchanged.
nlohmann::json build_run_inputs_row(
    const std::string& trade_ngin_sha, const nlohmann::json& config_snapshot,
    const std::vector<std::string>& universe, const std::vector<Bar>& all_bars,
    const std::string& benchmark_mode, std::optional<Timestamp> start_date = std::nullopt,
    std::optional<Timestamp> end_date = std::nullopt);

// The enabled_live subset of a strategies_config JSON object, with
// allocations normalized to sum to 1.0 and names sorted for the
// deterministic Tier-2 combined-id convention (LIVE_<sorted names>).
struct StrategySelection {
    std::vector<std::string> names;
    std::unordered_map<std::string, double> allocations;
    std::unordered_map<std::string, nlohmann::json> configs;
    // Sum of default_allocation values before normalization, so a caller
    // can reproduce the original "allocations don't sum to 1.0" WARN.
    double allocation_sum_before_normalization{0.0};
};

// Selects strategies with enabled_live=true from a strategies_config JSON
// object -- either AppConfig::strategies_config (the live path) or the
// "strategies" key of a run_inputs.config_snapshot (a replay). Pure JSON
// parsing: a replay reconstructs the exact same selection the live run
// made on the day it recorded that snapshot, regardless of what today's
// config files say.
// Errors: strategies_config is null/not an object, or no strategy has
// enabled_live=true.
Result<StrategySelection> select_enabled_live_strategies(const nlohmann::json& strategies_config);

// LIVE_<sorted_names_joined_by__> -- the Tier-2 combined strategy_id
// convention. Takes selection.names (already sorted by
// select_enabled_live_strategies) rather than a StrategySelection directly,
// since benchmark_replay derives the same id from a run_inputs row's
// recorded universe/config rather than from a fresh selection call.
std::string build_combined_strategy_id(const std::vector<std::string>& sorted_strategy_names);

// Builds and starts one StrategyInterface instance per selection.names
// entry (TrendFollowingStrategy / *Fast / *Slow, chosen by each strategy's
// "type" field, defaulting to TrendFollowingStrategy). Shared by the live
// path's build_strategy_set and benchmark_replay so the ADR-005 7 parity
// gate compares positions produced by identical construction code -- a
// hand-duplicated copy in benchmark_replay would risk silent drift as this
// logic evolves.
// slow_max_symbol_concentration_override mirrors LivePortfolioConfig's
// per-portfolio override applied only to TrendFollowingSlowStrategy's
// hardcoded-defaults branch (see live_portfolio_runner.hpp).
// Throws std::runtime_error on an unknown strategy type or a failed
// initialize()/start() -- matching the original inline lambda this
// replaces, since a live/replay run cannot proceed with a partial
// strategy set.
std::vector<std::shared_ptr<StrategyInterface>> build_strategy_instances(
    const StrategySelection& selection, const StrategyConfig& base_strategy_config,
    double initial_capital, const StrategyDefaultsConfig& strategy_defaults,
    std::optional<double> slow_max_symbol_concentration_override,
    std::shared_ptr<PostgresDatabase> db, std::shared_ptr<InstrumentRegistry> registry_ptr);

}  // namespace trade_ngin
