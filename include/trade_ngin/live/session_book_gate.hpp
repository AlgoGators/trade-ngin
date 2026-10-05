// include/trade_ngin/live/session_book_gate.hpp
#pragma once

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <unordered_set>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/roll_series.hpp"
#include "trade_ngin/data/session_classifier.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/live/live_roll_legs.hpp"

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
 *   4. LOOP_SPEC v6.1 section 2.1 (K-01): every WITHHELD bar of the window (a JUNK bar or a thin
 *      first print, SymbolDayVerdict::k01_withheld) is kept out of the strategy and portfolio feed
 *      on every run (k01_consumed_bars, session_classifier.hpp), so a withheld T-1 bar's signal is
 *      not updated today and the bar is never fed later. The price manager is not given that feed;
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

/// One summary line, then JUNK WARN / feed hole ERROR / closure INFO per symbol. A closure whose
/// hole is older than `tolerance_days` (when given) is an ERROR too (T-7b-1 C7b R3): a feed that
/// has been dead for more than eight weeks is no longer "expected" and reads as a closure.
inline void log_t1_classification(const T1Classification& t1, int tolerance_days = -1) {
    INFO("T1_CLASSIFIER t1=" + t1.t1_date + " session=" + std::to_string(t1.session) +
         " junk=" + std::to_string(t1.junk) + " closure=" + std::to_string(t1.closure) +
         " feed_hole=" + std::to_string(t1.feed_hole) +
         " max_hole_age_days=" + std::to_string(t1.max_hole_age_days));
    for (const auto& v : t1.verdicts) {
        // T-7b-2 C10a: T-1 answers the previous session's unconfirmed instrument id change.
        if (!v.id_note.empty()) {
            INFO("T1_CLASSIFIER INSTRUMENT_ID " + v.symbol + " " + v.date + ": " + v.id_note);
        }
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
                if (tolerance_days >= 0 && v.hole_age_days > tolerance_days) {
                    ERROR("T1_CLASSIFIER closure " + v.symbol + " " + v.date + ": " + v.reason +
                          "; last bar " + v.last_bar_date + " (" +
                          std::to_string(v.hole_age_days) +
                          " day(s)), past live.data_staleness_tolerance_days=" +
                          std::to_string(tolerance_days) + " -- held at its last mark, no order");
                } else {
                    INFO("T1_CLASSIFIER closure " + v.symbol + " " + v.date + ": " + v.reason +
                         " -- held at its last mark, no order");
                }
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

/// The oldest hole among the no-bar verdicts, feed hole or closure (0 when every symbol printed).
inline int max_no_bar_age_days(const T1Classification& t1) {
    int age = 0;
    for (const auto& v : t1.verdicts) {
        if (!v.has_bar) age = std::max(age, v.hole_age_days);
    }
    return age;
}

/**
 * @brief The held symbols (a non-zero stored T-1 quantity) with no bar for longer than the
 *        tolerance. T-4c F4 / T-5 F7 as ruled 2026-09-19: the per-symbol form of the feed
 *        freshness guard, keyed on a HELD symbol; a hole on a symbol the book does not hold
 *        changes nothing and is only logged. Keyed on ANY no-bar verdict (T-7b-1 C7b R3): a
 *        feed dead for more than eight weeks stops being "expected" and is reclassified as a
 *        closure, and a nine-week-old mark must refuse like a one-week-old one.
 */
inline std::vector<HeldFeedHole> held_feed_holes_past_tolerance(
    const T1Classification& t1, const std::unordered_map<std::string, Position>& held_book,
    int tolerance_days) {
    std::vector<HeldFeedHole> out;
    for (const auto& v : t1.verdicts) {
        if (v.has_bar) continue;
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
// 4b. The T-1 settlement on the consumed bars (T-ROLLX-FIX; LOOP_SPEC v6.1 sections 2.1, 6.6)
// ------------------------------------------------------------------------------------------------

struct ConsumedT1Settlement {
    /// Symbols whose T-1 bar books no move: a change bar (a roll's switch day or either bar of a
    /// flip) or a WITHHELD bar (K-01).
    std::unordered_set<std::string> zero_pnl_symbols;
    /// The close each symbol's T-1 move is booked against: its previous CONSUMED bar's close when its
    /// last consumed bar is dated T-1 (a withheld bar in between is skipped); otherwise the raw T-2
    /// close passed in (unused: no consumed T-1 bar books no move).
    std::unordered_map<std::string, double> t2_close_prices;
    /// The close each symbol's T-1 move is booked to: the raw T-1 map, except a symbol whose roll
    /// this run legs LATE (section 6.5), which books every unbooked consumed bar's move on the T-1
    /// row (its last consumed close against LateRollSettlement::settle_from).
    std::unordered_map<std::string, double> t1_close_prices;
};

/**
 * @brief The live T-1 finalize's inputs on the consumed bars. `consumed` is the window without the
 *        withheld bars (k01_consumed_bars), `t1_date` the run's T-1, `roll_status` the status of
 *        each symbol's last consumed bar (roll_series::roll_status_of(consumed)),
 *        `withheld_t1_symbols` the symbols whose T-1 bar was withheld, `raw_t2` and `raw_t1` the
 *        price manager's T-2 and T-1 maps, `late_rolls` LiveRollState::late.
 */
inline ConsumedT1Settlement consumed_t1_settlement(
    const std::vector<Bar>& consumed, const std::string& t1_date,
    const std::unordered_map<std::string, roll_series::RollTracker::Status>& roll_status,
    const std::vector<std::string>& withheld_t1_symbols,
    const std::unordered_map<std::string, double>& raw_t2,
    const std::unordered_map<std::string, double>& raw_t1,
    const std::unordered_map<std::string, LateRollSettlement>& late_rolls) {
    ConsumedT1Settlement out;
    out.zero_pnl_symbols.insert(withheld_t1_symbols.begin(), withheld_t1_symbols.end());
    out.t2_close_prices = raw_t2;
    out.t1_close_prices = raw_t1;
    std::map<std::string, std::vector<const Bar*>> by_symbol;
    for (const auto& bar : consumed) by_symbol[bar.symbol].push_back(&bar);
    for (auto& [symbol, seq] : by_symbol) {
        std::stable_sort(seq.begin(), seq.end(),
                         [](const Bar* a, const Bar* b) { return a->timestamp < b->timestamp; });
        if (SessionClassifier::ymd(SessionClassifier::day_of(seq.back()->timestamp)) != t1_date) continue;
        if (seq.size() >= 2) out.t2_close_prices[symbol] = static_cast<double>(seq[seq.size() - 2]->close);
        const auto rs = roll_status.find(symbol);
        if (rs != roll_status.end() && rs->second.change) out.zero_pnl_symbols.insert(symbol);
    }
    // A late roll's symbol books its unbooked consumed bars on this run, whether or not it has a
    // T-1 bar (a withheld T-1 print stays out: the sum is over consumed bars).
    for (const auto& [symbol, late] : late_rolls) {
        out.zero_pnl_symbols.erase(symbol);
        out.t1_close_prices[symbol] = late.settle_to;
        out.t2_close_prices[symbol] = late.settle_from;
    }
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
    bool classified{true};   ///< false: outside the classified universe (no T-1 verdict)
    bool change_bar{false};  ///< held under D37 (its last consumed bar is pending), T-1 a SESSION
};

/**
 * @brief Holds every non-SESSION symbol, and every symbol of `change_bar_holds`, at its stored T-1
 *        quantity, on every per-strategy book.
 *
 * LOOP_SPEC v6.2 section 6.1 (D37; T-ROLLX-FIX commit 4, D-A): `change_bar_holds` is the D37 set, the
 * symbols whose last consumed bar is pending (a change bar, either bar of a flip, an id-less bar
 * inside a pending roll). Such a bar can carry a SESSION verdict (a full-volume switch day, a flip's
 * reverting bar, an id-less bar), so the verdict alone does not hold it; the hold applies on EVERY
 * rebalance, never only on a day the risk gate cuts.
 *
 * For each sleeve: a symbol whose verdict is not SESSION and whose target differs from the
 * sleeve's stored T-1 quantity gets that quantity back (flat if nothing was stored); the row's
 * mark is left as the strategy wrote it (the last bar's close, the last mark), and the runner
 * stores a JUNK symbol at its T-1 close because it is in the price map. A symbol held yesterday
 * but absent from today's target map is re-inserted from its stored row, so the execution step's
 * close-out loop cannot flatten it. Returns one record per held change; the caller logs and
 * rebuilds the combined book.
 */
inline std::vector<BookHold> hold_non_session_symbols(
    StrategyBooks& books, const StrategyBooks& previous, const T1Classification& t1,
    const Timestamp& now, const std::unordered_set<std::string>& change_bar_holds) {
    static const std::unordered_map<std::string, Position> kEmpty;
    std::vector<BookHold> holds;
    // Held under D37 alone: the verdict would have let it trade.
    auto d37_only = [&](const std::string& symbol) {
        return t1.is_session(symbol) && change_bar_holds.count(symbol) > 0;
    };
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
            if (t1.is_session(symbol) && !change_bar_holds.count(symbol)) continue;
            auto& pos = book[symbol];
            auto prev_it = prev.find(symbol);
            const double prev_qty =
                prev_it != prev.end() ? prev_it->second.quantity.as_double() : 0.0;
            const double target = pos.quantity.as_double();
            if (std::abs(target - prev_qty) <= 1e-6) continue;
            pos.quantity = Decimal(prev_qty);
            const auto* v = t1.find(symbol);
            holds.push_back({name, symbol, v ? v->verdict : SessionVerdict::NO_BAR_CLOSURE,
                             prev_qty, target, false, v != nullptr, d37_only(symbol)});
        }

        std::vector<std::string> prev_symbols;
        for (const auto& [symbol, _] : prev) prev_symbols.push_back(symbol);
        std::sort(prev_symbols.begin(), prev_symbols.end());
        for (const auto& symbol : prev_symbols) {
            if (book.count(symbol)) continue;
            if (t1.is_session(symbol) && !change_bar_holds.count(symbol)) continue;
            const Position& row = prev.at(symbol);
            if (std::abs(row.quantity.as_double()) <= 1e-6) continue;
            book[symbol] = row;
            book[symbol].last_update = now;
            const auto* v = t1.find(symbol);
            holds.push_back({name, symbol, v ? v->verdict : SessionVerdict::NO_BAR_CLOSURE,
                             row.quantity.as_double(), 0.0, true, v != nullptr, d37_only(symbol)});
        }
    }
    return holds;
}

inline void log_book_holds(const std::vector<BookHold>& holds) {
    size_t without_session = 0;
    for (const auto& h : holds) {
        const std::string what = h.reinserted
                                     ? "absent from today's target, held at " +
                                           std::to_string(h.held_quantity) + "; no close-out today"
                                     : "book held at " + std::to_string(h.held_quantity) +
                                           " instead of target " +
                                           std::to_string(h.target_quantity) + "; no order today";
        // D37 (section 6.1): a pending symbol whose T-1 is a SESSION is held by the change-bar rule,
        // in its own family.
        if (h.change_bar) {
            WARN("CHANGE_BAR_HOLD " + h.symbol + " (" + h.strategy_name + "): " + what);
            continue;
        }
        ++without_session;
        // T-7b-1 C7b R9: a symbol nobody classified (removed from the universe, or filtered out of
        // get_symbols) is held because nothing vouched for it, not because T-1 was a closure.
        WARN("BOOK_GATE " + h.symbol + " (" + h.strategy_name + "): " +
             (h.classified ? "T-1 " + std::string(to_string(h.verdict))
                           : std::string("outside the classified universe (no T-1 verdict)")) +
             " -- " + what);
    }
    if (without_session > 0) {
        INFO("BOOK_GATE held " + std::to_string(without_session) +
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
