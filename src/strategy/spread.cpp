// src/strategy/spread.cpp
#include "trade_ngin/strategy/spread.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

namespace {
constexpr double kEpsilon = 1e-12;

std::string direction_name(int direction) {
    if (direction > 0) return "LONG_SPREAD";
    if (direction < 0) return "SHORT_SPREAD";
    return "FLAT";
}
}  // namespace

SpreadStrategy::SpreadStrategy(std::string id, StrategyConfig config, SpreadConfig spread_config,
                               std::shared_ptr<PostgresDatabase> db,
                               std::shared_ptr<InstrumentRegistry> registry)
    : BaseStrategy(std::move(id), std::move(config), std::move(db)),
      spread_config_(std::move(spread_config)),
      registry_(std::move(registry)) {
    state_.beta = spread_config_.initial_beta;

    // Both legs are cash instruments: PnL only becomes real when the pair is unwound.
    set_pnl_accounting_method(PnLAccountingMethod::UNREALIZED_ONLY);
}

Result<void> SpreadStrategy::initialize() {
    auto base_result = BaseStrategy::initialize();
    if (base_result.is_error()) {
        return base_result;
    }

    auto validation_result = validate_config();
    if (validation_result.is_error()) {
        return validation_result;
    }

    state_ = SpreadState();
    state_.beta = spread_config_.initial_beta;
    pending_.clear();
    has_processed_ = false;

    // Seed both legs flat so downstream consumers see the pair from the first bar.
    for (const auto& symbol : {spread_config_.symbol_a, spread_config_.symbol_b}) {
        Position pos;
        pos.symbol = symbol;
        pos.quantity = Decimal(0.0);
        pos.average_price = Price(0.0);
        pos.last_update = std::chrono::system_clock::now();
        positions_[symbol] = pos;
    }

    INFO("Spread Strategy initialized successfully");
    INFO("  Pair: " + spread_config_.symbol_a + " / " + spread_config_.symbol_b);
    INFO("  Spread type: " + std::string(spread_config_.spread_type == SpreadType::LOG_RATIO
                                             ? "LOG_RATIO"
                                             : "PRICE_DIFF"));
    INFO("  Beta period: " + std::to_string(spread_config_.beta_period));
    INFO("  Z-score period: " + std::to_string(spread_config_.zscore_period));
    INFO("  Entry / exit / stop z: " + std::to_string(spread_config_.entry_z) + " / " +
         std::to_string(spread_config_.exit_z) + " / " + std::to_string(spread_config_.stop_z));
    INFO("  Notional per leg: " + std::to_string(spread_config_.capital_per_leg_pct * 100.0) + "%");

    return Result<void>();
}

Result<void> SpreadStrategy::validate_config() const {
    if (spread_config_.symbol_a.empty() || spread_config_.symbol_b.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Both spread legs must be specified", "SpreadStrategy");
    }

    if (spread_config_.symbol_a == spread_config_.symbol_b) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Spread legs must be two different symbols", "SpreadStrategy");
    }

    if (spread_config_.beta_period < 2) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Beta period must be at least 2", "SpreadStrategy");
    }

    if (spread_config_.zscore_period < 2) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Z-score period must be at least 2", "SpreadStrategy");
    }

    if (spread_config_.entry_z <= spread_config_.exit_z) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Entry z-score must be greater than exit z-score",
                                "SpreadStrategy");
    }

    if (spread_config_.stop_z <= spread_config_.entry_z) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Stop z-score must be greater than entry z-score",
                                "SpreadStrategy");
    }

    if (spread_config_.capital_per_leg_pct <= 0.0 || spread_config_.capital_per_leg_pct > 1.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Capital per leg must be between 0 and 1", "SpreadStrategy");
    }

    if (spread_config_.min_holding_period < 0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Minimum holding period cannot be negative", "SpreadStrategy");
    }

    if (config_.capital_allocation <= 0.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Capital allocation must be positive", "SpreadStrategy");
    }

    return Result<void>();
}

std::optional<double> SpreadStrategy::transform_price(double price) const {
    if (!std::isfinite(price)) {
        return std::nullopt;
    }

    if (spread_config_.spread_type == SpreadType::LOG_RATIO) {
        if (price <= 0.0) {
            return std::nullopt;
        }
        return std::log(price);
    }

    return price;
}

std::vector<std::tuple<Timestamp, double, double>> SpreadStrategy::collect_aligned_bars(
    const std::vector<Bar>& data) {
    for (const auto& bar : data) {
        const bool is_a = bar.symbol == spread_config_.symbol_a;
        const bool is_b = bar.symbol == spread_config_.symbol_b;
        if (!is_a && !is_b) {
            continue;  // Not our pair
        }

        // A bar at or before the last pair we acted on would pair legs out of
        // order, so drop it rather than let it shift the spread series.
        if (has_processed_ && bar.timestamp <= last_processed_) {
            WARN("Discarding stale bar for " + bar.symbol + " at or before last processed pair");
            continue;
        }

        auto& slot = pending_[bar.timestamp];
        if (is_a) {
            slot.price_a = bar.close.as_double();
        } else {
            slot.price_b = bar.close.as_double();
        }
    }

    std::vector<std::tuple<Timestamp, double, double>> aligned;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.price_a.has_value() && it->second.price_b.has_value()) {
            aligned.emplace_back(it->first, *it->second.price_a, *it->second.price_b);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }

    // Drop half-filled timestamps that are now older than a completed pair: one
    // leg never reported, so they can never align.
    if (!aligned.empty()) {
        const Timestamp newest = std::get<0>(aligned.back());
        for (auto it = pending_.begin(); it != pending_.end();) {
            it = (it->first <= newest) ? pending_.erase(it) : std::next(it);
        }
    }

    // Bound the buffer if one leg stops reporting entirely.
    while (pending_.size() > spread_config_.max_history) {
        pending_.erase(pending_.begin());
    }

    return aligned;
}

std::optional<double> SpreadStrategy::estimate_beta() const {
    const size_t period = static_cast<size_t>(spread_config_.beta_period);
    if (state_.a_history.size() < period || state_.b_history.size() < period) {
        return std::nullopt;
    }

    const size_t offset = state_.a_history.size() - period;
    const double n = static_cast<double>(period);

    double sum_a = 0.0;
    double sum_b = 0.0;
    for (size_t i = offset; i < state_.a_history.size(); ++i) {
        sum_a += state_.a_history[i];
        sum_b += state_.b_history[i];
    }
    const double mean_a = sum_a / n;
    const double mean_b = sum_b / n;

    double covariance = 0.0;
    double variance_b = 0.0;
    for (size_t i = offset; i < state_.a_history.size(); ++i) {
        const double da = state_.a_history[i] - mean_a;
        const double db = state_.b_history[i] - mean_b;
        covariance += da * db;
        variance_b += db * db;
    }

    if (variance_b < kEpsilon) {
        return std::nullopt;  // Hedge leg is flat over the window; keep the previous beta
    }

    const double beta = covariance / variance_b;
    if (!std::isfinite(beta)) {
        return std::nullopt;
    }

    return beta;
}

std::optional<std::pair<double, double>> SpreadStrategy::spread_statistics() const {
    const size_t period = static_cast<size_t>(spread_config_.zscore_period);
    if (state_.spread_history.size() < period) {
        return std::nullopt;
    }

    const size_t offset = state_.spread_history.size() - period;
    const double n = static_cast<double>(period);

    const double mean =
        std::accumulate(state_.spread_history.begin() + static_cast<long>(offset),
                        state_.spread_history.end(), 0.0) /
        n;

    double sum_sq = 0.0;
    for (size_t i = offset; i < state_.spread_history.size(); ++i) {
        const double d = state_.spread_history[i] - mean;
        sum_sq += d * d;
    }

    // Sample standard deviation; period >= 2 is enforced by validate_config().
    const double stddev = std::sqrt(sum_sq / (n - 1.0));
    if (!std::isfinite(stddev) || stddev < kEpsilon) {
        return std::nullopt;
    }

    return std::make_pair(mean, stddev);
}

int SpreadStrategy::target_direction() const {
    const double z = state_.current_zscore;
    const double abs_z = std::abs(z);

    if (state_.direction != 0) {
        // Stop first: a blown-out spread is exited regardless of holding period.
        if (abs_z >= spread_config_.stop_z) {
            return 0;
        }
        if (abs_z <= spread_config_.exit_z &&
            state_.holding_period >= spread_config_.min_holding_period) {
            return 0;
        }
        return state_.direction;  // Hold
    }

    // Flat. After a stop, wait until the spread is back inside the exit band.
    if (state_.stopped_out) {
        return 0;
    }

    // Don't open straight into a stop-out.
    if (abs_z >= spread_config_.stop_z) {
        return 0;
    }

    if (z <= -spread_config_.entry_z) {
        return 1;  // Spread is cheap: long A, short B
    }
    if (z >= spread_config_.entry_z) {
        return -1;  // Spread is rich: short A, long B
    }

    return 0;
}

std::pair<double, double> SpreadStrategy::compute_leg_quantities(int direction, double price_a,
                                                                double price_b) const {
    if (direction == 0 || price_a <= 0.0 || price_b <= 0.0) {
        return {0.0, 0.0};
    }

    const double notional = config_.capital_allocation * spread_config_.capital_per_leg_pct;
    const double shares_a = notional / price_a;

    double shares_b = 0.0;
    if (spread_config_.spread_type == SpreadType::LOG_RATIO) {
        // spread = log(Pa) - beta*log(Pb): hedge in proportional terms, so leg B
        // carries beta times leg A's *notional*.
        shares_b = (state_.beta * notional) / price_b;
    } else {
        // spread = Pa - beta*Pb: hedge in unit terms, so leg B carries beta times
        // leg A's *share count*.
        shares_b = state_.beta * shares_a;
    }

    return {direction * shares_a, -direction * shares_b};
}

Result<void> SpreadStrategy::publish_positions(Timestamp ts, double price_a, double price_b) {
    const std::pair<std::string, double> legs[2] = {
        {spread_config_.symbol_a, state_.quantity_a},
        {spread_config_.symbol_b, state_.quantity_b}};
    const double prices[2] = {price_a, price_b};
    const double entries[2] = {state_.entry_price_a, state_.entry_price_b};

    for (int i = 0; i < 2; ++i) {
        Position pos;
        pos.symbol = legs[i].first;
        pos.quantity = Decimal(legs[i].second);
        pos.average_price = Price(legs[i].second == 0.0 ? 0.0 : entries[i]);
        pos.last_update = ts;
        pos.unrealized_pnl =
            Decimal(legs[i].second == 0.0 ? 0.0 : legs[i].second * (prices[i] - entries[i]));

        auto result = update_position(legs[i].first, pos);
        if (result.is_error()) {
            return result;
        }
    }

    return Result<void>();
}

Result<void> SpreadStrategy::on_data(const std::vector<Bar>& data) {
    if (get_state() != StrategyState::RUNNING) {
        return Result<void>();
    }

    try {
        // Note: mutex_ is deliberately not held here. update_position() takes it,
        // and mutex_ is not recursive.
        for (const auto& [ts, price_a, price_b] : collect_aligned_bars(data)) {
            auto xa = transform_price(price_a);
            auto xb = transform_price(price_b);
            if (!xa.has_value() || !xb.has_value()) {
                WARN("Skipping unusable prices for " + spread_config_.symbol_a + "/" +
                     spread_config_.symbol_b);
                continue;
            }

            last_processed_ = ts;
            has_processed_ = true;

            state_.a_history.push_back(*xa);
            state_.b_history.push_back(*xb);
            state_.last_price_a = price_a;
            state_.last_price_b = price_b;
            state_.last_update = ts;
            state_.observations++;

            if (state_.a_history.size() > spread_config_.max_history) {
                state_.a_history.erase(state_.a_history.begin());
                state_.b_history.erase(state_.b_history.begin());
            }

            // Rolling OLS hedge ratio over data up to and including this bar.
            if (auto beta = estimate_beta(); beta.has_value()) {
                state_.beta = *beta;
            }

            state_.current_spread = *xa - state_.beta * (*xb);
            state_.spread_history.push_back(state_.current_spread);
            if (state_.spread_history.size() > spread_config_.max_history) {
                state_.spread_history.erase(state_.spread_history.begin());
            }

            auto stats = spread_statistics();
            if (!stats.has_value()) {
                state_.zscore_valid = false;
                continue;  // Still warming up, or a degenerate spread window
            }

            state_.spread_mean = stats->first;
            state_.spread_std = stats->second;
            state_.current_zscore = (state_.current_spread - state_.spread_mean) / state_.spread_std;
            state_.zscore_valid = true;

            if (state_.direction != 0) {
                state_.holding_period++;
            }

            const int previous_direction = state_.direction;
            const int desired = target_direction();

            if (desired == previous_direction) {
                if (previous_direction != 0) {
                    // Mark the open pair to market at today's close.
                    auto result = publish_positions(ts, price_a, price_b);
                    if (result.is_error()) {
                        return result;
                    }
                }
                continue;
            }

            if (desired == 0) {
                const bool was_stop = std::abs(state_.current_zscore) >= spread_config_.stop_z;
                const double pnl =
                    state_.quantity_a * (price_a - state_.entry_price_a) +
                    state_.quantity_b * (price_b - state_.entry_price_b);

                DEBUG("Closing " + direction_name(previous_direction) + " at z=" +
                      std::to_string(state_.current_zscore) + " after " +
                      std::to_string(state_.holding_period) + " bars, leg PnL " +
                      std::to_string(pnl) + (was_stop ? " (stop)" : ""));

                state_.direction = 0;
                state_.quantity_a = 0.0;
                state_.quantity_b = 0.0;
                state_.entry_price_a = 0.0;
                state_.entry_price_b = 0.0;
                state_.entry_zscore = 0.0;
                state_.holding_period = 0;
                state_.stopped_out = was_stop;
            } else {
                auto [qty_a, qty_b] = compute_leg_quantities(desired, price_a, price_b);
                if (qty_a == 0.0 && qty_b == 0.0) {
                    continue;  // Unusable sizing; stay flat
                }

                state_.direction = desired;
                state_.quantity_a = qty_a;
                state_.quantity_b = qty_b;
                state_.entry_price_a = price_a;
                state_.entry_price_b = price_b;
                state_.entry_zscore = state_.current_zscore;
                state_.holding_period = 0;

                DEBUG("Opening " + direction_name(desired) + " at z=" +
                      std::to_string(state_.current_zscore) + ", beta=" +
                      std::to_string(state_.beta) + ", qty " + spread_config_.symbol_a + "=" +
                      std::to_string(qty_a) + ", qty " + spread_config_.symbol_b + "=" +
                      std::to_string(qty_b));
            }

            auto result = publish_positions(ts, price_a, price_b);
            if (result.is_error()) {
                return result;
            }
        }

        // Clear the stop block once the spread is back inside the exit band.
        if (state_.stopped_out && state_.zscore_valid &&
            std::abs(state_.current_zscore) <= spread_config_.exit_z) {
            state_.stopped_out = false;
        }

        auto metrics_result = update_metrics();
        if (metrics_result.is_error()) {
            WARN("Failed to update metrics: " + std::string(metrics_result.error()->what()));
        }

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error processing data in SpreadStrategy: " + std::string(e.what()));
        return make_error<void>(ErrorCode::STRATEGY_ERROR,
                                std::string("Error processing data: ") + e.what(),
                                "SpreadStrategy");
    }
}

Result<void> SpreadStrategy::on_execution(const ExecutionReport& report) {
    // PnL is marked in on_data(); only the trade count is tracked here, matching
    // TrendFollowingStrategy.
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.total_trades++;

    DEBUG("Execution received for " + report.symbol + ": " +
          (report.side == Side::BUY ? "BUY" : "SELL") + " " +
          std::to_string(report.filled_quantity.as_double()) + " @ " +
          std::to_string(report.fill_price.as_double()));

    return Result<void>();
}

std::unordered_map<std::string, std::vector<double>> SpreadStrategy::get_price_history() const {
    std::unordered_map<std::string, std::vector<double>> history;

    if (spread_config_.spread_type == SpreadType::LOG_RATIO) {
        // Report raw prices, not the log transform used internally.
        std::vector<double> a;
        std::vector<double> b;
        a.reserve(state_.a_history.size());
        b.reserve(state_.b_history.size());
        for (double v : state_.a_history) a.push_back(std::exp(v));
        for (double v : state_.b_history) b.push_back(std::exp(v));
        history[spread_config_.symbol_a] = std::move(a);
        history[spread_config_.symbol_b] = std::move(b);
    } else {
        history[spread_config_.symbol_a] = state_.a_history;
        history[spread_config_.symbol_b] = state_.b_history;
    }

    return history;
}

}  // namespace trade_ngin
