// src/risk/carver_risk_module.cpp
#include "trade_ngin/risk/overlay_record.hpp"
#include "trade_ngin/risk/carver_risk_module.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
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
    return {RiskAction::SCALE, RiskAction::WARN};
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

        // (2) CAP THE WINDOW AT DATES, NOT BARS. lookback_period is 252 DISTINCT dates, of which
        // the sparse-date filter below keeps the complete ones: about 187-200 on the shipped
        // futures books, because a date on which only some symbols printed counts here and is
        // dropped there.
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
    // (4) THE PARTICIPANTS (T-7b-2 CGW, the E-7 mechanism in the gate). The strict rule is over
    // the symbols the book can hold this rebalance (RiskContext::gate_participants: signalled by
    // some strategy, or targeted or held), not over every symbol with a bar in the window: a symbol
    // nobody signals, targets or holds has zero weight in every term the gate computes, so its
    // columns add nothing, while its missing dates removed those dates from EVERY symbol's returns
    // (on the frozen futures backtest the roots with no Sunday bar dropped 50 to 67 dates a day
    // while they were still warming up). The window itself keeps every bar, so a symbol that starts
    // to signal enters with its whole history. An empty or absent set leaves the whole window.
    std::vector<Bar> participant_bars;
    std::set<std::string> left_out;
    const bool participants_only = ctx.gate_participants != nullptr && !ctx.gate_participants->empty();
    if (participants_only) {
        participant_bars.reserve(window_.size());
        for (const auto& bar : window_) {
            if (ctx.gate_participants->count(bar.symbol)) {
                participant_bars.push_back(bar);
            } else {
                left_out.insert(bar.symbol);
            }
        }
    }
    const std::vector<Bar>& pool =
        participants_only && !participant_bars.empty() ? participant_bars : window_;
    if (&pool == &participant_bars && !left_out.empty()) {
        std::string list;
        for (const auto& s : left_out) list += (list.empty() ? "" : ",") + s;
        INFO("GATE_NOT_SIGNALLING count=" + std::to_string(left_out.size()) + " symbols=" + list +
             ": no strategy signals, targets or holds them; left out of the risk gate's window and "
             "its date intersection this rebalance");
    }
    std::set<std::string> symbols;
    std::map<Timestamp, std::set<std::string>> by_date;
    for (const auto& bar : pool) {
        symbols.insert(bar.symbol);
        by_date[bar.timestamp].insert(bar.symbol);
    }
    std::set<Timestamp> complete;
    for (const auto& [ts, syms] : by_date) {
        if (syms.size() == symbols.size()) complete.insert(ts);
    }
    f5_engaged_ = complete.size() >= kF5MinGateDates;
    dates_dropped_ = f5_engaged_ ? by_date.size() - complete.size() : 0;
    // Say so when the gate falls back to the unfiltered window (T-6b INTERIM ADVERSARIAL B-5:
    // commit 9 dropped ARM 1's per-lap WARN, and 149 backtest laps then read the zero-filled
    // window with only f5_engaged=0 inside an INFO line). On ENTERING the fallback -- the first
    // time in a run and again on any re-entry -- not on every lap: the bus-driven BASE runner
    // evaluates ~7,000 times a day on a window that warms from empty, and a per-lap line would be
    // most of its log.
    if (!f5_engaged_ && !in_f5_fallback_) {
        WARN("T4_F5_FALLBACK complete_dates=" + std::to_string(complete.size()) +
             " window_dates=" + std::to_string(by_date.size()) +
             " floor=" + std::to_string(kF5MinGateDates) +
             ": the gate reads the UNFILTERED window (sparse dates zero-filled) until it holds "
             "the floor's complete dates; logged on entering the fallback, not per lap");
    }
    in_f5_fallback_ = !f5_engaged_;

    // T-ROLLX (LOOP_SPEC v6.1 section 2.3): the gate's returns are adjusted. The levels are built
    // on the WHOLE pool (every date, F5's filter not yet applied), so a contract switch on a date
    // the filter drops is removed from the level and the return across that gap is a real move.
    const RiskManager::AdjustedLevels adjusted = RiskManager::adjusted_levels_of(pool);
    if (f5_engaged_) {
        std::vector<Bar> filtered;
        filtered.reserve(pool.size());
        for (const auto& bar : pool) {
            if (complete.count(bar.timestamp)) filtered.push_back(bar);
        }
        market_data_ = rm_.create_market_data(filtered, &adjusted);
        market_data_built_this_rebalance_ = true;
    } else {
        // Below the floor F5 does not engage and the gate reads the UNFILTERED date-capped
        // window. The fallback is deliberately the larger, dirtier window: a small-sample matrix
        // is the failure this change set removes, and a 2-date window additionally drives
        // create_market_data into its divide-by-(n-1)==0 branch, where the gate goes blind with
        // every multiplier at 1.0.
        market_data_ = rm_.create_market_data(pool, &adjusted);
        market_data_built_this_rebalance_ = true;
    }
}

namespace {

std::string fixed6(double v) {
    return std::to_string(v);
}

// LOOP_SPEC section 7.7: the OVERLAY line of one evaluation: the readings, the limits, the
// multipliers, m, the binding term or "none", the window's dates, its complete dates, the dates it
// dropped, whether the complete-date covariance engaged, and the symbols left out of each reading.
std::string overlay_log_line(const RiskContext& ctx, double capital, const overlay::Inputs& inputs,
                             const overlay::Evaluation& e) {
    auto list = [&](const std::vector<char>& mask) {
        std::string out;
        for (size_t i = 0; i < mask.size() && i < inputs.symbols.size(); ++i) {
            if (mask[i]) out += (out.empty() ? "" : " ") + inputs.symbols[i];
        }
        return out.empty() ? std::string("-") : out;
    };
    std::string outside;
    for (const auto& s : e.outside) outside += (outside.empty() ? "" : " ") + s;
    const auto& w = e.window;
    const bool blind = w.blind();
    return "OVERLAY lap=" + std::to_string(ctx.lap) + " m=" + fixed6(e.multiplier.m) +
           " binding=" + e.multiplier.binding + " R=" + (blind ? "blind" : fixed6(e.readings.risk)) +
           " R_jump=" + (blind ? "blind" : fixed6(e.readings.jump)) +
           " R_shock=" + (blind ? "blind" : fixed6(e.readings.shock)) +
           " L_g=" + fixed6(e.readings.gross) + " L_n=" + fixed6(e.readings.net) +
           " limits R_max=" + fixed6(e.limits.risk) + " R_jump_max=" + fixed6(e.limits.jump) +
           " R_shock_max=" + fixed6(e.limits.shock) + " L_max=" + fixed6(e.limits.gross) +
           " L_net_max=" + fixed6(e.limits.net) + " multipliers R=" + fixed6(e.multiplier.risk) +
           " R_jump=" + fixed6(e.multiplier.jump) + " R_shock=" + fixed6(e.multiplier.shock) +
           " L_g=" + fixed6(e.multiplier.gross) + " L_n=" + fixed6(e.multiplier.net) +
           " capital=" + fixed6(capital) + " tau=" + fixed6(inputs.tau) +
           " window=" + overlay::GateWindow::name(w.mode) +
           " dates=" + std::to_string(w.window_dates) +
           " first=" + (w.window_dates ? overlay::ordinal_date(w.first_ordinal) : "none") +
           " last=" + (w.window_dates ? overlay::ordinal_date(w.last_ordinal) : "none") +
           " complete_dates=" + std::to_string(w.complete_dates) +
           " dropped_dates=" + std::to_string(w.window_dates - w.complete_dates) +
           " f5_engaged=" + (w.mode == overlay::GateWindow::Mode::kComplete ? "1" : "0") +
           " participants=" + std::to_string(inputs.symbols.size()) +
           " no_return=[" + list(w.no_return) + "] short_history=[" + list(w.short_history) +
           "] outside=[" + (outside.empty() ? "-" : outside) + "]";
}

}  // namespace

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
    auto result = rm_.process_positions(book, market_data_, {});
    if (result.is_error()) {
        return make_error<RiskDecision>(result.error()->code(), result.error()->what(),
                                        "RiskManager");
    }

    RiskResult risk_result = result.value();
    // LOOP_SPEC section 4: the overlay in CAPITAL TERMS. With limits set and the rebalance's
    // inputs on the context, the four readings are taken on the book's weights on the sizing
    // capital (x = N M P / E) over the gate window of the participants' adjusted returns, each
    // against its limit (the three risk limits as ratios to tau), and m is the smallest
    // multiplier. The readings and the multipliers above are replaced by these.
    overlay_read_ = false;
    if (overlay_ratios_.set() && ctx.overlay_inputs != nullptr) {
        const overlay::Inputs& inputs = *ctx.overlay_inputs;
        const double capital = static_cast<double>(rm_.get_config().capital);
        std::vector<std::pair<std::string, double>> quantities;
        quantities.reserve(book.size());
        for (const auto& [symbol, position] : book) {
            quantities.emplace_back(symbol, static_cast<double>(position.quantity));
        }
        last_overlay_ = overlay::evaluate(inputs, quantities, capital, overlay_ratios_);
        overlay_read_ = true;
        const overlay::Evaluation& e = last_overlay_;
        risk_result.portfolio_var = e.readings.risk;
        risk_result.jump_risk = e.readings.jump;
        risk_result.correlation_risk = e.readings.shock;
        risk_result.gross_leverage = e.readings.gross;
        risk_result.net_leverage = e.readings.net;
        risk_result.portfolio_multiplier = e.multiplier.risk;
        risk_result.jump_multiplier = e.multiplier.jump;
        risk_result.correlation_multiplier = e.multiplier.shock;
        risk_result.leverage_multiplier = std::min(e.multiplier.gross, e.multiplier.net);
        risk_result.recommended_scale = e.multiplier.m;
        risk_result.risk_exceeded = e.multiplier.m < 1.0;
        INFO(overlay_log_line(ctx, capital, inputs, e));
        std::vector<double> by_participant(inputs.symbols.size(), 0.0);
        for (const auto& [symbol, quantity] : quantities) {
            const auto at = std::lower_bound(inputs.symbols.begin(), inputs.symbols.end(), symbol);
            if (at != inputs.symbols.end() && *at == symbol) {
                by_participant[static_cast<size_t>(at - inputs.symbols.begin())] = quantity;
            }
        }
        overlay::append_overlay_record(ctx.portfolio_id, ctx.lap, ctx.is_warmup, capital, inputs, e,
                                       by_participant);
    }
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
    // In capital terms (the overlay read the book) every term is a MAGNITUDE reading of the book
    // as it stands on this lap: the whole of m is a rate, and no term is a level.
    const double s_inv =
        overlay_read_ ? 1.0
                      : std::min({static_cast<double>(risk_result.portfolio_multiplier),
                                  static_cast<double>(risk_result.jump_multiplier),
                                  static_cast<double>(risk_result.correlation_multiplier)});
    const double s_lev = overlay_read_ ? risk_result.recommended_scale
                                       : static_cast<double>(risk_result.leverage_multiplier);
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
    // dates to estimate anything from, and a book with no capital to divide by. On the old
    // 252-BAR window (about 7 futures dates) this flag was true on most futures laps, which is
    // the finding T-4 made; on the date-keyed window it is false on every shipped futures lap.
    // Data only: it is not logged and not stored in T-6a (T-7 item 10 stores it).
    decision.blind = market_data_.returns.empty() || market_data_.covariance.empty() ||
                     market_data_.symbol_indices.empty() || market_data_.ordered_symbols.empty() ||
                     !mapped || complete_dates_in_window() < min_gate_dates_ ||
                     static_cast<double>(rm_.get_config().capital) <= 0.0;
    // In capital terms BLIND is the gate window's own state (fewer than 21 complete dates): the
    // three covariance readings ask for nothing and the leverage terms still apply (D27).
    if (overlay_read_) decision.blind = last_overlay_.window.blind();
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

Result<RiskDecision> CarverRiskModule::finalize(
    const std::unordered_map<std::string, Position>& book, const RiskContext& ctx) {
    RiskDecision d;
    d.module_id = id_;
    // A warm-up rebalance ships nothing, so it has nothing to warn about (and on a replaying runner
    // it is a replayed book, not the day's: T-6b-fix AUDIT section 8).
    if (ctx.is_warmup || book.empty() || market_data_.symbol_indices.empty()) {
        return Result<RiskDecision>(std::move(d));
    }
    // The trading day: the context's as_of (the backtest passes it), else the newest bar of this
    // rebalance (the live runners' dated day), else one key for the run.
    std::optional<Timestamp> when = ctx.as_of;
    if (!when && ctx.bars != nullptr) {
        for (const auto& bar : *ctx.bars) {
            if (!when || bar.timestamp > *when) when = bar.timestamp;
        }
    }
    std::string day = "run";
    if (when) {
        const std::time_t tt = std::chrono::system_clock::to_time_t(*when);
        std::tm tm{};
        gmtime_r(&tt, &tm);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        day = buf;
    }
    if (day == leverage_warned_day_) {
        return Result<RiskDecision>(std::move(d));
    }
    const RiskManager::LeverageReading r = rm_.leverage_of(book, market_data_);
    if (!(r.multiplier < 1.0 - 1e-9) || !(r.multiplier > 0.0)) {
        return Result<RiskDecision>(std::move(d));
    }
    leverage_warned_day_ = day;
    double gross_contracts = 0.0;
    for (const auto& [symbol, pos] : book) gross_contracts += std::abs(static_cast<double>(pos.quantity));
    const double ratio = 1.0 / r.multiplier;
    d.action = RiskAction::WARN;
    d.reason = "RISK_LEVERAGE_ROUNDED day=" + day + " lap=" + std::to_string(ctx.lap) +
               " the final book of this rebalance, after whole-contract rounding (the book the "
               "runner stores), is over its leverage limit by about " +
               std::to_string(gross_contracts * (ratio - 1.0)) + " contracts (" +
               std::to_string(ratio) + "x the limit on a " +
               std::to_string(static_cast<long>(std::llround(gross_contracts))) +
               "-contract book; gross " + std::to_string(r.gross_leverage) + ", net " +
               std::to_string(r.net_leverage) + ", capital " +
               std::to_string(static_cast<double>(rm_.get_config().capital)) +
               "). The limit is enforced to within whole-contract rounding (config_template risk "
               "rationale); logged at most once per trading day, never on a warm-up rebalance.";
    return Result<RiskDecision>(std::move(d));
}

Result<void> CarverRiskModule::set_capital(Decimal capital) {
    RiskConfig config = rm_.get_config();
    config.capital = capital;
    return rm_.update_config(config);
}

nlohmann::json CarverRiskModule::describe() const {
    nlohmann::json terms_json = nlohmann::json::array();
    for (auto t : terms()) terms_json.push_back(risk_term_name(t));
    return nlohmann::json{
        {"id", id_}, {"type", type_}, {"terms", terms_json}, {"config", rm_.get_config().to_json()}};
}

}  // namespace trade_ngin
