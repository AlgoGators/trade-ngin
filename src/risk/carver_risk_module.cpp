// src/risk/carver_risk_module.cpp
#include "trade_ngin/risk/carver_risk_module.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

CarverRiskModule::CarverRiskModule(std::string id, RiskConfig config, int min_gate_dates)
    : id_(std::move(id)), rm_(std::move(config)), min_gate_dates_(min_gate_dates) {}

int CarverRiskModule::complete_dates_in_window() const {
    if (window_.empty()) return 0;
    // One pass: each symbol gets a bit, each date a mask of the symbols that printed on
    // it. A date is complete when its mask holds every symbol the window has seen.
    // Above 64 symbols the mask cannot hold them all, so the count falls back to the
    // exact set comparison rather than quietly reporting the wrong number.
    std::unordered_map<std::string, size_t> symbol_index;
    std::unordered_map<int64_t, uint64_t> mask_by_date;
    std::unordered_map<int64_t, std::set<std::string>> set_by_date;
    bool wide = false;
    for (const auto& bar : window_) {
        const auto key = std::chrono::duration_cast<std::chrono::seconds>(
                             bar.timestamp.time_since_epoch())
                             .count();
        auto [it, inserted] = symbol_index.emplace(bar.symbol, symbol_index.size());
        (void)inserted;
        if (it->second >= 64) wide = true;
        if (wide) {
            set_by_date[key].insert(bar.symbol);
        } else {
            mask_by_date[key] |= (uint64_t{1} << it->second);
        }
    }
    const size_t symbols = symbol_index.size();
    int complete = 0;
    if (wide) {
        // Rebuild the per-date sets from scratch: the masks collected before the 65th
        // symbol appeared are incomplete.
        set_by_date.clear();
        for (const auto& bar : window_) {
            const auto key = std::chrono::duration_cast<std::chrono::seconds>(
                                 bar.timestamp.time_since_epoch())
                                 .count();
            set_by_date[key].insert(bar.symbol);
        }
        for (const auto& [date, syms] : set_by_date) {
            (void)date;
            if (syms.size() == symbols) ++complete;
        }
        return complete;
    }
    const uint64_t all = symbols >= 64 ? ~uint64_t{0} : ((uint64_t{1} << symbols) - 1);
    for (const auto& [date, mask] : mask_by_date) {
        (void)date;
        if (mask == all) ++complete;
    }
    return complete;
}

std::set<RiskTerm> CarverRiskModule::terms() const {
    return {RiskTerm::COMPOSITION, RiskTerm::MAGNITUDE};
}

std::set<RiskAction> CarverRiskModule::capabilities() const {
    return {RiskAction::SCALE};
}

void CarverRiskModule::begin_rebalance(const RiskContext& ctx) {
    (void)ctx;
    appended_this_rebalance_ = false;
    market_data_built_this_rebalance_ = false;
    applied_level_ = 1.0;
    level_partial_ = false;
    last_requested_ = 1.0;
}

size_t CarverRiskModule::window_dates() const {
    std::set<Timestamp> dates;
    for (const auto& bar : window_) dates.insert(bar.timestamp);
    return dates.size();
}

void CarverRiskModule::on_bars(const std::vector<Bar>& bars, const RiskContext& ctx) {
    (void)ctx;
    // (1) APPEND ONCE PER REBALANCE, not once per lap.
    //
    // The window used to be re-filled on every lap of the optimizer/risk loop, which calls this
    // with the SAME day's bars up to five times. With 36 futures symbols and a 252-BAR cap that
    // is 36 fresh copies a lap evicting older dates, so the window shrank 8 -> 7 -> 5 -> 4 -> 3
    // DATES inside one day. At two or three return rows |rho| is 1.0 by arithmetic, and
    // correlation_mult pinned at exactly 0.850000 on 90.5 % of binding laps against an honest
    // 252-date value of 0.872091 (T-4 M-02).
    //
    // "Is this a new rebalance?" is the EXPLICIT flag begin_rebalance resets, never something
    // inferred from the bars. The inferred test C2 used -- "is the newest timestamp in `data`
    // already in the window?" -- is equivalent only when every later call of a rebalance carries
    // the same bars. The PortfolioManager's own bar subscriber delivers ONE DATE ACROSS N CALLS,
    // so bars 2..N of a date were silently discarded and the gate measured a one-name book.
    if (!appended_this_rebalance_) {
        appended_this_rebalance_ = true;
        for (auto const& bar : bars) {
            window_.push_back(bar);
        }

        // (2) CAP THE WINDOW AT DATES, NOT BARS. lookback_period is 252 COMPLETE DATES.
        //
        // The bar cap made the window's span a function of how many symbols the book holds: the
        // same 252 is a year for one symbol and seven sessions for thirty-six. The key is the bar
        // timestamp, which is exactly what create_market_data keys prices and return rows by, and
        // every bar in futures_data.ohlcv_1d and equities_data.ohlcv_1d is stamped 00:00:00 UTC,
        // so one timestamp is one date. A date on which only some symbols printed is normal here
        // (agricultural roots have no Sunday bar, MBT prints on Saturdays) and stays one date.
        //
        // Duplicate (symbol, timestamp) rows -- 970 extra rows inside the frozen window
        // (project_futures_dup_bars) -- are deliberately NOT removed here: create_market_data
        // collapses them into one price per (symbol, timestamp) anyway, and de-duplicating would
        // make this run incomparable with the baseline, which does not.
        const size_t lookback = rm_.get_config().lookback_period;
        std::set<Timestamp> dates;
        for (const auto& bar : window_) dates.insert(bar.timestamp);
        if (dates.size() > lookback) {
            const Timestamp cutoff = *std::prev(dates.end(), static_cast<long>(lookback));
            window_.erase(std::remove_if(window_.begin(), window_.end(),
                                         [&cutoff](const Bar& b) { return b.timestamp < cutoff; }),
                          window_.end());
        }
    }

    // The window changes ONCE per rebalance (see (1)), so everything below -- F5's scan and
    // create_market_data -- produces the SAME MarketData on laps 2..n as it did on lap 1.
    // Recomputing it was pure waste: measured at ~3 ms per lap on a 252-date, 36-symbol window
    // (1.4 ms on the old 252-BAR one), against ~41,000 gate evaluations a day on the
    // bus-driven multi-strategy runner. Byte-identical by construction: same inputs, same
    // output, and market_data_ is not mutated between laps.
    if (appended_this_rebalance_ && market_data_built_this_rebalance_) {
        return;
    }
    // market_data_built_this_rebalance_ is set only AFTER market_data_ has been assigned below, so
    // a build that throws on lap 1 is retried on lap 2 instead of leaving laps 2..n reading the
    // PREVIOUS rebalance's MarketData (T-6b INTERIM ADVERSARIAL E-1).

    // (3) F5: DROP SPARSE DATES FROM THE GATE'S WINDOW.
    //
    // create_market_data builds one return row per consecutive pair of timestamps in the window,
    // initialises every symbol's cell to 0.0, and overwrites it only when that symbol printed on
    // BOTH dates. A symbol that did not print therefore enters the covariance as a genuine
    // observation of ZERO return, and a sparse date wedged between two dense ones destroys the
    // real return across it. On a 252-date futures window that is 1,188 fabricated cells of 9,036
    // (13.15 %) and 491 destroyed real returns -- and it REORDERS which pair binds the gate: the
    // honest maximum |rho| is ZF/ZN at 0.9757, the zero-fill knocks that to 0.9331 and hands the
    // maximum to MES/MNQ at 0.9520, so the gate reads correlation_mult 0.8928 where the honest
    // answer is 0.8700.
    //
    // The rule is the STRICT one: a date survives only if EVERY symbol in the window printed on
    // it, so every cell of every surviving row is a real return and it needs no new statistics.
    // Deliberately NOT done: forward-filling absent prices (it invents prices and assigns a
    // multi-day gap move to one date); a pairwise-overlap correlation (per-pair sample sets can
    // produce a covariance that is not positive semi-definite, and the same matrix feeds
    // w'Sigma w in the VaR gate where a negative quadratic form is swallowed by sqrt(max(0, v))).
    std::set<std::string> symbols;
    std::map<Timestamp, std::set<std::string>> by_date;
    for (const auto& bar : window_) {
        symbols.insert(bar.symbol);
        by_date[bar.timestamp].insert(bar.symbol);
    }
    std::set<Timestamp> complete;
    for (const auto& [ts, syms] : by_date) {
        if (syms.size() == symbols.size()) complete.insert(ts);
    }
    f5_engaged_ = complete.size() >= kF5MinGateDates;
    dates_dropped_ = f5_engaged_ ? by_date.size() - complete.size() : 0;

    if (f5_engaged_) {
        std::vector<Bar> filtered;
        filtered.reserve(window_.size());
        for (const auto& bar : window_) {
            if (complete.count(bar.timestamp)) filtered.push_back(bar);
        }
        market_data_ = rm_.create_market_data(filtered);
        market_data_built_this_rebalance_ = true;
    } else {
        // Below the floor F5 does not engage and the gate reads the UNFILTERED date-capped
        // window. The fallback is deliberately the larger, dirtier window: a small-sample matrix
        // is the failure this change set removes, and a 2-date window additionally drives
        // create_market_data into its divide-by-(n-1)==0 branch, where the gate goes blind with
        // every multiplier at 1.0.
        market_data_ = rm_.create_market_data(window_);
        market_data_built_this_rebalance_ = true;
    }
}

RiskDecision CarverRiskModule::to_decision(const RiskResult& r, const std::string& module_id) {
    RiskDecision d;
    d.module_id = module_id;
    if (r.risk_exceeded) {
        d.action = RiskAction::SCALE;
        d.scale = r.recommended_scale;
    } else {
        d.action = RiskAction::NONE;
        d.scale = 1.0;
    }
    d.metrics = r;
    return d;
}

Result<RiskDecision> CarverRiskModule::evaluate(
    const std::unordered_map<std::string, Position>& book, const RiskContext& ctx) {
    (void)ctx;
    auto result = rm_.process_positions(book, market_data_, {});
    if (result.is_error()) {
        return make_error<RiskDecision>(result.error()->code(), result.error()->what(),
                                        "RiskManager");
    }

    const auto& risk_result = result.value();
    // What the gate actually read: the window AFTER F5's filter, with the unfiltered span
    // recoverable as window_dates. `dates` is the number of dates create_market_data saw.
    INFO("T4_RISK_WINDOW dates=" +
         std::to_string(market_data_.returns.empty() && market_data_.ordered_symbols.empty()
                            ? 0
                            : market_data_.returns.size() + 1) +
         " symbols=" + std::to_string(market_data_.ordered_symbols.size()) +
         " returns_rows=" + std::to_string(market_data_.returns.size()) +
         " dates_dropped=" + std::to_string(dates_dropped_) +
         " f5_engaged=" + std::to_string(f5_engaged_ ? 1 : 0) +
         " window_dates=" + std::to_string(window_dates()) +
         " window_bars=" + std::to_string(window_.size()));
    INFO("Risk management result: risk_exceeded=" + std::to_string(risk_result.risk_exceeded) +
         ", scale=" + std::to_string(risk_result.recommended_scale) +
         ", portfolio_mult=" + std::to_string(risk_result.portfolio_multiplier) +
         ", jump_mult=" + std::to_string(risk_result.jump_multiplier) +
         ", correlation_mult=" + std::to_string(risk_result.correlation_multiplier) +
         ", leverage_mult=" + std::to_string(risk_result.leverage_multiplier));

    // ---- THE TERM-AWARE LEVEL CUT (T-4f ARM 1) -------------------------------------
    //
    // The shipped loop multiplied the book by s_k on EVERY lap. The gate is scale-invariant --
    // the weights are normalised by the book's own gross -- so s_k barely moves as the book
    // shrinks, and five laps of x0.85 ship 0.4437 of a book the gate asked to cut by 15 %.
    //
    // A LEVEL is the right rule only for a multiplier that reads the book's COMPOSITION, because
    // such a reading does not change when the book is scaled. Three of the four are of that kind:
    //     portfolio_multiplier (VaR), jump_multiplier, correlation_multiplier.
    // The fourth is not. calculate_leverage_multiplier is gross = total_value / capital and
    // net = sum(position_values) / capital, a MAGNITUDE reading of the book as it now stands.
    // Comparing that with the level treats a fresh magnitude measurement as an absolute request,
    // which is how C2f came to discard every leverage request after lap 1 and end over the net
    // limit on 100 of 368 days.
    //
    // So the level tracks the three INVARIANT terms only, and the leverage term is applied as a
    // RATE on every lap -- compared with 1.0, never with the level. Both are honoured with ONE
    // factor, the largest that satisfies both:
    //     s_inv = min(portfolio, jump, correlation)   (a level request)
    //     s_lev = leverage_multiplier                  (a rate on this book)
    //     f     = min(1, s_inv / applied_level_, s_lev)
    // min(), not a product: a deep leverage cut also satisfies a shallower correlation request
    // and must not be charged twice for it. A lap on which the gate goes blind has all four
    // multipliers at 1.0 and risk_exceeded false, so it still applies nothing.
    const double s_inv = std::min({static_cast<double>(risk_result.portfolio_multiplier),
                                   static_cast<double>(risk_result.jump_multiplier),
                                   static_cast<double>(risk_result.correlation_multiplier)});
    const double s_lev = static_cast<double>(risk_result.leverage_multiplier);
    last_invariant_ = s_inv;
    last_leverage_ = s_lev;

    RiskDecision decision = to_decision(risk_result, id_);
    if (decision.action == RiskAction::SCALE) {
        // The head-room the invariant terms still allow, given what has already been applied.
        // A level marked PARTIAL is not a true statement about the book this module measured
        // (a sleeve was pinned and skipped by the multiply, T-6a ADVERSARIAL A-5), so nothing
        // divides by it: that lap honours the leverage rate alone, which is measured fresh on
        // the current book and is always valid. Zero laps on every shipped book.
        //
        // The level is the product of the QUANTISED factors the PM actually multiplied in
        // (Decimal rounds each to 1e-8), while s_inv is the raw reading. When the reading has not
        // moved since the lap that applied it, the two differ only by that rounding: a factor that
        // rounded UP leaves s_inv / level = 0.99999999x, which is not a request, and returning it
        // as a SCALE turned whole contracts into 0.99999999 of one (T-6b INTERIM ADVERSARIAL B-1:
        // 20 of 380 stored futures-backtest executions). So the invariant terms are satisfied
        // whenever s_inv is within one Decimal quantum of the level already applied; a lap whose
        // reading is genuinely deeper than that is charged exactly s_inv / level, as before.
        double headroom = 1.0;
        if (!level_partial_ && applied_level_ > 0.0 && s_inv < applied_level_ - kLevelQuantum) {
            headroom = s_inv / applied_level_;
        }
        const double factor = std::min({1.0, headroom, s_lev});
        if (factor < 1.0) {
            decision.scale = factor;
        } else {
            // The invariant terms are already satisfied at this level or deeper and the leverage
            // term reads 1.0: there is nothing left to apply this lap.
            decision.action = RiskAction::NONE;
            decision.scale = 1.0;
        }
    }
    last_requested_ = decision.scale;
    // Blind: the same tests RiskManager's own early returns make, recorded as data only.
    bool mapped = false;
    for (const auto& [symbol, pos] : book) {
        (void)pos;
        if (market_data_.symbol_indices.count(symbol)) {
            mapped = true;
            break;
        }
    }
    // ... plus the two the schema names (HD, LEAD_RULINGS_C7 item 3): too few complete
    // dates to estimate anything from, and a book with no capital to divide by. With
    // lookback_unit "bars" a 252-BAR window is about 7 futures dates, so this flag is
    // true on most futures laps -- which is the finding T-4 made, stated rather than
    // hidden. Data only: it is not logged and not stored in T-6a (T-7 item 10 stores it).
    decision.blind = market_data_.returns.empty() || market_data_.covariance.empty() ||
                     market_data_.symbol_indices.empty() || market_data_.ordered_symbols.empty() ||
                     !mapped || complete_dates_in_window() < min_gate_dates_ ||
                     static_cast<double>(rm_.get_config().capital) <= 0.0;
    return Result<RiskDecision>(std::move(decision));
}

void CarverRiskModule::on_applied(const RiskApplied& applied, const RiskContext& ctx) {
    (void)ctx;
    if (applied.action == RiskAction::SCALE) {
        applied_level_ *= static_cast<double>(applied.factor);
        // The multiply skipped a pinned sleeve, so the aggregated book this module measured was
        // cut by less than `factor` and applied_level_ now OVER-STATES the cut. Recorded rather
        // than guessed at: a rule that divided by it would under-cut by exactly the pinned
        // sleeve's share. Zero laps on every shipped book (no sleeve module is assigned).
        if (applied.partial) level_partial_ = true;
    }
}

nlohmann::json CarverRiskModule::describe() const {
    nlohmann::json terms_json = nlohmann::json::array();
    for (auto t : terms()) terms_json.push_back(risk_term_name(t));
    return nlohmann::json{
        {"id", id_}, {"type", type_}, {"terms", terms_json}, {"config", rm_.get_config().to_json()}};
}

}  // namespace trade_ngin
