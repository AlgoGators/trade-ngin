// Pure, testable helpers shared by the live-portfolio runner's benchmark
// pass and its replay-contract logging (ADR-005). Extracted out of
// apps/strategies/live_portfolio_runner.cpp specifically so they can be
// unit-tested: apps/ binaries aren't linked into trade_ngin_tests, but
// anything declared here and implemented against the trade_ngin library is.
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

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
nlohmann::json build_run_inputs_row(const std::string& trade_ngin_sha,
                                     const nlohmann::json& config_snapshot,
                                     const std::vector<std::string>& universe,
                                     const std::vector<Bar>& all_bars,
                                     const std::string& benchmark_mode);

}  // namespace trade_ngin
