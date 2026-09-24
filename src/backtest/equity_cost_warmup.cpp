// src/backtest/equity_cost_warmup.cpp
//
// The equity backtest's cost warm-up: the 30 calendar days BEFORE start_date, loaded with the
// MarketDataBus off, feeding only the two cost registrations. See the header (ledger H-13).

#include "trade_ngin/backtest/equity_cost_warmup.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

namespace trade_ngin {
namespace backtest {

namespace {

// register_equity_costs_from_bars' default lookback (transaction_cost_manager.hpp): the ADV is the
// mean volume of the last min(20, n) bars. Used here only to print the ADV the tier was set from.
constexpr size_t kAdvLookbackBars = 20;

// Bus publishing off for the lifetime of the guard, then back to what it was (on, at every call
// site today), also when the load throws.
class BusPublishingOff {
public:
    BusPublishingOff() : previous_(MarketDataBus::instance().is_publish_enabled()) {
        MarketDataBus::instance().set_publish_enabled(false);
    }
    ~BusPublishingOff() { MarketDataBus::instance().set_publish_enabled(previous_); }
    BusPublishingOff(const BusPublishingOff&) = delete;
    BusPublishingOff& operator=(const BusPublishingOff&) = delete;

private:
    bool previous_;
};

}  // namespace

EquityCostWarmupWindow equity_cost_warmup_window(const Timestamp& start_date) {
    EquityCostWarmupWindow w;
    w.start = start_date - std::chrono::hours(24 * kEquityCostWarmupCalendarDays);
    w.end = start_date - std::chrono::seconds(1);
    return w;
}

EquityCostWarmupSummary register_equity_cost_warmup(
    const std::vector<std::string>& symbols, const Timestamp& start_date,
    const EquityCostWarmupLoader& load,
    transaction_cost::TransactionCostManager& execution_costs, PortfolioManager& portfolio) {
    EquityCostWarmupSummary summary;
    summary.window = equity_cost_warmup_window(start_date);

    INFO("EQUITY_COST_WARMUP window=[" + core::format_utc_datetime(summary.window.start) + "Z, " +
         core::format_utc_datetime(summary.window.end) + "Z] start_date=" +
         core::format_utc_datetime(start_date) + "Z: the " +
         std::to_string(kEquityCostWarmupCalendarDays) +
         " calendar days before the backtest, loaded with MarketDataBus publishing DISABLED, so "
         "the bars reach only the two equity cost registrations");

    Result<std::shared_ptr<arrow::Table>> warmup_result = [&]() {
        BusPublishingOff bus_off;
        return load(summary.window.start, summary.window.end);
    }();

    if (warmup_result.is_error()) {
        WARN("Failed to load equity cost warmup data: " +
             std::string(warmup_result.error()->what()) +
             " -- equity cost configs may use unconfigured-symbol fallback");
        return summary;
    }

    auto warmup_bars_result = DataConversionUtils::arrow_table_to_bars(warmup_result.value());
    if (warmup_bars_result.is_error()) {
        WARN("Failed to convert warmup bars: " +
             std::string(warmup_bars_result.error()->what()) +
             " -- equity cost configs may use unconfigured-symbol fallback");
        return summary;
    }

    const auto& warmup_bars = warmup_bars_result.value();
    summary.loaded = true;
    summary.bars = warmup_bars.size();
    for (const auto& bar : warmup_bars) {
        summary.bars_by_symbol[bar.symbol].push_back(bar);
    }

    summary.registered_execution_costs =
        execution_costs.register_equity_costs_from_bars(symbols, summary.bars_by_symbol);
    // E2-C9: PortfolioManager owns a SECOND, independent cost manager, and it is the one whose
    // numbers reach backtest.executions and the equity curve -- the coordinator's only feeds the
    // reported metrics. Registering just one left a single run carrying two different cost bases.
    summary.registered_portfolio_costs =
        portfolio.register_equity_cost_configs(symbols, summary.bars_by_symbol);

    size_t symbols_with_bars = 0;
    std::string last_bar = "-";
    for (const auto& symbol : symbols) {
        auto it = summary.bars_by_symbol.find(symbol);
        const size_t n = (it == summary.bars_by_symbol.end()) ? 0 : it->second.size();
        const size_t adv_n = std::min(kAdvLookbackBars, n);
        double adv = 0.0;
        if (adv_n > 0) {
            for (size_t i = n - adv_n; i < n; ++i) adv += it->second[i].volume;
            adv /= static_cast<double>(adv_n);
            ++symbols_with_bars;
            last_bar = std::max(last_bar == "-" ? std::string() : last_bar,
                                core::format_utc_date(it->second.back().timestamp));
        }
        const auto config = execution_costs.get_asset_config(symbol);
        INFO("EQUITY_COST_WARMUP_TIER symbol=" + symbol + " bars=" + std::to_string(n) +
             " adv_bars=" + std::to_string(adv_n) + " adv=" + std::to_string(adv) +
             " spread_ticks=" + std::to_string(config.baseline_spread_ticks) + "/" +
             std::to_string(config.min_spread_ticks) + "/" +
             std::to_string(config.max_spread_ticks) +
             " max_impact_bps=" + std::to_string(config.max_impact_bps) +
             " max_total_implicit_bps=" + std::to_string(config.max_total_implicit_bps));
    }

    INFO("EQUITY_COST_WARMUP loaded bars=" + std::to_string(summary.bars) + " symbols=" +
         std::to_string(symbols_with_bars) + "/" + std::to_string(symbols.size()) +
         " last_bar=" + last_bar + " registered=" +
         std::to_string(summary.registered_execution_costs) + "+" +
         std::to_string(summary.registered_portfolio_costs) +
         " (MarketDataBus publishing re-enabled after the load)");
    return summary;
}

}  // namespace backtest
}  // namespace trade_ngin
