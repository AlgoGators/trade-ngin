#include "trade_ngin/backtest/backtest_metrics_calculator.hpp"
#include <map>
#include "trade_ngin/backtest/backtest_types.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"
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

double BacktestMetricsCalculator::calculate_annualized_return(double total_return, int trading_days,
                                                              double sessions_per_year) const {
    // The CAGR, (1 + R)^(K / n) - 1 (T-8D R36): the live statistics' own function, so the
    // backtest's annualised return and the live total_annualized_return are one formula.
    return grid_annualized_return(1.0 + total_return, trading_days, sessions_per_year);
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
    double sessions_per_year,
    double risk_free_rate) const {
    // No returns, no sessions a year, or no volatility to divide by: the ratio has no value
    // (T-8D R12), which is not a measured 0.
    if (returns.empty() || !(sessions_per_year > 0.0)) {
        return kNoRatio;
    }

    double mean_return = calculate_mean(returns);
    double volatility = calculate_volatility(returns, sessions_per_year);

    if (!(volatility > 0.0)) {
        return kNoRatio;
    }

    // Annualize the mean grid return: multiply by the grid's sessions a year
    double annualized_return = mean_return * sessions_per_year;

    // Sharpe = (annualized return - risk free rate) / annualized volatility
    return (annualized_return - risk_free_rate) / volatility;
}

double BacktestMetricsCalculator::calculate_sortino_ratio(
    const std::vector<double>& returns,
    double sessions_per_year,
    double minimum_acceptable_return) const {
    if (returns.empty() || !(sessions_per_year > 0.0)) {
        return kNoRatio;
    }

    double mean_return = calculate_mean(returns);
    double downside_vol =
        calculate_downside_volatility(returns, sessions_per_year, minimum_acceptable_return);

    if (!(downside_vol > 0.0)) {
        // No return under the target: there is no downside to divide by, so the ratio has no
        // value (T-8D R12). It was 999 with a mean at or above 0 and 0 with one below.
        return kNoRatio;
    }

    // Annualize the mean grid return: multiply by the grid's sessions a year
    double annualized_return = mean_return * sessions_per_year;
    return (annualized_return - minimum_acceptable_return) / downside_vol;
}

double BacktestMetricsCalculator::calculate_calmar_ratio(double annualized_return, double max_drawdown) const {
    // No drawdown to divide by: no value (T-8D R12). It was 999 or 0 by the return's sign.
    if (!(max_drawdown > 0.0)) {
        return kNoRatio;
    }
    return annualized_return / max_drawdown;
}

// ========== Volatility Metrics ==========

double BacktestMetricsCalculator::calculate_volatility(const std::vector<double>& returns,
                                                       double sessions_per_year) const {
    if (returns.empty()) {
        return 0.0;
    }

    // The live statistics' own function, so the two calculators give one number for one series:
    // the SAMPLE standard deviation (n - 1), two-pass, x sqrt(sessions a year). (A one-pass
    // E[r^2] - mean^2 suffers catastrophic cancellation for small daily returns and can round to
    // a slightly negative variance, turning sqrt() into NaN, which passes the `volatility <= 0`
    // guard in calculate_sharpe_ratio.)
    double volatility =
        LiveHistoricalMetricsCalculator::calculate_annualized_volatility(returns, sessions_per_year);

    // A constant series still leaves rounding dust (~1e-17) in the deviations;
    // collapse it to a clean 0 so the `volatility <= 0` guard in
    // calculate_sharpe_ratio treats the degenerate case as such (no value) instead
    // of dividing by dust and reporting an absurd Sharpe.
    if (volatility > 0.0 && volatility < 1e-12) {
        WARN("Volatility collapsed to 0 (rounding dust; constant or near-constant "
             "series of " + std::to_string(returns.size()) +
             " returns); Sharpe has no value -- distinguish flat from broken data upstream");
        return 0.0;
    }
    return volatility;
}

double BacktestMetricsCalculator::calculate_downside_volatility(
    const std::vector<double>& returns,
    double sessions_per_year,
    double target) const {
    // The live statistics' own function: the squared shortfalls are averaged over EVERY return
    // of the series (a return at or above target counts as a zero), then annualized with
    // sqrt(sessions a year).
    double downside_vol = LiveHistoricalMetricsCalculator::calculate_annualized_downside_deviation(
        returns, sessions_per_year, target);

    // Same rounding-dust collapse calculate_volatility applies: returns sitting a
    // hair below target otherwise leave a ~1e-17 denominator and Sortino explodes
    // to an absurd finite value instead of having no value.
    if (downside_vol > 0.0 && downside_vol < 1e-12) {
        WARN("Downside volatility collapsed to 0 (rounding dust " +
             std::to_string(downside_vol) + "); Sortino has no value");
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

    // The index calculate_var_95 reads: the tail is the returns up to AND INCLUDING the VaR
    // observation (T-8D R36), so CVaR is never smaller than VaR.
    size_t var_index = static_cast<size_t>(returns.size() * 0.05);
    if (var_index >= sorted_returns.size()) {
        var_index = sorted_returns.size() - 1;
    }

    double cvar_sum = 0.0;
    for (size_t i = 0; i <= var_index; ++i) {
        cvar_sum += sorted_returns[i];
    }

    return -cvar_sum / static_cast<double>(var_index + 1);
}

std::unordered_map<std::string, double> BacktestMetricsCalculator::calculate_risk_metrics(
    const std::vector<double>& returns,
    double sessions_per_year) const {
    std::unordered_map<std::string, double> metrics;

    if (returns.empty()) {
        return metrics;
    }

    metrics["var_95"] = calculate_var_95(returns);
    metrics["cvar_95"] = calculate_cvar_95(returns);
    metrics["downside_volatility"] = calculate_downside_volatility(returns, sessions_per_year, 0.0);

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

// The rank of a row inside a bar: ROLL legs, then the STRATEGY fills, then the BORROW rows
// (LOOP_SPEC section 6.5).
int type_rank(const ExecutionReport& e) {
    return e.execution_type == ExecutionType::ROLL       ? 0
           : e.execution_type == ExecutionType::STRATEGY ? 1
                                                         : 2;
}

double signed_quantity(const ExecutionReport& e) {
    const double quantity = static_cast<double>(e.filled_quantity);
    return e.side == Side::BUY ? quantity : -quantity;
}

// The pairing of the account's fills into trades, the one walk the trade statistics and the
// per-symbol P&L both read (T-8D R11), so the two cannot part. Its rows are
// BacktestMetricsCalculator::account_fills' (the book's fills, netted between sleeves).
//
//   the tracker    one per contract: the account's net position and its average entry price.
//   the rows       STRATEGY rows only. A ROLL leg and a BORROW row move no position and realise
//                  nothing; a roll's leg gap is carried into the open entry once per contract
//                  (section 6.5, RollEntryCarry) before the bar's first row is scored.
//   the dollars    what the closed quantity made: quantity x (fill price - entry) x the side of the
//                  position x the point value of the symbol, from the source the equity curve
//                  reads (BacktestPnLManager::get_point_value: the metadata).
//   the flip       a fill that takes the position through zero closes what was held and opens the
//                  remainder at the fill price: the entry is reset there.
//   flat           a position under 1e-9 contracts or shares after a fill is flat (T-8D R11 (d)):
//                  fractional share quantities summed in binary leave a residue near 1e-14 after a
//                  full close, which is no position; the next fill then opens, it is not a trade.
//                  Stored quantities carry six decimals, so no real position is that small.
class TradePairing {
public:
    static constexpr double kFlatBelow = 1e-9;  ///< contracts or shares

    struct Fill {
        bool is_closing{false};  ///< reduces, closes or flips the position held before it
        double realized{0.0};    ///< dollars the closed quantity made, before any cost
        double position_before{0.0};
        double signed_quantity{0.0};
        double position_after{0.0};
    };

    TradePairing(const std::vector<ExecutionReport>& account_rows,
                 const BacktestMetricsCalculator::PointValueSource& point_value)
        : point_value_(point_value), roll_carry_(account_rows) {}

    /// The next row of the list the pairing was made on, applied to its contract's tracker.
    Fill apply(const ExecutionReport& exec) {
        Tracker& t = trackers_[exec.symbol];
        Fill fill;
        const double fill_price = static_cast<double>(exec.fill_price);
        const double signed_qty = signed_quantity(exec);
        const double current_pos = t.position;
        fill.position_before = fill.position_after = current_pos;
        fill.signed_quantity = signed_qty;

        // The bar's rolls, carried into the open entry before the bar's first row is scored.
        for (const double gap : roll_carry_.take(exec.symbol, exec.fill_time, current_pos)) {
            t.entry_price += gap;
        }
        if (exec.execution_type != ExecutionType::STRATEGY) return fill;

        if (current_pos == 0.0) {
            // Opening new position
            t.position = signed_qty;
            t.entry_price = fill_price;
        } else if ((current_pos > 0 && signed_qty > 0) || (current_pos < 0 && signed_qty < 0)) {
            // Adding to existing position
            double total_value = current_pos * t.entry_price + signed_qty * fill_price;
            t.position = current_pos + signed_qty;
            if (t.position != 0.0) {
                t.entry_price = total_value / t.position;
            }
        } else {
            // Reducing or closing position - realize P&L, in dollars
            double close_qty = std::min(std::abs(signed_qty), std::abs(current_pos));
            fill.realized = close_qty * (fill_price - t.entry_price) *
                            (current_pos > 0 ? 1.0 : -1.0) * point_value_of(exec.symbol);
            t.position = current_pos + signed_qty;
            if (std::abs(t.position) < kFlatBelow) {
                // Flat: a residue of the sum is no position and carries no entry.
                t.position = 0.0;
                t.entry_price = 0.0;
            } else if ((current_pos > 0 && t.position < 0) || (current_pos < 0 && t.position > 0)) {
                // Through zero: the remainder is a new position, opened at this fill's price.
                t.entry_price = fill_price;
            }
        }
        fill.position_after = t.position;
        fill.is_closing = std::abs(signed_qty) > 1e-6 && current_pos != 0.0 &&
            ((current_pos > 0 && signed_qty < 0) || (current_pos < 0 && signed_qty > 0));
        return fill;
    }

private:
    struct Tracker {
        double position{0.0};
        double entry_price{0.0};
    };

    double point_value_of(const std::string& symbol) {
        auto it = point_values_.find(symbol);
        if (it == point_values_.end()) it = point_values_.emplace(symbol, point_value_(symbol)).first;
        return it->second;
    }

    const BacktestMetricsCalculator::PointValueSource& point_value_;
    RollEntryCarry roll_carry_;
    std::unordered_map<std::string, Tracker> trackers_;
    std::unordered_map<std::string, double> point_values_;
};

}  // namespace

// ========== The Account's Fills ==========

std::vector<ExecutionReport> BacktestMetricsCalculator::account_fills(
    const std::vector<ExecutionReport>& executions) {
    // The STRATEGY rows of one contract on one bar, over every sleeve.
    struct Day {
        std::vector<const ExecutionReport*> rows;
    };
    std::map<std::pair<Timestamp, std::string>, Day> days;
    std::vector<ExecutionReport> out;
    for (const auto& exec : executions) {
        if (exec.execution_type == ExecutionType::STRATEGY) {
            days[{exec.fill_time, exec.symbol}].rows.push_back(&exec);
        } else {
            out.push_back(exec);
        }
    }
    for (const auto& [key, day] : days) {
        if (day.rows.size() == 1) {
            out.push_back(*day.rows.front());  // one sleeve's row is the account's fill
            continue;
        }
        Decimal net, own_cost, adjustment, commissions, slippage;
        for (const ExecutionReport* row : day.rows) {
            if (row->side == Side::BUY) {
                net += row->filled_quantity;
            } else {
                net -= row->filled_quantity;
            }
            own_cost += row->total_transaction_costs;
            adjustment += row->netting_adjustment;
            commissions += row->commissions_fees;
            slippage += row->slippage_market_impact;
        }
        const bool crossed = net == Decimal();
        // A full cross: the account sent no order, so there is no fill. Its sleeves' costs after
        // netting sum to 0 at one price; a sum that is not 0 (the sleeves filled at different
        // prices, never netted) is kept on a row of quantity 0, which is a cost and no fill.
        if (crossed && own_cost == adjustment) continue;
        const Side side = crossed ? day.rows.front()->side : (Decimal() < net ? Side::BUY : Side::SELL);
        // The price: the one price the sleeves filled at; when they differ, the quantity-weighted
        // price of the fills on the side of the net change.
        const ExecutionReport* first = nullptr;
        bool one_price = true;
        double side_quantity = 0.0, side_value = 0.0;
        for (const ExecutionReport* row : day.rows) {
            if (row->fill_price != day.rows.front()->fill_price) one_price = false;
            if (row->side != side) continue;
            if (!first) first = row;
            side_quantity += static_cast<double>(row->filled_quantity);
            side_value += static_cast<double>(row->filled_quantity) *
                          static_cast<double>(row->fill_price);
        }
        ExecutionReport fill = *first;
        fill.side = side;
        fill.filled_quantity = crossed ? Decimal() : (side == Side::BUY ? net : Decimal() - net);
        if (!one_price && side_quantity > 0.0) fill.fill_price = Price(side_value / side_quantity);
        fill.total_transaction_costs = own_cost;
        fill.netting_adjustment = adjustment;
        fill.commissions_fees = commissions;
        fill.slippage_market_impact = slippage;
        out.push_back(std::move(fill));
    }
    // A stated order, whatever order the rows came in: by bar, then ROLL legs, STRATEGY fills,
    // BORROW rows, then by contract; the legs and BORROW rows of one contract and bar by exec id
    // (shorter first, so RL-<sleeve>-9 precedes RL-<sleeve>-10), then by what they are.
    std::sort(out.begin(), out.end(), [](const ExecutionReport& a, const ExecutionReport& b) {
        if (a.fill_time != b.fill_time) return a.fill_time < b.fill_time;
        if (type_rank(a) != type_rank(b)) return type_rank(a) < type_rank(b);
        if (a.symbol != b.symbol) return a.symbol < b.symbol;
        if (a.exec_id.size() != b.exec_id.size()) return a.exec_id.size() < b.exec_id.size();
        if (a.exec_id != b.exec_id) return a.exec_id < b.exec_id;
        if (a.instrument_id != b.instrument_id) return a.instrument_id < b.instrument_id;
        if (a.side != b.side) return a.side < b.side;
        if (a.fill_price != b.fill_price) return a.fill_price < b.fill_price;
        return a.filled_quantity < b.filled_quantity;
    });
    return out;
}

// ========== The Counts Of The Account's Fills ==========

BacktestMetricsCalculator::FillCounts BacktestMetricsCalculator::account_fill_counts(
    const std::vector<ExecutionReport>& executions) {
    // calculate_trade_statistics' walk and its three tests, on the same rows. A count reads no
    // dollars: no point value, and no entry price, so the pairing is made without the ROLL legs
    // and carries no leg gap. The legs are counted as they are stored; legs whose ids cannot be
    // paired for a gap (a live book's two sleeves store one roll's legs under one exec id) are
    // therefore counted here, where the dollar statistics refuse them.
    FillCounts counts;
    const std::vector<ExecutionReport> rows = account_fills(executions);
    std::vector<ExecutionReport> no_legs;
    for (const auto& exec : rows) {
        if (exec.execution_type != ExecutionType::ROLL) no_legs.push_back(exec);
    }
    const PointValueSource unit = [](const std::string&) { return 1.0; };
    TradePairing pairing(no_legs, unit);
    for (const auto& exec : rows) {
        const TradePairing::Fill fill = pairing.apply(exec);
        if (exec.execution_type == ExecutionType::ROLL) {
            counts.roll_fills++;
            continue;
        }
        if (exec.execution_type == ExecutionType::BORROW) continue;
        if (fill.signed_quantity != 0.0) counts.strategy_fills++;
        if (fill.is_closing) counts.round_trips++;
    }
    return counts;
}

// ========== Trade Statistics ==========

BacktestMetricsCalculator::TradeStatistics BacktestMetricsCalculator::calculate_trade_statistics(
    const std::vector<ExecutionReport>& executions, const PointValueSource& point_value) const {
    TradeStatistics stats;

    // The book's fills (netted between sleeves), in the stated order; one tracker per contract.
    const std::vector<ExecutionReport> rows = account_fills(executions);
    TradePairing pairing(rows, point_value);
    std::map<std::string, Timestamp> open_times;         // symbol -> first trade time
    std::vector<double> holding_periods;

    for (const auto& exec : rows) {
        const std::string& symbol = exec.symbol;
        const TradePairing::Fill fill = pairing.apply(exec);

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

        // The account's fills: a row of quantity 0 is a crossed day's cost, not a fill.
        if (fill.signed_quantity != 0.0) stats.strategy_fills++;

        // A trade is a closing fill: the dollars its closed quantity made less the fill's own cost
        // after netting. The opening fills' costs are not inside a trade, with one exception: a
        // fill through zero (a reversal) is ONE account fill with one cost, and its whole cost,
        // the part that opens the new position included, is charged to the trade it closes.
        double commission = static_cast<double>(transaction_cost::net_cost(exec));
        double trade_pnl = -commission;
        trade_pnl += fill.realized;

        if (fill.position_before == 0.0) {
            open_times[symbol] = exec.fill_time;
        }

        if (fill.is_closing) {
            stats.actual_trades.push_back(exec);
            INFO("DEBUG_TRADE: " + symbol + " pos=" + std::to_string(fill.position_before) +
                 " qty=" + std::to_string(fill.signed_quantity) + " -> " +
                 std::to_string(fill.position_after));

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

    // The profit factor divides by the losing trades' loss; with none it has no value (T-8D
    // R12). It was 999 with a profit and no loss, and 0 with neither.
    stats.profit_factor = stats.total_loss > 0 ? stats.total_profit / stats.total_loss : kNoRatio;

    if (!holding_periods.empty()) {
        stats.avg_holding_period = std::accumulate(holding_periods.begin(),
            holding_periods.end(), 0.0) / holding_periods.size();
    }

    return stats;
}

// ========== Per-Symbol Analysis ==========

std::map<std::string, double> BacktestMetricsCalculator::calculate_symbol_pnl(
    const std::vector<ExecutionReport>& executions, const PointValueSource& point_value) const {
    // The pairing of calculate_trade_statistics (the same walk over the account's fills: in
    // dollars, the entry carried across a roll and reset on a flip), summed per symbol, with every
    // row's cost charged: an opening fill's, a ROLL leg's and a BORROW row's too (X-4), each
    // after netting. No trade P&L is scored on a leg.
    const std::vector<ExecutionReport> rows = account_fills(executions);
    TradePairing pairing(rows, point_value);
    std::map<std::string, double> symbol_pnl_map;

    for (const auto& exec : rows) {
        const TradePairing::Fill fill = pairing.apply(exec);
        double commission = static_cast<double>(transaction_cost::net_cost(exec));

        double trade_pnl = -commission;
        trade_pnl += fill.realized;

        symbol_pnl_map[exec.symbol] += trade_pnl;
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

// ========== The Statistics Grid ==========

std::vector<std::pair<Timestamp, double>> BacktestMetricsCalculator::statistics_grid_curve(
    const std::vector<std::pair<Timestamp, double>>& curve,
    const BacktestStatisticsGrid& grid) const {
    if (grid.own_rows || curve.empty()) {
        return curve;
    }

    std::unordered_map<std::string, Timestamp> row_time;
    for (const auto& [timestamp, level] : curve) {
        (void)level;
        row_time[core::format_utc_date(timestamp)] = timestamp;
    }
    const StatisticsSeries series = statistics_series(curve, grid);

    std::vector<std::pair<Timestamp, double>> folded;
    folded.reserve(series.levels.size() + 1);
    folded.push_back(curve.front());
    for (size_t i = 0; i < series.dates.size(); ++i) {
        Timestamp timestamp;
        const auto row = row_time.find(series.dates[i]);
        if (row != row_time.end()) {
            timestamp = row->second;
        } else if (!core::parse_utc_date(series.dates[i], timestamp)) {
            continue;
        }
        folded.emplace_back(timestamp, series.levels[i]);
    }
    return folded;
}

StatisticsSeries BacktestMetricsCalculator::statistics_series(
    const std::vector<std::pair<Timestamp, double>>& curve,
    const BacktestStatisticsGrid& grid) const {
    if (curve.empty()) {
        return StatisticsSeries();
    }

    // The live statistics' own fold (T-8D R1, R6): the curve's first row is the book's start and
    // its level the base, so it is never a return row; every later grid date takes the last
    // level on or before it. A run on its own rows has every row's date as its grid.
    std::vector<DatedLevel> levels;
    std::vector<std::string> own_dates;
    levels.reserve(curve.size());
    for (const auto& [timestamp, level] : curve) {
        levels.push_back({core::format_utc_date(timestamp), level});
        if (grid.own_rows) own_dates.push_back(levels.back().date);
    }
    return build_statistics_series(levels, grid.own_rows ? own_dates : grid.dates,
                                   levels.front().date, curve.front().second, levels.back().date);
}

// ========== Composite Calculation ==========

backtest::BacktestResults BacktestMetricsCalculator::calculate_all_metrics(
    const std::vector<std::pair<Timestamp, double>>& equity_curve,
    const std::vector<ExecutionReport>& executions,
    int warmup_days,
    const PointValueSource& point_value,
    const BacktestStatisticsGrid& grid,
    const BacktestStatisticsInputs& statistics_inputs) const {
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

    // Basic performance metrics: a level, on every row
    results.total_return = calculate_total_return(
        filtered_curve.front().second, filtered_curve.back().second);

    // The statistics grid (T-8D R1, R3): the rows after the warm-up folded onto the grid's
    // dates, and the returns taken on its levels. Every annualised figure below uses the grid's
    // own sessions a year, which the result records.
    const double sessions_per_year = grid.sessions_per_year;
    const auto grid_curve = statistics_grid_curve(filtered_curve, grid);
    auto returns = calculate_returns_from_equity(grid_curve);
    results.statistics_sessions_per_year = sessions_per_year;
    results.statistics_days = static_cast<int>(grid_curve.size()) - 1;

    // Volatility metrics
    results.volatility = calculate_volatility(returns, sessions_per_year);

    // Risk-adjusted metrics
    results.sharpe_ratio = calculate_sharpe_ratio(returns, sessions_per_year);
    results.sortino_ratio = calculate_sortino_ratio(returns, sessions_per_year);

    // Drawdown metrics: the curve of every row, and the maximum over the grid's levels
    results.drawdown_curve = calculate_drawdowns(filtered_curve);
    results.max_drawdown = calculate_max_drawdown(grid_curve);

    // Calmar ratio: the CAGR on the grid, (1 + R)^(K / n) - 1 with R from the base to the last
    // grid level and n the grid's returns, over the maximum drawdown (T-8D R36)
    // Under one year of sessions (n < K) a return is not annualised: the annualised return and
    // the Calmar ratio have no value (T-8D R40 as T-8D-2 R78 extends it to the backtest).
    if (under_one_year_of_sessions(results.statistics_days, sessions_per_year)) {
        results.annualized_return = kNoRatio;
        results.calmar_ratio = kNoRatio;
    } else {
        results.annualized_return = calculate_annualized_return(
            calculate_total_return(grid_curve.front().second, grid_curve.back().second),
            results.statistics_days, sessions_per_year);
        results.calmar_ratio =
            calculate_calmar_ratio(results.annualized_return, results.max_drawdown);
    }

    // Risk metrics
    auto risk_metrics = calculate_risk_metrics(returns, sessions_per_year);
    results.var_95 = risk_metrics["var_95"];
    results.cvar_95 = risk_metrics["cvar_95"];
    results.downside_volatility = risk_metrics["downside_volatility"];

    // Beta and correlation
    auto [beta, correlation] = calculate_beta_correlation(returns);
    results.beta = beta;
    results.correlation = correlation;

    // Trade statistics
    auto trade_stats = calculate_trade_statistics(executions, point_value);
    results.total_trades = trade_stats.total_trades;
    results.win_rate = trade_stats.win_rate;
    results.profit_factor = trade_stats.profit_factor;
    results.avg_win = trade_stats.avg_win;
    results.avg_loss = trade_stats.avg_loss;
    results.max_win = trade_stats.max_win;
    results.max_loss = trade_stats.max_loss;
    results.avg_holding_period = trade_stats.avg_holding_period;
    results.actual_trades = trade_stats.actual_trades;

    // The statistics of migration 031 (LOOP_SPEC sections 7.4 and 10), on the same rows as the
    // statistics above: the grid series of the rows after the warm-up, whose first row is the
    // base. The fill count is the trade statistics' own count of the account's fills.
    results.statistics_columns = backtest_statistics_columns(
        statistics_series(filtered_curve, grid), sessions_per_year,
        core::format_utc_date(filtered_curve.front().first),
        core::format_utc_date(filtered_curve.back().first), statistics_inputs);
    results.statistics_columns.total_strategy_fills = trade_stats.strategy_fills;

    // Per-symbol P&L
    auto symbol_pnl = calculate_symbol_pnl(executions, point_value);
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

std::vector<std::pair<Timestamp, double>> BacktestMetricsCalculator::filter_warmup_period(
    const std::vector<std::pair<Timestamp, double>>& equity_curve,
    int warmup_days) const {
    if (warmup_days <= 0 || equity_curve.size() <= static_cast<size_t>(warmup_days)) {
        return equity_curve;
    }

    return std::vector<std::pair<Timestamp, double>>(
        equity_curve.begin() + warmup_days, equity_curve.end());
}

std::string statistics_convention_line(const BacktestStatisticsGrid& grid, bool futures_series,
                                       const backtest::BacktestResults& results,
                                       size_t curve_rows) {
    return std::string("STATISTICS_CONVENTION series=") + (futures_series ? "futures" : "equities") +
           " K=" + std::to_string(results.statistics_sessions_per_year) + " grid=" +
           (grid.own_rows ? std::string("the run's own rows")
                          : "Sunday to Friday with at least " +
                                std::to_string(kStatisticsGridMinSymbols) +
                                " stored symbols printing (" + std::to_string(grid.dates.size()) +
                                " dates of the window)") +
           " n=" + std::to_string(results.statistics_days) + " curve_rows=" +
           std::to_string(curve_rows) + " warmup_days=" + std::to_string(results.warmup_days) +
           " annualized_return=" +
           (has_value(results.annualized_return) ? std::to_string(results.annualized_return)
                                                 : std::string("n/a"));
}

} // namespace trade_ngin
