# Regime-Aware Portfolio Engine

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Architecture Overview](#2-architecture-overview)
3. [Prerequisites](#3-prerequisites)
4. [Regime Policy Layer (Layer 7)](#4-regime-policy-layer-layer-7)
   - 4.1 [Macro → Strategic Constraints](#41-macro--strategic-constraints)
   - 4.2 [Market → Tactical Constraints](#42-market--tactical-constraints)
   - 4.3 [Policy Rule Engine](#43-policy-rule-engine)
5. [Allocation & Risk Engine (Layer 8)](#5-allocation--risk-engine-layer-8)
   - 5.1 [Regime-Conditioned Configuration](#51-regime-conditioned-configuration)
   - 5.2 [Transition Smoothing](#52-transition-smoothing)
   - 5.3 [Risk Budget Allocation](#53-risk-budget-allocation)
   - 5.4 [Cost-Aware Allocation Objective](#54-cost-aware-allocation-objective)
6. [Strategy Layer Integration (Layer 9)](#6-strategy-layer-integration-layer-9)
   - 6.1 [Strategy Interface](#61-strategy-interface)
   - 6.2 [Regime Consumption Pattern](#62-regime-consumption-pattern)
   - 6.3 [Strategy Eligibility](#63-strategy-eligibility)
7. [Execution Bridge Layer (Layer 10)](#7-execution-bridge-layer-layer-10)
   - 7.1 [Execution Controls](#71-execution-controls)
   - 7.2 [Emergency De-Risk Protocol](#72-emergency-de-risk-protocol)
   - 7.3 [Staged Transition Protocol](#73-staged-transition-protocol)
8. [Complete Data Flow](#8-complete-data-flow)
9. [Configuration System](#9-configuration-system)
10. [Failure-State Controls](#10-failure-state-controls)
11. [Monitoring Dashboard KPIs](#11-monitoring-dashboard-kpis)
12. [Implementation Guide](#12-implementation-guide)
13. [Testing Strategy](#13-testing-strategy)
14. [Design Patterns](#14-design-patterns)
15. [Success Criteria](#15-success-criteria)

---

## 1. Executive Summary

### Purpose

The Regime-Aware Portfolio Engine translates regime beliefs into actionable portfolio constraints and strategy parameters. This system ensures that:

- **Beliefs become constraints, not trades** - Regimes define boundaries, not positions
- **Transitions are smooth** - No hard switches unless risk mandates
- **Strategies remain regime-agnostic** - They ask permission, they don't infer
- **Explainability is preserved** - Every decision traces back to regime beliefs

### Key Principle

> **Strategies are regime-aware but regime-agnostic**  
> They ask: "Am I allowed?" "What's my budget?" "What's my multiplier?"  
> They never infer regimes themselves.

### Architecture Layers

| Layer | Name | Purpose |
|-------|------|---------|
| Layer 7 | Regime Policy Layer | Belief → Constraints |
| Layer 8 | Allocation & Risk Engine | Blend configs, smooth transitions, enforce limits |
| Layer 9 | Strategy Layer | Execute within constraints |

**Dependencies**: This document assumes `regime_detection_architecture.md` is complete (Layers 1-6).

---

## 2. Architecture Overview

### Complete System Stack

```
┌───────────────────────────────────────────────────────────┐
│              Strategy Implementations                      │ Layer 9
│  (TrendFollowing, MeanReversion, StatArb, etc.)           │
│  Ask: "Am I eligible?" "What's my risk budget?"           │
└────────────────────────▲──────────────────────────────────┘
                         │
                         │ RegimeConstraints + RiskBudget
                         │
┌────────────────────────┴──────────────────────────────────┐
│          Allocation & Risk Engine                          │ Layer 8
│  • Blend regime-conditioned configs                        │
│  • Smooth transitions                                      │
│  • Enforce hard risk limits                                │
│  • Allocate risk budget across strategies                  │
└────────────────────────▲──────────────────────────────────┘
                         │
                         │ PolicyConfig (per regime)
                         │
┌────────────────────────┴──────────────────────────────────┐
│            Regime Policy Layer                             │ Layer 7
│  • Macro → Strategic Constraints                           │
│  • Market → Tactical Constraints                           │
│  • Policy Rule Engine                                      │
└────────────────────────▲──────────────────────────────────┘
                         │
                         │ RegimeBelief
                         │
┌────────────────────────┴──────────────────────────────────┐
│       Unified Regime Belief Object                         │ Layer 6
│  (From regime detection architecture)                      │
└───────────────────────────────────────────────────────────┘
```

### Key Concepts

**Belief vs Constraint vs Action**:
- **Belief**: P(Recession) = 0.7 (from regime detection)
- **Constraint**: equity_max = 30%, leverage = 0 (from policy layer)
- **Action**: Reduce equity exposure from 80% → 30% (from allocation engine)

**Separation of Concerns**:
- **Regime detection** (Layers 1-6): Infers what regime we're in
- **Policy layer** (Layer 7): Defines what's allowed in each regime
- **Allocation engine** (Layer 8): Decides actual positions within constraints
- **Strategies** (Layer 9): Generate signals within their risk budget

---

## 3. Prerequisites

### Required Completions

1. **Regime Detection Architecture** (`regime_detection_architecture.md`)
   - ✅ Layers 1-6 implemented
   - ✅ RegimeBelief object produced
   - ✅ Market and macro regime ontologies defined

2. **Statistics Module** (`statistics_module_foundation.md` + `statistics_module_enhancements.md`)
   - ✅ All regime detection models implemented

3. **Config System** (`config_deliverable.md`)
   - ✅ JSON configuration
   - ✅ Validation framework
   - ✅ Environment-specific overrides

4. **Portfolio Manager**
   - ✅ Position management
   - ✅ Risk calculations
   - ✅ Order execution interface

5. **Strategy Infrastructure**
   - ✅ Base strategy interface
   - ✅ Strategy registration
   - ✅ Signal generation

---

## 4. Regime Policy Layer (Layer 7)

### Purpose

Convert regime beliefs into concrete constraints. This is where regimes become actionable but still not trades.

### Key Principle

> **Macro regimes cap risk. Market regimes adjust timing.**  
> **Market cannot override macro. Stress overrides opportunity.**

---

### 4.1 Macro → Strategic Constraints

Macro regimes define long-term strategic boundaries.

#### Policy Configuration Structure

```cpp
struct MacroRegimePolicy {
    MacroRegime regime;
    
    // Portfolio-level constraints
    double max_equity_exposure;      // [0, 1] max % of portfolio in equities
    double max_credit_exposure;      // [0, 1] max % in credit instruments
    double max_leverage;             // [0, 3] max gross leverage
    double target_volatility;        // Target portfolio volatility
    
    // Duration/rates
    double max_duration;             // Max portfolio duration (years)
    double min_liquidity_buffer;     // Min % in cash equivalents
    
    // Asset class tilts
    std::map<std::string, double> asset_class_max;  // Max % per asset class
    
    // Risk budget allocation
    std::map<std::string, double> strategy_group_allocation;  // % of risk budget
};
```

#### Example Policies

```cpp
// EXPANSION: Growth optimized, moderate risk
MacroRegimePolicy expansion_policy{
    .regime = MacroRegime::EXPANSION,
    .max_equity_exposure = 0.80,
    .max_credit_exposure = 0.40,
    .max_leverage = 2.0,
    .target_volatility = 0.12,
    .max_duration = 7.0,
    .min_liquidity_buffer = 0.05,
    .asset_class_max = {
        {"equities", 0.80},
        {"credit", 0.40},
        {"commodities", 0.20},
        {"rates", 0.30}
    },
    .strategy_group_allocation = {
        {"trend", 0.40},
        {"mean_reversion", 0.30},
        {"stat_arb", 0.20},
        {"vol_trading", 0.10}
    }
};

// RECESSION: Capital preservation, defensive
MacroRegimePolicy recession_policy{
    .regime = MacroRegime::RECESSION,
    .max_equity_exposure = 0.30,
    .max_credit_exposure = 0.10,
    .max_leverage = 1.0,
    .target_volatility = 0.06,
    .max_duration = 3.0,
    .min_liquidity_buffer = 0.20,
    .asset_class_max = {
        {"equities", 0.30},
        {"credit", 0.10},
        {"commodities", 0.10},
        {"rates", 0.60}  // Flight to quality
    },
    .strategy_group_allocation = {
        {"trend", 0.20},      // Reduced
        {"mean_reversion", 0.10},  // Reduced
        {"stat_arb", 0.30},   // Relative value
        {"vol_trading", 0.40}  // Increased
    }
};

// CRISIS: Maximum defense
MacroRegimePolicy crisis_policy{
    .regime = MacroRegime::CRISIS,
    .max_equity_exposure = 0.10,
    .max_credit_exposure = 0.0,   // No credit risk
    .max_leverage = 0.5,          // Minimal leverage
    .target_volatility = 0.04,
    .max_duration = 2.0,
    .min_liquidity_buffer = 0.40,  // 40% cash
    .asset_class_max = {
        {"equities", 0.10},
        {"credit", 0.0},
        {"commodities", 0.05},
        {"rates", 0.50}
    },
    .strategy_group_allocation = {
        {"trend", 0.10},
        {"mean_reversion", 0.0},  // OFF
        {"stat_arb", 0.20},
        {"vol_trading", 0.70}     // Volatility trading dominant
    }
};
```

---

### 4.2 Market → Tactical Constraints

Market regimes control short-term tactical adjustments.

#### Policy Configuration Structure

```cpp
struct MarketRegimePolicy {
    MarketRegime regime;
    
    // Strategy eligibility
    std::map<std::string, bool> strategy_enabled;  // Which strategies can run
    
    // Sizing adjustments
    double sizing_multiplier;        // [0, 2] scale position sizes
    double max_turnover_daily;       // Max % portfolio turnover per day
    double rebalance_threshold;      // Min deviation to trigger rebalance
    
    // Entry/exit aggressiveness
    double entry_threshold_mult;     // Scale entry signals (tighter in volatile)
    double exit_threshold_mult;      // Scale exit signals
    double stop_loss_mult;           // Scale stop losses
    
    // Timing constraints
    int min_holding_period_bars;     // Min bars to hold position
    double slippage_adjustment;      // Adjust slippage assumptions
};
```

#### Example Policies

```cpp
// LOW_VOL_TREND: Optimal for momentum strategies
MarketRegimePolicy low_vol_trend_policy{
    .regime = MarketRegime::LOW_VOL_TREND,
    .strategy_enabled = {
        {"trend_following", true},
        {"mean_reversion", true},
        {"stat_arb", true},
        {"breakout", true}
    },
    .sizing_multiplier = 1.2,         // Increase size in favorable conditions
    .max_turnover_daily = 0.30,
    .rebalance_threshold = 0.05,
    .entry_threshold_mult = 0.9,      // Easier to enter
    .exit_threshold_mult = 1.0,
    .stop_loss_mult = 1.2,            // Wider stops (less noise)
    .min_holding_period_bars = 5,
    .slippage_adjustment = 1.0
};

// MEAN_REVERTING: Favor stat arb, reduce momentum
MarketRegimePolicy mean_reverting_policy{
    .regime = MarketRegime::MEAN_REVERTING,
    .strategy_enabled = {
        {"trend_following", false},    // OFF
        {"mean_reversion", true},
        {"stat_arb", true},
        {"breakout", false}
    },
    .sizing_multiplier = 1.0,
    .max_turnover_daily = 0.40,       // Allow higher turnover
    .rebalance_threshold = 0.03,      // More frequent rebalancing
    .entry_threshold_mult = 0.95,
    .exit_threshold_mult = 0.95,      // Exit faster
    .stop_loss_mult = 0.8,            // Tighter stops
    .min_holding_period_bars = 2,
    .slippage_adjustment = 1.1
};

// CRASH_STRESS: Extreme defense
MarketRegimePolicy crash_stress_policy{
    .regime = MarketRegime::CRASH_STRESS,
    .strategy_enabled = {
        {"trend_following", false},
        {"mean_reversion", false},
        {"stat_arb", false},
        {"breakout", false}
        // Only allow explicitly defensive strategies
    },
    .sizing_multiplier = 0.2,         // Drastically reduce size
    .max_turnover_daily = 0.50,       // Allow rapid de-risking
    .rebalance_threshold = 0.02,
    .entry_threshold_mult = 2.0,      // Very hard to enter
    .exit_threshold_mult = 0.5,       // Very easy to exit
    .stop_loss_mult = 0.5,            // Very tight stops
    .min_holding_period_bars = 1,
    .slippage_adjustment = 2.0        // Expect high slippage
};
```

---

### 4.3 Policy Rule Engine

Combines macro and market policies with hierarchical rules.

#### Interface

```cpp
class RegimePolicyEngine {
public:
    struct Config {
        std::map<MacroRegime, MacroRegimePolicy> macro_policies;
        std::map<MarketRegime, MarketRegimePolicy> market_policies;
        
        // Blending parameters
        double macro_dominance = 0.7;  // Macro gets 70% weight in conflicts
        bool enforce_hierarchy = true;  // Macro caps override market
    };
    
    struct RegimeConstraints {
        // Combined constraints from both macro and market
        double max_equity_exposure;
        double max_leverage;
        double target_volatility;
        double sizing_multiplier;
        
        std::map<std::string, bool> strategy_enabled;
        std::map<std::string, double> strategy_risk_budget;  // % of total risk
        
        // Metadata
        MacroRegime macro_regime_used;
        MarketRegime market_regime_used;
        std::string constraint_source;  // For auditability
    };
    
    explicit RegimePolicyEngine(const Config& config);
    
    // Main method: belief → constraints
    Result<RegimeConstraints> compute_constraints(
        const RegimeBelief& belief
    );
    
private:
    Config config_;
    
    // Blend macro and market policies
    RegimeConstraints blend_policies(
        const MacroRegimePolicy& macro_policy,
        const MarketRegimePolicy& market_policy,
        const RegimeBelief& belief
    );
    
    // Apply hierarchy rules
    RegimeConstraints enforce_hierarchy_rules(
        const RegimeConstraints& constraints,
        const RegimeBelief& belief
    );
};
```

#### Hierarchy Rules (Hard-Coded)

```cpp
RegimeConstraints enforce_hierarchy_rules(
    const RegimeConstraints& constraints,
    const RegimeBelief& belief
) {
    RegimeConstraints adjusted = constraints;
    
    // Rule 1: Macro CRISIS overrides all market optimism
    if (belief.macro_probs.at(MacroRegime::CRISIS) > 0.5) {
        adjusted.max_equity_exposure = std::min(
            adjusted.max_equity_exposure, 
            0.10  // Hard cap in crisis
        );
        adjusted.max_leverage = std::min(adjusted.max_leverage, 0.5);
        
        // Disable aggressive strategies
        adjusted.strategy_enabled["trend_following"] = false;
        adjusted.strategy_enabled["breakout"] = false;
    }
    
    // Rule 2: Macro RECESSION limits market regime flexibility
    if (belief.macro_probs.at(MacroRegime::RECESSION) > 0.6) {
        adjusted.max_equity_exposure = std::min(
            adjusted.max_equity_exposure,
            0.40
        );
        adjusted.sizing_multiplier = std::min(adjusted.sizing_multiplier, 0.7);
    }
    
    // Rule 3: Market CRASH_STRESS forces immediate de-risking
    if (belief.market_probs.at(MarketRegime::CRASH_STRESS) > 0.7) {
        adjusted.sizing_multiplier = std::min(adjusted.sizing_multiplier, 0.3);
        adjusted.max_leverage = std::min(adjusted.max_leverage, 1.0);
    }
    
    // Rule 4: STAGFLATION limits duration and equity
    if (belief.macro_probs.at(MacroRegime::STAGFLATION) > 0.5) {
        adjusted.max_equity_exposure = std::min(
            adjusted.max_equity_exposure,
            0.50
        );
        // Shorten duration exposure
    }
    
    return adjusted;
}
```

#### Probability-Weighted Blending

```cpp
RegimeConstraints blend_policies(
    const MacroRegimePolicy& macro_policy,
    const MarketRegimePolicy& market_policy,
    const RegimeBelief& belief
) {
    RegimeConstraints result;
    
    // Blend exposure limits (probability-weighted across regimes)
    result.max_equity_exposure = 0.0;
    for (const auto& [regime, prob] : belief.macro_probs) {
        const auto& policy = config_.macro_policies.at(regime);
        result.max_equity_exposure += prob * policy.max_equity_exposure;
    }
    
    // Blend sizing multiplier (weighted across market regimes)
    result.sizing_multiplier = 0.0;
    for (const auto& [regime, prob] : belief.market_probs) {
        const auto& policy = config_.market_policies.at(regime);
        result.sizing_multiplier += prob * policy.sizing_multiplier;
    }
    
    // Strategy eligibility: enabled if probability-weighted score > threshold
    for (const auto& [strategy_name, _] : market_policy.strategy_enabled) {
        double enabled_prob = 0.0;
        for (const auto& [regime, prob] : belief.market_probs) {
            if (config_.market_policies.at(regime).strategy_enabled.at(strategy_name)) {
                enabled_prob += prob;
            }
        }
        result.strategy_enabled[strategy_name] = (enabled_prob > 0.5);
    }
    
    return result;
}
```

---

## 5. Allocation & Risk Engine (Layer 8)

### Purpose

Execute portfolio rebalancing within regime constraints while maintaining stability.

### Key Responsibilities

1. **Blend regime-conditioned configurations** - Apply constraints to portfolio
2. **Smooth transitions** - Prevent abrupt position changes
3. **Enforce hard risk limits** - Never violate regulatory/firm limits
4. **Allocate risk budget** - Distribute risk across strategies

---

### 5.1 Regime-Conditioned Configuration

#### Interface

```cpp
class RegimeAwareAllocationEngine {
public:
    struct Config {
        // Transition smoothing
        double transition_speed = 0.1;  // Max % change per rebalance
        int min_bars_between_regime_trades = 10;
        
        // Risk limits (hard caps, never violated)
        double max_gross_exposure = 2.0;
        double max_net_exposure = 1.0;
        double max_position_size = 0.05;  // % of portfolio
        double max_sector_concentration = 0.25;
        
        // Rebalancing
        double rebalance_threshold = 0.05;  // Min deviation to trade
        std::string rebalance_schedule = "daily";  // or "intraday", "weekly"
    };
    
    struct AllocationOutput {
        std::map<std::string, double> target_positions;  // Instrument → target %
        std::map<std::string, double> current_positions;
        std::map<std::string, double> trades_required;   // Delta
        
        double target_gross_exposure;
        double target_net_exposure;
        double target_volatility;
        
        RegimeConstraints constraints_applied;
        std::string rationale;  // For logging
    };
    
    explicit RegimeAwareAllocationEngine(const Config& config);
    
    // Main method: constraints + current portfolio → target allocation
    Result<AllocationOutput> compute_allocation(
        const RegimeConstraints& constraints,
        const Portfolio& current_portfolio,
        const std::map<std::string, StrategySignal>& strategy_signals
    );
    
private:
    Config config_;
    AllocationOutput previous_allocation_;
    std::chrono::system_clock::time_point last_regime_trade_;
    
    // Core methods
    AllocationOutput blend_strategy_signals(
        const std::map<std::string, StrategySignal>& signals,
        const RegimeConstraints& constraints
    );
    
    AllocationOutput apply_transition_smoothing(
        const AllocationOutput& target,
        const AllocationOutput& current
    );
    
    AllocationOutput enforce_hard_limits(
        const AllocationOutput& target
    );
};
```

---

### 5.2 Transition Smoothing

Prevent abrupt position changes when regimes shift.

```cpp
AllocationOutput apply_transition_smoothing(
    const AllocationOutput& target,
    const AllocationOutput& current
) {
    AllocationOutput smoothed = target;
    
    for (auto& [instrument, target_pos] : smoothed.target_positions) {
        double current_pos = current.current_positions.at(instrument);
        double delta = target_pos - current_pos;
        
        // Limit change to transition_speed per rebalance
        double max_change = config_.transition_speed * std::abs(current_pos);
        if (std::abs(delta) > max_change) {
            double sign = (delta > 0) ? 1.0 : -1.0;
            smoothed.target_positions[instrument] = current_pos + sign * max_change;
            
            LOG_INFO("Position transition smoothed",
                "instrument", instrument,
                "target", target_pos,
                "smoothed", smoothed.target_positions[instrument],
                "current", current_pos
            );
        }
    }
    
    // Exception: If macro regime is CRISIS, allow faster de-risking
    if (constraints_applied.macro_regime_used == MacroRegime::CRISIS) {
        // Override smoothing for risk reduction only
        for (auto& [instrument, target_pos] : smoothed.target_positions) {
            double current_pos = current.current_positions.at(instrument);
            if (std::abs(target_pos) < std::abs(current_pos)) {
                // Reducing risk → allow full transition
                smoothed.target_positions[instrument] = target_pos;
            }
        }
    }
    
    return smoothed;
}
```

---

### 5.3 Risk Budget Allocation

Distribute risk budget across strategies based on regime policy.

```cpp
struct StrategyRiskBudget {
    std::string strategy_name;
    double risk_budget_pct;      // % of total portfolio risk
    double max_volatility;       // Max vol contribution
    double max_gross_notional;   // Max $ exposure
    bool is_enabled;
};

std::map<std::string, StrategyRiskBudget> allocate_risk_budget(
    const RegimeConstraints& constraints,
    double total_portfolio_risk
) {
    std::map<std::string, StrategyRiskBudget> budgets;
    
    for (const auto& [strategy_name, is_enabled] : constraints.strategy_enabled) {
        if (!is_enabled) {
            budgets[strategy_name] = {
                .strategy_name = strategy_name,
                .risk_budget_pct = 0.0,
                .max_volatility = 0.0,
                .max_gross_notional = 0.0,
                .is_enabled = false
            };
            continue;
        }
        
        double risk_pct = constraints.strategy_risk_budget.at(strategy_name);
        
        budgets[strategy_name] = {
            .strategy_name = strategy_name,
            .risk_budget_pct = risk_pct,
            .max_volatility = risk_pct * constraints.target_volatility,
            .max_gross_notional = risk_pct * constraints.max_leverage * NAV,
            .is_enabled = true
        };
    }
    
    return budgets;
}
```

---

### 5.4 Cost-Aware Allocation Objective

When computing target allocations, the optimizer must incorporate transaction costs, liquidity constraints, and turnover limits:

#### 5.4.1 Objective Function

```cpp
// Objective: Maximize risk-adjusted returns subject to cost and capacity constraints
// min_{w} -μ'w + (λ_risk/2) * w'Σw + λ_cost * f_cost(w, w_prev)
//         + λ_impact * f_impact(w) + λ_turnover * f_turnover(w, w_prev)

struct AllocationObjective {
    Eigen::VectorXd expected_returns;     // μ
    Eigen::MatrixXd covariance_matrix;    // Σ
    Eigen::VectorXd current_weights;      // w_prev
    
    // Penalty weights (regime-conditioned)
    double lambda_risk;
    double lambda_cost;
    double lambda_impact;
    double lambda_turnover;
    
    double evaluate(const Eigen::VectorXd& target_weights) const;
};
```

#### 5.4.2 Market Impact Model

```cpp
// Market impact: I(Δw) = κ * |Δw|^1.5 / sqrt(ADV)
// where ADV = average daily volume

double compute_market_impact(
    double delta_weight,
    double adv_normalized,    // ADV as fraction of position
    double kappa = 0.1        // Impact coefficient
) {
    return kappa * std::pow(std::abs(delta_weight), 1.5) / std::sqrt(adv_normalized);
}
```

#### 5.4.3 Turnover Constraints

| Constraint | Hard Limit | Soft Penalty Threshold |
|------------|------------|------------------------|
| Daily turnover | 15% of NAV | 8% of NAV |
| Weekly turnover | 40% of NAV | 25% of NAV |
| Per-instrument turnover | 25% of position | 15% of position |

```cpp
struct TurnoverConstraints {
    double max_daily_turnover = 0.15;
    double soft_daily_turnover = 0.08;
    double max_weekly_turnover = 0.40;
    double soft_weekly_turnover = 0.25;
    double max_position_turnover = 0.25;
    
    bool is_feasible(double proposed_turnover) const {
        return proposed_turnover <= max_daily_turnover;
    }
    
    double compute_penalty(double proposed_turnover) const {
        if (proposed_turnover <= soft_daily_turnover) return 0.0;
        return std::pow((proposed_turnover - soft_daily_turnover), 2);
    }
};
```

---

## 6. Strategy Layer Integration (Layer 9)

### Purpose

Strategies execute within regime-defined constraints without inferring regimes themselves.

---

### 6.1 Strategy Interface

Updated base strategy interface to consume regime information.

```cpp
class BaseStrategy {
public:
    struct RegimeContext {
        bool is_enabled;             // Can this strategy run?
        double risk_budget_pct;      // % of portfolio risk allocated
        double sizing_multiplier;    // Scale position sizes
        double max_gross_notional;   // Max $ exposure
        
        // Optional: regime-specific parameters
        double entry_threshold_mult;
        double exit_threshold_mult;
        double stop_loss_mult;
        
        // Metadata (for logging)
        MacroRegime current_macro_regime;
        MarketRegime current_market_regime;
    };
    
    virtual ~BaseStrategy() = default;
    
    // Generate signals (regime-agnostic logic)
    virtual Result<StrategySignal> generate_signal(
        const MarketData& data
    ) = 0;
    
    // Size positions according to regime context
    virtual Result<StrategySignal> apply_regime_context(
        const StrategySignal& raw_signal,
        const RegimeContext& regime_ctx
    ) = 0;
    
    // Check if strategy should be active
    virtual bool should_execute(const RegimeContext& regime_ctx) const {
        return regime_ctx.is_enabled;
    }
    
protected:
    std::string strategy_name_;
};
```

---

### 6.2 Regime Consumption Pattern

Strategies ask questions, they don't infer regimes.

#### Example: Trend Following Strategy

```cpp
class TrendFollowingStrategy : public BaseStrategy {
public:
    Result<StrategySignal> generate_signal(const MarketData& data) override {
        // Regime-agnostic signal generation
        double fast_ma = compute_ma(data.prices, fast_period_);
        double slow_ma = compute_ma(data.prices, slow_period_);
        
        StrategySignal signal;
        signal.direction = (fast_ma > slow_ma) ? Direction::LONG : Direction::SHORT;
        signal.confidence = std::abs(fast_ma - slow_ma) / slow_ma;
        signal.base_size = 1.0;  // Unit size, will be scaled
        
        return signal;
    }
    
    Result<StrategySignal> apply_regime_context(
        const StrategySignal& raw_signal,
        const RegimeContext& regime_ctx
    ) override {
        // Strategy asks: "Am I allowed to run?"
        if (!regime_ctx.is_enabled) {
            LOG_INFO("Trend strategy disabled by regime",
                "macro_regime", regime_ctx.current_macro_regime,
                "market_regime", regime_ctx.current_market_regime
            );
            return StrategySignal{};  // Empty signal
        }
        
        StrategySignal adjusted = raw_signal;
        
        // Strategy asks: "What's my risk budget?"
        adjusted.base_size *= regime_ctx.risk_budget_pct;
        
        // Strategy asks: "What's my sizing multiplier?"
        adjusted.base_size *= regime_ctx.sizing_multiplier;
        
        // Regime-specific parameter adjustments
        double entry_threshold = base_entry_threshold_ * regime_ctx.entry_threshold_mult;
        
        // Only enter if confidence exceeds regime-adjusted threshold
        if (adjusted.confidence < entry_threshold) {
            LOG_DEBUG("Signal below regime-adjusted threshold",
                "confidence", adjusted.confidence,
                "threshold", entry_threshold
            );
            return StrategySignal{};
        }
        
        // Apply stop loss adjustment
        adjusted.stop_loss = compute_stop_loss() * regime_ctx.stop_loss_mult;
        
        LOG_INFO("Trend signal adjusted for regime",
            "original_size", raw_signal.base_size,
            "adjusted_size", adjusted.base_size,
            "sizing_mult", regime_ctx.sizing_multiplier,
            "macro_regime", regime_ctx.current_macro_regime,
            "market_regime", regime_ctx.current_market_regime
        );
        
        return adjusted;
    }
    
private:
    int fast_period_;
    int slow_period_;
    double base_entry_threshold_;
};
```

#### Example: Mean Reversion Strategy

```cpp
class MeanReversionStrategy : public BaseStrategy {
public:
    bool should_execute(const RegimeContext& regime_ctx) const override {
        // Mean reversion is OFF in trending regimes
        if (regime_ctx.current_market_regime == MarketRegime::LOW_VOL_TREND ||
            regime_ctx.current_market_regime == MarketRegime::HIGH_VOL_TREND) {
            LOG_INFO("Mean reversion disabled in trending regime");
            return false;
        }
        
        return regime_ctx.is_enabled;
    }
    
    Result<StrategySignal> apply_regime_context(
        const StrategySignal& raw_signal,
        const RegimeContext& regime_ctx
    ) override {
        if (!should_execute(regime_ctx)) {
            return StrategySignal{};
        }
        
        StrategySignal adjusted = raw_signal;
        
        // Mean reversion benefits from MEAN_REVERTING regime
        if (regime_ctx.current_market_regime == MarketRegime::MEAN_REVERTING) {
            // Boost confidence
            adjusted.confidence *= 1.2;
            LOG_INFO("Mean reversion boosted in favorable regime");
        }
        
        // Standard regime adjustments
        adjusted.base_size *= regime_ctx.risk_budget_pct;
        adjusted.base_size *= regime_ctx.sizing_multiplier;
        
        return adjusted;
    }
};
```

---

### 6.3 Strategy Eligibility

Centralized strategy eligibility checker.

```cpp
class StrategyEligibilityManager {
public:
    struct EligibilityReport {
        std::string strategy_name;
        bool is_eligible;
        std::vector<std::string> reasons;  // Why enabled/disabled
    };
    
    EligibilityReport check_eligibility(
        const std::string& strategy_name,
        const RegimeContext& regime_ctx,
        const Portfolio& portfolio
    ) {
        EligibilityReport report;
        report.strategy_name = strategy_name;
        report.is_eligible = true;
        
        // Check regime constraints
        if (!regime_ctx.is_enabled) {
            report.is_eligible = false;
            report.reasons.push_back(
                fmt::format("Disabled by {} macro / {} market regime",
                    to_string(regime_ctx.current_macro_regime),
                    to_string(regime_ctx.current_market_regime))
            );
        }
        
        // Check risk budget
        if (regime_ctx.risk_budget_pct < 0.01) {
            report.is_eligible = false;
            report.reasons.push_back("Insufficient risk budget allocated");
        }
        
        // Check portfolio constraints
        if (portfolio.get_gross_exposure() > 0.95 * portfolio.max_gross_exposure) {
            report.is_eligible = false;
            report.reasons.push_back("Portfolio at max gross exposure");
        }
        
        if (report.is_eligible) {
            report.reasons.push_back("All checks passed");
        }
        
        return report;
    }
---

## 7. Execution Bridge Layer (Layer 10)

### Purpose

Bridge between portfolio decisions and actual order execution, ensuring feasibility, cost control, and emergency responsiveness.

---

### 7.1 Execution Controls

#### 7.1.1 Feasible Trade Delta Computation

```cpp
struct FeasibleTradeResult {
    std::map<std::string, double> feasible_trades;  // Instrument → feasible delta
    std::map<std::string, double> deferred_trades;  // Trades to spread over time
    double total_feasible_turnover;
    double total_deferred_turnover;
};

FeasibleTradeResult compute_feasible_trades(
    const std::map<std::string, double>& target_trades,
    const LiquidityData& liquidity,
    const TurnoverConstraints& turnover_limits
) {
    FeasibleTradeResult result;
    
    for (const auto& [instrument, target_trade] : target_trades) {
        double max_trade = liquidity.get_max_trade(instrument);
        double feasible_trade = std::min(std::abs(target_trade), max_trade);
        feasible_trade *= (target_trade > 0 ? 1.0 : -1.0);  // Preserve sign
        
        if (std::abs(feasible_trade) < std::abs(target_trade)) {
            result.deferred_trades[instrument] = target_trade - feasible_trade;
        }
        result.feasible_trades[instrument] = feasible_trade;
    }
    
    // Check aggregate turnover
    result.total_feasible_turnover = compute_total_turnover(result.feasible_trades);
    if (result.total_feasible_turnover > turnover_limits.max_daily_turnover) {
        scale_down_trades(result.feasible_trades, turnover_limits.max_daily_turnover);
    }
    
    return result;
}
```

#### 7.1.2 Participation Constraints

| Constraint | Value | Description |
|------------|-------|-------------|
| Max ADV participation | 15% | Max % of average daily volume |
| Max single-order participation | 5% of ADV | Per-order limit |
| Time spread | 30 min minimum | Spread orders over time |

```cpp
struct ParticipationConstraints {
    double max_adv_participation = 0.15;      // 15% of ADV max
    double max_single_order_participation = 0.05;
    int min_time_spread_minutes = 30;
    
    bool is_feasible(double trade_size, double adv) const {
        return (trade_size / adv) <= max_adv_participation;
    }
};
```

#### 7.1.3 Order Slicing

```cpp
struct SlicedOrder {
    std::string instrument;
    double total_quantity;
    std::vector<double> slice_quantities;
    std::vector<int> slice_intervals_ms;
};

std::vector<SlicedOrder> slice_orders(
    const std::map<std::string, double>& trades,
    const ParticipationConstraints& constraints,
    const LiquidityData& liquidity
) {
    std::vector<SlicedOrder> sliced_orders;
    
    for (const auto& [instrument, trade] : trades) {
        SlicedOrder sliced;
        sliced.instrument = instrument;
        sliced.total_quantity = trade;
        
        double adv = liquidity.get_adv(instrument);
        double max_slice = adv * constraints.max_single_order_participation;
        int num_slices = std::ceil(std::abs(trade) / max_slice);
        
        double slice_size = trade / num_slices;
        for (int i = 0; i < num_slices; ++i) {
            sliced.slice_quantities.push_back(slice_size);
            sliced.slice_intervals_ms.push_back(
                constraints.min_time_spread_minutes * 60 * 1000 / num_slices
            );
        }
        
        sliced_orders.push_back(sliced);
    }
    
    return sliced_orders;
}
```

#### 7.1.4 Slippage Guardrails

| Market Condition | Max Slippage | Action if Exceeded |
|------------------|--------------|-------------------|
| Normal | 10 bps | Continue |
| Elevated vol | 20 bps | Slow down execution |
| Stress | 50 bps | Pause non-critical trades |

```cpp
struct SlippageGuardrails {
    double normal_max_slippage_bps = 10.0;
    double elevated_vol_max_slippage_bps = 20.0;
    double stress_max_slippage_bps = 50.0;
    
    enum class MarketCondition { NORMAL, ELEVATED_VOL, STRESS };
    
    bool should_pause(double actual_slippage_bps, MarketCondition condition) const {
        double threshold = normal_max_slippage_bps;
        if (condition == MarketCondition::ELEVATED_VOL) threshold = elevated_vol_max_slippage_bps;
        if (condition == MarketCondition::STRESS) threshold = stress_max_slippage_bps;
        return actual_slippage_bps > threshold;
    }
};
```

---

### 7.2 Emergency De-Risk Protocol

> [!WARNING]
> Emergency de-risk bypasses normal transition smoothing to rapidly reduce exposure.

#### 7.2.1 Trigger Conditions

| Trigger | Condition | Response Level |
|---------|-----------|----------------|
| Regime crisis signal | P(CRISIS) > 0.8 | Level 3 (Full de-risk) |
| Drawdown breach | Drawdown > 5% daily | Level 2 (Partial de-risk) |
| Liquidity crisis | Spreads > 3x normal | Level 1 (Halt new trades) |
| VaR breach | VaR > 1.5x limit | Level 2 (Partial de-risk) |

```cpp
enum class EmergencyLevel {
    NONE = 0,
    LEVEL_1_HALT_NEW = 1,      // No new positions, hold existing
    LEVEL_2_PARTIAL_DERISK = 2, // Reduce to 50% exposure
    LEVEL_3_FULL_DERISK = 3     // Close all positions
};

struct EmergencyTriggers {
    double crisis_probability_threshold = 0.8;
    double daily_drawdown_threshold = 0.05;
    double spread_multiple_threshold = 3.0;
    double var_breach_multiple = 1.5;
    
    EmergencyLevel evaluate(
        const RegimeBelief& belief,
        const PortfolioMetrics& metrics,
        const MarketConditions& conditions
    ) {
        // Level 3: Full de-risk
        if (belief.macro_probs.at(MacroRegimeL1::RECESSION_INFLATIONARY) > crisis_probability_threshold ||
            belief.market_probs.at(MarketRegimeL1::STRESS_LIQUIDITY) > crisis_probability_threshold) {
            return EmergencyLevel::LEVEL_3_FULL_DERISK;
        }
        
        // Level 2: Partial de-risk
        if (metrics.daily_drawdown > daily_drawdown_threshold ||
            metrics.current_var > var_breach_multiple * metrics.var_limit) {
            return EmergencyLevel::LEVEL_2_PARTIAL_DERISK;
        }
        
        // Level 1: Halt new trades
        if (conditions.spread_multiple > spread_multiple_threshold) {
            return EmergencyLevel::LEVEL_1_HALT_NEW;
        }
        
        return EmergencyLevel::NONE;
    }
};
```

#### 7.2.2 De-Risk Execution

```cpp
void execute_emergency_derisk(
    EmergencyLevel level,
    Portfolio& portfolio,
    ExecutionEngine& execution
) {
    LOG_CRITICAL("EMERGENCY DE-RISK TRIGGERED", "level", static_cast<int>(level));
    
    switch (level) {
        case EmergencyLevel::LEVEL_1_HALT_NEW:
            execution.halt_new_orders();
            LOG_INFO("New orders halted, existing positions maintained");
            break;
            
        case EmergencyLevel::LEVEL_2_PARTIAL_DERISK:
            for (auto& position : portfolio.get_positions()) {
                double target = position.quantity * 0.5;  // Reduce to 50%
                execution.submit_immediate(position.instrument, 
                    target - position.quantity);
            }
            LOG_INFO("Positions reduced to 50% of current exposure");
            break;
            
        case EmergencyLevel::LEVEL_3_FULL_DERISK:
            for (auto& position : portfolio.get_positions()) {
                execution.submit_immediate(position.instrument, 
                    -position.quantity);  // Close all
            }
            LOG_INFO("All positions closed");
            break;
            
        case EmergencyLevel::NONE:
            break;
    }
}
```

---

### 7.3 Staged Transition Protocol

For non-emergency regime transitions, use staged transitions to minimize market impact and costs:

#### 7.3.1 Transition Tiers

| Transition Type | Tier | Execution Speed |
|-----------------|------|-----------------|
| Risk reduction only | Emergency | Immediate |
| Major regime shift | Fast | 2-3 days |
| Minor adjustment | Normal | 5-7 days |
| Routine rebalance | Slow | 10+ days |

```cpp
enum class TransitionTier {
    EMERGENCY,  // Same day
    FAST,       // 2-3 days
    NORMAL,     // 5-7 days
    SLOW        // 10+ days
};

struct StagedTransition {
    std::map<std::string, double> target_delta;
    TransitionTier tier;
    int num_tranches;
    
    std::vector<std::map<std::string, double>> compute_tranches() const {
        std::vector<std::map<std::string, double>> tranches;
        
        for (int i = 0; i < num_tranches; ++i) {
            std::map<std::string, double> tranche;
            double fraction = 1.0 / num_tranches;
            for (const auto& [instrument, delta] : target_delta) {
                tranche[instrument] = delta * fraction;
            }
            tranches.push_back(tranche);
        }
        
        return tranches;
    }
    
    static int get_tranches_for_tier(TransitionTier tier) {
        switch (tier) {
            case TransitionTier::EMERGENCY: return 1;
            case TransitionTier::FAST: return 3;
            case TransitionTier::NORMAL: return 5;
            case TransitionTier::SLOW: return 10;
        }
        return 5;
    }
};
```

---

## 8. Complete Data Flow

### End-to-End Example

```
1. Market Data Arrives
   ↓
2. Regime Detection (Layers 1-6)
   → RegimeBelief{
        macro_probs: {RECESSION: 0.7, EXPANSION: 0.3},
        market_probs: {MEAN_REVERTING: 0.6, CRASH_STRESS: 0.4},
        confidence: 0.8
     }
   ↓
3. Regime Policy Engine (Layer 7)
   → RegimeConstraints{
        max_equity_exposure: 0.35,  // Weighted blend
        max_leverage: 1.2,
        sizing_multiplier: 0.7,
        strategy_enabled: {
            "trend_following": false,  // OFF due to mean-reverting
            "mean_reversion": true,
            "stat_arb": true
        },
        strategy_risk_budget: {
            "mean_reversion": 0.40,
            "stat_arb": 0.60
        }
     }
   ↓
4. Strategy Signal Generation (Layer 9)
   → TrendFollowing.generate_signal() 
      → Returns signal but will be zeroed out (not enabled)
   → MeanReversion.generate_signal()
      → Returns signal with confidence 0.7
   ↓
5. Apply Regime Context (Layer 9)
   → MeanReversion.apply_regime_context()
      → signal.base_size *= 0.40 (risk budget)
      → signal.base_size *= 0.7 (sizing multiplier)
      → Final size = 0.28 (vs 1.0 in favorable regime)
   ↓
6. Allocation Engine (Layer 8)
   → Aggregate signals from enabled strategies
   → Apply transition smoothing
   → Enforce hard limits
   → Output: target_positions
   ↓
7. Order Execution
   → Generate orders to reach target positions
   → Execute via ExecutionEngine
```

---

## 8. Configuration System

### Configuration Structure

```json
{
  "regime_policy": {
    "macro_policies": {
      "EXPANSION": {
        "max_equity_exposure": 0.80,
        "max_leverage": 2.0,
        "target_volatility": 0.12,
        "strategy_group_allocation": {
          "trend": 0.40,
          "mean_reversion": 0.30,
          "stat_arb": 0.20,
          "vol_trading": 0.10
        }
      },
      "RECESSION": {
        "max_equity_exposure": 0.30,
        "max_leverage": 1.0,
        "target_volatility": 0.06,
        "strategy_group_allocation": {
          "trend": 0.20,
          "mean_reversion": 0.10,
          "stat_arb": 0.30,
          "vol_trading": 0.40
        }
      }
      // ... other macro regimes
    },
    "market_policies": {
      "LOW_VOL_TREND": {
        "strategy_enabled": {
          "trend_following": true,
          "mean_reversion": true,
          "stat_arb": true
        },
        "sizing_multiplier": 1.2,
        "entry_threshold_mult": 0.9
      },
      "MEAN_REVERTING": {
        "strategy_enabled": {
          "trend_following": false,
          "mean_reversion": true,
          "stat_arb": true
        },
        "sizing_multiplier": 1.0,
        "entry_threshold_mult": 0.95
      }
      // ... other market regimes
    }
  },
  "allocation_engine": {
    "transition_speed": 0.1,
    "min_bars_between_regime_trades": 10,
    "max_gross_exposure": 2.0,
    "max_position_size": 0.05,
    "rebalance_threshold": 0.05
  }
}
```

### Configuration Validation

```cpp
class RegimePolicyConfigValidator {
public:
    Result<void> validate(const nlohmann::json& config) {
        // Check all regime types have policies
        if (!config.contains("macro_policies")) {
            return Error("Missing macro_policies");
        }
        
        for (int i = 0; i < static_cast<int>(MacroRegime::CRISIS) + 1; ++i) {
            MacroRegime regime = static_cast<MacroRegime>(i);
            std::string regime_name = to_string(regime);
            
            if (!config["macro_policies"].contains(regime_name)) {
                return Error(fmt::format("Missing policy for {}", regime_name));
            }
        }
        
        // Validate constraint ranges
        for (const auto& [regime_name, policy] : config["macro_policies"].items()) {
            if (policy["max_equity_exposure"] < 0.0 || 
                policy["max_equity_exposure"] > 1.0) {
                return Error("max_equity_exposure must be in [0, 1]");
            }
            
            // Validate strategy allocations sum to ~1.0
            double sum = 0.0;
            for (const auto& [strat, alloc] : policy["strategy_group_allocation"].items()) {
                sum += alloc.get<double>();
            }
            if (std::abs(sum - 1.0) > 0.01) {
                return Error("strategy_group_allocation must sum to 1.0");
            }
        }
        
        return Result<void>::ok();
    }
};
```

---

## 10. Failure-State Controls

### Portfolio-Specific Failure Modes

| Failure Mode | Symptoms | Mitigation |
|--------------|----------|------------|
| **Position thrashing** | Frequent position flip-flops | Transition smoothing, min trade thresholds, cost-aware optimizer |
| **Constraint violation** | Positions exceed limits | Hard enforcement layer cannot be bypassed, pre-trade checks |
| **Execution failure** | Orders rejected, fills at bad prices | Order slicing, participation limits, slippage guardrails |
| **Strategy leakage** | Strategies inferring regimes | Strict API enforcement, code reviews, runtime checks |
| **Budget over-allocation** | Risk budgets sum > 100% | Validation at config load time, runtime checks |
| **Emergency trigger failure** | De-risk not executing | Periodic drills, automated monitoring, manual override capability |
| **Stale context** | Strategies using outdated regime info | Context staleness checks, automatic refresh triggers |

### Emergency Response Matrix

| Scenario | Detection | Automatic Response | Manual Escalation |
|----------|-----------|-------------------|-------------------|
| VaR breach | Real-time VaR monitor | Level 2 de-risk | Risk committee notification |
| Drawdown breach | PnL monitoring | Level 2 de-risk | Portfolio manager alert |
| Liquidity crisis | Spread/volume monitoring | Level 1 halt | Trading desk escalation |
| Regime crisis | P(Crisis) > 0.8 | Level 3 de-risk | Full committee notification |

---

## 11. Monitoring Dashboard KPIs

### Real-Time Metrics

| KPI | Description | Alert Threshold |
|-----|-------------|-----------------|
| Gross exposure | Current gross exposure / limit | > 95% |
| Net exposure | Current net exposure / limit | > 95% |
| Daily turnover | Turnover today / daily limit | > 80% |
| Position concentration | Max position / limit | > 90% |
| Strategy budget utilization | Risk used / risk budget | > 100% |

### Transition Metrics

| KPI | Description | Alert Threshold |
|-----|-------------|-----------------|
| Regime switch count | # regime changes this week | > 5/week |
| Transition completion rate | % of planned transitions completed | < 80% |
| Cost vs budget | Actual costs / cost budget | > 120% |
| Slippage average | Mean slippage (bps) | > 15 bps |

### Operational Metrics

| KPI | Description | Alert Threshold |
|-----|-------------|-----------------|
| Engine latency | Time from belief to constraints (ms) | > 50ms |
| Execution latency | Time from target to order (ms) | > 100ms |
| Emergency trigger count | # emergency triggers this month | Any event |
| Constraint violation count | # violations attempted | Any event |
| Context staleness | Age of regime context (seconds) | > 60s |

---

## 12. Implementation Guide

### Phase 1: Policy Layer (Weeks 1-2)
- [ ] Define MacroRegimePolicy and MarketRegimePolicy structs
- [ ] Implement RegimePolicyEngine
- [ ] Create default policy configurations for all regimes
- [ ] Implement hierarchy rules
- [ ] Unit tests for policy blending

### Phase 2: Allocation Engine (Weeks 3-4)
- [ ] Implement RegimeAwareAllocationEngine
- [ ] Transition smoothing logic
- [ ] Hard limit enforcement
- [ ] Risk budget allocation
- [ ] Integration tests with mock portfolio

### Phase 3: Strategy Integration (Weeks 5-6)
- [ ] Update BaseStrategy interface with RegimeContext
- [ ] Implement regime consumption pattern in existing strategies
- [ ] StrategyEligibilityManager
- [ ] Strategy-specific regime tests

### Phase 4: Configuration (Week 7)
- [ ] JSON configuration schema
- [ ] Configuration validation
- [ ] Environment-specific overrides (production vs backtest)
- [ ] Configuration versioning

### Phase 5: Integration Testing (Week 8)
- [ ] End-to-end pipeline tests
- [ ] Regime transition scenarios
- [ ] Crisis scenario stress tests
- [ ] Performance benchmarking

### Phase 6: Production Readiness (Weeks 9-10)
- [ ] Comprehensive logging
- [ ] Monitoring and alerting
- [ ] Documentation
- [ ] Operational runbooks

---

## 10. Testing Strategy

### Unit Tests

**Policy Layer**:
- Policy blending with various regime probability distributions
- Hierarchy rule enforcement
- Probability-weighted constraint calculation
- Edge cases (extreme probabilities)

**Allocation Engine**:
- Transition smoothing effectiveness
- Hard limit enforcement (never violated)
- Risk budget allocation math
- Empty portfolio initialization

**Strategy Integration**:
- Regime context application
- Eligibility checking
- Signal scaling
- Strategy on/off switching

### Integration Tests

**Regime Transition Scenarios**:
```cpp
TEST(RegimeAwarePortfolio, SmoothTransitionFromExpansionToRecession) {
    // Start in EXPANSION
    auto belief_expansion = create_belief(MacroRegime::EXPANSION, 1.0);
    auto constraints1 = policy_engine.compute_constraints(belief_expansion);
    auto allocation1 = alloc_engine.compute_allocation(constraints1, portfolio, signals);
    
    // Gradual shift to RECESSION over 20 bars
    for (int i = 1; i <= 20; ++i) {
        double recession_prob = i / 20.0;
        auto belief = create_belief(
            {{MacroRegime::EXPANSION, 1.0 - recession_prob},
             {MacroRegime::RECESSION, recession_prob}}
        );
        
        auto constraints = policy_engine.compute_constraints(belief);
        auto allocation = alloc_engine.compute_allocation(constraints, portfolio, signals);
        
        // Verify smooth transition (no jumps > transition_speed)
        verify_smooth_transition(allocation, previous_allocation);
        
        previous_allocation = allocation;
    }
}
```

**Crisis Scenarios**:
```cpp
TEST(RegimeAwarePortfolio, ImmediateDeRiskingInCrisis) {
    // Normal regime
    auto belief_normal = create_belief(MacroRegime::EXPANSION, 1.0);
    auto allocation_normal = compute_full_allocation(belief_normal);
    EXPECT_GT(allocation_normal.target_gross_exposure, 1.5);
    
    // Sudden crisis
    auto belief_crisis = create_belief(MacroRegime::CRISIS, 1.0);
    auto allocation_crisis = compute_full_allocation(belief_crisis);
    
    // Should allow fast de-risking (override transition smoothing)
    EXPECT_LT(allocation_crisis.target_gross_exposure, 0.5);
    EXPECT_GT(allocation_crisis.min_liquidity_buffer, 0.3);
}
```

### Backtesting Validation

- Historical regime detection accuracy
- Portfolio performance in each regime
- Regime transition smoothness
- Drawdown control in crisis periods
- Comparison vs non-regime-aware baseline

---

## 11. Success Criteria

### Phase 1 (Policy Layer)
- [ ] All regime types have default policies defined
- [ ] Policy blending produces valid constraints (all values in range)
- [ ] Hierarchy rules prevent logical contradictions
- [ ] Configuration validation catches invalid policies

### Phase 2 (Allocation Engine)
- [ ] Transition smoothing limits position changes to config.transition_speed
- [ ] Hard limits never violated in any test scenario
- [ ] Risk budget allocation sums to 100%
- [ ] Engine handles empty portfolios correctly

### Phase 3 (Strategy Integration)
- [ ] Strategies never infer regimes directly
- [ ] All strategies respect is_enabled flag
- [ ] Signal scaling matches regime context
- [ ] Eligibility checks comprehensive

### Phase 4-6 (Integration & Production)
- [ ] End-to-end pipeline completes in <100ms
- [ ] No regime thrashing (min duration respected)
- [ ] All decisions auditable and logged
- [ ] Crisis scenarios trigger appropriate de-risking
- [ ] Backtest performance meets targets
- [ ] Documentation complete

---

## Appendix: Design Patterns

### Pattern 1: Ask, Don't Infer

❌ **Bad**: Strategy infers regime
```cpp
if (current_volatility > 0.25) {
    // High vol regime → reduce size
    signal.size *= 0.5;
}
```

✅ **Good**: Strategy asks for sizing multiplier
```cpp
signal.size *= regime_ctx.sizing_multiplier;
```

### Pattern 2: Smooth, Don't Jump

❌ **Bad**: Immediate full transition
```cpp
target_position = new_regime_position;
```

✅ **Good**: Gradual transition
```cpp
target_position = current_position + 
    transition_speed * (new_regime_position - current_position);
```

### Pattern 3: Hierarchical Constraints

❌ **Bad**: Market overrides macro
```cpp
if (market_regime == LOW_VOL_TREND) {
    max_exposure = 1.0;  // Ignores macro recession
}
```

✅ **Good**: Macro caps, market adjusts
```cpp
max_exposure = std::min(
    macro_policy.max_exposure,
    market_policy.sizing_mult * base_exposure
);
```

### Pattern 4: Probability-Weighted, Not Binary

❌ **Bad**: Hard regime assignment
```cpp
if (P(RECESSION) > 0.5) {
    strategy_enabled = false;
}
```

✅ **Good**: Probability-weighted sizing
```cpp
sizing_mult = P(EXPANSION) * 1.2 + P(RECESSION) * 0.3;
```

---

This completes the regime-aware portfolio engine architecture. Combined with `regime_detection_architecture.md`, this provides a complete specification for building a production-ready regime-based trading system.
