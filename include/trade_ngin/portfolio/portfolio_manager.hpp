// include/trade_ngin/portfolio/portfolio_manager.hpp
#pragma once

#include <iostream>
#include <memory>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/risk/carver_risk_module.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/risk/risk_module.hpp"
#include "trade_ngin/risk/risk_module_config.hpp"
#include "trade_ngin/risk/risk_scale_report.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

namespace trade_ngin {

/**
 * @brief Configuration for portfolio management
 */
struct PortfolioConfig : public ConfigBase {
    Decimal total_capital{Decimal(0.0)};    // Total portfolio capital
    double max_strategy_allocation{
        1.0};  // Maximum allocation to any strategy (keep as double - it's a ratio)
    double min_strategy_allocation{
        0.0};  // Minimum allocation to any strategy (keep as double - it's a ratio)
    bool use_optimization{false};     // Whether to use position optimization
    // The risk modules this book runs, portfolio scope, in evaluation order. There is
    // no boolean any more: a book that runs no risk layer carries a single `none`
    // assignment naming who ruled it and when, and an EMPTY list is a configuration
    // mistake the constructor throws on rather than a book that quietly runs ungated.
    std::vector<RiskModuleConfig> risk_modules;
    // Per strategy id, that sleeve's own modules. Empty on every shipped book.
    std::map<std::string, std::vector<RiskModuleConfig>> sleeve_risk_modules;
    // Whether a fractional target quantity is a legitimate end state for this
    // portfolio. Futures trade whole contracts and leave this false, so the
    // optimizer/risk loop keeps iterating until positions are integral, exactly
    // as it always has. Equities with fractional shares enabled set it true:
    // there is nothing to converge to, and re-entering the loop would re-apply
    // the risk scale to an already-scaled book (see E2-F1).
    bool allow_fractional_positions{false};
    // How many daily closes per symbol the PortfolioManager keeps for the optimiser's
    // covariance (portfolio.json "covariance_history_prices"; absent means 756). The PM
    // records one close per symbol per date from the bars it is fed and drops the oldest
    // date first once a symbol holds more than this. 756 is the trend sleeve's own history
    // cap (max(vol_lookback_long, 756)), so a single-sleeve trend book covaries the prices
    // it always did. Fewer than 2 cannot give a return and is refused (loader and
    // constructor). Not written by to_json(): that object is stored verbatim in
    // backtest.run_metadata.portfolio_config.
    size_t covariance_history_prices{756};
    // How many dates of the UNION of the covariance participants' dates a participant's last
    // close may trail the newest such date before the optimiser leaves it out of the date
    // intersection (portfolio.json "covariance_stale_dates"; absent means 5; T-7b-1 7d). A
    // participant left out gets the guarded 0.01 variance column. Not written by to_json(), for
    // the reason covariance_history_prices is not.
    size_t covariance_stale_dates{5};
    DynamicOptConfig opt_config;      // Optimization configuration
    RiskConfig risk_config;           // Risk management configuration

    std::string version{"1.0.0"};  // Configuration version

    PortfolioConfig() = default;

    PortfolioConfig(Decimal total_capital, double max_strategy_allocation,
                    double min_strategy_allocation, bool use_optimization)
        : total_capital(total_capital),
          max_strategy_allocation(max_strategy_allocation),
          min_strategy_allocation(min_strategy_allocation),
          use_optimization(use_optimization) {}

    // JSON serialization
    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["total_capital"] = static_cast<double>(total_capital);
        j["max_strategy_allocation"] = max_strategy_allocation;
        j["min_strategy_allocation"] = min_strategy_allocation;
        j["use_optimization"] = use_optimization;
        // risk_modules is deliberately absent: this object is stored verbatim in
        // backtest.run_metadata.portfolio_config, and T-6a's declared diff is deletions
        // only. The module list is recorded there by T-7 item 10, through describe().
        j["allow_fractional_positions"] = allow_fractional_positions;
        j["opt_config"] = opt_config.to_json();
        j["risk_config"] = risk_config.to_json();
        j["version"] = version;
        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("total_capital"))
            total_capital = Decimal(j.at("total_capital").get<double>());
        if (j.contains("max_strategy_allocation")) {
            max_strategy_allocation = j.at("max_strategy_allocation").get<double>();
        }
        if (j.contains("min_strategy_allocation")) {
            min_strategy_allocation = j.at("min_strategy_allocation").get<double>();
        }
        if (j.contains("use_optimization")) {
            use_optimization = j.at("use_optimization").get<bool>();
        }
        if (j.contains("allow_fractional_positions")) {
            allow_fractional_positions = j.at("allow_fractional_positions").get<bool>();
        }
        if (j.contains("covariance_history_prices")) {
            covariance_history_prices = j.at("covariance_history_prices").get<size_t>();
        }
        if (j.contains("covariance_stale_dates")) {
            covariance_stale_dates = j.at("covariance_stale_dates").get<size_t>();
        }
        if (j.contains("opt_config"))
            opt_config.from_json(j.at("opt_config"));
        if (j.contains("risk_config"))
            risk_config.from_json(j.at("risk_config"));
        if (j.contains("version"))
            version = j.at("version").get<std::string>();
    }
};

/**
 * @brief T-7b-3 rulings 7 and 8 (HD 2026-09-27): every cut lap is delivered once and ends the
 *        loop. When its cut rule fixed symbols the BOOK_GATE will hold, the cuttable symbols are
 *        exhausted and the held contracts alone keep the cut book's gross notional above the gate's
 *        level, the day is stored over the limit: this record says so (RISK_OVER_LIMIT_BY_HOLD).
 */
struct OverLimitByHold {
    bool over_limit_by_hold = false;
    std::vector<std::string> symbols;  ///< the held symbols the cut rule fixed, sorted
    double target = 0.0;               ///< the gate's level: factor x the lap book's gross notional
    double cut_book = 0.0;             ///< the cut book's gross notional
    int lap = 0;
};

/**
 * @brief Manages multiple strategies and their allocations
 * Optionally applies optimization and risk management
 */
class PortfolioManager {
public:
    /**
     * @brief Constructor
     * @param config Portfolio configuration
     * @param id Optional identifier for this manager
     */
    explicit PortfolioManager(PortfolioConfig config, std::string id = "PORTFOLIO_MANAGER",
                              std::shared_ptr<InstrumentRegistry> registry = nullptr);

    /**
     * @brief Add a strategy to the portfolio
     * @param strategy Strategy to add
     * @param initial_allocation Initial capital allocation
     * @param use_optimization Whether this strategy uses optimization
     * @return Result indicating success or failure
     */
    Result<void> add_strategy(std::shared_ptr<StrategyInterface> strategy,
                              double initial_allocation, bool use_optimization = false);

    /**
     * @brief Process new market data
     * @param data New market data
     * @param skip_execution_generation If true, skip execution generation (used during warmup)
     * @param current_timestamp Optional current day's timestamp for execution fill_time (if not provided, uses data[0].timestamp)
     * @param session_symbols Optional (T-7a C4, the backtest predicate): the symbols whose bar in
     *        `data` is a SESSION. When given, a symbol NOT in it gets no fill AND no book change:
     *        its current_positions entry is held at the filled-ledger quantity (a symbol with no
     *        bar, or a JUNK bar, in the signal group). Null (the default, every live caller and the
     *        equity backtest) keeps the old skip: no fill, the book moves to the target.
     * @return Result indicating success or failure
     */
    Result<void> process_market_data(
        const std::vector<Bar>& data, bool skip_execution_generation = false,
        std::optional<Timestamp> current_timestamp = std::nullopt,
        const std::unordered_set<std::string>* session_symbols = nullptr);

    /**
     * @brief The capital the book is sized on from the next process_market_data (T-7b-2 9c,
     *        HD 2026-09-25: compounding, sizing on current equity).
     *
     * Every sizing input moves together (T-4c's W-B rule: a capital change that moves the
     * strategies but not the gate or the optimizer changes what the leverage limit means):
     * each strategy's capital becomes capital x its allocation (the position line, the buffer
     * width's Carver term and the notional concentration cap all read it); the optimizer's
     * weight per contract is notional / capital; every risk module is handed the capital
     * (the Carver gate's gross and net leverage divide by it); the RiskContext carries it.
     * Valuation does not move: the equity curve, P&L, returns and margin are the account's.
     *
     * Never called, the PM sizes on PortfolioConfig::total_capital as before (the equity
     * runner and the equity backtest do not call it). Refuses a capital that is not a finite
     * positive number (nothing changes), and stops at the first strategy or module that refuses
     * it (the ones before it already carry the new capital): the caller treats any error as
     * fatal for the rebalance and never sizes on.
     */
    Result<void> set_sizing_capital(double capital);

    /// Hands every sleeve the consumed bars that precede the first bar it will be fed, for the
    /// estimators whose window reaches back before a run's own window (StrategyInterface::
    /// seed_history). It reaches the sleeves' own histories only: the manager's price history, its
    /// cost model and its risk modules are not fed these bars. The first refusal stops the call.
    Result<void> seed_strategy_history(const std::vector<Bar>& bars);

    /// The capital the next process_market_data sizes on (PortfolioConfig::total_capital until
    /// set_sizing_capital is called).
    double sizing_capital() const;

    /**
     * @brief Update strategy allocations
     * @param allocations Map of strategy ID to allocation
     * @return Result indicating success or failure
     */
    Result<void> update_allocations(const std::unordered_map<std::string, double>& allocations);

    /**
     * @brief Get current portfolio positions, scaled by each strategy's allocation.
     *
     * Returns Σᵢ qᵢ × allocᵢ per symbol. Each strategy is already sized for its
     * own capital slice (capital_allocation = initial_capital × allocation), so
     * the broker actually holds Σᵢ qᵢ — applying allocation here under-states
     * exposure on multi-strategy portfolios and produces fractional contracts.
     *
     * Use this only for legacy paths (debug logging, price-only lookups, or
     * the backward-compatibility portfolio-level execution generator). For
     * margin, risk, CSV, or anything that must reflect broker reality, sum
     * `get_strategy_positions()` per symbol instead.
     */
    std::unordered_map<std::string, Position> get_portfolio_positions() const;

    /**
     * @brief Get position changes needed
     * @return Map of symbol to required position change
     */
    std::unordered_map<std::string, double> get_required_changes() const;

    /**
     * @brief Get recent execution reports from position changes
     * @return Vector of execution reports since last call
     */
    std::vector<ExecutionReport> get_recent_executions() const;

    /**
     * @brief Get recent execution reports per strategy
     * @return Map of strategy ID to vector of execution reports
     */
    std::unordered_map<std::string, std::vector<ExecutionReport>> get_strategy_executions() const;

    /**
     * @brief Clear the execution history (useful after retrieving them)
     */
    void clear_execution_history();

    /**
     * @brief Append a synthetic ExecutionReport to a strategy's history.
     *
     * Used by carry-cost accrual (e.g., overnight borrow fees) to keep the
     * per-execution audit trail in sync with the daily equity-curve cost
     * roll-up. The exec is appended as-is; callers are responsible for
     * setting symbol, filled_quantity (typically 0 for synthetic carry),
     * timestamps, and cost fields. Idempotent at the per-call level; not
     * deduped across calls.
     */
    /// T-ROLLX (section 6.5): inserts the ROLL legs of a confirming bar AHEAD of that bar's
    /// STRATEGY fills (at `index`, the sleeve's execution count before the bar) so the stored
    /// order is closing leg, opening leg, then the day's fills; returns the index used.
    size_t insert_executions_at(const std::string& strategy_id, size_t index,
                                const std::vector<ExecutionReport>& execs);
    /// How many ROLL legs the sleeve has booked so far (the RL-<sid>-<n> counter).
    size_t roll_leg_count(const std::string& strategy_id) const;
    void append_synthetic_execution(const std::string& strategy_id,
                                    const ExecutionReport& exec);

    /**
     * @brief Clear all executions including strategy-level (used during warmup)
     */
    void clear_all_executions();

    /**
     * @brief Update market data for transaction cost calculations
     * @param symbol Symbol to update
     * @param volume Trading volume
     * @param close_price Current close price
     * @param prev_close_price Previous close price
     */
    void update_cost_manager_market_data(const std::string& symbol, double volume,
                                         double close_price, double prev_close_price);

    /**
     * @brief This manager's own cost model: it prices the optimizer's cost vector
     *        (calculate_trading_costs) and the executions this manager generates.
     *
     * H-2 (T-7b-1 C8d): the backtest feeds it through update_cost_manager_market_data; the live
     * futures runners feed it through feed_futures_cost_model (futures_cost_feed.hpp), the same
     * K2 feed as the execution manager's, before process_market_data. Unfed, every entry of the
     * cost vector is priced off the fallbacks (ADV 100,000, vol_mult 1.0).
     */
    transaction_cost::TransactionCostManager& get_transaction_cost_manager() {
        return cost_manager_;
    }

    /**
     * @brief Register ADV-tiered equity cost configs on THIS manager's cost model. E2-C9.
     *
     * PortfolioManager owns its own TransactionCostManager, separate from the one
     * BacktestCoordinator holds, and the two cost different things:
     *
     *   - This one prices the executions PortfolioManager generates, which are what reach
     *     backtest.executions AND the equity curve (via calculate_period_transaction_costs).
     *   - The coordinator's prices the re-costed copies that feed the reported METRICS.
     *
     * Only the coordinator's was ever registered (bt_equity_mean_reversion.cpp calls
     * register_equity_costs_from_bars on it), so the stored executions and the equity curve
     * were priced from the static hardcoded configs while the metrics used ADV-tiered ones --
     * two different cost bases inside a single backtest run. It stayed invisible because the
     * equity universe is exactly the eight symbols initialize_default_configs() hardcodes, so
     * both managers at least had EQUITY configs; only the tier parameters differed.
     *
     * Register both, or they disagree. Futures is unaffected: futures roots are pre-registered
     * in initialize_default_configs() and this is only called from the equity apps.
     */
    int register_equity_cost_configs(
        const std::vector<std::string>& symbols,
        const std::unordered_map<std::string, std::vector<Bar>>& bars_by_symbol,
        int adv_lookback_days = 20);

    /**
     * @brief Get all strategies managed by this portfolio
     * @return Vector of strategy interfaces
     */
    std::vector<std::shared_ptr<StrategyInterface>> get_strategies() const;

    /**
     * @brief Get optimized positions per strategy (after optimization/rounding)
     * @return Map of strategy ID to map of symbol to position
     */
    std::unordered_map<std::string, std::unordered_map<std::string, Position>> get_strategy_positions() const;

    /**
     * @brief The book each strategy has actually traded into: the filled-position ledger, per
     *        strategy and symbol, as Positions carrying the symbol and the filled quantity.
     *
     * get_strategy_positions() returns the sleeves' current TARGETS, which a cycle that generates
     * no executions (the backtest's warm-up) sets without filling them. A consumer that needs the
     * book HELD at the start of a bar (the roll legs, LOOP_SPEC section 6.5) reads this one: a
     * target no fill stands behind is not a position, and has nothing to roll.
     */
    std::unordered_map<std::string, std::unordered_map<std::string, Position>>
    get_filled_strategy_positions() const;

    /**
     * @brief Update a specific position for a strategy (e.g., to update PnL values)
     * @param strategy_id Strategy identifier
     * @param symbol Position symbol
     * @param updated_pos Updated position object
     * @return Result indicating success or failure
     */
    Result<void> update_strategy_position(
        const std::string& strategy_id,
        const std::string& symbol,
        const Position& updated_pos);

    /**
     * @brief Get portfolio's current total value
     * @param current_prices Map of symbol to current price
     * @return Current portfolio value including cash
     */
    double get_portfolio_value(const std::unordered_map<std::string, double>& current_prices) const;

    /**
     * @brief Get the portfolio's configuration
     * @return Portfolio configuration
     */
    const PortfolioConfig& get_config() const {
        return config_;
    }

    /**
     * @brief Replace the risk modules: the portfolio scope's (by default the Carver module the
     *        constructor builds from config.risk_config) and, per strategy id, a sleeve's.
     *        Call after add_strategy and before process_market_data.
     * @return An error, leaving the modules unchanged, for: a null module; a duplicate module id
     *         within a scope; a sleeve key that is not a registered strategy; more than one
     *         REPLACE-capable module in a scope; more than one COMPOSITION-term module along a
     *         sleeve -> portfolio chain (the scale-invariant terms would be counted twice).
     */
    Result<void> set_risk_modules(
        std::vector<RiskModulePtr> portfolio_modules,
        std::unordered_map<std::string, std::vector<RiskModulePtr>> sleeve_modules = {});

    /**
     * @brief The last process_market_data call's risk decisions: every module, every lap,
     *        requested and applied. A copy. Cleared at every rebalance boundary, so after a
     *        runner's explicit call it holds that call only.
     */
    std::vector<RiskDecisionRecord> last_risk_decisions() const;

    /**
     * @brief build_risk_decisions_json over describe() of every module (annotated with its
     *        scope) and last_risk_decisions().
     */
    nlohmann::json risk_decisions_json() const;

    /**
     * @brief The last process_market_data call's delivered cut (T-7b-2 C9a, T-VOL C4): the
     *        account book's gross notional after lap 1's optimizer step and at the end of the
     *        call, both at one notional per contract per symbol. A copy. Reset at the same
     *        rebalance boundary as last_risk_decisions(). Log only; nothing reads it back.
     *        Each field: DeliveredCut in risk_scale_report.hpp.
     */
    DeliveredCut last_delivered_cut() const;

    /**
     * @brief T-7b-3 ruling 7: the last process_market_data call's over-limit-by-hold record (see
     *        OverLimitByHold). Reset at the start of every call; set only when the BOOK_GATE holds
     *        left the delivered cut book above the gate's level. A copy.
     */
    OverLimitByHold last_over_limit_by_hold() const;

    /**
     * @brief T-7b-2 C9a3: the same rebalance's delivered cut with final_gross measured on `stored_book` (the
     *        account book the runner actually stores, e.g. after a live runner's BOOK_GATE hold), at the notionals
     *        of the end-of-call measurement; a symbol those did not cover (a position the hold re-inserted) is
     *        valued by delivered_notional_per_contract now. The lap-1 book is unchanged. Returns
     *        last_delivered_cut() unchanged (every figure na) when no measurement ran.
     */
    DeliveredCut delivered_cut_for_book(const std::map<std::string, double>& stored_book) const;

    /**
     * @brief Mark this manager as driven by a backtest (BacktestCoordinator::run_portfolio). Read into
     *        RiskContext::is_backtest and scope_is_seeded, and it switches on the per-bar netting of the
     *        sleeves' execution reports (K3), which are the stored fills only in a backtest (T-7b-2 C8b4).
     */
    void set_backtest_mode(bool is_backtest) {
        is_backtest_ = is_backtest;
    }

    /**
     * @brief The symbols whose book the caller's BOOK_GATE will hold at the held quantity after the
     *        next process_market_data returns (T-7b-3 D-1b, HD 2026-09-27). A lap the risk gate cuts
     *        fixes each of them at its held quantity and never cuts it (cut_delivery.hpp), so the cut
     *        is taken from contracts that can trade. The live futures runners pass every symbol whose
     *        T-1 verdict is not SESSION, the key hold_non_session_symbols uses.
     *
     * One call, one rebalance: the next process_market_data takes the set when it starts and leaves
     * it empty, so a later call without the setter holds nothing. Each call replaces the set. The
     * futures backtest does not need it: there process_market_data's session_symbols gives the set.
     */
    void set_book_gate_holds(std::unordered_set<std::string> symbols) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_book_gate_holds_ = std::move(symbols);
    }

private:
    PortfolioConfig config_;
    std::string id_;
    // T-7b-2 9c: what the book is sized on (set_sizing_capital); config_.total_capital, the
    // configured account size, is left as configured. Guarded by mutex_.
    Decimal sizing_capital_{Decimal(0.0)};

    std::unique_ptr<DynamicOptimizer> optimizer_;
    std::vector<RiskModulePtr> risk_modules_;  // portfolio scope, evaluated in order
    std::unordered_map<std::string, std::vector<RiskModulePtr>> sleeve_risk_modules_;  // by strategy id
    std::vector<RiskDecisionRecord> risk_decisions_;  // guarded by mutex_
    // T-7b-2 C9a: the account book after lap 1's optimizer step (contracts per symbol) and the
    // rebalance's delivered cut; both reset at the rebalance boundary. guarded by mutex_
    std::map<std::string, double> delivered_lap1_book_;
    bool delivered_has_lap1_{false};
    DeliveredCut delivered_cut_;
    std::map<std::string, double> delivered_npc_;  // the end-of-call measurement's notionals (C9a3)
    bool is_backtest_{false};
    // Per rebalance, cleared at the boundary: strategies pinned by a risk REFUSE / REPLACE (skipped
    // by the optimiser, later scales, the fraction scan, forced rounding and the final check), and
    // per scope the product of the quantised factors applied so far (-> RiskContext::applied).
    std::unordered_set<std::string> pinned_scopes_;
    std::unordered_map<std::string, double> rebalance_applied_;
    // Scopes whose previous book was actually SEEDED by the runner (update_strategy_position).
    // A REFUSE means "ship yesterday's book", and yesterday's book is only in current_positions
    // if somebody put it there: on a live PM that was never seeded, pinning would ship a FLAT
    // book and the runner's diff against trading.positions would liquidate (T-6a ADVERSARIAL
    // A-2). Never cleared: seeding is a fact about the run, not about the rebalance.
    std::unordered_set<std::string> seeded_scopes_;
    // process_market_data has run at least once, so the sleeve keys have been checked against the
    // registered strategies (they cannot be checked in the constructor: strategies are added
    // afterwards).
    bool sleeve_keys_validated_{false};
    std::shared_ptr<InstrumentRegistry> registry_{nullptr};

    struct StrategyInfo {
        std::shared_ptr<StrategyInterface> strategy;
        double allocation;
        bool use_optimization;
        std::unordered_map<std::string, Position> current_positions;
        std::unordered_map<std::string, Position> target_positions;
    };

    std::unordered_map<std::string, StrategyInfo> strategies_;
    std::vector<ExecutionReport> recent_executions_;  // Portfolio-level (aggregated)
    std::unordered_map<std::string, std::vector<ExecutionReport>> strategy_executions_;  // Per-strategy executions
    // Per-strategy filled position (symbol -> net qty) accumulated from the
    // executions this manager generates; baseline for sizing the next order as
    // (target - filled). Strategy-agnostic -- does not read strategy positions_.
    std::unordered_map<std::string, std::unordered_map<std::string, double>> filled_positions_;
    
    // Track previous day close prices for PnL Lag Model (prevents lookahead bias)
    std::unordered_map<std::string, double> previous_day_close_prices_;

    mutable std::mutex mutex_;
    const std::string instance_id_;

    // The optimiser's price history, kept by the PM itself from the bars process_market_data
    // is fed: symbol -> (UTC day number -> close). One close per symbol per date (a repeated
    // date overwrites, so duplicate bars cannot lengthen a series), at most
    // config_.covariance_history_prices dates per symbol, oldest dropped first. A symbol that
    // leaves the feed keeps its series, which stops growing. No strategy's history is read.
    std::unordered_map<std::string, std::map<int64_t, double>> closes_by_date_;
    /// T-ROLLX: each stored close's vendor instrument id (empty when unknown), the same keys as
    /// closes_by_date_, so the optimizer's covariance reads the ADJUSTED returns (roll_series.hpp).
    std::unordered_map<std::string, std::map<int64_t, std::string>> ids_by_date_;
    std::unordered_map<std::string, std::vector<double>> historical_returns_;
    MarketData current_market_data_;

    size_t max_history_length_ = 2520;  // Keep up to 1 year of return data

    // Covariance cache for multi-iteration convergence optimization
    bool covariance_cache_valid_{false};
    std::vector<std::string> cached_symbols_;
    std::vector<std::vector<double>> cached_covariance_;

    // Transaction cost manager for calculating execution costs
    transaction_cost::TransactionCostManager cost_manager_;

    /**
     * @brief Calculate weights per contract for each symbol
     * @param symbols List of symbols to calculate weights for
     * @param capital Total capital available for allocation
     * @return Vector of weights per contract for each symbol
     */
    std::vector<double> calculate_weights_per_contract(const std::vector<std::string>& symbols,
                                                       double capital) const;

    /**
     * @brief The optimizer's cost vector: per symbol, the cost of one contract over that
     *        contract's notional (F4, T-7b-1 C8d), so |dw| x costs[i] with dw in weight is the
     *        trade's dollar cost as a fraction of capital
     * @param symbols List of symbols to calculate costs for
     * @param capital Unused since F4 (the entry does not depend on capital); kept for the call
     * @return Vector of cost_per_contract / notional_per_contract, 0 for a symbol no trend
     *         sleeve holds
     */
    std::vector<double> calculate_trading_costs(const std::vector<std::string>& symbols,
                                                double capital) const;

    /**
     * @brief Update historical returns for all symbols
     * @param data New market data
     */
    void update_historical_returns(const std::vector<Bar>& data);

    /**
     * @brief The optimizer's return series, aligned by DATE (T-7a INSERT S3)
     * @param closes_by_symbol The symbols in the matrix, each with its date-keyed closes
     * @return Per symbol, its returns between consecutive dates of the INTERSECTION of the
     *         symbols' dates (a date one symbol lacks is dropped for all, and the next return
     *         spans it for every symbol); all non-empty series have the same length. A symbol
     *         with fewer than two usable closes gets an empty series and does not shrink the
     *         intersection. T-7b-1 7d: a participant whose last usable date trails the newest
     *         date of the participants' union by more than config_.covariance_stale_dates dates,
     *         and (while the intersection gives fewer than 20 returns) the participant with the
     *         fewest usable dates, is left out with an empty series and a WARN, so
     *         calculate_covariance_matrix guards its column instead of the window ending at a
     *         stale date or the whole matrix falling to the 0.01 diagonal. Logs one
     *         COVARIANCE_DATE_ALIGNED line.
     */
    std::unordered_map<std::string, std::vector<double>> date_aligned_returns(
        const std::unordered_map<std::string, std::map<int64_t, double>>& closes_by_symbol) const;

    /**
     * @brief date_aligned_returns on the ADJUSTED series (T-ROLLX; LOOP_SPEC v6.1 section 2.3)
     * @param closes_by_symbol The raw closes (the usable dates and the return's denominator)
     * @param adjusted_by_symbol Per symbol, the adjusted level at each of the same dates (the raw
     *        close plus the later contract-switch steps, roll_series::adjusted_levels); a symbol
     *        absent here reads its raw closes
     * @return As above, with r_t = (A(D[t]) - A(D[t-1])) / c(D[t-1]): a switch's step between two
     *         intersection dates is not a return. With adjusted == raw this is the function above.
     */
    std::unordered_map<std::string, std::vector<double>> date_aligned_returns(
        const std::unordered_map<std::string, std::map<int64_t, double>>& closes_by_symbol,
        const std::unordered_map<std::string, std::map<int64_t, double>>& adjusted_by_symbol) const;

    /**
     * @brief The adjusted level of every stored close of the given symbols (closes_by_date_ and
     *        ids_by_date_ in date order, anchored on each symbol's latest stored close).
     */
    std::unordered_map<std::string, std::map<int64_t, double>> adjusted_closes_by_symbol(
        const std::unordered_map<std::string, std::map<int64_t, double>>& closes_by_symbol) const;

    /**
     * @brief Calculate covariance matrix from returns
     * @param returns_by_symbol Map of symbol to returns. The optimizer passes the output of
     *        date_aligned_returns, whose series are already paired by date and of one length,
     *        so the tail-by-count alignment here is the identity on them.
     * @return Covariance matrix
     */
    std::vector<std::vector<double>> calculate_covariance_matrix(
        const std::unordered_map<std::string, std::vector<double>>& returns_by_symbol);

    /**
     * @brief Optimize positions for strategies that use optimization
     * @return Result indicating success or failure
     */
    Result<void> optimize_positions();

    // T-7b-2 9e, the gate's cut delivered in whole contracts (include/trade_ngin/portfolio/
    // cut_delivery.hpp): the account book the lap's optimizer produced before the gate, the
    // factor the gate multiplied the book by on this lap (1 = none), and each symbol's notional
    // per contract as the optimizer last priced it (weight per contract x sizing capital).
    std::map<std::string, double> lap_book_before_gate_;
    double lap_cut_factor_{1.0};
    std::unordered_map<std::string, double> cut_notional_per_contract_;
    // T-7b-3 D-1b: the symbols the BOOK_GATE will hold, which the cut fixes at their held
    // quantity: every symbol in `holds` (set_book_gate_holds), and, when `session_symbols` is
    // given, every symbol of the lap or held book not in it.
    // Sets over_limit_by_hold_ when those held symbols keep the cut book above the gate's level
    // (ruling 7). The loop ends on every lap that calls it (ruling 8).
    void deliver_lap_cut(int lap, const std::unordered_set<std::string>& holds,
                         const std::unordered_set<std::string>* session_symbols);
    OverLimitByHold over_limit_by_hold_;
    // set_book_gate_holds' set, taken (and emptied) by the next process_market_data.
    std::unordered_set<std::string> pending_book_gate_holds_;

    /// T-7b-2 CGW: the risk gate's participants this rebalance (RiskContext::gate_participants),
    /// rebuilt before lap 1 by gate_participants_for_rebalance.
    std::set<std::string> gate_participants_;

    /**
     * @brief Build the context a risk module sees for one call
     */
    RiskContext make_risk_context(RiskPhase phase, int lap, RiskScope scope,
                                  const std::string& scope_id, Decimal capital,
                                  const std::vector<Bar>& data,
                                  std::optional<Timestamp> as_of, bool is_warmup) const;

    /// What a portfolio-scope REFUSE or REPLACE asks the loop to do after apply_risk_management.
    struct RiskLapOutcome {
        bool pin_all{false};
        RiskAction action{RiskAction::NONE};
        std::string module_id;
        std::unordered_map<std::string, Position> replace_book;
        // A REFUSE on a LIVE scope whose previous book was never seeded. Pinning would ship a
        // FLAT book, which the runner's diff against trading.positions reads as "liquidate
        // everything" -- the opposite of the refusal's meaning. process_market_data fails.
        bool refuse_unseeded{false};
        std::string unseeded_scope;
    };

    /// The combination of one scope's decisions (precedence REFUSE > REPLACE > SCALE > WARN >
    /// NONE; the SCALE applied is the minimum valid one). rows[k] = the applied action and factor
    /// recorded against decision k.
    struct RiskVerdict {
        RiskAction action{RiskAction::NONE};
        size_t winner{static_cast<size_t>(-1)};
        double scale{1.0};
        Decimal factor{Decimal(1.0)};
        std::vector<std::pair<RiskAction, Decimal>> rows;
    };

    /**
     * @brief Apply risk management to positions
     * @param data This call's bars
     * @param lap_ctx The lap's context (phase LAP, portfolio scope)
     * @param outcome Set when a REFUSE or REPLACE must pin every strategy and end the loop
     * @return Result indicating success or failure
     */
    Result<void> apply_risk_management(const std::vector<Bar>& data, const RiskContext& lap_ctx,
                                       RiskLapOutcome& outcome);

    /// Combine one scope's decisions; logs an invalid SCALE (ERROR) and every WARN, in module order.
    RiskVerdict combine_risk_decisions(const std::vector<RiskDecision>& decisions,
                                       const RiskContext& ctx) const;

    /// Push one row to risk_decisions_ (takes mutex_).
    void record_risk_decision(const RiskContext& ctx, const std::string& module_id,
                              RiskDecision requested, RiskAction applied_action,
                              Decimal applied_factor, bool empty_book, std::string error);

    /// One scope's evaluate (or finalize) pass, fail-CLOSED. Every module is called; one that
    /// returns an error Result OR THROWS contributes a NONE decision and its message to
    /// `errors[k]`, and the scope then combines and applies what the HEALTHY modules returned.
    /// A REFUSE or a SCALE a module already returned is never discarded because a later module
    /// failed: combine first, then fail closed (T-6a ADVERSARIAL A-1). Returns one decision per
    /// module, in module order; `errors` is sized to match.
    std::vector<RiskDecision> evaluate_scope_modules(
        std::vector<RiskModulePtr>& modules,
        const std::unordered_map<std::string, Position>& book, const RiskContext& ctx,
        bool finalize_phase, std::vector<std::string>& errors);

    /// The other half of fail-closed: a module that FAILED is treated as a refusal of its scope,
    /// because its silence cannot be read as consent. At PORTFOLIO scope that holds for a module
    /// of any capability (HD 2026-09-21, option b: the lone Carver, {SCALE, WARN}, included); at
    /// SLEEVE scope only for a module that could have REFUSED. Returns true when `verdict` was
    /// upgraded to REFUSE, with the failed module's row set to REFUSE; names it in `module_id`.
    bool refuse_on_failed_gatekeeper(const std::vector<RiskModulePtr>& modules,
                                     const std::vector<std::string>& errors,
                                     const RiskContext& ctx, RiskVerdict& verdict,
                                     std::string& module_id) const;

    /// Tell every module evaluated in a scope what was applied, then record its row. A module
    /// whose `errors[k]` is non-empty was never evaluated: it is recorded with its error and
    /// applied_action NONE (REFUSE when its failure refused the scope), and its on_applied is
    /// NOT called.
    void deliver_and_record(const std::vector<RiskModulePtr>& modules,
                            std::vector<RiskDecision>& decisions, const RiskVerdict& verdict,
                            const RiskContext& ctx, bool pinned,
                            const std::vector<std::string>& errors, size_t scopes_skipped = 0);

    /// The sleeve scope: once per rebalance, before the loop, each sleeve's own modules on its
    /// own targets. A no-op when no sleeve has modules.
    Result<void> apply_sleeve_risk(
        const std::vector<Bar>& data,
        const std::unordered_map<std::string, std::unordered_map<std::string, Position>>& prev_positions,
        std::optional<Timestamp> as_of, bool is_warmup);

    /// The post-rounding point: finalize() on the final book, portfolio and sleeves.
    Result<void> apply_post_rounding_risk(
        const std::vector<Bar>& data, int lap,
        const std::unordered_map<std::string, std::unordered_map<std::string, Position>>& prev_positions,
        std::optional<Timestamp> as_of, bool is_warmup);

    /// Can a REFUSE on this scope honestly pin to the previous book? True in a backtest (the
    /// coordinator seeds every day and day one's empty book IS the previous book) and, live,
    /// only once update_strategy_position has put the stored book there.
    bool scope_is_seeded(const std::string& scope_id) const {
        return is_backtest_ || seeded_scopes_.count(scope_id) > 0;
    }

    /// The ONE set of module rules, shared by the constructor and set_risk_modules (T-6a
    /// ADVERSARIAL D-1: the constructor used to validate nothing and the only copy of these
    /// checks had no production caller). Per scope: no null module, unique ids, at most one
    /// REPLACE-capable module; and at most one COMPOSITION-term module along any
    /// sleeve -> portfolio chain. `known_strategy_ids` is checked only when it is engaged: the
    /// constructor runs before add_strategy, so a sleeve key naming no strategy is caught on the
    /// first process_market_data instead (see sleeve_keys_validated_).
    static Result<void> validate_risk_modules(
        const std::vector<RiskModulePtr>& portfolio_modules,
        const std::unordered_map<std::string, std::vector<RiskModulePtr>>& sleeve_modules,
        const std::unordered_set<std::string>* known_strategy_ids);

    /// First process_market_data only: every key of sleeve_risk_modules_ must name a registered
    /// strategy. The loader checks the JSON key against portfolio.json's `strategies`, which is
    /// NOT always the id the runner registers (the live equity runner registers
    /// LIVE_EQUITY_MEAN_REVERSION for the config key MEAN_REVERSION), so a sleeve module could
    /// silently never fire live while firing in the backtest (T-6a ADVERSARIAL A-3). Refuse.
    Result<void> validate_sleeve_keys_once();

    /**
     * @brief Validate allocations sum to 1
     * @param allocations Strategy allocations
     * @return Result indicating if allocations are valid
     */
    Result<void> validate_allocations(
        const std::unordered_map<std::string, double>& allocations) const;

    /**
     * @brief Get positions from all strategies (internal, mutex-not-held variant
     *        of get_portfolio_positions). Same allocation-scaling caveat: returns
     *        Σᵢ qᵢ × allocᵢ, NOT broker truth. See get_portfolio_positions().
     */
    std::unordered_map<std::string, Position> get_positions_internal() const;

    /**
     * @brief T-7b-2 C9a: one notional per contract for each symbol, for the delivered cut. The
     *        optimizer's own figure (contract_size x the latest price of the TrendFollowingStrategy
     *        that carries the symbol, the later strategy winning as in optimize_positions);
     *        otherwise this manager's latest close for it (closes_by_date_) x the registry's
     *        multiplier (1 without a registry entry); a symbol with neither is left out
     *        (DeliveredCut::unpriced). Called with mutex_ held; logs nothing.
     */
    std::map<std::string, double> delivered_notional_per_contract(
        const std::set<std::string>& symbols) const;
};

}  // namespace trade_ngin
