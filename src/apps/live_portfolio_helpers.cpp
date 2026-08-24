#include "trade_ngin/apps/live_portfolio_helpers.hpp"

#include <chrono>
#include <sstream>

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
                                     const std::string& benchmark_mode) {
    nlohmann::json row;
    row["trade_ngin_sha"] = trade_ngin_sha;
    row["config_snapshot"] = config_snapshot;
    row["universe"] = universe;

    nlohmann::json data_window;
    data_window["schema"] = "trading";
    data_window["table"] = "bar";
    data_window["start"] = 0;
    data_window["end"] = 0;
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

}  // namespace trade_ngin
