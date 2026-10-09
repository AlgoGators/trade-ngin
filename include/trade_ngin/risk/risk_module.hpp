// include/trade_ngin/risk/risk_module.hpp
#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/risk/risk_manager.hpp"  // RiskResult

namespace trade_ngin {

/// What a module asks the PortfolioManager to do with its scope's book. Exactly one per decision.
enum class RiskAction {
    NONE,    ///< nothing to do; the book passes untouched
    SCALE,   ///< multiply the scope's book by `scale`. A RATE for this lap, never a level. Applied iff
             ///< scale < 1.0 (the test RiskManager::process_positions sets risk_exceeded with);
             ///< scale >= 1.0 is recorded and never multiplied.
    WARN,    ///< the book passes untouched; the PM logs `reason` and records the breach
    REFUSE,  ///< do not trade this scope this rebalance: pin it to the previous positions. A VALUE,
             ///< never an error Result, so none of the loop's fail-open sites can swallow it.
    REPLACE  ///< the module hands back `book` as the scope's targets; at most one REPLACE-capable
             ///< module per scope
};

/// Term families a module measures. Read only by the validator, never by the loop.
enum class RiskTerm {
    COMPOSITION,  ///< scale-invariant: correlation, VaR, jump. At most once along any sleeve->portfolio chain.
    MAGNITUDE,    ///< not scale-invariant: leverage, per-symbol caps, a constant cut. Composes
                  ///< multiplicatively across levels.
    PATH,         ///< depends on the P&L path (drawdown). Composes multiplicatively across levels.
    CUSTOM        ///< warn/refuse conditions and anything else; no composition rule
};

enum class RiskScope { PORTFOLIO, SLEEVE };
enum class RiskPhase { REBALANCE_START, SLEEVE, LAP, POST_ROUNDING };

const char* risk_action_name(RiskAction a);  ///< "NONE" "SCALE" "WARN" "REFUSE" "REPLACE"
const char* risk_term_name(RiskTerm t);      ///< "composition" "magnitude" "path" "custom"
const char* risk_scope_name(RiskScope s);    ///< "portfolio" "sleeve"
const char* risk_phase_name(RiskPhase p);    ///< "rebalance_start" "sleeve" "lap" "post_rounding"

struct RiskDecision {
    RiskAction action{RiskAction::NONE};
    double scale{1.0};      ///< meaningful iff SCALE; the requested double, before Decimal quantisation
    std::string module_id;  ///< the module's id(); set by the module
    std::string reason;     ///< logged and stored on WARN / REFUSE / REPLACE
    bool blind{false};      ///< the module could not measure. Carried and stored; NEVER changes action
    std::optional<RiskResult> metrics;               ///< Carver: the full RiskResult
    std::unordered_map<std::string, Position> book;  ///< only for REPLACE
    nlohmann::json detail = nlohmann::json::object();  ///< per-module trace; left EMPTY on the Carver path
};

/// Everything a module may know about the call. Built by the PM; a module never sees the strategies.
struct RiskContext {
    RiskPhase phase{RiskPhase::LAP};
    int lap{0};                      ///< 0 for REBALANCE_START and SLEEVE; the PM's loop iteration
                                     ///< (1..5) for LAP; the last iteration for POST_ROUNDING
    std::optional<Timestamp> as_of;  ///< process_market_data's current_timestamp (only the backtest
                                     ///< coordinator passes one today)
    bool is_backtest{false};         ///< PortfolioManager::set_backtest_mode(true)
    bool is_warmup{false};           ///< process_market_data's skip_execution_generation
    Decimal capital{Decimal(0.0)};   ///< PORTFOLIO: PortfolioConfig::total_capital. SLEEVE: allocation x
                                     ///< total_capital. The Carver module IGNORES it and keeps
                                     ///< RiskConfig::capital, the value its gate divides by. A module
                                     ///< that reads it must treat capital <= 0 as blind.
    std::string portfolio_id;        ///< the PM's id
    RiskScope scope{RiskScope::PORTFOLIO};
    std::string scope_id;            ///< portfolio_id, or the strategy id for a sleeve
    const std::vector<Bar>* bars{nullptr};  ///< THIS process_market_data call's data; never null from the PM
    std::unordered_map<std::string, double> applied;  ///< scope_id -> product of the QUANTISED factors
                                     ///< applied to that scope so far this rebalance
};

/// What the PM did, delivered to every module it evaluated in that scope and phase/lap.
struct RiskApplied {
    RiskAction action{RiskAction::NONE};  ///< what the PM did to the scope (combined over its modules)
    double requested_scale{1.0};          ///< THIS module's own request (1.0 unless it asked for SCALE)
    Decimal factor{Decimal(1.0)};         ///< the QUANTISED factor actually multiplied: Decimal(scale),
                                          ///< i.e. int64(scale*1e8 + 0.5) (types.hpp); each quantity then
                                          ///< becomes int64(double(q.raw)*double(factor.raw)/1e8), a
                                          ///< truncation toward zero. 1 when nothing was multiplied.
    bool won{false};                      ///< this module's request is the one applied
    bool pinned{false};                   ///< the scope is pinned after this apply
    /// The multiply did NOT reach every strategy of the scope: `scopes_skipped` of them were
    /// already pinned by a sleeve REFUSE or REPLACE and were skipped. The book the module
    /// measured was therefore cut by LESS than `factor`, and a module that keeps a cumulative
    /// level must not claim otherwise (T-6a ADVERSARIAL A-5, before commit 9 reads the level).
    bool partial{false};
    size_t scopes_skipped{0};
};

class RiskModule {
public:
    virtual ~RiskModule() = default;
    virtual const std::string& id() const = 0;
    virtual const std::string& type() const = 0;           ///< "carver" "constant_scale" "warn" "refuse"
    virtual std::set<RiskTerm> terms() const = 0;          ///< for the validator
    virtual std::set<RiskAction> capabilities() const = 0; ///< what evaluate may return besides NONE
    /// EXACTLY ONCE per process_market_data, by the PM, at the rebalance boundary, before any
    /// lap. Must not log or register a component. One call per bar on the bus-driven runners,
    /// one per rebalance on the explicit-call runners and the backtests.
    virtual void begin_rebalance(const RiskContext& ctx) { (void)ctx; }
    /// Once per lap, BEFORE the PM builds the book and BEFORE its empty-book return.
    /// Must not log, must not register a Logger component.
    virtual void on_bars(const std::vector<Bar>& bars, const RiskContext& ctx) {
        (void)bars;
        (void)ctx;
    }
    /// Once per lap on a NON-EMPTY book. An error Result is for failures only; REFUSE is a value.
    virtual Result<RiskDecision> evaluate(const std::unordered_map<std::string, Position>& book,
                                          const RiskContext& ctx) = 0;
    /// Verbatim enough to reconstruct the module.
    virtual nlohmann::json describe() const = 0;
    /// After the PM applied the scope's combined decision (every evaluate that returned OK).
    virtual void on_applied(const RiskApplied& applied, const RiskContext& ctx) {
        (void)applied;
        (void)ctx;
    }
    /// Once per rebalance at the post-rounding point, on the final (rounded) book, before the
    /// final fractional check. The PM honours NONE, WARN and REFUSE here; SCALE and REPLACE are
    /// rejected (ERROR, recorded, not applied): a multiply would re-fractionalise a
    /// whole-contract book. The default decides NONE and must stay silent.
    virtual Result<RiskDecision> finalize(const std::unordered_map<std::string, Position>& book,
                                          const RiskContext& ctx) {
        (void)book;
        (void)ctx;
        RiskDecision d;
        d.module_id = id();
        return Result<RiskDecision>(std::move(d));
    }
};
using RiskModulePtr = std::shared_ptr<RiskModule>;

/// One row of PortfolioManager::last_risk_decisions(): one module, one scope, one lap.
struct RiskDecisionRecord {
    RiskPhase phase{RiskPhase::LAP};
    int lap{0};
    RiskScope scope{RiskScope::PORTFOLIO};
    std::string scope_id;
    std::string module_id;
    RiskDecision requested;                      ///< verbatim as the module returned it
    RiskAction applied_action{RiskAction::NONE}; ///< what the PM did to the scope because of this row
    Decimal applied_factor{Decimal(1.0)};        ///< the quantised multiplicand actually used,
                                                 ///< Decimal(scale); 1 when nothing was multiplied
    bool empty_book{false};                      ///< the book was empty: on_bars ran, evaluate did not
    std::string error;                           ///< non-empty iff evaluate failed (fail-open, logged)
};

/// Pure helper. `modules` = describe() of every module, each annotated by the PM with "scope" and
/// "scope_id". Deterministic, sorted keys; never called on a run path yet.
nlohmann::json build_risk_decisions_json(const std::vector<nlohmann::json>& modules,
                                         const std::vector<RiskDecisionRecord>& records);

}  // namespace trade_ngin
