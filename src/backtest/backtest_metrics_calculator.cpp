#include "trade_ngin/backtest/backtest_metrics_calculator.hpp"
#include <map>
#include "trade_ngin/backtest/backtest_types.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/transaction_cost/netting.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace trade_ngin {

// ========== Return Calculations ==========

double BacktestMetricsCalculator::calculate_total_return(double start_value, double end_value) const {
    // !(x > 0) instead of (x <= 0): NaN compares false to everything, so the
    // old form let a NaN start value through to the division.
    if (!(start_value > 0.0) || !std::isfinite(end_value)) {
        return 0.0;
    }
    return (end_value - start_value) / start_value;
}

double BacktestMetricsCalculator::calculate_annualized_return(double total_return, int trading_days) const {
    if (trading_days <= 0) {
        return 0.0;
    }
    // Simple annualization: scale by 252/trading_days
    double annualization_factor = 252.0 / static_cast<double>(trading_days);
    return total_return * annualization_factor;
}

std::vector<double> BacktestMetricsCalculator::calculate_returns_from_equity(
    const std::vector<std::pair<Timestamp, double>>& equity_curve) const {
    std::vector<double> returns;
    if (equity_curve.size() < 2) {
        return returns;
    }

    returns.reserve(equity_curve.size() - 1);
    for (size_t i = 1; i < equity_curve.size(); ++i) {
        // prev > 0.0 already excludes NaN prev; the current point must also be
        // finite or the computed return itself is NaN.
        if (equity_curve[i - 1].second > 0.0 && std::isfinite(equity_curve[i].second)) {
            double ret = (equity_curve[i].second - equity_curve[i - 1].second) /
                        equity_curve[i - 1].second;
            returns.push_back(ret);
        }
    }
    return returns;
}

// ========== Risk-Adjusted Return Metrics ==========

double BacktestMetricsCalculator::calculate_sharpe_ratio(
    const std::vector<double>& returns,
    int trading_days,
    double risk_free_rate) const {
    if (returns.empty() || trading_days <= 0) {
        return 0.0;
    }

    double mean_return = calculate_mean(returns);
    double volatility = calculate_volatility(returns);

    if (volatility <= 0.0) {
        return 0.0;
    }

    // Annualize mean daily return: multiply by 252 (trading days per year)
    // mean_return is already a daily rate; 252 scales it to annual
    double annualized_return = mean_return * 252.0;

    // Sharpe = (annualized return - risk free rate) / annualized volatility
    return (annualized_return - risk_free_rate) / volatility;
}

double BacktestMetricsCalculator::calculate_sortino_ratio(
    const std::vector<double>& returns,
    int trading_days,
    double minimum_acceptable_return) const {
    if (returns.empty() || trading_days <= 0) {
        return 0.0;
    }

    double mean_return = calculate_mean(returns);
    double downside_vol = calculate_downside_volatility(returns, minimum_acceptable_return);

    if (downside_vol <= 0.0) {
        // No negative returns - cap at reasonable value
        return (mean_return * 252.0) >= 0 ? 999.0 : 0.0;
    }

    // Annualize mean daily return: multiply by 252 (trading days per year)
    double annualized_return = mean_return * 252.0;
    return (annualized_return - minimum_acceptable_return) / downside_vol;
}

double BacktestMetricsCalculator::calculate_calmar_ratio(double annualized_return, double max_drawdown) const {
    if (max_drawdown <= 0.0) {
        return annualized_return >= 0 ? 999.0 : 0.0;
    }
    return annualized_return / max_drawdown;
}

// ========== Volatility Metrics ==========

double BacktestMetricsCalculator::calculate_volatility(const std::vector<double>& returns) const {
    if (returns.empty()) {
        return 0.0;
    }

    // Two-pass standard deviation. The previous one-pass formula
    // (E[r^2] - mean^2) suffers catastrophic cancellation for small daily
    // returns and can round to a slightly negative variance, turning
    // sqrt() into NaN. That NaN passed the `volatility <= 0` guard in
    // calculate_sharpe_ratio (NaN comparisons are false) and silently
    // propagated into Sharpe/reporting.
    double mean_return = calculate_mean(returns);

    // Annualize using sqrt(252) for daily volatility
    double volatility = calculate_std_dev(returns, mean_return) * std::sqrt(252.0);

    // A constant series still leaves rounding dust (~1e-17) in the deviations;
    // collapse it to a clean 0 so the `volatility <= 0` guard in
    // calculate_sharpe_ratio treats the degenerate case as such instead of
    // dividing by dust and reporting an absurd Sharpe.
    if (volatility > 0.0 && volatility < 1e-12) {
        WARN("Volatility collapsed to 0 (rounding dust; constant or near-constant "
             "series of " + std::to_string(returns.size()) +
             " returns); Sharpe will report 0 -- distinguish flat from broken data upstream");
        return 0.0;
    }
    return volatility;
}

double BacktestMetricsCalculator::calculate_downside_volatility(
    const std::vector<double>& returns,
    double target) const {
    double downside_sum = 0.0;
    int downside_count = 0;

    for (double ret : returns) {
        if (ret < target) {
            double deviation = ret - target;
            downside_sum += deviation * deviation;
            downside_count++;
        }
    }

    if (downside_count <= 0) {
        return 0.0;
    }

    // The squared shortfalls are averaged over EVERY return of the series (a return at or
    // above target counts as a zero), then annualized using sqrt(252)
    double downside_vol =
        std::sqrt(downside_sum / static_cast<double>(returns.size())) * std::sqrt(252.0);

    // Same rounding-dust collapse calculate_volatility applies: returns sitting a
    // hair below target otherwise leave a ~1e-17 denominator and Sortino explodes
    // to an absurd finite value instead of hitting its degenerate-case sentinel.
    if (downside_vol > 0.0 && downside_vol < 1e-12) {
        WARN("Downside volatility collapsed to 0 (rounding dust " +
             std::to_string(downside_vol) + "); Sortino will report its degenerate-case value");
        return 0.0;
    }
    return downside_vol;
}

// ========== Drawdown Metrics ==========

std::vector<std::pair<Timestamp, double>> BacktestMetricsCalculator::calculate_drawdowns(
    const std::vector<std::pair<Timestamp, double>>& equity_curve) const {
    std::vector<std::pair<Timestamp, double>> drawdowns;
    drawdowns.reserve(equity_curve.size());

    if (equity_curve.empty()) {
        return drawdowns;
    }

    double peak = equity_curve[0].second;

    for (const auto& [timestamp, equity] : equity_curve) {
        peak = std::max(peak, equity);
        double drawdown = equity < peak ? (peak - equity) / peak : 0.0;
        drawdowns.emplace_back(timestamp, drawdown);
    }

    return drawdowns;
}

double BacktestMetricsCalculator::calculate_max_drawdown(
    const std::vector<std::pair<Timestamp, double>>& equity_curve) const {
    auto drawdowns = calculate_drawdowns(equity_curve);
    if (drawdowns.empty()) {
        return 0.0;
    }

    auto max_it = std::max_element(drawdowns.begin(), drawdowns.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });
    return max_it->second;
}

// ========== Risk Metrics ==========

double BacktestMetricsCalculator::calculate_var_95(const std::vector<double>& returns) const {
    if (returns.empty()) {
        return 0.0;
    }

    std::vector<double> sorted_returns = returns;
    std::sort(sorted_returns.begin(), sorted_returns.end());

    size_t var_index = static_cast<size_t>(returns.size() * 0.05);
    if (var_index >= sorted_returns.size()) {
        var_index = sorted_returns.size() - 1;
    }

    return -sorted_returns[var_index];
}

double BacktestMetricsCalculator::calculate_cvar_95(const std::vector<double>& returns) const {
    if (returns.empty()) {
        return 0.0;
    }

    std::vector<double> sorted_returns = returns;
    std::sort(sorted_returns.begin(), sorted_returns.end());

    size_t var_index = static_cast<size_t>(returns.size() * 0.05);
    if (var_index == 0) {
        var_index = 1;  // Need at least one value
    }

    double cvar_sum = 0.0;
    for (size_t i = 0; i < var_index; ++i) {
        cvar_sum += sorted_returns[i];
    }

    return -cvar_sum / var_index;
}

std::unordered_map<std::string, double> BacktestMetricsCalculator::calculate_risk_metrics(
    const std::vector<double>& returns,
    int /* trading_days */) const {
    std::unordered_map<std::string, double> metrics;

    if (returns.empty()) {
        return metrics;
    }

    metrics["var_95"] = calculate_var_95(returns);
    metrics["cvar_95"] = calculate_cvar_95(returns);
    metrics["downside_volatility"] = calculate_downside_volatility(returns, 0.0);

    return metrics;
}

namespace {

// LOOP_SPEC v6.2 section 6.5: a trade held through a roll scores the held contracts' whole move, so
// the open trade's entry price is carried across the roll by the leg gap,
// entry := entry + (opening leg price - closing leg price), longs and shorts alike.
//
// T-ROLLX-FIX commit 5 (finding 11): the gap of a roll is read from the legs' OWN identity, never
// from the order they arrive in. The legs of one (symbol, bar) are collected first; the bar's
// rolls are resolved when the walk reaches the bar's first row of any type, so a STRATEGY fill
// stored ahead of its bar's legs is scored against the carried entry too (the legs are priced on
// the bars before it). Per leg:
//   the role     the exec id when it carries it: the engine's RL-<sleeve>-<n> (a roll takes n and
//                n + 1, n even: the closing leg) and the live EXEC_..._RC / _RO; otherwise the
//                side against the tracked position (the closing leg trades against it);
//   the pair     the exec id again (RL-<sleeve>-<n / 2>, EXEC_... without its suffix); legs whose
//                ids carry no pair are one roll when the bar has one closing (price, contract) and
//                one opening (price, contract).
// The tracker is keyed by symbol over every sleeve, so one roll's gap is carried once: a second
// pair of the same bar with the same two prices is another sleeve's copy of that roll, a pair with
// other prices is another roll confirmed in the same cycle. Legs that cannot be paired (a closing
// leg without its opening leg, two rolls with no pair in the ids) are an error, thrown: a guessed
// gap would be scored into every later trade of the symbol.
class RollEntryCarry {
public:
    explicit RollEntryCarry(const std::vector<ExecutionReport>& executions) {
        for (const auto& exec : executions) {
            if (exec.execution_type != ExecutionType::ROLL) continue;
            bars_[{exec.symbol, exec.fill_time}].legs.push_back(&exec);
        }
    }

    /// The gaps to add to the symbol's open entry price, one per roll of the bar (symbol,
    /// fill_time), in the order of the rolls' first legs. Non-empty once per bar, on the first
    /// row of it the walk reaches, and only while a trade is open (`tracked_position` != 0: with
    /// no open entry there is nothing to carry and the legs are not read).
    std::vector<double> take(const std::string& symbol, const Timestamp& fill_time,
                             double tracked_position) {
        const auto it = bars_.find({symbol, fill_time});
        if (it == bars_.end() || it->second.taken) return {};
        it->second.taken = true;
        if (tracked_position == 0.0) return {};
        return gaps_of(symbol, it->second.legs, tracked_position);
    }

private:
    struct Bar {
        std::vector<const ExecutionReport*> legs;
        bool taken{false};
    };
    struct Roll {
        std::string pair;  ///< empty: no pair in the ids
        bool has_closing{false};
        bool has_opening{false};
        double closing_price{0.0};
        double opening_price{0.0};
    };

    [[noreturn]] static void fail(const std::string& symbol, const std::string& why) {
        const std::string what = "ROLL_LEG STOP: the trade statistics cannot pair the ROLL legs of " +
                                 symbol + ": " + why;
        ERROR(what);
        throw std::runtime_error(what);
    }

    /// The role and pair an exec id carries: +1 closing, -1 opening, 0 none.
    static int role_of_id(const std::string& id, std::string* pair) {
        const auto ends_with = [&](const char* suffix) {
            const std::string s(suffix);
            return id.size() > s.size() && id.compare(id.size() - s.size(), s.size(), s) == 0;
        };
        if (ends_with("_RC") || ends_with("_RO")) {
            *pair = id.substr(0, id.size() - 3);
            return ends_with("_RC") ? 1 : -1;
        }
        const auto dash = id.rfind('-');
        if (id.rfind("RL-", 0) == 0 && dash != std::string::npos && dash > 2 && dash + 1 < id.size() &&
            std::all_of(id.begin() + static_cast<std::ptrdiff_t>(dash) + 1, id.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
            const unsigned long long n = std::stoull(id.substr(dash + 1));
            *pair = id.substr(0, dash + 1) + std::to_string(n / 2);
            return n % 2 == 0 ? 1 : -1;
        }
        return 0;
    }

    static std::vector<double> gaps_of(const std::string& symbol,
                                       const std::vector<const ExecutionReport*>& legs,
                                       double tracked_position) {
        std::vector<Roll> rolls;  // by pair, in the order of the first leg
        std::vector<std::pair<double, std::string>> loose_closing, loose_opening;
        bool loose_placed = false;  // the id-less legs' roll, placed at their first leg
        for (const ExecutionReport* leg : legs) {
            const double price = static_cast<double>(leg->fill_price);
            std::string pair;
            int role = role_of_id(leg->exec_id, &pair);
            if (role == 0) {
                // No role in the id: the closing leg trades against the tracked position.
                const bool against = (leg->side == Side::BUY) == (tracked_position < 0.0);
                auto& side = against ? loose_closing : loose_opening;
                const std::pair<double, std::string> key{price, leg->instrument_id};
                if (std::find(side.begin(), side.end(), key) == side.end()) side.push_back(key);
                if (!loose_placed) rolls.push_back(Roll{});
                loose_placed = true;
                continue;
            }
            auto roll = std::find_if(rolls.begin(), rolls.end(),
                                     [&](const Roll& r) { return !r.pair.empty() && r.pair == pair; });
            if (roll == rolls.end()) {
                rolls.push_back(Roll{pair});
                roll = rolls.end() - 1;
            }
            bool& has = role > 0 ? roll->has_closing : roll->has_opening;
            if (has) fail(symbol, "two " + std::string(role > 0 ? "closing" : "opening") +
                                      " legs carry the pair " + pair);
            has = true;
            (role > 0 ? roll->closing_price : roll->opening_price) = price;
        }
        if (!loose_closing.empty() || !loose_opening.empty()) {
            if (loose_closing.size() != 1 || loose_opening.size() != 1) {
                fail(symbol, std::to_string(loose_closing.size()) + " closing and " +
                                 std::to_string(loose_opening.size()) +
                                 " opening (price, contract) legs on one bar carry no pair in "
                                 "their exec ids");
            }
            for (auto& roll : rolls) {
                if (!roll.pair.empty() || roll.has_closing) continue;
                roll.has_closing = roll.has_opening = true;
                roll.closing_price = loose_closing.front().first;
                roll.opening_price = loose_opening.front().first;
            }
        }
        std::vector<double> gaps;
        std::vector<std::pair<double, double>> carried;
        for (const auto& roll : rolls) {
            if (!roll.has_closing || !roll.has_opening) {
                fail(symbol, "the pair " + roll.pair + " has " +
                                 (roll.has_closing ? "no opening leg" : "no closing leg"));
            }
            const std::pair<double, double> prices{roll.closing_price, roll.opening_price};
            if (std::find(carried.begin(), carried.end(), prices) != carried.end()) continue;
            carried.push_back(prices);
            gaps.push_back(prices.second - prices.first);
        }
        return gaps;
    }

    std::map<std::pair<std::string, Timestamp>, Bar> bars_;
};

}  // namespace

// ========== Trade Statistics ==========

BacktestMetricsCalculator::TradeStatistics BacktestMetricsCalculator::calculate_trade_statistics(
    const std::vector<ExecutionReport>& executions) const {
    TradeStatistics stats;

    std::unordered_map<std::string, double> positions;   // symbol -> net position
    RollEntryCarry roll_carry(executions);               // the leg gap of each roll, carried once
    std::unordered_map<std::string, double> avg_prices;  // symbol -> average entry price
    std::map<std::string, Timestamp> open_times;         // symbol -> first trade time
    std::vector<double> holding_periods;

    for (const auto& exec : executions) {
        const std::string& symbol = exec.symbol;
        double fill_price = static_cast<double>(exec.fill_price);
        double quantity = static_cast<double>(exec.filled_quantity);
        double commission = static_cast<double>(transaction_cost::net_cost(exec));

        // Adjust quantity based on side
        double signed_qty = (exec.side == Side::BUY) ? quantity : -quantity;

        double current_pos = positions[symbol];

        // The bar's rolls, carried into the open entry before the bar's first row is scored.
        for (const double gap : roll_carry.take(symbol, exec.fill_time, current_pos)) {
            avg_prices[symbol] += gap;
        }

        // T-ROLLX-FIX (LOOP_SPEC v6.2 section 6.5; code review D1): a ROLL leg is mechanical. It
        // never moves the tracked position (the pair nets to 0 per sleeve), scores no trade and
        // leaves the open time. The open trade's entry price is carried across the roll by the leg
        // gap (opening leg price - closing leg price), so the trade's later close scores the move of
        // the contracts actually held: entry to the closing leg in the old contract plus the opening
        // leg to the exit in the new one. The tracker is keyed by symbol over every sleeve, so the
        // gap is carried once per roll and does not wait for the summed position to pass through 0.
        // Its cost goes to the roll total, never into a trade.
        if (exec.execution_type == ExecutionType::ROLL) {
            stats.roll_fills++;
            // The same figure results.roll_costs reads: the leg's own cost (a leg is never netted;
            // one carrying an adjustment is refused).
            stats.roll_costs += static_cast<double>(transaction_cost::unnetted_cost(exec));
            continue;
        }
        // X-4: a BORROW row (quantity 0) is a cost on an open short, not a trade: it moves no
        // position, opens nothing and scores no trade.
        if (exec.execution_type == ExecutionType::BORROW) continue;

        double trade_pnl = -commission;

        if (current_pos == 0.0) {
            // Opening new position
            positions[symbol] = signed_qty;
            avg_prices[symbol] = fill_price;
            open_times[symbol] = exec.fill_time;
        } else if ((current_pos > 0 && signed_qty > 0) || (current_pos < 0 && signed_qty < 0)) {
            // Adding to existing position
            double total_value = current_pos * avg_prices[symbol] + signed_qty * fill_price;
            positions[symbol] = current_pos + signed_qty;
            if (positions[symbol] != 0.0) {
                avg_prices[symbol] = total_value / positions[symbol];
            }
        } else {
            // Reducing or closing position - realize P&L
            double close_qty = std::min(std::abs(signed_qty), std::abs(current_pos));
            trade_pnl += close_qty * (fill_price - avg_prices[symbol]) *
                        (current_pos > 0 ? 1.0 : -1.0);

            positions[symbol] = current_pos + signed_qty;
        }

        // Check if this is a position-closing trade
        bool is_closing_trade = std::abs(signed_qty) > 1e-6 && current_pos != 0.0 &&
            ((current_pos > 0 && signed_qty < 0) || (current_pos < 0 && signed_qty > 0));

        if (is_closing_trade) {
            stats.actual_trades.push_back(exec);
            INFO("DEBUG_TRADE: " + symbol + " pos=" + std::to_string(current_pos) +
                 " qty=" + std::to_string(signed_qty) + " -> " + std::to_string(positions[symbol]));

            if (trade_pnl > 0) {
                stats.total_profit += trade_pnl;
                stats.winning_trades++;
                stats.max_win = std::max(stats.max_win, trade_pnl);
            } else {
                stats.total_loss -= trade_pnl;  // total_loss is positive
                stats.max_loss = std::max(stats.max_loss, -trade_pnl);
            }

            // Calculate holding period
            auto it = open_times.find(symbol);
            if (it != open_times.end()) {
                auto duration = std::chrono::duration_cast<std::chrono::hours>(
                    exec.fill_time - it->second);
                double hours = static_cast<double>(duration.count());
                if (hours > 0) {
                    holding_periods.push_back(hours / 24.0);
                }
                open_times[symbol] = exec.fill_time;
            }
        }
    }

    stats.total_trades = static_cast<int>(stats.actual_trades.size());

    if (stats.total_trades > 0) {
        stats.win_rate = static_cast<double>(stats.winning_trades) / stats.total_trades;
        stats.avg_win = stats.winning_trades > 0 ?
            stats.total_profit / stats.winning_trades : 0.0;
        int losing_trades = stats.total_trades - stats.winning_trades;
        stats.avg_loss = losing_trades > 0 ?
            stats.total_loss / losing_trades : 0.0;
    }

    if (stats.total_loss > 0) {
        stats.profit_factor = stats.total_profit / stats.total_loss;
    } else if (stats.total_trades > 0 && stats.total_profit > 0) {
        stats.profit_factor = 999.0;
    }

    if (!holding_periods.empty()) {
        stats.avg_holding_period = std::accumulate(holding_periods.begin(),
            holding_periods.end(), 0.0) / holding_periods.size();
    }

    return stats;
}

// ========== Per-Symbol Analysis ==========

std::map<std::string, double> BacktestMetricsCalculator::calculate_symbol_pnl(
    const std::vector<ExecutionReport>& executions) const {
    std::unordered_map<std::string, double> positions;
    RollEntryCarry roll_carry(executions);  // the leg gap of each roll, carried once
    std::unordered_map<std::string, double> avg_prices;
    std::map<std::string, double> symbol_pnl_map;

    for (const auto& exec : executions) {
        const std::string& symbol = exec.symbol;
        double fill_price = static_cast<double>(exec.fill_price);
        double quantity = static_cast<double>(exec.filled_quantity);
        double commission = static_cast<double>(transaction_cost::net_cost(exec));

        double signed_qty = (exec.side == Side::BUY) ? quantity : -quantity;

        double current_pos = positions[symbol];

        // T-ROLLX-FIX: a roll carries the open entry price across it exactly as in
        // calculate_trade_statistics (the leg gap, once per roll, on the bar's first row, the
        // position untouched) and each leg's cost is charged to the symbol; no trade P&L is
        // scored on a leg.
        for (const double gap : roll_carry.take(symbol, exec.fill_time, current_pos)) {
            avg_prices[symbol] += gap;
        }
        if (exec.execution_type == ExecutionType::ROLL) {
            symbol_pnl_map[symbol] -= commission;
            continue;
        }
        // X-4: a BORROW row charges its cost to the symbol and moves no position.
        if (exec.execution_type == ExecutionType::BORROW) {
            symbol_pnl_map[symbol] -= commission;
            continue;
        }

        double trade_pnl = -commission;

        if (current_pos == 0.0) {
            positions[symbol] = signed_qty;
            avg_prices[symbol] = fill_price;
        } else if ((current_pos > 0 && signed_qty > 0) || (current_pos < 0 && signed_qty < 0)) {
            double total_value = current_pos * avg_prices[symbol] + signed_qty * fill_price;
            positions[symbol] = current_pos + signed_qty;
            if (positions[symbol] != 0.0) {
                avg_prices[symbol] = total_value / positions[symbol];
            }
        } else {
            double close_qty = std::min(std::abs(signed_qty), std::abs(current_pos));
            trade_pnl += close_qty * (fill_price - avg_prices[symbol]) *
                        (current_pos > 0 ? 1.0 : -1.0);
            positions[symbol] = current_pos + signed_qty;
        }

        symbol_pnl_map[symbol] += trade_pnl;
    }

    return symbol_pnl_map;
}

std::unordered_map<std::string, double> BacktestMetricsCalculator::calculate_monthly_returns(
    const std::vector<std::pair<Timestamp, double>>& equity_curve) const {
    std::unordered_map<std::string, double> monthly_returns;

    for (size_t i = 1; i < equity_curve.size(); ++i) {
        // Skip non-positive OR non-finite equity points, matching
        // calculate_returns_from_equity. !(x > 0) instead of (x <= 0): NaN
        // compares false to everything, so the old form let NaN through into
        // the monthly totals.
        if (!(equity_curve[i - 1].second > 0.0) || !std::isfinite(equity_curve[i].second)) {
            continue;
        }

        auto time_t = std::chrono::system_clock::to_time_t(equity_curve[i].first);
        std::tm tm;
        core::safe_localtime(&time_t, &tm);

        std::ostringstream month_key;
        month_key << std::setw(4) << (tm.tm_year + 1900) << "-"
                  << std::setw(2) << std::setfill('0') << (tm.tm_mon + 1);

        double period_return = (equity_curve[i].second - equity_curve[i - 1].second) /
                              equity_curve[i - 1].second;
        monthly_returns[month_key.str()] += period_return;
    }

    return monthly_returns;
}

// ========== Beta and Correlation (Autocorrelation-Based) ==========
//
// NOTE: These are AUTOCORRELATION metrics, NOT market beta/correlation.
// - beta: regression slope of today's return on yesterday's return
// - correlation: lag-1 autocorrelation (today vs yesterday)
// For true market beta/correlation vs a benchmark (e.g. S&P 500), benchmark
// returns would need to be supplied and used instead of lagged strategy returns.

std::pair<double, double> BacktestMetricsCalculator::calculate_beta_correlation(
    const std::vector<double>& returns) const {
    if (returns.size() <= 1) {
        return {0.0, 0.0};
    }

    double mean_return = calculate_mean(returns);

    // Lag-1 autocorrelation: regress returns[i] on returns[i-1]
    double covariance = 0.0;
    double variance_benchmark = 0.0;
    double variance_strategy = 0.0;

    for (size_t i = 1; i < returns.size(); ++i) {
        double prev_return = returns[i - 1];
        double curr_return = returns[i];
        covariance += (prev_return - mean_return) * (curr_return - mean_return);
        variance_benchmark += (prev_return - mean_return) * (prev_return - mean_return);
        variance_strategy += (curr_return - mean_return) * (curr_return - mean_return);
    }

    double beta = 0.0;
    double correlation = 0.0;

    if (variance_benchmark > 0) {
        beta = covariance / variance_benchmark;
        correlation = covariance / std::sqrt(variance_benchmark * variance_strategy);
    }

    return {beta, correlation};
}

// ========== Composite Calculation ==========

backtest::BacktestResults BacktestMetricsCalculator::calculate_all_metrics(
    const std::vector<std::pair<Timestamp, double>>& equity_curve,
    const std::vector<ExecutionReport>& executions,
    int warmup_days) const {
    backtest::BacktestResults results;

    if (equity_curve.empty()) {
        return results;
    }

    // One row per UTC date, the last one, before any metric
    size_t repeated = 0;
    const auto dated_curve = core::last_row_per_utc_date(
        equity_curve,
        [](const std::pair<Timestamp, double>& row) { return core::format_utc_date(row.first); },
        &repeated);
    if (repeated > 0) {
        WARN("Equity curve: " + std::to_string(repeated) +
             " row(s) dropped before the metrics, a UTC date carried more than once (the last "
             "row of a date is kept)");
    }

    // Filter warmup period
    auto filtered_curve = filter_warmup_period(dated_curve, warmup_days);
    if (filtered_curve.empty()) {
        return results;
    }

    // Calculate returns from filtered data
    auto returns = calculate_returns_from_equity(filtered_curve);

    // Basic performance metrics
    results.total_return = calculate_total_return(
        filtered_curve.front().second, filtered_curve.back().second);

    int actual_trading_days = static_cast<int>(filtered_curve.size()) - 1;
    if (actual_trading_days <= 0) {
        actual_trading_days = 1;
    }

    // Volatility metrics
    results.volatility = calculate_volatility(returns);

    // Risk-adjusted metrics
    results.sharpe_ratio = calculate_sharpe_ratio(returns, actual_trading_days);
    results.sortino_ratio = calculate_sortino_ratio(returns, actual_trading_days);

    // Drawdown metrics
    auto drawdowns = calculate_drawdowns(filtered_curve);
    if (!drawdowns.empty()) {
        results.max_drawdown = std::max_element(drawdowns.begin(), drawdowns.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; })->second;
        results.drawdown_curve = drawdowns;
    }

    // Calmar ratio: annualized return / max drawdown (industry standard)
    double mean_return = calculate_mean(returns);
    double annualized_return = mean_return * 252.0;
    results.calmar_ratio = calculate_calmar_ratio(annualized_return, results.max_drawdown);

    // Risk metrics
    auto risk_metrics = calculate_risk_metrics(returns, actual_trading_days);
    results.var_95 = risk_metrics["var_95"];
    results.cvar_95 = risk_metrics["cvar_95"];
    results.downside_volatility = risk_metrics["downside_volatility"];

    // Beta and correlation
    auto [beta, correlation] = calculate_beta_correlation(returns);
    results.beta = beta;
    results.correlation = correlation;

    // Trade statistics
    auto trade_stats = calculate_trade_statistics(executions);
    results.total_trades = trade_stats.total_trades;
    results.win_rate = trade_stats.win_rate;
    results.profit_factor = trade_stats.profit_factor;
    results.avg_win = trade_stats.avg_win;
    results.avg_loss = trade_stats.avg_loss;
    results.max_win = trade_stats.max_win;
    results.max_loss = trade_stats.max_loss;
    results.avg_holding_period = trade_stats.avg_holding_period;
    results.actual_trades = trade_stats.actual_trades;

    // Per-symbol P&L
    auto symbol_pnl = calculate_symbol_pnl(executions);
    for (const auto& [symbol, pnl] : symbol_pnl) {
        results.symbol_pnl[symbol] = pnl;
    }

    // Monthly returns
    results.monthly_returns = calculate_monthly_returns(dated_curve);

    // Store warmup days
    results.warmup_days = warmup_days;

    return results;
}

// ========== Helper Methods ==========

double BacktestMetricsCalculator::calculate_mean(const std::vector<double>& values) const {
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double BacktestMetricsCalculator::calculate_std_dev(const std::vector<double>& values, double mean) const {
    if (values.empty()) {
        return 0.0;
    }

    double sq_sum = 0.0;
    for (double val : values) {
        sq_sum += (val - mean) * (val - mean);
    }
    return std::sqrt(sq_sum / values.size());
}

std::vector<std::pair<Timestamp, double>> BacktestMetricsCalculator::filter_warmup_period(
    const std::vector<std::pair<Timestamp, double>>& equity_curve,
    int warmup_days) const {
    if (warmup_days <= 0 || equity_curve.size() <= static_cast<size_t>(warmup_days)) {
        return equity_curve;
    }

    return std::vector<std::pair<Timestamp, double>>(
        equity_curve.begin() + warmup_days, equity_curve.end());
}

} // namespace trade_ngin
