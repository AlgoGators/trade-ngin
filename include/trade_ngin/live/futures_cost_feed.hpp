// include/trade_ngin/live/futures_cost_feed.hpp
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

namespace trade_ngin {

/**
 * @brief K2 redesigned (T-7b-1 C8a, HD 2026-09-17 / 2026-09-19): the live futures runners'
 *        cost feed, with two inputs.
 *
 * The cost of a fill of q contracts at the T-1 close P (transaction_cost_manager.cpp,
 * impact_model.cpp, spread_model.cpp):
 *
 *   impact  = min(k_bps(V) * sqrt(clamp(q / max(V, 100), 0, 0.1)), max_impact_bps) / 1e4 * P
 *   spread  = spread_cost_multiplier * clamp(baseline_ticks * vol_mult, min_ticks, max_ticks)
 *             * tick_size
 *   implicit = spread + impact (capped at max_total_implicit_bps); slippage = implicit * q *
 *   point_value; total = commission + slippage. k_bps(V) is the liquidity tier (10/20/40/60/80
 *   bps for V above 1M/200k/50k/20k/below).
 *
 * The two inputs, as ruled:
 *
 *   V        the FILL DAY'S OWN VOLUME: the volume of the symbol's latest bar in the feed,
 *            which is the T-1 bar whose close prices the fill. It prices the participation
 *            term AND keys the k_bps tier, so a thin session is charged its own cost. The
 *            impact model's window is given exactly this one observation, so its "ADV" is V.
 *   vol_mult from the walk of the symbol's consecutive log returns ending at that T-1 bar:
 *            the spread model keeps the last 20 (SpreadModel lookback_days), so vol_mult =
 *            clip(1 + 0.15 * clip((stdev_20 - 0.01) / 0.005, -2, 2), 0.8, 1.5). The first bar
 *            of a symbol has no previous close, so it contributes no return (nothing
 *            fabricated); fewer than 2 returns leave the model's neutral 1.0.
 *
 * Before this, both runners fed ONE bar per symbol through ExecutionManager's 3-arg form: V was
 * the same own-day volume, and the one return was log(close / close) = 0, so vol_mult was
 * always 1.0. The impact term is therefore unchanged by construction; only the spread term
 * moves, through vol_mult.
 *
 * The weekend merge (T-7b-2 C8c3, HD 2026-09-25 rulings 25 and 28: "merge the weekend stub into the
 * next session for the participation volume of a weekday fill; a fill in the thin session itself
 * keeps its own volume, per the 09-17 ruling"; a Monday fill's cost comes back to real session
 * liquidity). Only V moves; the volatility walk does not. A run dated T (the fill day) prices its fill
 * at the symbol's latest bar B, the T-1 bar. A weekend bar is one dated Saturday or Sunday (UTC); a
 * session is a weekday-dated bar; the weekend block is the run of the symbol's weekend bars with no
 * session between them. The fill day is a weekday or a weekend day by its UTC date.
 *
 *   (1) B is a session: V = B's volume + the weekend block right before B, if any. A Tuesday-dated
 *       run: Monday + the Sunday stub (+ Saturday where one printed).
 *   (2) B is a weekend bar and the fill day is a weekday: V = the volume of the symbol's last
 *       session before the block (its own volume, nothing merged into it) + the whole block, B
 *       included. A Monday-dated run: Friday + the stub(s); Monday's bar does not exist yet.
 *   (3) Otherwise V = B's own volume: B is a session with no weekend bar right before it (Tuesday to
 *       Friday, a thin weekday session, a Monday of a contract that prints no Sunday session), or
 *       B is a weekend bar and the fill day is a weekend day (a Sunday-dated run whose T-1 bar is
 *       MBT's Saturday session: the fill is in the thin session itself).
 *
 * Holidays: no calendar is read. A holiday, for a symbol, is a weekday on which it printed no bar,
 * so its weekend block merges into the next session it prints: after a Monday with no bar, a
 * Tuesday-dated run is (2) (Friday + the stub) and a Wednesday-dated run is (1) (Tuesday + the
 * stub). A weekday bar printed on an exchange holiday (the US holiday Mondays, when every symbol
 * with a Sunday session prints a Monday bar) is a session like any other and takes the block.
 * A symbol whose first bars are a weekend block has no session before it: (2) is the block alone.
 *
 * The feed is the runner's strategy feed (a JUNK symbol's T-1 bar withheld, T-7b-1 C7b R10),
 * already one bar per symbol-instant (the futures loader keeps the max-volume copy, T-6c B0). It
 * is NOT de-duplicated again here: a repeated instant is fed as given and reported, so a loader
 * regression shows in the log instead of being papered over.
 *
 * Only the cost manager passed in is fed. The live futures runners pass the ExecutionManager's
 * (C8a) and, from the same feed, the PortfolioManager's, whose model prices the optimizer's cost
 * vector (T-7b-1 C8d, H-2). The futures backtest feeds its two managers the same basis one cycle
 * at a time (feed_futures_cost_model_step below, T-7b-2 8c); the equity feed
 * (LiveDailyCycle::feed_cost_model) is untouched; the equity runners' optimizer is off.
 */
struct FuturesCostFeedSymbol {
    std::string symbol;
    size_t bars = 0;             ///< bars of this symbol in the feed
    size_t returns = 0;          ///< log returns handed to the volatility window (all of them;
                                 ///< the window keeps the last 20)
    double own_day_volume = 0.0; ///< the latest bar's own volume
    Timestamp own_day_time{};    ///< the latest bar's timestamp
    size_t merged_weekend_bars = 0;       ///< weekend bars in the participation volume (C8c3)
    double merged_weekend_volume = 0.0;   ///< their volume (B's own included when B is one)
    double previous_session_volume = 0.0; ///< case (2): the last session's volume before the block
    double participation_volume = 0.0;    ///< the impact model's only input (C8c3 cases 1-3)
};

namespace futures_cost_feed_detail {
/// The UTC day number of an instant (floor), and whether it is a Saturday or a Sunday.
inline bool is_weekend_day(Timestamp t) {
    const long long secs =
        std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    const long long days = (secs >= 0 ? secs : secs - 86399) / 86400;  // floor
    const long long weekday = ((days + 4) % 7 + 7) % 7;  // 1970-01-01 was a Thursday; 0 = Sunday
    return weekday == 0 || weekday == 6;
}
}  // namespace futures_cost_feed_detail

/// A bar dated Saturday or Sunday (the UTC date of its timestamp): a weekend bar.
inline bool is_weekend_bar(const Bar& bar) {
    return futures_cost_feed_detail::is_weekend_day(bar.timestamp);
}

/// A fill day (a run's date T, or a backtest cycle's) that is a Monday to Friday by its UTC date.
inline bool is_weekday_fill(Timestamp fill_time) {
    return !futures_cost_feed_detail::is_weekend_day(fill_time);
}

/**
 * @brief One symbol's volumes for the weekend merge, built from its bars oldest first (C8c3). Both
 *        feeds below build it with the same add(), so the one-shot and the per-cycle form agree.
 */
struct FuturesSessionVolume {
    bool has_latest = false;
    bool latest_is_weekend = false;
    double latest_volume = 0.0;
    Timestamp latest_time{};
    /// latest a session: the weekend block right before it; latest a weekend bar: the block so
    /// far, the latest included
    double block_volume = 0.0;
    size_t block_bars = 0;
    bool has_previous_session = false;    ///< latest a weekend bar: a session precedes the block
    double previous_session_volume = 0.0; ///< that session's own volume

    void add(const Bar& bar) {
        const bool weekend = is_weekend_bar(bar);
        if (weekend) {
            if (!has_latest || !latest_is_weekend) {  // a block starts after a session (or first)
                has_previous_session = has_latest;
                previous_session_volume = has_latest ? latest_volume : 0.0;
                block_volume = 0.0;
                block_bars = 0;
            }
            block_volume += bar.volume;
            ++block_bars;
        } else if (!has_latest || !latest_is_weekend) {  // a session after a session: no block
            block_volume = 0.0;
            block_bars = 0;
        }  // else a session right after a block: the block is merged into it
        has_latest = true;
        latest_is_weekend = weekend;
        latest_volume = bar.volume;
        latest_time = bar.timestamp;
    }

    /// Cases (1)-(3) of the rule above; fills entry's merge fields and returns V.
    double participation(bool weekday_fill, FuturesCostFeedSymbol& entry) const {
        entry.merged_weekend_bars = 0;
        entry.merged_weekend_volume = 0.0;
        entry.previous_session_volume = 0.0;
        if (!latest_is_weekend) {                      // (1), or (3) when no block precedes it
            entry.merged_weekend_bars = block_bars;
            entry.merged_weekend_volume = block_volume;
            entry.participation_volume = latest_volume + block_volume;
        } else if (weekday_fill) {                     // (2)
            entry.merged_weekend_bars = block_bars;
            entry.merged_weekend_volume = block_volume;
            entry.previous_session_volume = has_previous_session ? previous_session_volume : 0.0;
            entry.participation_volume = entry.previous_session_volume + block_volume;
        } else {                                       // (3), a weekend fill in the thin session
            entry.participation_volume = latest_volume;
        }
        return entry.participation_volume;
    }
};

struct FuturesCostFeedResult {
    std::vector<FuturesCostFeedSymbol> symbols;  ///< one entry per fed symbol, by symbol
    size_t returns_fed = 0;
    std::vector<std::string> thin;               ///< fewer than min_bars bars (< 20 returns)
    std::vector<std::string> repeated_instants;  ///< a symbol with two bars at one instant
    /// per-cycle form only: symbols with no bar in the cycle whose participation volume was reset
    /// because the fill day changed class (a weekend bar as B, rule 2 vs 3)
    std::vector<FuturesCostFeedSymbol> reevaluated;
};

/// `fill_time`: the run's date T (the day the fill is made), which decides rule (2) vs (3).
inline FuturesCostFeedResult feed_futures_cost_model(
    transaction_cost::TransactionCostManager& tcm, const std::vector<Bar>& feed_bars,
    Timestamp fill_time, size_t min_bars = 21) {
    std::map<std::string, std::vector<Bar>> by_symbol;
    for (const auto& bar : feed_bars) by_symbol[bar.symbol].push_back(bar);
    const bool weekday_fill = is_weekday_fill(fill_time);

    FuturesCostFeedResult out;
    for (auto& [symbol, bars] : by_symbol) {
        std::stable_sort(bars.begin(), bars.end(),
                         [](const Bar& a, const Bar& b) { return a.timestamp < b.timestamp; });

        FuturesCostFeedSymbol entry;
        entry.symbol = symbol;
        entry.bars = bars.size();

        // The volatility term: every consecutive return, oldest first; the window keeps the
        // last 20, which end at the T-1 bar. 0.0 = "no previous close" on the first bar.
        FuturesSessionVolume sessions;
        double prev_close = 0.0;
        std::string last_id;  // T-ROLLX: the last known contract id fed (roll_series.hpp)
        for (size_t i = 0; i < bars.size(); ++i) {
            if (i > 0 && bars[i].timestamp == bars[i - 1].timestamp &&
                (out.repeated_instants.empty() || out.repeated_instants.back() != symbol)) {
                out.repeated_instants.push_back(symbol);
            }
            const double close = static_cast<double>(bars[i].close);
            // A bar whose contract id differs from the last known one is a change bar: its
            // return enters the volatility window as 0 (LOOP_SPEC v6.1 section 2.3, L-08).
            const bool change_bar = !bars[i].instrument_id.empty() && !last_id.empty() &&
                                    bars[i].instrument_id != last_id;
            if (!bars[i].instrument_id.empty()) last_id = bars[i].instrument_id;
            tcm.record_log_return(symbol, close, prev_close, change_bar);
            if (prev_close > 0.0 && close > 0.0) ++entry.returns;
            prev_close = close;
            sessions.add(bars[i]);
        }

        // The impact term: one observation, the participation volume of the weekend merge (C8c3).
        entry.own_day_volume = bars.back().volume;
        entry.own_day_time = bars.back().timestamp;
        tcm.record_volume(symbol, sessions.participation(weekday_fill, entry));

        out.returns_fed += entry.returns;
        if (bars.size() < min_bars) out.thin.push_back(symbol);
        out.symbols.push_back(entry);
    }
    return out;
}

/**
 * @brief The same two inputs for the futures backtest (T-7b-2 8c, COST-H3, T-VOL §4), one cycle at
 *        a time.
 *
 * A live run feeds a fresh manager the whole window ending at T-1 once. The backtest's managers
 * live for the whole run, so each cycle feeds only the bars that are new to them, the cycle's
 * signal feed (the T-1 group the strategies and the PortfolioManager are fed, a JUNK bar withheld
 * on its own cycle and fed on the next ahead of the symbol's newer bar): each bar's return against
 * the symbol's previously fed close (`carry`, one per manager; none on a symbol's first bar), and
 * the impact model's window SET to the symbol's participation volume on the cycle's fill day. A
 * symbol with no bar in the cycle keeps its returns and its volumes; its participation volume is
 * reset only when the fill day's class changes it (its latest bar a weekend bar: rule 2 on a weekday
 * cycle, rule 3 on a weekend one). After every cycle the manager holds, for every symbol fed so far,
 * exactly what feed_futures_cost_model gives on the same bars and the same fill day.
 */
struct FuturesCostFeedCarry {
    std::map<std::string, double> last_close;  ///< the close of the symbol's last fed bar
    std::map<std::string, std::string> last_id;  ///< T-ROLLX: the symbol's last known contract id
    std::map<std::string, FuturesSessionVolume> sessions;  ///< the weekend merge's state (C8c3)
    std::map<std::string, double> applied_volume;  ///< the participation volume last set
};

inline FuturesCostFeedResult feed_futures_cost_model_step(
    transaction_cost::TransactionCostManager& tcm, const std::vector<Bar>& new_bars,
    Timestamp fill_time, FuturesCostFeedCarry& carry) {
    std::map<std::string, std::vector<Bar>> by_symbol;
    for (const auto& bar : new_bars) by_symbol[bar.symbol].push_back(bar);
    const bool weekday_fill = is_weekday_fill(fill_time);

    FuturesCostFeedResult out;
    for (auto& [symbol, bars] : by_symbol) {
        std::stable_sort(bars.begin(), bars.end(),
                         [](const Bar& a, const Bar& b) { return a.timestamp < b.timestamp; });

        FuturesCostFeedSymbol entry;
        entry.symbol = symbol;
        entry.bars = bars.size();

        auto it = carry.last_close.find(symbol);
        double prev_close = it == carry.last_close.end() ? 0.0 : it->second;
        std::string& last_id = carry.last_id[symbol];
        auto& sessions = carry.sessions[symbol];
        for (size_t i = 0; i < bars.size(); ++i) {
            if (i > 0 && bars[i].timestamp == bars[i - 1].timestamp &&
                (out.repeated_instants.empty() || out.repeated_instants.back() != symbol)) {
                out.repeated_instants.push_back(symbol);
            }
            const double close = static_cast<double>(bars[i].close);
            const bool change_bar = !bars[i].instrument_id.empty() && !last_id.empty() &&
                                    bars[i].instrument_id != last_id;  // T-ROLLX, as above
            if (!bars[i].instrument_id.empty()) last_id = bars[i].instrument_id;
            tcm.record_log_return(symbol, close, prev_close, change_bar);
            if (prev_close > 0.0 && close > 0.0) ++entry.returns;
            prev_close = close;
            sessions.add(bars[i]);
        }
        carry.last_close[symbol] = prev_close;

        entry.own_day_volume = bars.back().volume;
        entry.own_day_time = bars.back().timestamp;
        const double v = sessions.participation(weekday_fill, entry);
        tcm.set_own_day_volume(symbol, v);
        carry.applied_volume[symbol] = v;

        out.returns_fed += entry.returns;
        out.symbols.push_back(entry);
    }

    // A symbol with no bar in this cycle whose latest bar is a weekend bar: rule (2) or (3) follows
    // the fill day, so its volume is reset when the class changed (e.g. MBT's Saturday bar, B on
    // both the Sunday-dated and the Monday-dated cycle when it prints no Sunday bar).
    for (auto& [symbol, sessions] : carry.sessions) {
        if (by_symbol.count(symbol) || !sessions.has_latest || !sessions.latest_is_weekend) continue;
        FuturesCostFeedSymbol entry;
        entry.symbol = symbol;
        entry.own_day_volume = sessions.latest_volume;
        entry.own_day_time = sessions.latest_time;
        const double v = sessions.participation(weekday_fill, entry);
        auto applied = carry.applied_volume.find(symbol);
        if (applied != carry.applied_volume.end() && applied->second == v) continue;
        tcm.set_own_day_volume(symbol, v);
        carry.applied_volume[symbol] = v;
        out.reevaluated.push_back(entry);
    }
    return out;
}

}  // namespace trade_ngin
