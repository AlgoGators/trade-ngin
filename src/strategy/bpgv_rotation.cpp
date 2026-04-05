// src/strategy/bpgv_rotation.cpp
#include "trade_ngin/strategy/bpgv_rotation.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace trade_ngin {

// Per-asset extreme allocation tables
// Weights are looked up by symbol from the config's risk_on_symbols / risk_off_symbols.
// If a symbol isn't found in the table, it gets an equal share within its bucket.
// These defaults match the Python V3 ETF weights; the backtest entry point can
// override them via config when using stock proxies instead of ETFs.
const std::unordered_map<std::string, double> BPGVRotationStrategy::RISK_ON_EXTREME = {};
const std::unordered_map<std::string, double> BPGVRotationStrategy::RISK_OFF_EXTREME = {};

BPGVRotationStrategy::BPGVRotationStrategy(
    std::string id, StrategyConfig config, BPGVRotationConfig bpgv_config,
    std::shared_ptr<PostgresDatabase> db, std::shared_ptr<InstrumentRegistry> registry)
    : BaseStrategy(std::move(id), std::move(config), std::move(db)),
      bpgv_config_(std::move(bpgv_config)),
      registry_(registry) {
    Logger::register_component("BPGVRotation");

    metadata_.name = "BPGV Macro Regime Rotation Strategy";
    metadata_.description = "Macro-driven regime rotation using building permit growth volatility";
}

Result<void> BPGVRotationStrategy::validate_config() const {
    auto result = BaseStrategy::validate_config();
    if (result.is_error()) return result;

    if (bpgv_config_.rebalance_day_of_month < 1 || bpgv_config_.rebalance_day_of_month > 28) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Rebalance day must be between 1 and 28",
                                "BPGVRotationStrategy");
    }

    if (bpgv_config_.crash_threshold >= 0.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Crash threshold must be negative",
                                "BPGVRotationStrategy");
    }

    if (bpgv_config_.risk_on_symbols.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Risk-on symbols list cannot be empty",
                                "BPGVRotationStrategy");
    }

    if (bpgv_config_.risk_off_symbols.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Risk-off symbols list cannot be empty",
                                "BPGVRotationStrategy");
    }

    return Result<void>();
}

Result<void> BPGVRotationStrategy::initialize() {
    auto base_result = BaseStrategy::initialize();
    if (base_result.is_error()) {
        ERROR("Base strategy initialization failed: " + std::string(base_result.error()->what()));
        return base_result;
    }

    set_pnl_accounting_method(PnLAccountingMethod::MIXED);
    INFO("BPGV rotation strategy using MIXED PnL accounting");

    // Load macro regime data from CSV
    auto macro_result = MacroCSVLoader::load(bpgv_config_.macro_csv_path);
    if (macro_result.is_error()) {
        ERROR("Failed to load macro CSV: " + std::string(macro_result.error()->what()));
        return make_error<void>(ErrorCode::NOT_INITIALIZED,
                                "Cannot initialize BPGV strategy without macro data: " +
                                    std::string(macro_result.error()->what()),
                                "BPGVRotationStrategy");
    }
    macro_data_ = macro_result.value();
    INFO("Loaded " + std::to_string(macro_data_.size()) + " macro regime records (" +
         std::to_string(macro_data_.front().year) + "-" +
         std::to_string(macro_data_.back().year) + ")");

    // Initialize per-symbol state and positions for all assets
    auto all_symbols = bpgv_config_.risk_on_symbols;
    all_symbols.insert(all_symbols.end(),
                       bpgv_config_.risk_off_symbols.begin(),
                       bpgv_config_.risk_off_symbols.end());

    for (const auto& symbol : all_symbols) {
        symbol_state_[symbol] = SymbolState{};

        Position pos;
        pos.symbol = symbol;
        pos.quantity = 0.0;
        pos.average_price = 0.0;
        pos.last_update = std::chrono::system_clock::now();
        positions_[symbol] = pos;
    }

    portfolio_value_ = config_.capital_allocation;

    INFO("BPGV rotation strategy initialized for " + std::to_string(all_symbols.size()) + " symbols");
    return Result<void>();
}

// ============================================================================
// on_data — daily heartbeat
// ============================================================================

Result<void> BPGVRotationStrategy::on_data(const std::vector<Bar>& data) {
    if (state_ != StrategyState::RUNNING) {
        return make_error<void>(ErrorCode::STRATEGY_ERROR,
                                "Strategy is not in running state",
                                "BPGVRotationStrategy");
    }

    if (data.empty()) return Result<void>();

    try {
        // 1. Update price histories
        update_price_histories(data);

        // 2. Compute and track portfolio value for crash detection
        double pv = compute_portfolio_value();
        if (pv > 0.0) {
            portfolio_value_ = pv;
        }
        portfolio_value_history_.push_back(portfolio_value_);
        while (portfolio_value_history_.size() > static_cast<size_t>(bpgv_config_.crash_lookback_days + 10)) {
            portfolio_value_history_.pop_front();
        }

        // 3. Extract date from bars
        auto date = extract_date(data.front().timestamp);

        // 4. Crash override management
        if (crash_override_active_) {
            if (is_crash_override_expired(data.front().timestamp)) {
                crash_override_active_ = false;
                INFO("Crash override expired, resuming normal regime");
                // Force rebalance on next eligible day by clearing last rebalance tracking
                last_rebalance_year_ = 0;
                last_rebalance_month_ = 0;
            }
            // While crash override is active, keep crash weights — don't rebalance
        }

        if (!crash_override_active_ && detect_crash()) {
            activate_crash_override(data.front().timestamp);
            current_weights_ = build_crash_weights();
            update_positions_from_weights();
        }

        // 5. Monthly rebalance check (only if no crash override)
        if (!crash_override_active_ && should_rebalance(date.year, date.month, date.day)) {
            execute_rebalance(date.year, date.month);
        }

        // 6. Update position timestamps (DO NOT set average_price -- on_execution manages cost basis)
        for (const auto& bar : data) {
            auto pos_it = positions_.find(bar.symbol);
            if (pos_it != positions_.end()) {
                pos_it->second.last_update = bar.timestamp;
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error in BPGVRotationStrategy::on_data: " + std::string(e.what()));
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                "Failed to process market data: " + std::string(e.what()),
                                "BPGVRotationStrategy");
    }
}

// ============================================================================
// Price history management
// ============================================================================

void BPGVRotationStrategy::update_price_histories(const std::vector<Bar>& bars) {
    for (const auto& bar : bars) {
        auto it = symbol_state_.find(bar.symbol);
        if (it == symbol_state_.end()) continue;

        auto& state = it->second;
        state.current_price = bar.close.as_double();
        state.price_history.push_back(state.current_price);
        state.last_update = bar.timestamp;
        trim_price_history(state);
    }
}

void BPGVRotationStrategy::trim_price_history(SymbolState& state) const {
    while (state.price_history.size() > MAX_PRICE_HISTORY) {
        state.price_history.pop_front();
    }
}

// ============================================================================
// Rebalancing
// ============================================================================

bool BPGVRotationStrategy::should_rebalance(int year, int month, int day) const {
    if (day < bpgv_config_.rebalance_day_of_month) return false;
    if (year == last_rebalance_year_ && month == last_rebalance_month_) return false;
    return true;
}

void BPGVRotationStrategy::execute_rebalance(int year, int month) {
    // Use PREVIOUS month's regime data (permits released mid-month)
    int prev_year = (month == 1) ? year - 1 : year;
    int prev_month = (month == 1) ? 12 : month - 1;

    auto record = MacroCSVLoader::find_record(macro_data_, prev_year, prev_month);
    if (!record.has_value()) {
        // Fallback to most recent available
        record = MacroCSVLoader::find_record_before(macro_data_, prev_year, prev_month);
        if (!record.has_value()) {
            WARN("No macro data available for rebalance at " +
                 std::to_string(year) + "-" + std::to_string(month) + ", skipping");
            return;
        }
        WARN("Using fallback macro data from " +
             std::to_string(record->year) + "-" + std::to_string(record->month));
    }

    // Weight calculation pipeline
    auto weights = calculate_base_weights(*record);
    apply_momentum_tilt(weights);
    apply_homebuilder_tilt(weights, *record);
    apply_breakout_filter(weights);
    normalize_weights(weights);

    current_weights_ = weights;
    update_positions_from_weights();

    last_rebalance_year_ = year;
    last_rebalance_month_ = month;

    DEBUG("Rebalanced " + std::to_string(year) + "-" + std::to_string(month) +
          " | regime_score=" + std::to_string(record->regime_score) +
          " | strong_risk_on=" + std::to_string(record->strong_risk_on));
}

// ============================================================================
// Weight calculation pipeline
// ============================================================================

std::unordered_map<std::string, double> BPGVRotationStrategy::calculate_base_weights(
    const MonthlyMacroRecord& rec) const {

    // Interpolation: risk_off_pct maps [-1, +1] to [5%, 45%]
    double risk_off_pct = bpgv_config_.base_risk_off_min +
                          bpgv_config_.base_risk_off_range * ((rec.regime_score + 1.0) / 2.0);
    double risk_on_pct = 1.0 - risk_off_pct;

    std::unordered_map<std::string, double> weights;

    // Use configured extreme weights, or fall back to equal weight within bucket
    const auto& on_extreme = bpgv_config_.risk_on_extreme_weights;
    const auto& off_extreme = bpgv_config_.risk_off_extreme_weights;

    auto all_symbols = bpgv_config_.risk_on_symbols;
    all_symbols.insert(all_symbols.end(),
                       bpgv_config_.risk_off_symbols.begin(),
                       bpgv_config_.risk_off_symbols.end());

    double n_risk_on = static_cast<double>(bpgv_config_.risk_on_symbols.size());
    double n_risk_off = static_cast<double>(bpgv_config_.risk_off_symbols.size());

    for (const auto& sym : all_symbols) {
        double on_w = 0.0;
        double off_w = 0.0;

        if (!on_extreme.empty()) {
            auto it = on_extreme.find(sym);
            if (it != on_extreme.end()) on_w = it->second;
        } else {
            // Equal weight fallback: risk-on assets get 1/N of 95%, risk-off get 1/N of 5%
            bool is_risk_on = std::find(bpgv_config_.risk_on_symbols.begin(),
                                         bpgv_config_.risk_on_symbols.end(), sym)
                              != bpgv_config_.risk_on_symbols.end();
            on_w = is_risk_on ? (0.95 / n_risk_on) : (0.05 / n_risk_off);
        }

        if (!off_extreme.empty()) {
            auto it = off_extreme.find(sym);
            if (it != off_extreme.end()) off_w = it->second;
        } else {
            bool is_risk_on = std::find(bpgv_config_.risk_on_symbols.begin(),
                                         bpgv_config_.risk_on_symbols.end(), sym)
                              != bpgv_config_.risk_on_symbols.end();
            off_w = is_risk_on ? (0.30 / n_risk_on) : (0.70 / n_risk_off);
        }

        weights[sym] = on_w * risk_on_pct + off_w * risk_off_pct;
    }

    // Strong risk-on boost: tilt toward equities, away from bonds
    if (rec.strong_risk_on) {
        for (const auto& sym : bpgv_config_.risk_on_symbols) {
            weights[sym] *= (1.0 + bpgv_config_.strong_risk_on_equity_boost);
        }
        for (const auto& sym : bpgv_config_.risk_off_symbols) {
            weights[sym] *= (1.0 - bpgv_config_.strong_risk_on_bond_reduction);
        }
    }

    return weights;
}

void BPGVRotationStrategy::apply_momentum_tilt(
    std::unordered_map<std::string, double>& weights) const {

    // Compute trailing returns for risk-on assets
    struct MomentumEntry {
        std::string symbol;
        double ret;
    };
    std::vector<MomentumEntry> entries;

    for (const auto& sym : bpgv_config_.risk_on_symbols) {
        auto it = symbol_state_.find(sym);
        if (it == symbol_state_.end()) continue;

        if (static_cast<int>(it->second.price_history.size()) < bpgv_config_.momentum_lookback_days) {
            continue;
        }

        double ret = calculate_trailing_return(it->second.price_history,
                                               bpgv_config_.momentum_lookback_days);
        entries.push_back({sym, ret});
    }

    if (entries.size() < 2) return;  // Need at least 2 to rank

    // Sort by return descending
    std::sort(entries.begin(), entries.end(),
              [](const MomentumEntry& a, const MomentumEntry& b) { return a.ret > b.ret; });

    // Apply tilt: top-ranked gets +40%, bottom gets -40%, linearly interpolated
    int n = static_cast<int>(entries.size());
    for (int i = 0; i < n; ++i) {
        double rank_score = (n > 1) ? (1.0 - 2.0 * i / (n - 1)) : 0.0;  // [+1, -1]
        double tilt = 1.0 + bpgv_config_.momentum_tilt_scale * rank_score;
        tilt = std::max(0.5, tilt);  // Floor at 0.5x to prevent near-zero weights
        weights[entries[i].symbol] *= tilt;
    }
}

void BPGVRotationStrategy::apply_homebuilder_tilt(
    std::unordered_map<std::string, double>& weights,
    const MonthlyMacroRecord& rec) const {

    auto it = weights.find(bpgv_config_.homebuilder_symbol);
    if (it == weights.end()) return;

    if (rec.permit_growth > 0.0) {
        it->second *= (1.0 + bpgv_config_.homebuilder_tilt_scale);
    } else {
        it->second *= std::max(0.2, 1.0 - bpgv_config_.homebuilder_tilt_scale);
    }
}

void BPGVRotationStrategy::apply_breakout_filter(
    std::unordered_map<std::string, double>& weights) const {

    int window = bpgv_config_.breakout_sma_window;

    for (const auto& sym : bpgv_config_.risk_on_symbols) {
        auto state_it = symbol_state_.find(sym);
        if (state_it == symbol_state_.end()) continue;

        const auto& ph = state_it->second.price_history;
        if (static_cast<int>(ph.size()) < window) continue;

        double sma = calculate_sma(ph, window);
        double current_price = state_it->second.current_price;

        // Zero weight if price is below SMA
        if (current_price < sma) {
            weights[sym] = 0.0;
        }
    }
}

void BPGVRotationStrategy::normalize_weights(
    std::unordered_map<std::string, double>& weights) const {

    double total = 0.0;
    for (const auto& [sym, w] : weights) {
        total += std::max(0.0, w);
    }

    if (total < 1e-8) {
        // Safety: equal weight across all assets
        auto all_symbols = bpgv_config_.risk_on_symbols;
        all_symbols.insert(all_symbols.end(),
                           bpgv_config_.risk_off_symbols.begin(),
                           bpgv_config_.risk_off_symbols.end());
        double eq = 1.0 / static_cast<double>(all_symbols.size());
        for (const auto& sym : all_symbols) {
            weights[sym] = eq;
        }
        return;
    }

    for (auto& [sym, w] : weights) {
        w = std::max(0.0, w) / total;
    }
}

// ============================================================================
// Crash detection and override
// ============================================================================

bool BPGVRotationStrategy::detect_crash() const {
    int needed = bpgv_config_.crash_lookback_days + 1;
    if (static_cast<int>(portfolio_value_history_.size()) < needed) return false;

    double current = portfolio_value_history_.back();
    double lookback = portfolio_value_history_[portfolio_value_history_.size() - needed];

    if (lookback < 1e-8) return false;

    double drawdown = (current - lookback) / lookback;
    return drawdown < bpgv_config_.crash_threshold;
}

void BPGVRotationStrategy::activate_crash_override(const Timestamp& ts) {
    crash_override_active_ = true;
    crash_override_start_ = ts;
    crash_override_count_++;
    INFO("CRASH OVERRIDE activated (count: " + std::to_string(crash_override_count_) +
         ") — switching to defensive allocation for " +
         std::to_string(bpgv_config_.crash_override_calendar_days) + " days");
}

bool BPGVRotationStrategy::is_crash_override_expired(const Timestamp& ts) const {
    auto duration = ts - crash_override_start_;
    auto hours = std::chrono::duration_cast<std::chrono::hours>(duration).count();
    return hours >= (bpgv_config_.crash_override_calendar_days * 24);
}

std::unordered_map<std::string, double> BPGVRotationStrategy::build_crash_weights() const {
    // Python V3 crash weights:
    // 55% equity (SPY 58%, IYR 22%, EQR 10%, IWM 10% of equity share)
    // 45% bonds (TLT 67%, GLD 33% of bond share)
    double equity_share = 1.0 - bpgv_config_.crash_defensive_weight;
    double bond_share = bpgv_config_.crash_defensive_weight;

    std::unordered_map<std::string, double> weights;

    // Initialize all to zero
    for (const auto& sym : bpgv_config_.risk_on_symbols) weights[sym] = 0.0;
    for (const auto& sym : bpgv_config_.risk_off_symbols) weights[sym] = 0.0;

    // Equity portion — concentrated in lower-beta names
    weights["SPY"] = equity_share * 0.58;
    weights["IYR"] = equity_share * 0.22;
    weights["EQR"] = equity_share * 0.10;
    weights["IWM"] = equity_share * 0.10;

    // Bond/gold portion
    weights["TLT"] = bond_share * 0.67;
    weights["GLD"] = bond_share * 0.33;

    return weights;
}

// ============================================================================
// Position management
// ============================================================================

double BPGVRotationStrategy::compute_portfolio_value() const {
    double value = 0.0;
    bool has_positions = false;

    for (const auto& [sym, pos] : positions_) {
        double qty = pos.quantity.as_double();
        if (std::abs(qty) < 1e-8) continue;

        has_positions = true;
        auto state_it = symbol_state_.find(sym);
        if (state_it != symbol_state_.end() && state_it->second.current_price > 0.0) {
            value += qty * state_it->second.current_price;
        }
    }

    return has_positions ? value : 0.0;
}

void BPGVRotationStrategy::update_positions_from_weights() {
    for (const auto& [sym, weight] : current_weights_) {
        auto state_it = symbol_state_.find(sym);
        double price = (state_it != symbol_state_.end()) ? state_it->second.current_price : 0.0;

        double target_shares = 0.0;
        if (price > 1e-8) {
            double target_value = weight * portfolio_value_;
            target_shares = target_value / price;

            if (!bpgv_config_.allow_fractional_shares) {
                target_shares = std::floor(target_shares);
            }
        }

        auto pos_it = positions_.find(sym);
        if (pos_it != positions_.end()) {
            pos_it->second.quantity = Quantity(target_shares);
            // DO NOT set average_price here -- on_execution() manages cost basis
        }
    }
}

// ============================================================================
// Helper calculations
// ============================================================================

double BPGVRotationStrategy::calculate_sma(const std::deque<double>& prices, int period) const {
    if (prices.empty() || period <= 0 || static_cast<int>(prices.size()) < period) {
        return 0.0;
    }

    double sum = 0.0;
    for (size_t i = prices.size() - period; i < prices.size(); ++i) {
        sum += prices[i];
    }
    return sum / period;
}

double BPGVRotationStrategy::calculate_trailing_return(
    const std::deque<double>& prices, int days) const {

    if (static_cast<int>(prices.size()) < days + 1) return 0.0;

    double current = prices.back();
    double past = prices[prices.size() - days - 1];
    if (past < 1e-8) return 0.0;

    return (current - past) / past;
}

std::unordered_map<std::string, std::vector<double>> BPGVRotationStrategy::get_price_history() const {
    std::unordered_map<std::string, std::vector<double>> history;
    for (const auto& [sym, state] : symbol_state_) {
        history[sym] = std::vector<double>(state.price_history.begin(),
                                            state.price_history.end());
    }
    return history;
}

BPGVRotationStrategy::DateParts BPGVRotationStrategy::extract_date(const Timestamp& ts) {
    auto time_t = std::chrono::system_clock::to_time_t(ts);
    std::tm* tm = std::localtime(&time_t);
    return {tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday};
}

}  // namespace trade_ngin
