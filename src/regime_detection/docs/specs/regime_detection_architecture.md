# Regime Detection Architecture

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Design Philosophy](#2-design-philosophy)
3. [System Overview](#3-system-overview)
4. [Statistical Model Library (Layer 1)](#4-statistical-model-library-layer-1)
5. [Regime Inference Engines (Layer 2)](#5-regime-inference-engines-layer-2)
   - 5.1 [Market Regime Engine](#51-market-regime-engine)
   - 5.2 [Macro Regime Engine](#52-macro-regime-engine)
   - 5.3 [Data Specification](#53-data-specification)
6. [Shared Regime Ontology (Layer 3)](#6-shared-regime-ontology-layer-3)
   - 6.1 [Market Ontology](#61-market-ontology)
   - 6.2 [Macro Ontology](#62-macro-ontology)
   - 6.3 [State vs Overlay Rules](#63-state-vs-overlay-rules)
7. [Model-to-Ontology Mapping (Layer 4)](#7-model-to-ontology-mapping-layer-4)
8. [Regime Aggregation Layer (Layer 5)](#8-regime-aggregation-layer-layer-5)
9. [Unified Regime Belief Object (Layer 6)](#9-unified-regime-belief-object-layer-6)
10. [Module Structure](#10-module-structure)
11. [Failure-State Controls](#11-failure-state-controls)
12. [Validation and Acceptance Framework](#12-validation-and-acceptance-framework)
13. [Monitoring Dashboard KPIs](#13-monitoring-dashboard-kpis)
14. [Implementation Roadmap](#14-implementation-roadmap)
15. [Non-Negotiable Guardrails](#15-non-negotiable-guardrails)
16. [Dependencies & Prerequisites](#16-dependencies--prerequisites)
17. [Testing Strategy](#17-testing-strategy)
18. [Executive Checklist](#18-executive-checklist)

---

## 1. Executive Summary

### Purpose

The Regime Detection Architecture provides a robust, multi-model framework for inferring latent market and macroeconomic regimes. This system:

- **Separates beliefs from actions** - Models infer regimes, they don't dictate trades
- **Distinguishes market vs macro regimes** - Fast-moving behavior vs slow-moving structure
- **Aggregates heterogeneous models** - Combines statistical and ML approaches coherently
- **Produces stable regime beliefs** - Prevents thrashing through temporal constraints
- **Maintains explainability** - Every regime belief is auditable and traceable

### Key Principle

> **No single model can reliably identify regimes across time, assets, and shocks.**  
> Therefore, we use an ensemble of specialized models with explicit aggregation rules.

### Architecture Layers

| Layer | Name | Purpose |
|-------|------|---------|
| Layer 1 | Statistical Model Library | Raw inference tools (no regime awareness) |
| Layer 2 | Regime Inference Engines | Model-specific regime beliefs |
| Layer 3 | Shared Regime Ontology | Common regime vocabulary |
| Layer 4 | Model-to-Ontology Mapping | Translate model states to ontology |
| Layer 5 | Regime Aggregation | Combine beliefs hierarchically |
| Layer 6 | Unified Regime Belief Object | Single source of truth |

**Scope**: This document covers Layers 1-6 (regime detection and aggregation). Integration with portfolio management and strategies is covered in `regime_aware_portfolio_engine.md`.

---

## 2. Design Philosophy

### Core Principles

1. **Models infer beliefs, not actions**
   - No model directly triggers trades
   - Regime probabilities are intermediate outputs
   - Decisions consume aggregated beliefs only

2. **Regimes constrain decisions, they do not dictate trades**
   - Regimes define what is allowed/prudent
   - Strategies still drive execution
   - Constraints are probability-weighted

3. **Macro and market regimes are distinct but interacting**
   - Market regime ≠ macro regime
   - Market can flip without macro changing
   - Macro regimes cap risk (strategic), market regimes adjust timing (tactical)

4. **Stability, explainability, and hierarchy matter more than raw accuracy**
   - Prevents regime thrashing
   - Every belief is traceable
   - Hard-coded hierarchy rules prevent contradictions

5. **Probabilistic over deterministic**
   - No hard single-state dependence
   - Always work with probability distributions
   - Uncertainty is part of the output

6. **Structural + ML hybrid**
   - HMM/DFM core for interpretability
   - ML as confirmer with hard caps
   - Never let ML dominate statistical models

7. **Smooth transitions**
   - Avoid thrash/cost blow-ups
   - Persistence and hysteresis mechanisms
   - Staged rebalancing by default

8. **Versioned, auditable, reproducible outputs**
   - Every regime belief is traceable
   - Model versions, ontology versions, mapping versions tracked
   - Full lineage logging for any decision timestamp

9. **Fail-safe and degradable under outages**
   - Explicit fallback for data/model outages
   - Graceful degradation, not crashes
   - Pre-defined missing-data pipelines

### Anti-Patterns (What NOT to Do)

❌ **Single model regime detection** - Too fragile, overfits to training period  
❌ **Majority voting** - Treats all models equally, ignores confidence  
❌ **Flat averaging** - No hierarchy, no domain knowledge  
❌ **Direct regime-to-trade mapping** - Bypasses risk management  
❌ **Silent model changes** - Breaks reproducibility  

---

## 3. System Overview

### Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                  Unified Regime Belief Object                │ Layer 6
│         { macro_probs, market_probs, confidence }            │
└────────────────────────▲────────────────────────────────────┘
                         │
┌────────────────────────┴────────────────────────────────────┐
│              Regime Aggregation Layer                        │ Layer 5
│  • Intra-layer aggregation (weighted combination)            │
│  • Temporal persistence (transition penalties)               │
│  • Conflict resolution (explicit hierarchy)                  │
└────────────────────────▲────────────────────────────────────┘
                         │
                ┌────────┴────────┐
                │                 │
┌───────────────┴──────┐  ┌──────┴──────────────────────┐
│ Market Regime Probs  │  │  Macro Regime Probs         │ Layer 4
│ P(Low-vol trend)     │  │  P(Expansion)               │ (Mapping)
│ P(High-vol trend)    │  │  P(Reflation)               │
│ P(Mean-reverting)    │  │  P(Stagflation)             │
│ P(Vol expansion)     │  │  P(Recession)               │
│ P(Crash)             │  │  P(Crisis)                  │
└───────────────▲──────┘  └──────▲──────────────────────┘
                │                 │
┌───────────────┴──────┐  ┌──────┴──────────────────────┐
│ Market Regime Engine │  │  Macro Regime Engine        │ Layer 2
│ • HMM                │  │  • DFM                      │ (Inference)
│ • MSAR               │  │  • MS-DFM                   │
│ • GARCH/EGARCH       │  │  • Growth-Inflation Quad    │
│ • K-Means / GMM      │  │  • BSTS                     │
│ • RF / GBM           │  │  • XGBoost                  │
└───────────────▲──────┘  └──────▲──────────────────────┘
                │                 │
                └─────────┬───────┘
                          │
┌─────────────────────────┴────────────────────────────────┐
│         Statistical Model Library                         │ Layer 1
│  OLS, Ridge, Lasso, PCA, ADF, KPSS, GARCH, Kalman, etc.  │ (Tools)
└──────────────────────────────────────────────────────────┘
```

### Data Flow

1. **Raw statistical inference** - Statistical models process data
2. **Model-specific regime beliefs** - Each model infers latent states
3. **Ontology mapping** - Model states mapped to common vocabulary
4. **Aggregation** - Beliefs combined with hierarchy and persistence
5. **Unified belief** - Single regime probability distribution
6. **Downstream consumption** - Portfolio/strategy systems use beliefs

---

## 4. Statistical Model Library (Layer 1)

### Purpose

Provide raw statistical inference tools with **zero concept of regimes**.

### What Lives Here

These are the models from the statistics module (assumes `statistics_module_foundation.md` is complete):

**Data Transformers**:
- Normalizer (Z-score, Min-Max, Robust)
- PCA (dimensionality reduction)

**Stationarity Tests**:
- Augmented Dickey-Fuller (ADF)
- KPSS Test
- Phillips-Perron Test

**Regression Models**:
- OLS Regression
- Ridge Regression (L2)
- Lasso Regression (L1)

**Volatility Models**:
- GARCH(1,1)
- EGARCH (asymmetric volatility)
- GJR-GARCH

**State Estimation**:
- Kalman Filter
- Extended Kalman Filter
- Hidden Markov Models (HMM)

**Cointegration**:
- Johansen Test
- Engle-Granger Test

**Other**:
- Hurst Exponent
- Autocorrelation tools

### Rules (Critical)

❌ **No allocations**  
❌ **No regime labels**  
❌ **No decisions**  
❌ **No hierarchy**  

These models:
- Estimate parameters
- Extract latent factors
- Test stationarity
- Estimate volatility
- Detect state transitions

**They are building blocks, nothing more.**

### Interface Contract

All models return `Result<T>` for error handling and provide:
- Parameter estimates
- Confidence metrics
- Residuals/diagnostics
- No interpretation of what to do

### Location

```
include/trade_ngin/statistics/
src/statistics/
tests/statistics/
```

See `statistics_module_foundation.md` and `statistics_module_enhancements.md` for complete specifications.

---

## 5. Regime Inference Engines (Layer 2)

### Purpose

Infer **model-specific regime beliefs**. Each model produces its own latent state interpretation, but still not directly tradable.

### Key Principle

> **Market regime ≠ macro regime**  
> Market regimes are fast-moving and behavior-driven.  
> Macro regimes are slow-moving, structural, and policy-driven.

---

### 5.1 Market Regime Engine

**Characteristics**: Fast-moving, behavior-driven, asset-specific

#### 5.1.1 Core Models (State Inference)

##### Hidden Markov Models (HMM)

**Purpose**: Latent market states based on observed returns/volatility

**Interface**:
```cpp
class MarketHMM {
public:
    struct Config {
        int n_states = 3;  // e.g., low-vol, high-vol, crash
        int max_iterations = 100;
        double tolerance = 1e-6;
        bool use_log_space = true;  // Numerical stability
    };
    
    struct ModelState {
        int most_likely_state;
        Eigen::VectorXd state_probabilities;  // P(state_t | observations)
        Eigen::MatrixXd transition_matrix;
        std::vector<double> state_means;
        std::vector<double> state_volatilities;
        double confidence;
    };
    
    explicit MarketHMM(const Config& config);
    
    Result<ModelState> infer(const std::vector<double>& returns);
    Result<ModelState> update(double new_return);  // Online inference
    
private:
    // Uses HMM from statistics module
};
```

**Use Cases**:
- Volatility regime detection (low/high/extreme)
- Trend vs mean-reversion detection
- Crash state identification

##### Markov-Switching Autoregressive Models (MSAR)

**Purpose**: Dynamics-aware regime detection (AR parameters change by regime)

**Interface**:
```cpp
class MarketMSAR {
public:
    struct Config {
        int n_states = 2;  // e.g., momentum vs mean-reversion
        int ar_order = 1;
        double tolerance = 1e-6;
        int max_iterations = 100;
    };
    
    struct ModelState {
        int most_likely_state;
        Eigen::VectorXd state_probabilities;
        std::vector<double> regime_means;
        std::vector<double> regime_volatilities;
        std::vector<Eigen::VectorXd> ar_coefficients;  // Per regime
        double confidence;
    };
    
    explicit MarketMSAR(const Config& config);
    
    Result<ModelState> infer(const std::vector<double>& returns);
    Result<ModelState> update(double new_return);
    
private:
    // Uses Markov Switching Model from statistics module
};
```

**Use Cases**:
- Momentum vs mean-reversion regimes
- Persistent vs transient moves
- Autocorrelation structure changes

#### 5.1.2 Volatility & Structure Models

##### GARCH / EGARCH

**Purpose**: Volatility clustering and asymmetry (regime **features**, not regimes themselves)

**Interface**:
```cpp
class VolatilityFeatureExtractor {
public:
    struct Config {
        bool use_egarch = true;  // Capture asymmetry
        int lookback = 252;
    };
    
    struct Features {
        double current_volatility;
        double volatility_percentile;  // vs historical
        double gamma;  // EGARCH asymmetry (if use_egarch)
        bool is_vol_clustering;
        bool is_vol_spike;
    };
    
    explicit VolatilityFeatureExtractor(const Config& config);
    
    Result<Features> extract(const std::vector<double>& returns);
    
private:
    // Uses GARCH/EGARCH from statistics module
};
```

**Use Cases**:
- Volatility regime features
- Leverage effect detection
- Input to other regime models

#### 5.1.3 Unsupervised Structure Discovery

##### K-Means / Gaussian Mixture Models

**Purpose**: Exploratory regime discovery, initialization, validation

**Interface**:
```cpp
class UnsupervisedRegimeDetector {
public:
    struct Config {
        int n_clusters = 3;
        std::vector<std::string> features = {"return", "volatility", "volume"};
        bool use_gmm = true;  // vs k-means
    };
    
    struct ModelState {
        int most_likely_cluster;
        Eigen::VectorXd cluster_probabilities;
        std::vector<Eigen::VectorXd> cluster_centers;
        double silhouette_score;  // Quality metric
    };
    
    explicit UnsupervisedRegimeDetector(const Config& config);
    
    Result<ModelState> infer(const Eigen::MatrixXd& features);
    
private:
    // May use external ML library or custom implementation
};
```

**Use Cases**:
- Exploratory regime discovery
- Initialize HMM/MSAR states
- Validate supervised models

#### 5.1.4 Nonlinear Confirmation

##### Random Forest / Gradient Boosting

**Purpose**: Secondary classifiers for stress/crash confirmation

**Interface**:
```cpp
class MarketRegimeClassifier {
public:
    struct Config {
        std::string model_type = "gradient_boosting";  // or "random_forest"
        std::vector<std::string> features = {
            "realized_vol", "vol_of_vol", "skew", "tail_risk", 
            "drawdown", "correlation_breakdown"
        };
        int n_estimators = 100;
    };
    
    struct ModelState {
        std::string predicted_regime;  // "normal", "stress", "crash"
        std::map<std::string, double> regime_probabilities;
        std::vector<double> feature_importances;
        double confidence;
    };
    
    explicit MarketRegimeClassifier(const Config& config);
    
    Result<void> train(const Eigen::MatrixXd& features,
                       const std::vector<std::string>& labels);
    
    Result<ModelState> infer(const Eigen::VectorXd& features);
    
private:
    // Uses external ML library (e.g., XGBoost)
};
```

**Use Cases**:
- Stress regime confirmation
- Crash probability
- Tail risk adjustment
- **Only adjusts confidence, does not dominate**

---

### 5.2 Macro Regime Engine

**Characteristics**: Slow-moving, structural, policy-driven, asset-class level

#### 5.2.1 Structural Backbone

##### Dynamic Factor Models (DFM)

**Purpose**: Extract growth, inflation, liquidity factors - core macro signal

**Interface**:
```cpp
class DynamicFactorModel {
public:
    struct Config {
        int n_factors = 3;  // growth, inflation, liquidity
        int n_lags = 4;
        std::vector<std::string> indicators = {
            "gdp_growth", "cpi", "yield_curve", "credit_spreads",
            "pmi", "unemployment", "money_supply"
        };
    };
    
    struct ModelState {
        Eigen::VectorXd factors;  // Current factor values
        Eigen::VectorXd factor_growth;  // Factor momentum
        std::map<std::string, double> factor_contributions;
        double r_squared;
    };
    
    explicit DynamicFactorModel(const Config& config);
    
    Result<ModelState> infer(const Eigen::MatrixXd& indicator_data);
    Result<ModelState> update(const Eigen::VectorXd& new_indicators);
    
private:
    // Uses Kalman Filter + PCA from statistics module
};
```

**Use Cases**:
- Macro factor extraction
- Growth/inflation/liquidity trends
- Continuous macro signal

##### Markov-Switching Dynamic Factor Models (MS-DFM)

**Purpose**: Explicit macro regimes (expansion, recession, stagflation)

**Interface**:
```cpp
class MacroMSDFM {
public:
    struct Config {
        int n_regimes = 4;  // expansion, reflation, stagflation, recession
        int n_factors = 3;
        int n_lags = 4;
        std::vector<std::string> indicators;
    };
    
    struct ModelState {
        int most_likely_regime;
        Eigen::VectorXd regime_probabilities;
        Eigen::VectorXd factors;
        std::vector<std::string> regime_names;
        std::vector<Eigen::VectorXd> regime_factor_means;
        double confidence;
    };
    
    explicit MacroMSDFM(const Config& config);
    
    Result<ModelState> infer(const Eigen::MatrixXd& indicator_data);
    Result<ModelState> update(const Eigen::VectorXd& new_indicators);
    
private:
    // Combines Markov Switching + DFM
};
```

**Use Cases**:
- Explicit macro regime identification
- Structural regime changes
- Long-horizon strategic positioning

#### 5.2.2 Rule-Based Anchor

##### Growth–Inflation Quadrant Model

**Purpose**: Interpretability, asset mapping, sanity anchor

**Interface**:
```cpp
class GrowthInflationQuadrant {
public:
    struct Config {
        double growth_threshold = 2.5;  // % GDP growth
        double inflation_threshold = 2.5;  // % CPI
        int lookback_months = 3;
        bool use_expectations = true;  // vs realized
    };
    
    enum class Quadrant {
        GOLDILOCKS,     // High growth, low inflation
        REFLATION,      // High growth, high inflation
        STAGFLATION,    // Low growth, high inflation
        DEFLATION       // Low growth, low inflation
    };
    
    struct ModelState {
        Quadrant current_quadrant;
        double growth_value;
        double inflation_value;
        std::map<Quadrant, double> quadrant_probabilities;  // Fuzzy
        double confidence;
    };
    
    explicit GrowthInflationQuadrant(const Config& config);
    
    Result<ModelState> infer(double growth, double inflation);
    
private:
    // Simple rule-based logic with fuzzy boundaries
};
```

**Use Cases**:
- Simple interpretable baseline
- Asset class tilts
- Cross-check complex models

#### 5.2.3 Policy & Structure

##### Bayesian Structural Time Series (BSTS)

**Purpose**: Policy shifts, QE/QT regimes, structural breaks

**Interface**:
```cpp
class BayesianStructuralTimeSeries {
public:
    struct Config {
        std::vector<std::string> components = {
            "trend", "seasonal", "policy_intervention"
        };
        std::vector<std::string> policy_events = {
            "qe_start", "qe_end", "rate_hike_cycle", "crisis_intervention"
        };
    };
    
    struct ModelState {
        double trend;
        double seasonal_component;
        std::map<std::string, double> policy_impacts;
        std::vector<double> structural_break_probs;  // Per timestep
        bool is_structural_break;
        double confidence;
    };
    
    explicit BayesianStructuralTimeSeries(const Config& config);
    
    Result<ModelState> infer(const std::vector<double>& data,
                             const std::vector<bool>& policy_events);
    
private:
    // Uses Kalman Filter + Bayesian inference
};
```

**Use Cases**:
- Policy regime detection (QE/QT)
- Structural break identification
- Intervention impact estimation

#### 5.2.4 Nonlinear Confirmation

##### XGBoost (Gradient Boosting)

**Purpose**: Regime validation, crisis probability, nonlinear thresholds

**Interface**:
```cpp
class MacroRegimeClassifier {
public:
    struct Config {
        std::vector<std::string> features = {
            "yield_curve", "credit_spreads", "vol_index", 
            "currency_stress", "commodity_momentum"
        };
        int n_estimators = 100;
        bool use_crisis_oversampling = true;
    };
    
    struct ModelState {
        std::string predicted_regime;
        std::map<std::string, double> regime_probabilities;
        double crisis_probability;
        std::vector<double> feature_importances;
        double confidence;
    };
    
    explicit MacroRegimeClassifier(const Config& config);
    
    Result<void> train(const Eigen::MatrixXd& features,
                       const std::vector<std::string>& labels);
    
    Result<ModelState> infer(const Eigen::VectorXd& features);
    
private:
    // Uses XGBoost library
};
```

**Use Cases**:
- Nonlinear threshold detection
- Crisis probability adjustment
- **Validation only, does not dominate**

---

### 5.3 Data Specification

This section defines the data requirements and alignment rules for all regime inference models.

#### 5.3.1 Market Inputs

| Input Category | Specific Data | Update Frequency |
|----------------|---------------|------------------|
| Returns | Multi-horizon (1d, 5d, 21d, 63d) | Daily |
| Volatility | Realized vol, vol-of-vol | Daily |
| Drawdowns | Drawdown depth, drawdown speed | Daily |
| Correlations | Cross-asset rolling correlations | Daily |
| Dispersion | Sector/stock dispersion proxies | Daily |
| Liquidity | Spread, volume, market impact proxies | Intraday/Daily |

#### 5.3.2 Macro Inputs

| Input Category | Specific Data | Update Frequency |
|----------------|---------------|------------------|
| Inflation | CPI/PCE (headline/core), inflation surprises | Monthly |
| Growth | GDP nowcasts, industrial production | Monthly/Quarterly |
| Labor | Unemployment rate, claims, payrolls | Weekly/Monthly |
| Activity | PMI, ISM, consumer sentiment | Monthly |
| Yield Curve | Shape (2y-10y), level, slope | Daily |
| Credit | IG/HY spreads, credit conditions indices | Daily |
| Policy Rates | Real rates, fed funds, policy stance | Daily |
| Liquidity | Financial conditions indices | Daily/Weekly |

#### 5.3.3 Data Alignment Rules

> [!IMPORTANT]
> Every feature must carry **availability timestamp** (as-of, not event date).

```cpp
struct DataAlignment {
    // As-of timestamp discipline
    std::chrono::system_clock::time_point as_of_timestamp;  // When data was available
    std::chrono::system_clock::time_point event_timestamp;  // When event occurred
    
    // Publication lag handling
    int publication_lag_days;  // e.g., CPI has ~2 week lag
    
    // Revision handling
    bool is_preliminary;
    int expected_revisions;
};
```

**Critical Requirements**:
1. **As-of gating**: Only use data available at decision time (no look-ahead)
2. **Publication lag handling**: Account for delays in official data releases
3. **Missing-data fallback pipelines**: Pre-defined fallbacks for data outages
4. **Revision handling**: Use preliminary vs final data appropriately

---

## 6. Shared Regime Ontology (Layer 3)

### Purpose

Define the **single common vocabulary** that all models map into. This is the contract between inference and decision-making.

### Critical Rule

> **Ontology does not change silently**  
> All models must map into it  
> All decisions consume only this space  

> [!IMPORTANT]
> Use a **two-level ontology + overlays** rather than a giant flat state space. This provides interpretability and separates core states from conditional modifiers.

---

### 6.1 Market Ontology

#### Market L1 Primary States (Control States)

```cpp
enum class MarketRegimeL1 {
    TREND_LOWVOL,       // Trending with low volatility
    TREND_HIGHVOL,      // Trending with high volatility
    MEANREV_CHOPPY,     // Range-bound, oscillating, choppy
    STRESS_PRICE,       // Price stress (drawdowns, gaps)
    STRESS_LIQUIDITY    // Liquidity stress (spread widening, volume collapse)
};
```

| Market L1 State | Trend | Volatility | Correlation | Typical Duration |
|-----------------|-------|------------|-------------|------------------|
| TREND_LOWVOL | Strong | Low (<15%) | Normal | Weeks to months |
| TREND_HIGHVOL | Moderate | High (>25%) | Elevated | Days to weeks |
| MEANREV_CHOPPY | Weak/None | Moderate | Normal | Weeks to months |
| STRESS_PRICE | Negative | Spiking | Increasing | Days to weeks |
| STRESS_LIQUIDITY | Variable | Extreme (>40%) | Breakdown | Days |

#### Market Overlays (Modulators)

Overlays modify the interpretation of L1 states without changing the primary classification:

```cpp
enum class MarketOverlay {
    EVENT_SHOCK_TRANSIENT,  // Short-term event shock (earnings, news)
    CORRELATION_SPIKE,      // Cross-asset correlation breakdown
    VOL_OF_VOL_HIGH,        // Volatility of volatility elevated
    DISPERSION_HIGH,        // High dispersion across sectors/stocks
    GAP_RISK_HIGH           // Elevated overnight/weekend gap risk
};
```

#### Market L2 Sub-States (Optional Detail)

For more granular classification when needed:

| L1 State | L2 Sub-States |
|----------|---------------|
| TREND_LOWVOL | TREND_PERSISTENT, CARRY_FRIENDLY |
| TREND_HIGHVOL | BREAKOUT_VOLATILE, TREND_FRAGILE |
| MEANREV_CHOPPY | TIGHT_RANGE, WIDE_RANGE |
| STRESS_PRICE | DRAWDOWN_FAST, DRAWDOWN_GRINDING |
| STRESS_LIQUIDITY | FUNDING_STRESS, LIQUIDITY_GAP |

---

### 6.2 Macro Ontology

#### Macro L1 Primary States

```cpp
enum class MacroRegimeL1 {
    EXPANSION_DISINFLATION,   // High growth, low/falling inflation (Goldilocks)
    EXPANSION_INFLATIONARY,   // High growth, rising inflation (Reflation)
    SLOWDOWN_DISINFLATION,    // Slowing growth, falling inflation
    SLOWDOWN_INFLATIONARY,    // Slowing growth, persistent inflation
    RECESSION_DEFLATIONARY,   // Negative growth, falling prices
    RECESSION_INFLATIONARY    // Negative growth, high inflation (Stagflation)
};
```

| Macro L1 State | Growth | Inflation | Policy Stance | Typical Duration |
|----------------|--------|-----------|---------------|------------------|
| EXPANSION_DISINFLATION | High (>3%) | Low (1-2%) | Neutral/Tightening | 3-7 years |
| EXPANSION_INFLATIONARY | Rising | Rising (>3%) | Accommodative→Tightening | 6-18 months |
| SLOWDOWN_DISINFLATION | Slowing | Falling | Neutral/Easing | 1-3 years |
| SLOWDOWN_INFLATIONARY | Low (<2%) | High (>4%) | Constrained | 6-24 months |
| RECESSION_DEFLATIONARY | Negative | Falling | Emergency easing | 6-18 months |
| RECESSION_INFLATIONARY | Negative | High | Impossible choice | 6-24 months |

#### Macro Overlays (Modulators)

```cpp
enum class MacroOverlay {
    POLICY_SUPPORTIVE,              // Central bank in easing/supportive mode
    POLICY_RESTRICTIVE,             // Central bank tightening
    CREDIT_EASING,                  // Credit conditions loosening
    CREDIT_TIGHTENING,              // Credit conditions tightening
    INFLATION_STICKY,               // Inflation persistently above target
    GROWTH_MOMENTUM_DETERIORATING,  // Growth momentum declining
    REAL_RATE_PRESSURE_HIGH         // Real rates creating stress
};
```

#### Macro L2 Sub-States (Optional Detail)

| L1 State | L2 Sub-States |
|----------|---------------|
| EXPANSION_DISINFLATION | PRODUCTIVITY_DRIVEN, CREDIT_FUELED |
| EXPANSION_INFLATIONARY | DEMAND_LED, SUPPLY_CONSTRAINED |
| SLOWDOWN_DISINFLATION | SOFT_LANDING, EARNINGS_RECESSION |
| SLOWDOWN_INFLATIONARY | COST_PUSH, EMBEDDED_EXPECTATIONS |
| RECESSION_DEFLATIONARY | DEMAND_SHOCK_DOMINANT, DELEVERAGING |
| RECESSION_INFLATIONARY | SUPPLY_SHOCK_DOMINANT, BALANCE_SHEET_STRESS |

---

### 6.3 State vs Overlay Rules

> [!IMPORTANT]
> **Promote to L1 primary state** only if it changes:
> - Strategic leverage caps
> - Base risk budget
> - Volatility target
>
> **Keep as overlay** if it only changes:
> - Strategy multipliers
> - Signal thresholds
> - Hedging intensity

#### Promotion Decision Tree

```
Is this condition changing our fundamental risk capacity?
├─ YES → L1 Primary State
│   Examples: Recession, Liquidity Crisis, Stagflation
└─ NO → Is it modifying how we implement within current capacity?
    ├─ YES → Overlay
    │   Examples: Policy shift, Correlation spike, Event shock
    └─ NO → Ignore or log for research
```

---

### 6.4 Ontology Versioning

**Critical**: Ontology changes are **breaking changes**.

```cpp
namespace RegimeOntology {
    constexpr int MACRO_ONTOLOGY_VERSION = 2;  // Updated for two-level structure
    constexpr int MARKET_ONTOLOGY_VERSION = 2;
    
    // Compatibility tracking
    constexpr bool BACKWARD_COMPATIBLE_WITH_V1 = true;
}
```

All models and downstream systems must declare which ontology version they use.

---

## 7. Model-to-Ontology Mapping (Layer 4)

### Purpose

Translate model-specific states into shared ontology probabilities.

### Interface Contract

Every regime model implements:

```cpp
class RegimeModelBase {
public:
    // Model-specific inference (returns model's internal state)
    virtual Result<ModelState> infer(const DataInput& data) = 0;
    
    // Map internal state to ontology probabilities
    virtual RegimeDistribution map_to_ontology(const ModelState& state) = 0;
    
    // Metadata
    virtual std::string get_model_name() const = 0;
    virtual int get_ontology_version() const = 0;
};
```

### Regime Distribution Object

```cpp
struct RegimeDistribution {
    // For market regime models
    std::map<MarketRegime, double> market_probs;
    
    // For macro regime models
    std::map<MacroRegime, double> macro_probs;
    
    double confidence;  // Model's confidence in this mapping
    std::string model_name;
    
    // Validation
    bool is_valid() const {
        double sum = 0.0;
        for (const auto& [regime, prob] : market_probs) sum += prob;
        for (const auto& [regime, prob] : macro_probs) sum += prob;
        return std::abs(sum - 1.0) < 1e-6;
    }
};
```

### Mapping Examples

#### Example 1: HMM → Market Regime

```cpp
RegimeDistribution MarketHMM::map_to_ontology(const ModelState& state) {
    RegimeDistribution dist;
    dist.model_name = "MarketHMM";
    dist.confidence = state.confidence;
    
    // HMM state 0: Low volatility, positive drift
    if (state.most_likely_state == 0 && state.state_volatilities[0] < 0.15) {
        dist.market_probs[MarketRegime::LOW_VOL_TREND] = 0.8;
        dist.market_probs[MarketRegime::MEAN_REVERTING] = 0.2;
    }
    // HMM state 1: High volatility, positive drift
    else if (state.most_likely_state == 1 && state.state_volatilities[1] > 0.25) {
        dist.market_probs[MarketRegime::HIGH_VOL_TREND] = 0.7;
        dist.market_probs[MarketRegime::VOLATILITY_EXPANSION] = 0.3;
    }
    // HMM state 2: Extreme volatility, negative drift
    else if (state.most_likely_state == 2 && state.state_means[2] < -0.02) {
        dist.market_probs[MarketRegime::CRASH_STRESS] = 0.9;
        dist.market_probs[MarketRegime::VOLATILITY_EXPANSION] = 0.1;
    }
    
    return dist;
}
```

#### Example 2: MS-DFM → Macro Regime

```cpp
RegimeDistribution MacroMSDFM::map_to_ontology(const ModelState& state) {
    RegimeDistribution dist;
    dist.model_name = "MacroMSDFM";
    dist.confidence = state.confidence;
    
    double growth_factor = state.factors[0];  // Growth factor
    double inflation_factor = state.factors[1];  // Inflation factor
    
    // MS-DFM state 0: High growth, low inflation
    if (state.most_likely_regime == 0) {
        if (growth_factor > 0.5 && inflation_factor < 0.2) {
            dist.macro_probs[MacroRegime::EXPANSION] = 0.9;
            dist.macro_probs[MacroRegime::REFLATION] = 0.1;
        }
    }
    // MS-DFM state 1: Low growth, high inflation
    else if (state.most_likely_regime == 1) {
        if (growth_factor < -0.3 && inflation_factor > 0.5) {
            dist.macro_probs[MacroRegime::STAGFLATION] = 0.8;
            dist.macro_probs[MacroRegime::RECESSION] = 0.2;
        }
    }
    // ... other states
    
    return dist;
}
```

#### Example 3: BSTS Structural Break → Crisis Flag

```cpp
RegimeDistribution BSTS::map_to_ontology(const ModelState& state) {
    RegimeDistribution dist;
    dist.model_name = "BSTS";
    dist.confidence = state.confidence;
    
    // BSTS detects structural breaks, not regimes
    // But structural breaks increase crisis probability
    if (state.is_structural_break && 
        state.structural_break_probs.back() > 0.7) {
        // Boost crisis probability
        dist.macro_probs[MacroRegime::CRISIS] = 0.6;
        dist.macro_probs[MacroRegime::RECESSION] = 0.3;
        dist.macro_probs[MacroRegime::STAGFLATION] = 0.1;
    } else {
        // Neutral - defer to other models
        dist.macro_probs[MacroRegime::EXPANSION] = 0.25;
        dist.macro_probs[MacroRegime::REFLATION] = 0.25;
        dist.macro_probs[MacroRegime::DISINFLATION] = 0.25;
        dist.macro_probs[MacroRegime::RECESSION] = 0.25;
    }
    
    return dist;
}
```

### Mapping Rules

1. **Explicit, not learned**: Mappings are hard-coded logic, not ML
2. **Documented**: Every mapping decision must be explained
3. **Version-controlled**: Changes require version bump
4. **Testable**: Synthetic data tests validate mappings
5. **Fuzzy allowed**: Probabilistic mappings are preferred over hard assignments

---

### 7.1 Formal Mapping Procedure (Critical Control Point)

> [!CAUTION]
> **Do NOT manual-map ad hoc**. Use governed statistical mapping with constraints.

#### Step 1: Define Ontology Fingerprints

For each ontology state, define a target signature vector over these metrics:

| Metric | Description |
|--------|-------------|
| drift | Mean return direction and magnitude |
| vol | Realized volatility level |
| skew_tails | Return distribution skewness |
| drawdown_velocity | Speed of drawdowns |
| autocorr | Return autocorrelation structure |
| correlation_stress | Cross-asset correlation levels |
| liquidity_stress | Spread/volume indicators |

```cpp
struct OntologyFingerprint {
    std::string state_name;
    double expected_drift;
    double expected_vol_percentile;  // 0-100
    double expected_skew;
    double max_drawdown_velocity;
    double autocorr_sign;  // +1 trending, -1 mean-reverting
    double correlation_stress_threshold;
    double liquidity_stress_threshold;
};
```

#### Step 2: Build Model-State Empirical Fingerprints

For each latent model state, compute the same signature vector from historical assignments:

```cpp
OntologyFingerprint compute_empirical_fingerprint(
    const std::vector<ModelStateAssignment>& historical_assignments,
    const MarketData& data
) {
    // Compute statistics for periods assigned to this state
    OntologyFingerprint fp;
    fp.expected_drift = compute_mean_return(data, assignments);
    fp.expected_vol_percentile = compute_vol_percentile(data, assignments);
    // ... other metrics
    return fp;
}
```

#### Step 3: Similarity-Based Soft Mapping

Compute distances d(s,o) between model state s and ontology state o, then apply softmax:

$$p(o|s) = \frac{\exp(-d(s,o)/\tau)}{\sum_{o'}\exp(-d(s,o')/\tau)}$$

where **τ = mapping temperature** (calibrated parameter).

```cpp
double compute_mapping_probability(
    const OntologyFingerprint& model_fp,
    const OntologyFingerprint& ontology_fp,
    double temperature
) {
    double distance = compute_euclidean_distance(model_fp, ontology_fp);
    return std::exp(-distance / temperature);
}
```

#### Step 4: Out-of-Sample Calibration

Walk-forward calibrate these parameters:
- τ (temperature): Controls mapping sharpness
- Persistence coupling: How much prior state affects current mapping
- Model-specific scaling: Per-model confidence adjustments

```cpp
struct MappingCalibration {
    double temperature = 0.5;
    double persistence_coupling = 0.3;
    std::map<std::string, double> model_scaling;
    
    static MappingCalibration walk_forward_calibrate(
        const std::vector<HistoricalPeriod>& training_data
    );
};
```

#### Step 5: Constraint Enforcement

Apply constraints to ensure stable, interpretable mappings:

| Constraint | Description |
|------------|-------------|
| Monotonicity | Higher stress metrics → higher stress state probability |
| Continuity | Adjacent states should have overlapping probability mass |
| Sparsity | Each model state maps strongly to 1-2 ontology buckets |
| Versioning | Mapping changes require model retrain alignment |

## 8. Regime Aggregation Layer (Layer 5)

### Purpose

Combine heterogeneous model beliefs into unified regime probabilities.

### Key Principle

> **Combine beliefs, not votes**  
> Posterior belief = Prior belief × Evidence  
> Subject to persistence and hierarchy constraints  

### 8.1 Intra-Layer Aggregation

#### Market Regime Aggregation

```cpp
class MarketRegimeAggregator {
public:
    struct Config {
        // Model weights (sum to 1.0)
        double hmm_weight = 0.4;
        double msar_weight = 0.4;
        double ml_weight = 0.2;  // RF/GBM only adjusts tails
        
        // Minimum regime duration (prevents thrashing)
        int min_regime_duration_bars = 20;
        
        // Transition penalty
        double transition_penalty = 0.3;
        
        // Exponential smoothing factor
        double smoothing_alpha = 0.3;
    };
    
    explicit MarketRegimeAggregator(const Config& config);
    
    Result<RegimeDistribution> aggregate(
        const std::vector<RegimeDistribution>& model_beliefs
    );
    
private:
    Config config_;
    RegimeDistribution prior_belief_;
    int bars_in_current_regime_;
    MarketRegime current_regime_;
    
    RegimeDistribution apply_weights(
        const std::vector<RegimeDistribution>& beliefs
    );
    
    RegimeDistribution apply_temporal_persistence(
        const RegimeDistribution& new_belief
    );
    
    RegimeDistribution apply_transition_penalty(
        const RegimeDistribution& new_belief
    );
};
```

**Weighting Logic**:
- **HMM & MSAR dominate** (0.4 each) - Core state inference
- **GARCH informs volatility dimension** (implicit via features)
- **ML adjusts tail risk only** (0.2) - Stress/crash confirmation

#### Macro Regime Aggregation

```cpp
class MacroRegimeAggregator {
public:
    struct Config {
        // Model weights (sum to 1.0)
        double msdfe_weight = 0.5;  // MS-DFM dominates
        double dfm_weight = 0.2;    // Factor continuity
        double quadrant_weight = 0.15;  // Interpretability anchor
        double bsts_weight = 0.1;   // Structural breaks
        double ml_weight = 0.05;    // Confidence adjustment only
        
        // Much longer minimum duration than market
        int min_regime_duration_months = 6;
        
        double transition_penalty = 0.5;  // Stronger than market
        double smoothing_alpha = 0.1;     // More smoothing
    };
    
    explicit MacroRegimeAggregator(const Config& config);
    
    Result<RegimeDistribution> aggregate(
        const std::vector<RegimeDistribution>& model_beliefs
    );
    
private:
    // Similar structure to MarketRegimeAggregator
};
```

**Weighting Logic**:
- **MS-DFM dominates** (0.5) - Explicit regime identification
- **DFM provides factor continuity** (0.2) - Smooth factor evolution
- **Quadrant anchors interpretability** (0.15) - Sanity check
- **BSTS flags structural shifts** (0.1) - Break detection
- **XGBoost adjusts confidence only** (0.05) - Tail risk

#### No Flat Averaging

❌ **Bad**: Simple average
```cpp
// DON'T DO THIS
for (auto& belief : model_beliefs) {
    for (auto& [regime, prob] : belief.market_probs) {
        avg_probs[regime] += prob / model_beliefs.size();
    }
}
```

✅ **Good**: Weighted combination with hierarchy
```cpp
RegimeDistribution result;
for (size_t i = 0; i < model_beliefs.size(); ++i) {
    double weight = weights_[model_names[i]];
    for (auto& [regime, prob] : model_beliefs[i].market_probs) {
        result.market_probs[regime] += weight * prob;
    }
}
```

### 8.2 Aggregation Formulas

#### 8.2.1 Per-Bucket Weighted Blend

For each ontology bucket o at time t:

$$p_{o,t}^{raw} = \sum_{m} w_{m,t} \cdot \tilde{p}_{m,o,t}$$

where:
- $\tilde{p}_{m,o,t}$ = mapped + calibrated model probability from model m
- $w_{m,t}$ = reliability-aware, drift-penalized dynamic weight for model m

#### 8.2.2 Weight Constraints

```cpp
struct WeightConstraints {
    // Weights must sum to 1
    // Σ w_m,t = 1
    
    // ML family hard cap (critical guardrail)
    static constexpr double MAX_ML_FAMILY_WEIGHT = 0.25;
    
    // Per-model drift decay
    static double apply_drift_penalty(double base_weight, double drift_score) {
        return base_weight * std::exp(-drift_penalty_rate * drift_score);
    }
};
```

> [!WARNING]
> **ML model influence is capped at 20-25% max per bucket**. This prevents overfitting and ensures structural models dominate.

#### 8.2.3 Reliability-Aware Weighting

```cpp
double compute_dynamic_weight(
    const std::string& model_name,
    double base_weight,
    double calibration_score,
    double drift_score,
    double recency_score
) {
    double reliability = calibration_score * (1.0 - drift_score) * recency_score;
    return base_weight * reliability;
}
```

### 8.3 Persistence Smoothing

Apply exponential smoothing with adaptive speed:

$$p_{o,t}^{sm} = \lambda_t \cdot p_{o,t}^{raw} + (1 - \lambda_t) \cdot p_{o,t-1}^{sm}$$

where λ_t adapts to market conditions:
- **Lower λ_t in calm periods** → More stability, less responsiveness
- **Higher λ_t during confirmed shocks** → More responsiveness

```cpp
double compute_adaptive_lambda(
    double base_lambda,
    const RegimeBelief& current_belief,
    const MarketConditions& conditions
) {
    double lambda = base_lambda;
    
    // Increase lambda (faster update) during stress
    if (conditions.vol_zscore > 2.0) {
        lambda = std::min(0.8, lambda * 1.5);
    }
    
    // Decrease lambda (more smoothing) during calm
    if (conditions.vol_zscore < 0.5 && conditions.regime_age > 20) {
        lambda = std::max(0.1, lambda * 0.7);
    }
    
    return lambda;
}
```

### 8.4 Hysteresis (Entry/Exit Thresholds)

Use separate thresholds for entering and exiting regime-based policies to prevent thrashing:

| Transition | Entry Threshold | Exit Threshold |
|------------|-----------------|----------------|
| Enter risk-off | P(Stress) > 0.60 | P(Stress) < 0.45 |
| Enter defensive | P(Recession) > 0.55 | P(Recession) < 0.40 |
| Enter aggressive | P(Expansion) > 0.70 | P(Expansion) < 0.50 |

```cpp
struct HysteresisConfig {
    double entry_threshold;
    double exit_threshold;  // Always < entry_threshold
    
    bool should_transition(double current_prob, bool currently_in_state) const {
        if (currently_in_state) {
            return current_prob >= exit_threshold;  // Stay if above exit
        } else {
            return current_prob >= entry_threshold;  // Enter only if above entry
        }
    }
};
```

### 8.5 Temporal Persistence

Prevent regime thrashing by enforcing minimum durations and transition penalties.

```cpp
RegimeDistribution apply_temporal_persistence(
    const RegimeDistribution& new_belief
) {
    // If we haven't been in current regime long enough, penalize transitions
    if (bars_in_current_regime_ < config_.min_regime_duration_bars) {
        RegimeDistribution modified = new_belief;
        
        // Boost current regime probability
        modified.market_probs[current_regime_] += config_.transition_penalty;
        
        // Renormalize
        normalize(modified.market_probs);
        
        return modified;
    }
    
    return new_belief;
}
```

### 8.3 Conflict Resolution (Explicit Hierarchy)

Hard-coded rules prevent logical contradictions:

```cpp
class ConflictResolver {
public:
    struct Rules {
        // Macro regimes cap risk
        // Market regimes adjust timing
        // Market cannot override macro
        // Stress overrides opportunity
    };
    
    static RegimeDistribution resolve(
        const RegimeDistribution& market_belief,
        const RegimeDistribution& macro_belief
    ) {
        RegimeDistribution resolved;
        
        // Rule 1: If macro is CRISIS, market cannot be LOW_VOL_TREND
        if (macro_belief.macro_probs[MacroRegime::CRISIS] > 0.5) {
            resolved.market_probs[MarketRegime::LOW_VOL_TREND] = 0.0;
            resolved.market_probs[MarketRegime::CRASH_STRESS] = 
                std::max(0.5, market_belief.market_probs[MarketRegime::CRASH_STRESS]);
        }
        
        // Rule 2: If macro is EXPANSION, reduce CRASH_STRESS probability
        if (macro_belief.macro_probs[MacroRegime::EXPANSION] > 0.6) {
            resolved.market_probs[MarketRegime::CRASH_STRESS] *= 0.5;
        }
        
        // Rule 3: STAGFLATION limits trend opportunities
        if (macro_belief.macro_probs[MacroRegime::STAGFLATION] > 0.5) {
            resolved.market_probs[MarketRegime::LOW_VOL_TREND] *= 0.7;
        }
        
        // Renormalize
        normalize(resolved.market_probs);
        
        return resolved;
    }
};
```

---

## 9. Unified Regime Belief Object (Layer 6)

### Purpose

Single source of truth for regime beliefs consumed by downstream systems.

### Interface

```cpp
struct RegimeBelief {
    // Macro regime probabilities
    std::map<MacroRegime, double> macro_probs;
    MacroRegime most_likely_macro;
    
    // Market regime probabilities
    std::map<MarketRegime, double> market_probs;
    MarketRegime most_likely_market;
    
    // Confidence metrics
    double macro_confidence;
    double market_confidence;
    double overall_confidence;
    
    // Temporal tracking
    std::chrono::system_clock::time_point timestamp;
    int regime_age_bars;  // How long in current regime
    
    // Provenance (for auditability)
    std::map<std::string, RegimeDistribution> model_contributions;
    std::string aggregation_method;
    
    // Validation
    bool is_valid() const {
        return validate_probabilities(macro_probs) &&
               validate_probabilities(market_probs) &&
               macro_confidence >= 0.0 && macro_confidence <= 1.0 &&
               market_confidence >= 0.0 && market_confidence <= 1.0;
    }
    
    // Serialization (for logging)
    std::string to_json() const;
    static Result<RegimeBelief> from_json(const std::string& json);
};
```

### Logging & Auditability

Every regime belief is logged with full provenance:

```cpp
void log_regime_belief(const RegimeBelief& belief) {
    LOG_INFO("Regime Belief Update",
        "timestamp", belief.timestamp,
        "most_likely_macro", to_string(belief.most_likely_macro),
        "most_likely_market", to_string(belief.most_likely_market),
        "macro_confidence", belief.macro_confidence,
        "market_confidence", belief.market_confidence,
        "regime_age", belief.regime_age_bars,
        "model_contributions", belief.model_contributions
    );
}
```

### Storage

Regime beliefs are persisted to database for:
- Backtesting analysis
- Model performance evaluation
- Regulatory audit trail
- Post-trade analysis

```cpp
class RegimeBeliefStore {
public:
    Result<void> save(const RegimeBelief& belief);
    Result<RegimeBelief> load(const std::chrono::system_clock::time_point& timestamp);
    Result<std::vector<RegimeBelief>> query_range(
        const std::chrono::system_clock::time_point& start,
        const std::chrono::system_clock::time_point& end
    );
};
```

---

## 10. Module Structure

### Directory Layout

```
include/trade_ngin/regime/
├── regime_fwd.hpp                     # Forward declarations
├── regime_common.hpp                  # Common types, ontology enums
│
├── ontology/
│   ├── macro_regime.hpp               # MacroRegime enum, definitions
│   ├── market_regime.hpp              # MarketRegime enum, definitions
│   └── regime_distribution.hpp        # RegimeDistribution struct
│
├── models/                            # Regime inference models
│   ├── market/
│   │   ├── market_hmm.hpp
│   │   ├── market_msar.hpp
│   │   ├── volatility_feature_extractor.hpp
│   │   ├── unsupervised_regime_detector.hpp
│   │   └── market_regime_classifier.hpp
│   │
│   └── macro/
│       ├── dynamic_factor_model.hpp
│       ├── macro_msdfe.hpp
│       ├── growth_inflation_quadrant.hpp
│       ├── bayesian_structural_time_series.hpp
│       └── macro_regime_classifier.hpp
│
├── mapping/
│   ├── regime_model_base.hpp          # Base class with mapping interface
│   └── mapping_utils.hpp              # Mapping helper functions
│
├── aggregation/
│   ├── market_regime_aggregator.hpp
│   ├── macro_regime_aggregator.hpp
│   └── conflict_resolver.hpp
│
├── belief/
│   ├── regime_belief.hpp              # Unified RegimeBelief object
│   └── regime_belief_store.hpp        # Persistence
│
└── regime.hpp                         # Convenience header

src/regime/
├── ontology/
├── models/
│   ├── market/
│   └── macro/
├── mapping/
├── aggregation/
└── belief/

tests/regime/
├── test_market_regime_models.cpp
├── test_macro_regime_models.cpp
├── test_mapping.cpp
├── test_aggregation.cpp
└── test_regime_belief.cpp
```

### CMake Configuration

```cmake
# Regime Detection Library
add_library(regime
    # Ontology
    src/regime/ontology/macro_regime.cpp
    src/regime/ontology/market_regime.cpp
    src/regime/ontology/regime_distribution.cpp
    
    # Market models
    src/regime/models/market/market_hmm.cpp
    src/regime/models/market/market_msar.cpp
    src/regime/models/market/volatility_feature_extractor.cpp
    src/regime/models/market/unsupervised_regime_detector.cpp
    src/regime/models/market/market_regime_classifier.cpp
    
    # Macro models
    src/regime/models/macro/dynamic_factor_model.cpp
    src/regime/models/macro/macro_msdfe.cpp
    src/regime/models/macro/growth_inflation_quadrant.cpp
    src/regime/models/macro/bayesian_structural_time_series.cpp
    src/regime/models/macro/macro_regime_classifier.cpp
    
    # Mapping
    src/regime/mapping/regime_model_base.cpp
    
    # Aggregation
    src/regime/aggregation/market_regime_aggregator.cpp
    src/regime/aggregation/macro_regime_aggregator.cpp
    src/regime/aggregation/conflict_resolver.cpp
    
    # Belief
    src/regime/belief/regime_belief.cpp
    src/regime/belief/regime_belief_store.cpp
)

target_include_directories(regime PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(regime PUBLIC
    Eigen3::Eigen
    statistics  # From statistics module
    core        # For error handling, logging
    data        # For database access
)
```

---

## 11. Implementation Phases

### Phase 1: Foundation (Weeks 1-2)
- [ ] Regime ontology definitions (MacroRegime, MarketRegime enums)
- [ ] RegimeDistribution struct
- [ ] RegimeModelBase interface
- [ ] RegimeBelief object
- [ ] Basic aggregation framework (weighted combination)

### Phase 2: Market Regime Models (Weeks 3-5)
- [ ] MarketHMM implementation
- [ ] MarketMSAR implementation
- [ ] VolatilityFeatureExtractor (GARCH/EGARCH wrapper)
- [ ] Model-to-ontology mappings
- [ ] Unit tests for each model

### Phase 3: Macro Regime Models (Weeks 6-8)
- [ ] DynamicFactorModel implementation
- [ ] MacroMSDFM implementation
- [ ] GrowthInflationQuadrant (rule-based)
- [ ] Model-to-ontology mappings
- [ ] Unit tests for each model

### Phase 4: Aggregation Logic (Weeks 9-10)
- [ ] MarketRegimeAggregator with weighting
- [ ] MacroRegimeAggregator with weighting
- [ ] Temporal persistence logic
- [ ] ConflictResolver rules
- [ ] Integration tests

### Phase 5: ML Models (Optional) (Weeks 11-12)
- [ ] MarketRegimeClassifier (RF/GBM)
- [ ] MacroRegimeClassifier (XGBoost)
- [ ] UnsupervisedRegimeDetector (K-Means/GMM)
- [ ] Feature engineering pipeline

### Phase 6: Advanced Models (Optional) (Weeks 13-16)
- [ ] BayesianStructuralTimeSeries
- [ ] Advanced MS-DFM variants
- [ ] Regime transition prediction
- [ ] Calibration and backtesting

### Phase 7: Production Readiness (Weeks 17-18)
- [ ] RegimeBeliefStore persistence
- [ ] Comprehensive logging
- [ ] Performance optimization
- [ ] Documentation
- [ ] Deployment testing

---

## 12. Dependencies & Prerequisites

### Required (Must Complete First)

1. **Statistics Module Foundation** (`statistics_module_foundation.md`)
   - ✅ HMM with log-space implementation
   - ✅ Markov Switching Model
   - ✅ Kalman Filter (numerically stable)
   - ✅ GARCH/EGARCH
   - ✅ PCA
   - ✅ Base classes and modular structure

2. **Config System** (`config_deliverable.md`)
   - ✅ JSON configuration management
   - ✅ Validation framework
   - ✅ Environment-specific configs

3. **Core Infrastructure**
   - ✅ Error handling (`Result<T>`)
   - ✅ Logging framework
   - ✅ Database access

### External Dependencies

| Library | Purpose | License | Required? |
|---------|---------|---------|-----------|
| Eigen | Linear algebra | MPL2 | Yes |
| XGBoost | Gradient boosting | Apache 2.0 | Optional |
| nlopt | Optimization (GARCH) | LGPL | Recommended |
| Boost.Math | Statistical distributions | Boost | Optional |

### Optional (Enhances Capabilities)

- Statistics Module Enhancements (Hurst Exponent, Ridge/Lasso regression)
- ML library for unsupervised clustering
- Time series database for regime history

---

## 13. Testing Strategy

### Unit Tests

Each model requires:
- Synthetic data tests (known regimes)
- Edge case handling (missing data, NaN)
- Mapping validation (correct ontology probabilities)
- Numerical stability tests

### Integration Tests

- Multi-model aggregation with conflicting beliefs
- Temporal persistence (regime duration constraints)
- Conflict resolution (macro vs market contradictions)
- Full pipeline: data → models → mapping → aggregation → belief

### Validation Tests

- Compare regime detections against historical labeled periods
- Backtest regime stability (frequency of regime changes)
- Compare model beliefs vs aggregated beliefs
- Stress test with crisis periods (2008, 2020)

### Performance Benchmarks

- Model inference time (per model)
- Aggregation latency (total pipeline)
- Memory usage (model storage)
- Database query performance (regime history)

### Example Test Structure

```cpp
TEST(MarketHMM, DetectsLowVolRegime) {
    // Synthetic low-vol trending data
    auto returns = generate_trending_returns(0.001, 0.10, 252);
    
    MarketHMM hmm(config);
    auto state = hmm.infer(returns);
    auto dist = hmm.map_to_ontology(state.value());
    
    EXPECT_GT(dist.market_probs[MarketRegime::LOW_VOL_TREND], 0.6);
}

TEST(RegimeAggregation, PreventsThrashing) {
    MarketRegimeAggregator agg(config);
    
    // Simulate alternating beliefs
    for (int i = 0; i < 10; ++i) {
        RegimeDistribution belief = (i % 2 == 0) ? low_vol_belief : high_vol_belief;
        auto result = agg.aggregate({belief});
        
        // Should smooth out rapid changes
        EXPECT_LT(result.market_probs[result.most_likely_market], 0.9);
    }
}
```

---

## 11. Failure-State Controls

### 11.1 Failure Modes and Mitigations

| Failure Mode | Symptoms | Mitigation |
|--------------|----------|------------|
| **Thrashing** | Rapid regime flips, excessive turnover | Dwell time, hysteresis, smoothing, turnover cap |
| **ML Over-Dominance** | Statistical models ignored, unstable predictions | Hard weight caps (≤25%), calibration checks, drift-triggered weight decay |
| **Ontology Drift** | Model states no longer match ontology semantics | Canonical signatures, retrain state alignment, mapping versioning |
| **Strategy Leakage** | Strategies inferring regimes internally | Strict API: strategies get context only, no raw regime internals |
| **Hard Transition Costs** | Large trades on regime changes | Staged rebalance, cost-aware optimizer, emergency tiering |
| **Confidence Miscalibration** | Overconfident or underconfident predictions | Reliability tracking, decile calibration, Brier/log-loss monitoring |
| **Data Quality/Latency Issues** | Stale or missing data affecting inference | As-of gating, stale data alarms, fallback model subsets |
| **Overfitting** | Models fit to historical regimes, fail on new ones | Walk-forward validation, regime stress library, champion-challenger rollout |
| **Conflict Mis-handling** | Contradictory macro/market signals | Explicit precedence matrix + scenario unit tests |
| **Observability Gaps** | Cannot reproduce past decisions | Full lineage logs and replay reproducibility drills |

---

## 12. Validation and Acceptance Framework

### 12.1 Model-Level Validation

| Metric | Target | Frequency |
|--------|--------|-----------|
| Out-of-sample log-loss | < baseline | Each retrain |
| Brier score calibration | Within 5% of perfect | Monthly |
| State definition stability | < 10% drift | Each retrain |
| Cross-validation consistency | > 70% agreement | Each retrain |

### 12.2 Mapping-Level Validation

| Validation | Description |
|------------|-------------|
| Monotonicity tests | Higher stress metrics → higher stress probability |
| Continuity tests | Adjacent states have overlapping probability mass |
| Retrain alignment | New model states map consistently to same ontology buckets |
| Policy-impact delta reports | Document how policy outputs change by mapping version |

### 12.3 Portfolio-Level Validation

| Metric | Target |
|--------|--------|
| Net-of-cost Sharpe/Sortino | > baseline strategy |
| Max drawdown | < risk-off regime threshold |
| Recovery profiles | Faster recovery vs non-regime-aware baseline |
| Turnover utilization | < cost budget |
| Crash-window behavior | Reduced exposure within T+2 of regime flip |
| False-alarm costs | < 1% of risk budget |

### 12.4 Operations-Level Validation

| Check | Frequency |
|-------|-----------|
| Decision timestamp replayability | Weekly drill |
| Model-down failover | Quarterly drill |
| Data-gap failover | Quarterly drill |
| Latency SLOs (inference-to-order) | Real-time monitoring |

---

## 13. Monitoring Dashboard KPIs

| KPI | Description | Alert Threshold |
|-----|-------------|-----------------|
| Regime switch rate | # changes per month/quarter | > 3/month |
| Confidence calibration error | Decile calibration gap | > 5% |
| Feature/model drift | Distribution shift metrics | > 2σ from baseline |
| Mapping stability score | Consistency across retrains | < 80% |
| ML contribution share | % of total weight from ML models | > 25% |
| Conflict-rule trigger counts | # times conflict rules activated | Trend monitoring |
| Turnover vs cap | Actual turnover / allowed turnover | > 90% |
| Cost slippage | Expected vs actual transaction costs | > 20% |
| Exposure envelope compliance | Time spent within limits | < 99% |
| Emergency de-risk events | Count of emergency triggers | Any event |
| Reproducibility pass/fail | Weekly replay test results | Any failure |

---

## 14. Implementation Roadmap

### Phase 1: Foundations (Weeks 1-4)
- [ ] Finalize ontology v2 + contract schemas
- [ ] Build as-of data pipeline + versioned feature store
- [ ] Implement logging lineage from day 1
- [ ] Define ontology fingerprints
- [ ] Set up monitoring infrastructure

### Phase 2: Core Inference (Weeks 5-10)
- [ ] Deploy HMM/MSAR/GARCH engines
- [ ] Deploy DFM/MS-DFM/Growth-Inflation Quadrant engines
- [ ] Build mapping module with distance-based soft mapping
- [ ] Add capped ML confirmers (RF/GBM/XGBoost)
- [ ] Implement calibration framework

### Phase 3: Aggregation/Policy (Weeks 11-14)
- [ ] Reliability-weighted aggregation
- [ ] Persistence + hysteresis + transition penalties
- [ ] Conflict-resolution hierarchy rules
- [ ] RegimeBelief → RegimeConstraints converter
- [ ] Unit tests for all aggregation logic

### Phase 4: Allocation/Execution Integration (Weeks 15-18)
- [ ] Strategy context adapter
- [ ] Cost/liquidity/turnover-aware optimization
- [ ] Staged transition scheduler
- [ ] Emergency de-risk protocol
- [ ] Integration tests

### Phase 5: Hardening and Promotion (Weeks 19-22)
- [ ] Calibration, drift, failover controls
- [ ] Champion/challenger shadow running
- [ ] Governance signoff process
- [ ] Production cutover
- [ ] Operational runbooks

---

## 15. Non-Negotiable Guardrails

> [!CAUTION]
> The following rules are **non-negotiable** and must be enforced in all implementations.

| Guardrail | Rationale |
|-----------|-----------|
| ❌ No direct trades from regime models | Regimes inform constraints, not positions |
| ❌ No uncapped ML influence | ML confirms, doesn't dominate (≤25% weight) |
| ❌ No unversioned ontology or mappings | Breaking changes require explicit versioning |
| ❌ No strategy-local regime inference logic | Strategies consume context, never infer regimes |
| ❌ No deployment without replayable audit trail | Every decision must be reproducible |
| ❌ No production promotion without out-of-sample calibration and stress tests | Validation before deployment |

---

## 16. Dependencies & Prerequisites

### Required (Must Complete First)

1. **Statistics Module Foundation** (`statistics_module_foundation.md`)
   - ✅ HMM with log-space implementation
   - ✅ Markov Switching Model
   - ✅ Kalman Filter (numerically stable)
   - ✅ GARCH/EGARCH
   - ✅ PCA
   - ✅ Base classes and modular structure

2. **Config System** (`config_deliverable.md`)
   - ✅ JSON configuration management
   - ✅ Validation framework
   - ✅ Environment-specific configs

3. **Core Infrastructure**
   - ✅ Error handling (`Result<T>`)
   - ✅ Logging framework
   - ✅ Database access

### External Dependencies

| Library | Purpose | License | Required? |
|---------|---------|---------|-----------|
| Eigen | Linear algebra | MPL2 | Yes |
| XGBoost | Gradient boosting | Apache 2.0 | Optional |
| nlopt | Optimization (GARCH) | LGPL | Recommended |
| Boost.Math | Statistical distributions | Boost | Optional |

---

## 17. Testing Strategy

### Unit Tests

Each model requires:
- Synthetic data tests (known regimes)
- Edge case handling (missing data, NaN)
- Mapping validation (correct ontology probabilities)
- Numerical stability tests

### Integration Tests

- Multi-model aggregation with conflicting beliefs
- Temporal persistence (regime duration constraints)
- Conflict resolution (macro vs market contradictions)
- Full pipeline: data → models → mapping → aggregation → belief

### Validation Tests

- Compare regime detections against historical labeled periods
- Backtest regime stability (frequency of regime changes)
- Compare model beliefs vs aggregated beliefs
- Stress test with crisis periods (2008, 2020)

### Performance Benchmarks

- Model inference time (per model)
- Aggregation latency (total pipeline)
- Memory usage (model storage)
- Database query performance (regime history)

---

## 18. Executive Checklist

> [!IMPORTANT]
> **"What not to miss" checklist** - verify all items before production deployment.

- [ ] Two-level ontology + overlays finalized
- [ ] Mapping is distance-based, calibrated, and versioned
- [ ] Aggregation has reliability weighting + persistence + hysteresis
- [ ] Macro-vs-market conflict hierarchy is explicit and tested
- [ ] ML influence is capped and drift-governed
- [ ] RegimeBelief and RegimeConstraints interfaces frozen
- [ ] Allocation is cost/liquidity/turnover-aware
- [ ] Full lineage logging and replay are operational
- [ ] Failover runbooks exist for model/data outages
- [ ] Promotion framework is walk-forward + champion/challenger based

---

## 19. Multi-Asset Regime Architecture

This section specifies how the regime detection system operates across multiple asset classes (sleeves) while maintaining a unified ontology and portfolio coordination.

### 19.1 Global vs Local Regimes (Core Design Decision)

#### 19.1.1 Global Regime (Shared)

The engine maintains **one Global Macro Regime** representing common background conditions across the portfolio:

- Growth/inflation backdrop
- Policy stance
- Credit/liquidity conditions

This global macro regime sets the **portfolio-level risk envelope**:
- Risk ceiling
- Leverage caps
- Volatility target
- Liquidity posture

#### 19.1.2 Local Regimes (Per Sleeve)

In parallel, the engine maintains **sleeve-specific Market Regimes**:

| Sleeve | Description |
|--------|-------------|
| **Equities** | Index futures + optional cash equity enhancement |
| **FX Futures** | Currency futures |
| **Rates Futures** | Fixed-income futures |
| **Commodities Futures** | Energy, metals, agriculture futures |

Each sleeve has its own local probabilities for `TREND / CHOP / STRESS`, because **regime dynamics differ by sleeve even when labels are the same**.

#### 19.1.3 Hierarchical Decision Rule

```
Global Macro Regime  →  Risk ceiling (how much total risk)
Local Sleeve Regime  →  Tactical aggressiveness (how to deploy within ceiling)
```

> [!IMPORTANT]
> The macro regime acts as a **governor/limiter**. The sleeve regime acts as a **gas pedal**. The gas pedal cannot exceed the governor.

```cpp
// Pseudocode: hierarchical control
double effective_risk = std::min(
    macro_risk_ceiling,                    // Governor
    sleeve_base_risk * sleeve_multiplier   // Gas pedal
);
```

---

### 19.2 One Ontology Contract, Multiple Interpretation Profiles

#### 19.2.1 Unified Ontology Contract (Required)

Keep **one shared top-level vocabulary** for portfolio coordination:

**Market L1 States** (shared across all sleeves):

| State | Description |
|-------|-------------|
| `TREND_LOWVOL` | Directional, orderly |
| `TREND_HIGHVOL` | Directional, volatile |
| `MEANREV_CHOPPY` | Range-bound, noisy |
| `STRESS_PRICE` | Price-driven stress |
| `STRESS_LIQUIDITY` | Liquidity-driven stress |

**Macro L1 States** (global, one set):

| State | Description |
|-------|-------------|
| `EXPANSION_DISINFLATION` | Growth + falling inflation |
| `EXPANSION_INFLATIONARY` | Growth + rising inflation |
| `SLOWDOWN_DISINFLATION` | Slowing growth + falling inflation |
| `SLOWDOWN_INFLATIONARY` | Slowing growth + rising inflation |
| `RECESSION_DEFLATIONARY` | Contraction + deflation |
| `RECESSION_INFLATIONARY` | Contraction + inflation |

**Overlays** (applicable to global or any sleeve):

| Overlay | Scope |
|---------|-------|
| `CORRELATION_SPIKE` | Cross-asset |
| `POLICY_RESTRICTIVE` / `POLICY_SUPPORTIVE` | Global |
| `CREDIT_TIGHTENING` / `CREDIT_EASING` | Global |
| `EVENT_SHOCK_TRANSIENT` | Per-sleeve or global |
| `INFLATION_STICKY` | Global |

#### 19.2.2 Interpretation Profiles (Required Per Sleeve)

Each sleeve defines **how ontology states are interpreted in that market** via:

| Profile Component | What It Specifies |
|-------------------|-------------------|
| **Feature engineering schema** | Which raw data → which features for that sleeve |
| **Model set and hyperparameter ranges** | Which models run, with what config |
| **Mapping signatures (state fingerprints)** | What each ontology state "looks like" in that sleeve |
| **Policy response curves** | How beliefs translate to constraints |
| **Execution/risk constraints** | Liquidity, roll, slippage specific to that sleeve |

> [!NOTE]
> The ontology labels are shared (for portfolio coordination), but the **statistical definition** of what `TREND_LOWVOL` means in equities vs. FX vs. commodities is sleeve-specific. `TREND_LOWVOL` in equities might mean vol < 12%, directional R² > 0.3. In commodities it might mean vol < 18%, term structure in backwardation + trending.

---

### 19.3 Sleeve Profile Definitions

#### 19.3.1 Equities Profile

Traded via futures and/or cash equities data as enhancement layer.

| Component | Specification |
|-----------|---------------|
| **Features** | Breadth, dispersion, index vol, earnings/event shock proxies |
| **Stress signatures** | Correlation spike + drawdown acceleration |
| **Policy sensitivity** | High beta to growth/inflation changes |
| **Enhancement layer** | Cash-equity and sector-level data (not mandatory in v1) |

#### 19.3.2 Rates/Fixed-Income Futures Profile

| Component | Specification |
|-----------|---------------|
| **Features** | Curve shape changes, term premium proxies, realized rates vol |
| **Stress signatures** | Rate gap risk, liquidity thinning around macro releases |
| **Policy sensitivity** | Very high to inflation and policy overlays |

#### 19.3.3 FX Profile

| Component | Specification |
|-----------|---------------|
| **Features** | Carry differentials, realized vol, USD regime proxies, cross-asset sentiment |
| **Stress signatures** | Dollar funding stress, intervention/event shock |
| **Policy sensitivity** | Relative-growth/relative-rate framing |

#### 19.3.4 Commodities Profile

| Component | Specification |
|-----------|---------------|
| **Features** | Term structure (contango/backwardation), inventory proxies, seasonality, event flags |
| **Stress signatures** | Supply shocks, jump risk |
| **Policy sensitivity** | Inflation regime + growth-demand regime interactions |

---

### 19.4 Model Training Scope (Critical Constraint)

> [!CAUTION]
> **Do NOT fit one HMM/MSAR globally and reuse unchanged across all sleeves.**

Use the following training scope hierarchy:

| Scope | When to Use | Examples |
|-------|-------------|---------|
| **Per-symbol** | Highly idiosyncratic contracts | NG vs CL (natural gas vs crude oil) |
| **Per-cluster** | Homogeneous groups | DM FX majors, equity index futures cluster |
| **Per-sleeve** | Sleeve-level aggregate | Rates sleeve aggregate signal |

**Rationale**: Transition matrices, regime means, variances, and regime persistence are **sleeve/contract-dependent**. A transition matrix estimated on S&P 500 futures does not apply to 10-year Treasury futures.

```cpp
// Example: training scope config
struct ModelTrainingConfig {
    enum class Scope { PER_SYMBOL, PER_CLUSTER, PER_SLEEVE };
    
    Scope scope;
    std::string cluster_id;      // e.g., "dm_fx_majors", "energy_complex"
    std::string sleeve_id;       // e.g., "fx_futures", "rates_futures"
    
    // Per-scope hyperparameters
    int lookback_days;
    int n_states;
    int ar_order;
    double retrain_threshold;    // Drift threshold for retraining
};
```

---

### 19.5 Aggregation: Unified Formula, Profile-Specific Parameters

Use **one aggregation framework** across all sleeves, with **profile-specific calibration**:

| Parameter | Allowed to Differ by Sleeve | Example |
|-----------|----------------------------|---------|
| Model weights | ✅ | Rates: HMM 0.45, MSAR 0.35, GARCH 0.15, ML 0.05 |
| Persistence strength | ✅ | Commodities: higher (supply regimes persist) |
| Hysteresis bands | ✅ | FX: wider (carry regimes are sticky) |
| ML contribution cap | ✅ | All capped at ≤25%, but effective weight varies |
| Conflict-rule sensitivity | ✅ | Rates: stronger macro dominance |

**Sleeve-specific calibration examples**:
- **Rates**: Stronger macro regime dominance (policy overlay heavily weighted)
- **Commodities**: Stronger event/supply-shock overlay weighting
- **FX**: Higher sensitivity to USD stress overlays
- **Equities**: Standard balanced weighting

---

### 19.6 Futures-Only Book: What Differs by Sleeve

Even though all sleeves trade futures, these differ **materially**:

| Dimension | Why It Differs |
|-----------|---------------|
| **Return dynamics** | Equity mean-reversion vs. commodity momentum |
| **Volatility clustering** | Shape and persistence of vol regimes |
| **Jump/event behavior** | Earnings (equities) vs. OPEC (commodities) vs. NFP (rates) |
| **Macro sensitivity** | Equities: growth beta. Rates: inflation beta. FX: relative growth |
| **Liquidity + roll structure** | Quarterly rolls (equities) vs. monthly (commodities) |
| **Carry interpretation** | Roll yield (commodities), term premium (rates), interest differential (FX) |

Therefore **sleeve-specific configs are required** for:
- Features
- Model parameters
- Mapping fingerprints
- Policy response curves
- Execution constraints

---

### 19.7 Recommended Structure for Current Book

Use **one global engine** with (at minimum) these sleeves:

```
┌──────────────────────────────────────────────────┐
│             Global Macro Regime Engine            │
│  (GDP, CPI, PMI, yields, credit, policy)         │
│  → One macro belief + overlays                   │
└───────────────────┬──────────────────────────────┘
                    │ Risk ceiling
        ┌───────────┼───────────┬───────────┐
        ▼           ▼           ▼           ▼
  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐
  │ FX Sleeve│ │Rates     │ │Commodit. │ │Equities      │
  │ Futures  │ │Sleeve    │ │Sleeve    │ │Sleeve /      │
  │          │ │Futures   │ │Futures   │ │Sentiment     │
  │ Local    │ │ Local    │ │ Local    │ │Overlay       │
  │ HMM/MSAR│ │ HMM/MSAR│ │ HMM/MSAR│ │ HMM/MSAR    │
  │ +GARCH   │ │ +GARCH   │ │ +GARCH   │ │ +GARCH      │
  │ +ML cap  │ │ +ML cap  │ │ +ML cap  │ │ +ML cap     │
  └────┬─────┘ └────┬─────┘ └────┬─────┘ └──────┬──────┘
       │            │            │               │
       └────────────┼────────────┼───────────────┘
                    ▼
        ┌────────────────────────┐
        │ Shared Ontology +      │
        │ Portfolio Coordinator  │
        └────────────────────────┘
```

All sleeves share the **ontology contract + portfolio coordinator**, but run **local market inference stacks**: HMM/MSAR/GARCH + capped ML confirmer.

---

### 19.8 Risk Budget Equations

#### 19.8.1 Sleeve-Level Risk Budget

For sleeve *k*:

```
RiskBudget_k = B_k × f_macro(MacroBelief) × f_market,k(MarketBelief_k) × f_overlay,k
```

Where:
- **B_k**: Base sleeve risk budget (static allocation, sums to 1.0 across sleeves)
- **f_macro(MacroBelief)**: Shared multiplier from global macro regime (0.0–1.0)
- **f_market,k(MarketBelief_k)**: Sleeve-local multiplier from local market regime
- **f_overlay,k**: Stress/event/correlation/policy adjustments

#### 19.8.2 Sub-Sleeve Decomposition

If sleeve *k* has sub-sleeves *j* (e.g., energy/metals/ags within commodities):

```
B_k_final = B_k_base × M_macro × M_k,sleeve × O_k

B_k,j_final = B_k_final × w_k,j_base × M_k,j,sub
```

Then renormalize across sub-sleeves and enforce:
- Turnover constraints
- Cost constraints
- Liquidity constraints

```cpp
// Example implementation
struct SleeveRiskBudget {
    double base_budget;            // B_k
    double macro_multiplier;       // f_macro
    double market_multiplier;      // f_market,k
    double overlay_adjustment;     // f_overlay,k
    
    double effective() const {
        return base_budget * macro_multiplier * market_multiplier * overlay_adjustment;
    }
};

struct SubSleeveRiskBudget {
    double parent_budget;          // B_k_final
    double sub_weight;             // w_k,j_base
    double sub_multiplier;         // M_k,j,sub
    
    double effective() const {
        return parent_budget * sub_weight * sub_multiplier;
    }
};
```

---

### 19.9 Granularity Policy (Avoid Overfitting)

> [!WARNING]
> Use **hierarchical granularity**, not forced symmetry. Not every sleeve needs the same depth.

#### Materiality Gate

Add deeper splits (sub-sleeves) **only if all 3 criteria pass**:

| Criterion | Description |
|-----------|-------------|
| **Materiality** | Improves risk-adjusted return |
| **Stability** | Signal is robust out-of-sample |
| **Actionability** | Tradable/hedgeable sizing impact |

#### Default v1 Granularity

| Level | What | Required? |
|-------|------|-----------|
| **Level 0** | Global macro regime | Always |
| **Level 1** | Sleeve regimes (FX, Rates, Commodities, Equities) | Always |
| **Level 2** | Selective sub-sleeves | Only where all 3 criteria pass |

**Level 2 default guidance**:
- **Equities**: Sectors/styles (if traded)
- **Commodities**: Complexes (energy / metals / agriculture)
- **Rates/FX**: Mostly sleeve-level + overlays initially

---

### 19.10 Transition Intelligence Layer (Proactive Upgrade)

> [!IMPORTANT]
> This layer addresses the **reactive-only limitation** of the base architecture. It sits **between the Aggregation Layer (L5) and the Policy Layer (L7)** and provides forward-looking regime transition intelligence.

#### 19.10.1 Current Limitation

The base system is strong at **nowcasting** P(S_t | Y_{1:t}), but weak at:
- Transition forecasting P(S_{t+h} | Y_{1:t})
- Time-to-transition estimation
- Early warning

#### 19.10.2 Required Outputs

For macro + each sleeve, the Transition Intelligence Layer must produce:

| Output | Description |
|--------|-------------|
| **Transition term structure** | P(S_{t+h} ≠ S_t \| Y_{1:t}), for h ∈ {1, 5, 10, 20} days |
| **Time-to-transition estimate** | Median + confidence band |
| **Early-warning score** | 0–100 composite index + driver decomposition |
| **Directional transition probs** | e.g., P(TREND → CHOPPY), P(EXPANSION → SLOWDOWN) |

#### 19.10.3 Implementation Path (No Deep Learning Required)

**A) Markov Forward Propagation**:

Use estimated transition matrix to propagate current belief forward:

```
π_{t+h} = π_t × P_t^h
```

Where π_t is the current regime probability vector and P_t is the transition matrix.

**B) Time-Varying Transition Probabilities (TVTP)**:

Make transition probabilities depend on observable covariates:

```
P_{ij,t} = softmax(a_ij + b_ij^T × z_t)
```

Where z_t includes:
- Vol-of-vol
- Credit spreads
- Curve shifts
- Liquidity stress indicators

**C) Hazard Model for Switching Risk**:

```
h_t = P(switch at t+1 | still in current state, X_t)
```

Duration-dependent switching probability — the longer you've been in a regime, the higher (or lower, depending on regime type) the switching hazard.

**D) Early-Warning Composite Index**:

Constructed from:

| Signal | What It Measures | Weight |
|--------|-----------------|--------|
| Entropy rise | Increasing uncertainty across regime probabilities | High |
| Confidence decay | Dominant regime probability declining | High |
| Cross-model disagreement | Models diverging in their beliefs | Medium |
| Overlay stress escalation | Stress overlays activating | Medium |

#### 19.10.4 Policy Usage (Staged, Not Binary)

| Warning Level | Response |
|---------------|----------|
| **Low** (score 0–30) | Normal multipliers, no action |
| **Medium** (score 30–60) | Tighten thresholds, reduce marginal risk, increase hedge readiness |
| **High** (score 60–100) | Fractional pre-de-risk, shorter rebalance horizon, higher liquidity buffer, progressive leverage reduction |

```cpp
struct TransitionWarning {
    double warning_score;           // 0-100
    double time_to_transition;      // median days
    double transition_ci_lower;     // confidence interval lower bound
    double transition_ci_upper;     // confidence interval upper bound
    
    std::map<MarketRegime, double> directional_probs;  // P(current → target)
    std::vector<std::string> top_drivers;               // Ranked driver list
    
    WarningLevel level() const {
        if (warning_score < 30) return WarningLevel::LOW;
        if (warning_score < 60) return WarningLevel::MEDIUM;
        return WarningLevel::HIGH;
    }
};
```

---

### 19.11 Equities Integration

Equities can be integrated in **two ways**, depending on whether equities are traded directly:

| Mode | When to Use | What It Does |
|------|-------------|-------------|
| **Equity Sleeve** | If traded via futures/cash | Full local regime model + local risk budgets |
| **Equity Enhancement Overlay** | If NOT traded directly | Equity signals as cross-asset risk sentiment modifiers |

> [!NOTE]
> Cash-equity and sector-level data should be treated as an **enhancement layer** introduced after base sleeve stability is proven — not mandatory at initial rollout.

---

## Next Steps

Once this architecture is complete, proceed to:

**`regime_aware_portfolio_engine.md`** - Integration with portfolio management and strategies

This document describes:
- Regime Policy Layer (beliefs → constraints)
- Allocation & Risk Engine
- Strategy Layer integration
- How strategies consume regime beliefs without inferring them
