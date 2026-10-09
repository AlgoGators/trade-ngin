// include/trade_ngin/live/session_book_gate.hpp
#pragma once

#include <algorithm>
#include <cmath>
#include <ctime>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/session_classifier.hpp"
#include "trade_ngin/live/execution_manager.hpp"

namespace trade_ngin {

/**
 * The live futures runners' side of the per-symbol session rule (T-7a commit 4; HD 2026-09-17,
 * STAGE3_PLAN §27; T-CLASSIFIER_ADVERSARIAL verdict block, D1/D2). Both twins
 * (live_portfolio_conservative.cpp, live_portfolio.cpp) call these helpers in the same order:
 *
 *   1. classify every symbol's T-1 (classify_t1) and log it (log_t1_classification);
 *   2. above the live_run_metadata upsert: a held symbol whose feed hole is older than
 *      live.data_staleness_tolerance_days REFUSES the run when the run date is the host's date,
 *      and is a WARN on a replay of a past date (held_feed_holes_past_tolerance,
 *      run_date_is_host_date);
 *   3. the book-level rule stays the T-1 price map's `.empty()`: carry the whole book when no
 *      symbol printed; there is no abort arm any more (log_whole_book_carry names the reason);
 *   4. the JUNK symbols' T-1 bars are withheld from the strategy and portfolio feed
 *      (withhold_junk_t1_bars): their signal is not updated today. They stay in the T-1 price map,
 *      so they are marked at their bar's close;
 *   5. every symbol whose verdict is not SESSION is held at its stored T-1 quantity on EVERY
 *      per-strategy book (hold_non_session_symbols). The key is the verdict, never membership of
 *      the price map: a JUNK symbol HAS a T-1 price and would otherwise trade at the junk print;
 *   6. executions are generated with PricingPolicy::STRICT and a symbol that still could not be
 *      priced is rolled back to its stored row (execute_strategy_day_strict, the pattern of
 *      LiveDailyCycle::execute_day_t); afterwards no book change may be left without a price
 *      (unpriced_book_changes), or the run fails.
 */

using StrategyBooks = std::unordered_map<std::string, std::unordered_map<std::string, Position>>;

// ------------------------------------------------------------------------------------------------
// 1. Logging the day's classification
// ------------------------------------------------------------------------------------------------

/// One summary line, then JUNK WARN / feed hole ERROR / closure INFO per symbol.
inline void log_t1_classification(const T1Classification& t1) {
    INFO("T1_CLASSIFIER t1=" + t1.t1_date + " session=" + std::to_string(t1.session) +
         " junk=" + std::to_string(t1.junk) + " closure=" + std::to_string(t1.closure) +
         " feed_hole=" + std::to_string(t1.feed_hole) +
         " max_hole_age_days=" + std::to_string(t1.max_hole_age_days));
    for (const auto& v : t1.verdicts) {
        switch (v.verdict) {
            case SessionVerdict::SESSION:
                break;
            case SessionVerdict::JUNK:
                WARN("T1_CLASSIFIER JUNK " + v.symbol + " " + v.date + ": " + v.reason +
                     " -- marked at its close " + std::to_string(v.close) +
                     ", withheld from signal update, sizing and orders");
                break;
            case SessionVerdict::NO_BAR_FEED_HOLE:
                ERROR("T1_CLASSIFIER FEED HOLE " + v.symbol + " " + v.date + ": no bar; " +
                      v.reason + "; last bar " +
                      (v.last_bar_date.empty() ? std::string("none") : v.last_bar_date) + " (" +
                      std::to_string(v.hole_age_days) +
                      " day(s)) -- held at its last mark, no order");
                break;
            case SessionVerdict::NO_BAR_CLOSURE:
                INFO("T1_CLASSIFIER closure " + v.symbol + " " + v.date + ": " + v.reason +
                     " -- held at its last mark, no order");
                break;
        }
    }
}

/// The reason line of the whole-book carry (no symbol has a T-1 price). ERROR when any symbol
/// was expected to print (a book-level feed hole, e.g. the dead Sundays 2026-05-17 and 05-24),
/// INFO when every symbol is a closure.
inline void log_whole_book_carry(const T1Classification& t1) {
    const std::string line = "T1_CLASSIFIER no symbol has a T-1 price (t1=" + t1.t1_date +
                             ", closure=" + std::to_string(t1.closure) +
                             ", feed_hole=" + std::to_string(t1.feed_hole) +
                             "): the whole book is carried forward, no order";
    if (t1.feed_hole > 0) {
        ERROR(line + "; this is a FEED HOLE, not a closure: " +
              std::to_string(t1.feed_hole) + " symbol(s) normally print on this weekday");
    } else {
        INFO(line + "; every symbol is a closure");
    }
}

// ------------------------------------------------------------------------------------------------
// 2. The feed-hole refusal (true live only)
// ------------------------------------------------------------------------------------------------

struct HeldFeedHole {
    std::string symbol;
    int age_days{0};
    std::string last_bar_date;
    double held_quantity{0.0};
};

/**
 * @brief The held symbols (a non-zero stored T-1 quantity) whose feed hole is older than the
 *        tolerance. T-4c F4 / T-5 F7 as ruled 2026-09-19: the per-symbol form of the feed
 *        freshness guard, keyed on a HELD symbol; a hole on a symbol the book does not hold
 *        changes nothing and is only logged.
 */
inline std::vector<HeldFeedHole> held_feed_holes_past_tolerance(
    const T1Classification& t1, const std::unordered_map<std::string, Position>& held_book,
    int tolerance_days) {
    std::vector<HeldFeedHole> out;
    for (const auto& v : t1.verdicts) {
        if (v.verdict != SessionVerdict::NO_BAR_FEED_HOLE) continue;
        if (v.hole_age_days <= tolerance_days) continue;
        auto it = held_book.find(v.symbol);
        if (it == held_book.end()) continue;
        const double q = it->second.quantity.as_double();
        if (std::abs(q) <= 1e-9) continue;
        out.push_back({v.symbol, v.hole_age_days, v.last_bar_date, q});
    }
    return out;
}

/// The host-local calendar date of an instant, YYYY-MM-DD.
inline std::string local_ymd(const Timestamp& t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

/**
 * @brief True when the run is for the host's own date (HD 2026-09-19: the refusal is keyed on
 *        "run date equals the host's date"). The runners' run date is `now` in the host's local
 *        frame (the CLI date is parsed as local midnight, or `now` is the wall clock on a bare
 *        invocation), so both sides are compared as host-local dates. The date frame itself is
 *        not touched here.
 */
inline bool run_date_is_host_date(const Timestamp& run_now, const Timestamp& host_now) {
    return local_ymd(run_now) == local_ymd(host_now);
}

// ------------------------------------------------------------------------------------------------
// 4. JUNK bars withheld from the feed
// ------------------------------------------------------------------------------------------------

/**
 * @brief The bars with every JUNK symbol's T-1 bar removed. The strategy and the portfolio stage
 *        therefore compute that symbol from its history through T-2: its signal is not updated
 *        today (on the next run the bar is T-2 and part of the history again; a one-day deferral,
 *        T-CLASSIFIER_ADVERSARIAL D4). The price manager is NOT given this vector: the mark
 *        uses every bar received.
 */
inline std::vector<Bar> withhold_junk_t1_bars(const std::vector<Bar>& bars,
                                              const T1Classification& t1,
                                              std::vector<std::string>* withheld = nullptr) {
    if (withheld) withheld->clear();
    if (t1.junk_symbols.empty()) return bars;
    const std::set<std::string> junk(t1.junk_symbols.begin(), t1.junk_symbols.end());
    std::vector<Bar> out;
    out.reserve(bars.size());
    for (const auto& b : bars) {
        if (junk.count(b.symbol) && SessionClassifier::ymd(SessionClassifier::day_of(b.timestamp)) ==
                                        t1.t1_date) {
            if (withheld) withheld->push_back(b.symbol);
            continue;
        }
        out.push_back(b);
    }
    if (withheld) std::sort(withheld->begin(), withheld->end());
    return out;
}

// ------------------------------------------------------------------------------------------------
// 5. The hold
// ------------------------------------------------------------------------------------------------

struct BookHold {
    std::string strategy_name;
    std::string symbol;
    SessionVerdict verdict{SessionVerdict::NO_BAR_CLOSURE};
    double held_quantity{0.0};
    double target_quantity{0.0};
    bool reinserted{false};  ///< held yesterday, absent from today's target map
};

/**
 * @brief Holds every non-SESSION symbol at its stored T-1 quantity, on every per-strategy book.
 *
 * For each sleeve: a symbol whose verdict is not SESSION and whose target differs from the
 * sleeve's stored T-1 quantity gets that quantity back (flat if nothing was stored); the row's
 * mark is left as the strategy wrote it (the last bar's close, the last mark), and the runner
 * stores a JUNK symbol at its T-1 close because it is in the price map. A symbol held yesterday
 * but absent from today's target map is re-inserted from its stored row, so the execution step's
 * close-out loop cannot flatten it. Returns one record per held change; the caller logs and
 * rebuilds the combined book.
 */
inline std::vector<BookHold> hold_non_session_symbols(StrategyBooks& books,
                                                      const StrategyBooks& previous,
                                                      const T1Classification& t1,
                                                      const Timestamp& now) {
    static const std::unordered_map<std::string, Position> kEmpty;
    std::vector<BookHold> holds;
    // Deterministic order for the log.
    std::vector<std::string> names;
    for (const auto& [name, _] : books) names.push_back(name);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        auto& book = books[name];
        auto pit = previous.find(name);
        const auto& prev = pit != previous.end() ? pit->second : kEmpty;

        std::vector<std::string> symbols;
        for (const auto& [symbol, _] : book) symbols.push_back(symbol);
        std::sort(symbols.begin(), symbols.end());
        for (const auto& symbol : symbols) {
            if (t1.is_session(symbol)) continue;
            auto& pos = book[symbol];
            auto prev_it = prev.find(symbol);
            const double prev_qty =
                prev_it != prev.end() ? prev_it->second.quantity.as_double() : 0.0;
            const double target = pos.quantity.as_double();
            if (std::abs(target - prev_qty) <= 1e-6) continue;
            pos.quantity = Decimal(prev_qty);
            const auto* v = t1.find(symbol);
            holds.push_back({name, symbol, v ? v->verdict : SessionVerdict::NO_BAR_CLOSURE,
                             prev_qty, target, false});
        }

        std::vector<std::string> prev_symbols;
        for (const auto& [symbol, _] : prev) prev_symbols.push_back(symbol);
        std::sort(prev_symbols.begin(), prev_symbols.end());
        for (const auto& symbol : prev_symbols) {
            if (book.count(symbol) || t1.is_session(symbol)) continue;
            const Position& row = prev.at(symbol);
            if (std::abs(row.quantity.as_double()) <= 1e-6) continue;
            book[symbol] = row;
            book[symbol].last_update = now;
            const auto* v = t1.find(symbol);
            holds.push_back({name, symbol, v ? v->verdict : SessionVerdict::NO_BAR_CLOSURE,
                             row.quantity.as_double(), 0.0, true});
        }
    }
    return holds;
}

inline void log_book_holds(const std::vector<BookHold>& holds) {
    for (const auto& h : holds) {
        const std::string what = h.reinserted
                                     ? "absent from today's target, held at " +
                                           std::to_string(h.held_quantity) + "; no close-out today"
                                     : "book held at " + std::to_string(h.held_quantity) +
                                           " instead of target " +
                                           std::to_string(h.target_quantity) + "; no order today";
        WARN("BOOK_GATE " + h.symbol + " (" + h.strategy_name + "): T-1 " + to_string(h.verdict) +
             " -- " + what);
    }
    if (!holds.empty()) {
        INFO("BOOK_GATE held " + std::to_string(holds.size()) +
             " book change(s) on symbols without a T-1 session");
    }
}

/// The distinct held symbols, sorted (for live_results.config).
inline std::vector<std::string> held_symbols(const std::vector<BookHold>& holds) {
    std::set<std::string> s;
    for (const auto& h : holds) s.insert(h.symbol);
    return {s.begin(), s.end()};
}

/// The combined book as the runners build it: per symbol, the sum over sleeves (Σ qᵢ).
inline void rebuild_combined_positions(std::unordered_map<std::string, Position>& positions,
                                       const StrategyBooks& books) {
    positions.clear();
    for (const auto& [_, pos_map] : books) {
        for (const auto& [symbol, pos] : pos_map) {
            auto it = positions.find(symbol);
            if (it == positions.end()) {
                positions[symbol] = pos;
            } else {
                it->second.quantity += pos.quantity;
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// 6. STRICT execution with the rollback, and the assertion
// ------------------------------------------------------------------------------------------------

struct StrictExecutionOutcome {
    std::vector<ExecutionReport> executions;
    std::vector<std::string> unpriced;     ///< symbols the execution step could not price
    std::vector<std::string> rolled_back;  ///< of those, the ones put back to the stored row
};

/**
 * @brief One sleeve's executions under PricingPolicy::STRICT, rolling back what did not trade.
 *
 * Copied from LiveDailyCycle::execute_day_t rule 3: a symbol that could not be priced did not
 * trade, so the day-T book must not claim it did. Its target goes back to the stored T-1 row
 * (or is dropped if it was never held). With the hold above, every symbol that reaches the
 * execution step with a change has a positive T-1 price, so `unpriced` is empty by construction;
 * this is the belt, and a non-empty `unpriced` is a tripwire the caller logs as an ERROR.
 * MARK_FALLBACK (a fill at the position's own last mark) is no longer used by the futures runners.
 */
inline Result<StrictExecutionOutcome> execute_strategy_day_strict(
    ExecutionManager& execution_manager, std::unordered_map<std::string, Position>& current,
    const std::unordered_map<std::string, Position>& previous,
    const std::unordered_map<std::string, double>& t1_closes, const Timestamp& now) {
    StrictExecutionOutcome out;
    auto res = execution_manager.generate_daily_executions(current, previous, t1_closes, now,
                                                           PricingPolicy::STRICT, &out.unpriced);
    if (res.is_error()) {
        return make_error<StrictExecutionOutcome>(res.error()->code(), res.error()->what(),
                                                  "execute_strategy_day_strict");
    }
    out.executions = res.value();
    for (const auto& symbol : out.unpriced) {
        auto prev_it = previous.find(symbol);
        if (prev_it != previous.end() && prev_it->second.quantity.as_double() != 0.0) {
            current[symbol] = prev_it->second;
            current[symbol].last_update = now;
        } else {
            current.erase(symbol);
        }
        out.rolled_back.push_back(symbol);
    }
    return Result<StrictExecutionOutcome>(out);
}

/**
 * @brief The assertion: every book change left on any sleeve has a positive T-1 price. A change
 *        is a quantity that differs from the stored T-1 row, or a stored non-zero row absent from
 *        today's book (a close-out). Returns "STRATEGY/SYMBOL" for each violation; empty is the
 *        only acceptable answer.
 */
inline std::vector<std::string> unpriced_book_changes(
    const StrategyBooks& books, const StrategyBooks& previous,
    const std::unordered_map<std::string, double>& t1_closes) {
    static const std::unordered_map<std::string, Position> kEmpty;
    auto priced = [&t1_closes](const std::string& s) {
        auto it = t1_closes.find(s);
        return it != t1_closes.end() && it->second > 0.0;
    };
    std::vector<std::string> out;
    for (const auto& [name, book] : books) {
        auto pit = previous.find(name);
        const auto& prev = pit != previous.end() ? pit->second : kEmpty;
        for (const auto& [symbol, pos] : book) {
            auto p = prev.find(symbol);
            const double q0 = p != prev.end() ? p->second.quantity.as_double() : 0.0;
            if (std::abs(pos.quantity.as_double() - q0) > 1e-6 && !priced(symbol)) {
                out.push_back(name + "/" + symbol);
            }
        }
        for (const auto& [symbol, row] : prev) {
            if (!book.count(symbol) && row.quantity.as_double() != 0.0 && !priced(symbol)) {
                out.push_back(name + "/" + symbol);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace trade_ngin
