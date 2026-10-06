// include/trade_ngin/risk/carver_risk_module.hpp
#pragma once

#include <string>
#include <vector>
#include "trade_ngin/risk/overlay.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/risk/risk_module.hpp"

namespace trade_ngin {

/**
 * @brief The Carver risk gate as a RiskModule: wraps the unchanged RiskManager.
 *
 * Owns the risk window (the newest lookback_period DISTINCT dates of bars; it was the
 * PortfolioManager's risk_history_). The first lap of a rebalance appends that rebalance's
 * bars, trims the window and builds the MarketData from its complete dates; evaluate then runs
 * RiskManager::process_positions on the book, printing the loop's
 * "Risk management result:" line exactly as the PortfolioManager did. It registers
 * no Logger component of its own: its RiskManager registers "RiskManager" in the
 * init list, at the point the PortfolioManager's constructor always built it.
 */
class CarverRiskModule final : public RiskModule {
public:
    /// @param min_gate_dates the window has to hold at least this many COMPLETE dates
    ///        (every symbol present in the window has a bar on that date) before the
    ///        gate's measurement means anything. Below it the decision is marked
    ///        `blind` -- a data field only: it never changes the action or the scale.
    ///        The default is schema 2's own value, so the test constructions that do
    ///        not care about it read the same number the configs carry.
    CarverRiskModule(std::string id, RiskConfig config, int min_gate_dates = 21);

    const std::string& id() const override { return id_; }
    const std::string& type() const override { return type_; }
    std::set<RiskTerm> terms() const override;           ///< {COMPOSITION, MAGNITUDE}
    std::set<RiskAction> capabilities() const override;  ///< {SCALE, WARN}
    /// Resets the per-rebalance state (the appended flag, the applied level and the last
    /// request). Never clears the window: it spans rebalances.
    void begin_rebalance(const RiskContext& ctx) override;
    /// Appends this rebalance's bars to the window ONCE (behind the explicit per-rebalance
    /// flag), trims it to the newest `lookback_period` distinct DATES, applies F5 and builds the
    /// MarketData. Runs on every lap, including a lap whose book is empty; laps 2..n re-use the
    /// window lap 1 built rather than appending the same bars again. F5's intersection and the
    /// MarketData run over ctx.gate_participants' bars when the context carries a non-empty set
    /// (T-7b-2 CGW: a symbol nobody signals, targets or holds no longer cuts everyone's dates), and
    /// one GATE_NOT_SIGNALLING line names the window's symbols left out.
    void on_bars(const std::vector<Bar>& bars, const RiskContext& ctx) override;

    /// F5's floor. Below this many COMPLETE dates the sparse-date filter does not engage and the
    /// gate reads the unfiltered date-capped window, because a 21-date matrix is the
    /// small-sample regime this change set exists to leave.
    ///
    /// LATENT, recorded not fixed (T-4f_DECISION item 11): the strict rule makes the window's
    /// length a function of the NEWEST symbol's bar count, so a symbol listed k days ago caps
    /// the complete dates at k -- and this floor moves that cliff from 21 to 120, reachable by
    /// an ordinary listing. A >= 90 % coverage rule buys only +6 dates on today's universe and
    /// re-introduces the zero-fill, so it was not taken. The candidate for whenever a listing or
    /// delisting actually occurs is a STICKY ENGAGE that drops the offending SYMBOL rather than
    /// the dates; it cannot be measured until then.
    static constexpr size_t kF5MinGateDates = 120;

    /// One Decimal quantum (Decimal keeps 8 decimal places, core/types.hpp): the precision at
    /// which the PM multiplies a book by a factor. The level cut treats an invariant reading
    /// within one quantum of the level already applied as satisfied, because the level is a
    /// product of quantised factors and the reading is not.
    static constexpr double kLevelQuantum = 1e-8;

    /// Dates the window holds, before F5 drops any.
    size_t window_dates() const;
    /// Dates F5 dropped on the last on_bars (0 when it did not engage).
    size_t dates_dropped() const { return dates_dropped_; }
    bool f5_engaged() const { return f5_engaged_; }
    /// min(portfolio, jump, correlation) and the leverage multiplier as evaluate last read them.
    double last_invariant() const { return last_invariant_; }
    double last_leverage() const { return last_leverage_; }
    Result<RiskDecision> evaluate(const std::unordered_map<std::string, Position>& book,
                                  const RiskContext& ctx) override;
    nlohmann::json describe() const override;  ///< {"id","type","terms","config"}
    /// T-7b-2 9c: RiskConfig::capital, the denominator of the gate's gross and net leverage
    /// (the only capital-dependent term: the portfolio, jump and correlation terms read weights
    /// over the book's own value), follows the sizing capital. Refuses capital <= 0 and keeps
    /// the old value (RiskManager::update_config's rule).
    Result<void> set_capital(Decimal capital) override;
    /// Multiplies the applied level by the quantised factor when the scope was scaled (evaluate
    /// compares the raw reading with it to within kLevelQuantum), and
    /// records a PARTIAL apply: on a lap whose multiply skipped a pinned sleeve the level is
    /// no longer a true statement about the book this module measured, so it is marked and
    /// commit 9's level rule declines to divide by it.
    /// finalize (below) must NOT re-run process_positions, whose RISK_DEBUG and VAR_DEBUG lines
    /// and second result line would change the run's log; it reads the book with leverage_of.
    void on_applied(const RiskApplied& applied, const RiskContext& ctx) override;

    /// The written leverage policy (HD 2026-09-20): the leverage limit is enforced to within
    /// whole-contract rounding. At the PM's post-rounding point this reads the SHIPPED book's
    /// leverage with RiskManager::leverage_of (silent: process_positions is not re-run) and, the
    /// first time on a TRADING DAY (ctx.as_of, else the rebalance's newest bar) that the final
    /// book is over its limit, returns a WARN whose reason names the day and carries the excess in
    /// contracts. Never on a warm-up rebalance (ctx.is_warmup), which ships nothing. Otherwise NONE.
    /// (T-7b-2 C9w; it latched once per run before, so a backtest said nothing after its first
    /// over-limit day and a replaying runner's one line described a replayed book.)
    Result<RiskDecision> finalize(const std::unordered_map<std::string, Position>& book,
                                  const RiskContext& ctx) override;
    /// The trading day finalize last warned on ("" before the first WARN).
    const std::string& leverage_warned_day() const { return leverage_warned_day_; }

    /// SCALE iff r.risk_exceeded, with scale = r.recommended_scale bit for bit; else NONE with
    /// scale 1.0. metrics = r in both cases. Keyed on risk_exceeded, NOT on `scale != 1.0`: a NaN
    /// scale has risk_exceeded false, and SCALE(NaN) would throw in Decimal(double).
    static RiskDecision to_decision(const RiskResult& r, const std::string& module_id);

    /// Dates in the window on which every symbol present in the window has a bar
    /// (F5's definition). Recomputed from the window; nothing caches it.
    int complete_dates_in_window() const;

    /// LOOP_SPEC section 4: with limits set, evaluate reads the book in CAPITAL TERMS
    /// (risk/overlay.hpp) on the context's overlay inputs: the four readings on the weights
    /// x = N M P / E, the limits as ratios to tau, and m = the smallest multiplier, which is the
    /// rate it asks for on that lap's book. Without limits, or on a context that carries no
    /// inputs, it reads the book as before.
    void set_overlay_limits(const overlay::LimitRatios& ratios) { overlay_ratios_ = ratios; }
    const overlay::LimitRatios& overlay_limits() const { return overlay_ratios_; }
    /// The last evaluation in capital terms (empty window before the first).
    const overlay::Evaluation& last_overlay() const { return last_overlay_; }
    bool overlay_read() const { return overlay_read_; }

    const RiskManager& manager() const { return rm_; }
    int min_gate_dates() const { return min_gate_dates_; }
    const std::vector<Bar>& window() const { return window_; }
    /// The MarketData the gate will read this lap. Exposed so a test can prove the cache was
    /// REBUILT when the window changed, rather than only that the window changed.
    const MarketData& market_data() const { return market_data_; }
    bool appended_this_rebalance() const { return appended_this_rebalance_; }
    double applied_level() const { return applied_level_; }
    /// True once a partial apply has happened this rebalance: `applied_level_` over-states the
    /// cut the measured book actually took, and no rule may divide by it.
    bool level_partial() const { return level_partial_; }
    double last_requested() const { return last_requested_; }

private:
    std::string id_;
    std::string type_{"carver"};
    RiskManager rm_;          ///< registers "RiskManager"; this class registers and logs nothing more
    int min_gate_dates_{21};  ///< read only into RiskDecision::blind
    MarketData market_data_;  ///< built by on_bars, read by evaluate on the same lap
    overlay::LimitRatios overlay_ratios_;  ///< unset: the module reads the book as before
    overlay::Evaluation last_overlay_;
    bool overlay_read_{false};  ///< the last evaluate read the book in capital terms
    std::vector<Bar> window_;  ///< was PortfolioManager::risk_history_
    size_t dates_dropped_{0};  ///< F5's count on the last on_bars
    bool f5_engaged_{false};   ///< whether F5 filtered the last window
    bool in_f5_fallback_{false};  ///< the last build fell back (the WARN fires on entering it)
    double last_invariant_{1.0};  ///< min(portfolio, jump, correlation), last evaluate
    double last_leverage_{1.0};   ///< leverage_multiplier, last evaluate
    // Per-rebalance state, reset by begin_rebalance. Commit 9 reads all three: on_bars appends
    // only when appended_this_rebalance_ is false, and evaluate divides the invariant request by
    // applied_level_ so the cut is a LEVEL for this rebalance rather than a rate charged again
    // on every lap.
    bool appended_this_rebalance_{false};  ///< set by on_bars
    /// The MarketData for THIS rebalance's window has been built. The window only changes on the
    /// appending lap, so laps 2..n reuse it instead of rebuilding an identical one.
    bool market_data_built_this_rebalance_{false};
    double applied_level_{1.0};            ///< product of the factors applied this rebalance
    bool level_partial_{false};            ///< a multiply skipped a pinned scope: the level lies
    double last_requested_{1.0};           ///< the scale evaluate last requested this rebalance
    std::string leverage_warned_day_;      ///< the trading day finalize's WARN last fired on
};

}  // namespace trade_ngin
