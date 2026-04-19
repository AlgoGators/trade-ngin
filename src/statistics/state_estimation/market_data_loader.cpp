#include "trade_ngin/statistics/state_estimation/market_data_loader.hpp"
#include "trade_ngin/data/conversion_utils.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <unordered_map>

namespace trade_ngin {
namespace statistics {

MarketDataLoader::MarketDataLoader(PostgresDatabase& db)
    : db_(db) {}

// ============================================================================
// Compute log returns from close prices
// ============================================================================

std::vector<double> MarketDataLoader::compute_log_returns(const std::vector<Bar>& bars)
{
    std::vector<double> returns;
    returns.reserve(bars.size() - 1);
    for (size_t i = 1; i < bars.size(); ++i) {
        double p_prev = bars[i - 1].close.to_double();
        double p_curr = bars[i].close.to_double();
        if (p_prev > 0.0 && p_curr > 0.0) {
            returns.push_back(std::log(p_curr / p_prev));
        } else {
            returns.push_back(0.0);
        }
    }
    return returns;
}

// ============================================================================
// Load a single symbol from the database
// ============================================================================

Result<MarketPanel> MarketDataLoader::load_symbol(
    const std::string& symbol,
    AssetClass asset_class,
    const std::string& start_date,
    const std::string& end_date,
    DataFrequency freq) const
{
    // Parse ISO date strings ("YYYY-MM-DD") to Timestamps
    auto parse_iso_date = [](const std::string& s) -> Timestamp {
        std::tm tm = {};
        std::istringstream ss(s);
        ss >> std::get_time(&tm, "%Y-%m-%d");
        tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
        std::time_t t = std::mktime(&tm);
        return std::chrono::system_clock::from_time_t(t);
    };

    Timestamp ts_start, ts_end;
    if (!start_date.empty()) {
        ts_start = parse_iso_date(start_date);
    } else {
        ts_start = std::chrono::system_clock::time_point::min();
    }
    if (!end_date.empty()) {
        ts_end = parse_iso_date(end_date);
    } else {
        ts_end = std::chrono::system_clock::now();
    }

    // Query database
    auto result = db_.get_market_data(
        {symbol}, ts_start, ts_end, asset_class, freq, "ohlcv");
    if (result.is_error()) {
        return make_error<MarketPanel>(
            result.error()->code(),
            "Failed to load market data for " + symbol + ": " + result.error()->what(),
            "MarketDataLoader");
    }

    auto table = result.value();
    if (!table || table->num_rows() == 0) {
        return make_error<MarketPanel>(
            ErrorCode::DATA_NOT_FOUND,
            "No data found for " + symbol,
            "MarketDataLoader");
    }

    // Convert Arrow Table → Bars
    auto bars_result = DataConversionUtils::arrow_table_to_bars(table);
    if (bars_result.is_error()) {
        return make_error<MarketPanel>(
            bars_result.error()->code(),
            "Arrow conversion failed for " + symbol + ": " + bars_result.error()->what(),
            "MarketDataLoader");
    }

    auto bars = bars_result.value();  // copy — Result::value() is const&, sort needs mutable

    // Sort by timestamp ascending
    std::sort(bars.begin(), bars.end(),
        [](const Bar& a, const Bar& b) { return a.timestamp < b.timestamp; });

    // Build MarketPanel
    MarketPanel panel;
    panel.symbol = symbol;
    panel.bars = std::move(bars);
    panel.T = (int)panel.bars.size();

    // Extract dates as ISO strings
    auto format_date = [](const Timestamp& ts) -> std::string {
        auto t = std::chrono::system_clock::to_time_t(ts);
        std::tm tm;
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::ostringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%d");
        return ss.str();
    };

    panel.dates.reserve(panel.T);
    for (const auto& bar : panel.bars) {
        panel.dates.push_back(format_date(bar.timestamp));
    }

    // Compute log returns
    panel.returns = compute_log_returns(panel.bars);

    // Extract volumes
    panel.volumes.reserve(panel.T);
    for (const auto& bar : panel.bars) {
        panel.volumes.push_back(bar.volume);
    }

    std::cerr << "[MarketDataLoader] " << symbol
              << ": T=" << panel.T
              << " dates=[" << (panel.dates.empty() ? "?" : panel.dates.front())
              << " .. " << (panel.dates.empty() ? "?" : panel.dates.back()) << "]\n";

    return panel;
}

// ============================================================================
// Align multiple symbol panels to common dates
// ============================================================================

Result<SleevePanel> MarketDataLoader::align_panels(
    const std::string& sleeve_name,
    std::vector<MarketPanel>& panels)
{
    if (panels.empty()) {
        return make_error<SleevePanel>(
            ErrorCode::INVALID_ARGUMENT,
            "No panels to align", "MarketDataLoader");
    }

    // Find common dates across all panels
    std::set<std::string> common_dates(
        panels[0].dates.begin(), panels[0].dates.end());

    for (size_t s = 1; s < panels.size(); ++s) {
        std::set<std::string> sym_dates(
            panels[s].dates.begin(), panels[s].dates.end());
        std::set<std::string> intersection;
        std::set_intersection(
            common_dates.begin(), common_dates.end(),
            sym_dates.begin(), sym_dates.end(),
            std::inserter(intersection, intersection.begin()));
        common_dates = std::move(intersection);
    }

    if (common_dates.empty()) {
        return make_error<SleevePanel>(
            ErrorCode::DATA_NOT_FOUND,
            "No common dates across symbols in sleeve " + sleeve_name,
            "MarketDataLoader");
    }

    // Build date-indexed lookup for each panel
    const int T = (int)common_dates.size();
    const int N = (int)panels.size();

    SleevePanel sleeve;
    sleeve.sleeve_name = sleeve_name;
    sleeve.T = T;
    sleeve.N = N;
    sleeve.dates.assign(common_dates.begin(), common_dates.end());
    sleeve.composite_returns = Eigen::MatrixXd::Zero(T - 1, N);

    for (int s = 0; s < N; ++s) {
        // Build date→index map for this panel
        std::unordered_map<std::string, int> date_to_idx;
        for (int t = 0; t < panels[s].T; ++t)
            date_to_idx[panels[s].dates[t]] = t;

        // Build aligned returns: log(P_t / P_{t-1}) for common dates
        double prev_close = -1.0;
        int col = 0;
        for (const auto& date : sleeve.dates) {
            auto it = date_to_idx.find(date);
            if (it == date_to_idx.end()) continue;

            double close = panels[s].bars[it->second].close.to_double();
            if (prev_close > 0.0 && close > 0.0 && col > 0) {
                sleeve.composite_returns(col - 1, s) = std::log(close / prev_close);
            }
            prev_close = close;
            ++col;
        }
    }

    sleeve.symbol_panels = std::move(panels);

    std::cerr << "[MarketDataLoader] Sleeve '" << sleeve_name
              << "': T=" << T << " symbols=" << N
              << " common dates [" << sleeve.dates.front()
              << " .. " << sleeve.dates.back() << "]\n";

    return sleeve;
}

// ============================================================================
// Load an entire sleeve
// ============================================================================

Result<SleevePanel> MarketDataLoader::load_sleeve(
    const std::string& sleeve_name,
    const std::vector<std::string>& symbols,
    AssetClass asset_class,
    const std::string& start_date,
    const std::string& end_date,
    DataFrequency freq) const
{
    std::vector<MarketPanel> panels;
    panels.reserve(symbols.size());

    for (const auto& sym : symbols) {
        auto result = load_symbol(sym, asset_class, start_date, end_date, freq);
        if (result.is_error()) {
            std::cerr << "[MarketDataLoader] Warning: skipping " << sym
                      << " — " << result.error()->what() << "\n";
            continue;
        }
        panels.push_back(std::move(result.value()));
    }

    if (panels.empty()) {
        return make_error<SleevePanel>(
            ErrorCode::DATA_NOT_FOUND,
            "No symbols loaded for sleeve " + sleeve_name,
            "MarketDataLoader");
    }

    return align_panels(sleeve_name, panels);
}

} // namespace statistics
} // namespace trade_ngin
