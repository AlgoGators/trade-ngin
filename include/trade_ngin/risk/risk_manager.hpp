#pragma once

#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief Configuration for risk management
 */
struct RiskConfig : public ConfigBase {
    // Risk limits
    double var_limit{0.15};          // Value at Risk limit (15%)

    // ── Pair-wise correlation cap + per-bar 99th-pct jump cap ──
    double jump_risk_limit{0.10};    // Per-bar 99th-pct |w·r| cap (used by calculate_jump_multiplier)
    double max_correlation{0.7};     // Pair-wise |ρ| cap (used by calculate_correlation_multiplier)

    double max_gross_leverage{4.0};  // Maximum gross leverage
    double max_net_leverage{2.0};     // Maximum net leverage

    // Calculation parameters
    double confidence_level{0.99};  // Confidence level for risk calcs
    int lookback_period{252};       // Historical lookback period
    Decimal capital{Decimal(1e6)};  // Portfolio capital

    // Configuration metadata
    std::string version{"1.0.0"};  // Configuration version

    // Implement serialization methods
    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["var_limit"] = var_limit;
        j["jump_risk_limit"] = jump_risk_limit;
        j["max_correlation"] = max_correlation;
        j["max_gross_leverage"] = max_gross_leverage;
        j["max_net_leverage"] = max_net_leverage;
        j["confidence_level"] = confidence_level;
        j["lookback_period"] = lookback_period;
        j["capital"] = static_cast<double>(capital);
        j["version"] = version;

        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("var_limit"))
            var_limit = j.at("var_limit").get<double>();
        if (j.contains("jump_risk_limit"))
            jump_risk_limit = j.at("jump_risk_limit").get<double>();
        if (j.contains("max_correlation"))
            max_correlation = j.at("max_correlation").get<double>();
        if (j.contains("max_gross_leverage"))
            max_gross_leverage = j.at("max_gross_leverage").get<double>();
        if (j.contains("max_net_leverage"))
            max_net_leverage = j.at("max_net_leverage").get<double>();
        if (j.contains("confidence_level"))
            confidence_level = j.at("confidence_level").get<double>();
        if (j.contains("lookback_period"))
            lookback_period = j.at("lookback_period").get<int>();
        if (j.contains("capital"))
            capital = Decimal(j.at("capital").get<double>());
        if (j.contains("version"))
            version = j.at("version").get<std::string>();
    }
};

/**
 * @brief Result from risk calculations
 */
struct RiskResult {
    bool risk_exceeded{false};      // Flag indicating risk exceeded
    double recommended_scale{1.0};  // Recommended scale-down factor

    // Risk metrics
    double portfolio_var{0.0};     // Portfolio Value at Risk
    double jump_risk{0.0};         // Jump risk
    double correlation_risk{0.0};  // Correlation risk
    double gross_leverage{0.0};    // Gross leverage (total_value / capital)
    double net_leverage{0.0};      // Net leverage

    // Maximum observed risks
    double max_portfolio_risk{0.0};  // Maximum portfolio risk
    double max_jump_risk{0.0};       // Maximum jump risk
    double max_leverage_risk{0.0};   // Maximum leverage risk

    // Individual multipliers
    double portfolio_multiplier{1.0};    // Portfolio VaR multiplier
    double jump_multiplier{1.0};         // Jump risk multiplier
    double correlation_multiplier{1.0};  // Correlation multiplier
    double leverage_multiplier{1.0};     // Leverage multiplier
};

/**
 * @brief Market data for risk calculations
 */
struct MarketData {
    std::vector<std::vector<double>> returns;
    std::vector<std::vector<double>> covariance;
    std::unordered_map<std::string, size_t> symbol_indices;
    std::vector<std::string> ordered_symbols;
};

/**
 * @brief Risk management class
 */
class RiskManager {
public:
    explicit RiskManager(RiskConfig config);

    /**
     * @brief Process positions and calculate risk metrics
     * @param positions Current portfolio positions
     * @return Result containing risk calculations
     */
    Result<RiskResult> process_positions(const std::unordered_map<std::string, Position>& positions,
                                         const MarketData& market_data,
                                         const std::unordered_map<std::string, double>& current_prices = {});

    /**
     * @brief Update risk configuration
     * @param config New configuration
     * @return Result indicating success or failure
     */
    Result<void> update_config(const RiskConfig& config);

    /**
     * @brief Get current risk configuration
     * @return Current configuration
     */
    const RiskConfig& get_config() const {
        return config_;
    }

    /// Per symbol, the adjusted level at each bar timestamp (roll_series::adjusted_levels on the
    /// symbol's bars in time order): the raw close plus the later contract-switch steps.
    using AdjustedLevels = std::unordered_map<std::string, std::map<Timestamp, double>>;

    /**
     * @brief Create market data object from bar data
     * @param data Bar data to convert
     * @param adjusted T-ROLLX (LOOP_SPEC v6.1 section 2.3): the adjusted levels the return rows
     *        read, r = (A_t - A_prev) / P_prev over consecutive timestamps of `data`; when null
     *        they are built from `data`'s own bars (each bar's instrument_id). The gate passes the
     *        levels of its whole window so a switch on a date its F5 filter drops is still a
     *        step, not a return (adjusted_levels_of).
     * @return MarketData object
     */
    MarketData create_market_data(const std::vector<Bar>& data,
                                  const AdjustedLevels* adjusted = nullptr);

    /// The adjusted levels of every symbol's bars in `data` (one level per (symbol, timestamp);
    /// a repeated timestamp keeps the last bar, as create_market_data's price map does).
    static AdjustedLevels adjusted_levels_of(const std::vector<Bar>& data);

    /// Gross and net leverage of a book and the leverage multiplier the gate would give it,
    /// valued exactly as process_positions values it (average price x contract multiplier, over
    /// the holdings the window maps), logging nothing of its own and touching no state. The one
    /// exception is the registry's: a symbol it does not know makes InstrumentRegistry log its
    /// own "Instrument not found" ERROR, exactly as it does inside process_positions (0 such
    /// lines on every futures run of the gate). For reading a book the gate did not see -- the
    /// one shipped after rounding.
    struct LeverageReading {
        double gross_leverage{0.0};
        double net_leverage{0.0};  ///< signed
        double multiplier{1.0};    ///< min(1, max_gross / gross, max_net / |net|)
    };
    LeverageReading leverage_of(const std::unordered_map<std::string, Position>& positions,
                                const MarketData& market_data) const;

    /// The InstrumentRegistry's contract multiplier for a symbol (variant suffix stripped), 1.0
    /// when the registry has none. The one lookup process_positions and leverage_of share.
    static double contract_multiplier_for(const std::string& symbol);

private:
    RiskConfig config_;

    // POSGUARD's per-run state. process_positions filters out any holding whose symbol has no
    // bar in the gate's window, silently; the guard reports that once per run rather than once
    // per call (it was 19.5 % of the live equity log as a per-call line), and again whenever a
    // later call drops MORE non-zero holdings than any before it.
    mutable size_t posguard_high_water_{0};
    mutable bool posguard_reported_{false};

    /**
     * @brief Calculate position weights
     * @param positions Portfolio positions
     * @return Vector of position weights
     */
    std::vector<double> calculate_weights(
        const std::unordered_map<std::string, Position>& positions) const;

    /**
     * @brief Calculate the portfolio multiplier based on position weights
     * @param market_data Market data for calculations
     * @param weights Vector of position weights
     * @param result RiskResult object to store intermediate calculations
     * @return Portfolio multiplier
     */
    double calculate_portfolio_multiplier(const MarketData& market_data,
                                          const std::vector<double>& weights,
                                          RiskResult& result) const;

    /**
     * @brief Calculate the jump multiplier based on position weights
     * @param market_data Market data for calculations
     * @param weights Vector of position weights
     * @param result RiskResult object to store intermediate calculations
     * @return Jump multiplier
     */
    double calculate_jump_multiplier(const MarketData& market_data,
                                     const std::vector<double>& weights, RiskResult& result) const;

    /**
     * @brief Calculate the correlation multiplier based on position weights
     * @param market_data Market data for calculations
     * @param weights Vector of position weights
     * @param result RiskResult object to store intermediate calculations
     * @return Correlation multiplier
     */
    double calculate_correlation_multiplier(const MarketData& market_data,
                                            const std::vector<double>& weights,
                                            RiskResult& result) const;

    /**
     * @brief Calculate the leverage multiplier based on position weights
     * @param market_data Market data for calculations
     * @param weights Vector of position weights
     * @param total_value Total portfolio value
     * @param result RiskResult object to store intermediate calculations
     * @return Leverage multiplier
     */
    double calculate_leverage_multiplier(const MarketData& market_data,
                                         const std::vector<double>& weights, 
                                         const std::vector<double>& position_values,
                                         double total_value, RiskResult& result) const;

    /**
     * @brief Calculate historical returns from market data
     * @param data Market data bars
     * @return Matrix of returns
     */
    std::vector<std::vector<double>> calculate_returns(const std::vector<Bar>& data) const;

    /**
     * @brief Calculate covariance matrix from returns
     * @param returns Return matrix
     * @return Covariance matrix
     */
    std::vector<std::vector<double>> calculate_covariance(
        const std::vector<std::vector<double>>& returns) const;

    /**
     * @brief Calculate Value at Risk for portfolio
     * @param positions Portfolio positions
     * @param returns Historical returns
     * @return Calculated VaR
     */
    double calculate_var(const std::unordered_map<std::string, Position>& positions,
                         const std::vector<std::vector<double>>& returns) const;

    /**
     * @brief Calculate the nth percentile of given data
     * @param data Vector of values
     * @return Calculated percentile value
     */
    double calculate_99th_percentile(const std::vector<double>& data) const;
};

}  // namespace trade_ngin