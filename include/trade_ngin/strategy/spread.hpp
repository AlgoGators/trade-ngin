// include/trade_ngin/strategy/spread.hpp
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {

/**
 * @brief How the spread between the two legs is constructed
 */
enum class SpreadType {
    LOG_RATIO,   // spread = log(Pa) - beta * log(Pb)   (scale-free, default)
    PRICE_DIFF   // spread = Pa - beta * Pb             (raw dollar difference)
};

/**
 * @brief Configuration for a two-ticker spread (pairs) strategy.
 *
 * Every field except the two symbols carries a seed value, so a caller only has
 * to supply the pair to get a runnable strategy.
 */
struct SpreadConfig {
    std::string symbol_a;                          // Long leg when the spread is cheap
    std::string symbol_b;                          // Hedge leg
    SpreadType spread_type{SpreadType::LOG_RATIO}; // Spread construction
    int beta_period{60};                           // Rolling OLS window for the hedge ratio
    int zscore_period{20};                         // Rolling window for spread mean/std
    double entry_z{2.0};                           // |z| at which a position is opened
    double exit_z{0.5};                            // |z| at which an open position is flattened
    double stop_z{4.0};                            // |z| at which the position is stopped out
    double capital_per_leg_pct{0.10};              // Notional per leg as a fraction of capital
    int min_holding_period{1};                     // Bars a position must be held before exiting
    double initial_beta{1.0};                      // Hedge ratio used before the OLS window fills
    size_t max_history{512};                       // Cap on retained per-leg history
};

/**
 * @brief Rolling state for the pair.
 */
struct SpreadState {
    // Transformed price series (log prices for LOG_RATIO, raw prices for PRICE_DIFF)
    std::vector<double> a_history;
    std::vector<double> b_history;
    std::vector<double> spread_history;

    // Most recent raw closes
    double last_price_a{0.0};
    double last_price_b{0.0};

    // Rolling statistics
    double beta{1.0};
    double current_spread{0.0};
    double spread_mean{0.0};
    double spread_std{0.0};
    double current_zscore{0.0};
    bool zscore_valid{false};

    // Position state: +1 = long spread (long A / short B), -1 = short spread, 0 = flat
    int direction{0};
    double quantity_a{0.0};
    double quantity_b{0.0};
    double entry_price_a{0.0};
    double entry_price_b{0.0};
    double entry_zscore{0.0};
    int holding_period{0};

    // Set when a stop fires; blocks re-entry until the spread returns inside exit_z
    bool stopped_out{false};

    // Number of aligned (both legs present) observations processed
    size_t observations{0};

    Timestamp last_update{};
};

/**
 * @brief Mean-reverting spread strategy over exactly two tickers.
 *
 * Each bar where both legs report a close for the same timestamp, the strategy
 * re-estimates the hedge ratio by rolling OLS, forms the spread, converts it to
 * a rolling z-score, and holds equal-and-opposite beta-weighted legs while the
 * spread is dislocated.
 */
class SpreadStrategy : public BaseStrategy {
public:
    /**
     * @brief Constructor
     * @param id Strategy identifier
     * @param config Base strategy configuration
     * @param spread_config Spread specific configuration
     * @param db Database interface
     * @param registry Instrument registry (optional)
     */
    SpreadStrategy(std::string id, StrategyConfig config, SpreadConfig spread_config,
                   std::shared_ptr<PostgresDatabase> db,
                   std::shared_ptr<InstrumentRegistry> registry = nullptr);

    /**
     * @brief Initialize strategy
     */
    Result<void> initialize() override;

    /**
     * @brief Process new market data
     * @param data Vector of price bars, in any symbol order
     */
    Result<void> on_data(const std::vector<Bar>& data) override;

    /**
     * @brief Handle execution reports
     */
    Result<void> on_execution(const ExecutionReport& report) override;

    /**
     * @brief Return price history for both legs
     */
    std::unordered_map<std::string, std::vector<double>> get_price_history() const override;

    /**
     * @brief Current spread state (rolling statistics and position)
     */
    const SpreadState& get_state_snapshot() const { return state_; }

    /**
     * @brief Current rolling hedge ratio
     */
    double get_beta() const { return state_.beta; }

    /**
     * @brief Current spread z-score (0.0 until the rolling window has filled)
     */
    double get_zscore() const { return state_.zscore_valid ? state_.current_zscore : 0.0; }

    /**
     * @brief Current spread direction: +1 long spread, -1 short spread, 0 flat
     */
    int get_direction() const { return state_.direction; }

    /**
     * @brief Spread configuration in use
     */
    const SpreadConfig& get_spread_config() const { return spread_config_; }

protected:
    Result<void> validate_config() const override;

private:
    SpreadConfig spread_config_;
    std::shared_ptr<InstrumentRegistry> registry_;
    SpreadState state_;

    // Bars that have arrived for only one leg so far, keyed by bar timestamp
    struct PendingBar {
        std::optional<double> price_a;
        std::optional<double> price_b;
    };
    std::map<Timestamp, PendingBar> pending_;
    Timestamp last_processed_{};
    bool has_processed_{false};

    /**
     * @brief Buffer an incoming bar and return every newly completed pair in
     *        timestamp order. Bars older than the last processed timestamp are
     *        discarded rather than silently paired out of order.
     */
    std::vector<std::tuple<Timestamp, double, double>> collect_aligned_bars(
        const std::vector<Bar>& data);

    /**
     * @brief Apply the spread transform to a raw price.
     * @return std::nullopt if the price is unusable (non-positive under a log transform)
     */
    std::optional<double> transform_price(double price) const;

    /**
     * @brief Rolling OLS hedge ratio, cov(a,b)/var(b), over the trailing
     *        beta_period observations of the transformed series.
     * @return std::nullopt when the window is not full or var(b) underflows
     */
    std::optional<double> estimate_beta() const;

    /**
     * @brief Rolling mean and standard deviation of the trailing zscore_period
     *        spread observations (sample standard deviation).
     */
    std::optional<std::pair<double, double>> spread_statistics() const;

    /**
     * @brief Decide the target spread direction for the current z-score.
     * @return +1 long spread, -1 short spread, 0 flat
     */
    int target_direction() const;

    /**
     * @brief Size both legs for a given direction at the current prices.
     */
    std::pair<double, double> compute_leg_quantities(int direction, double price_a,
                                                     double price_b) const;

    /**
     * @brief Publish the current target quantities for both legs.
     */
    Result<void> publish_positions(Timestamp ts, double price_a, double price_b);
};

}  // namespace trade_ngin
