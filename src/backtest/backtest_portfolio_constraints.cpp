#include "trade_ngin/backtest/backtest_portfolio_constraints.hpp"
#include "trade_ngin/core/logger.hpp"
#include <algorithm>
#include <cmath>

namespace trade_ngin {
namespace backtest {

BacktestPortfolioConstraints::BacktestPortfolioConstraints(
    const PortfolioConstraintsConfig& config)
    : config_(config), risk_manager_(nullptr), optimizer_(nullptr) {}

BacktestPortfolioConstraints::BacktestPortfolioConstraints(
    const PortfolioConstraintsConfig& config,
    std::shared_ptr<RiskManager> risk_manager,
    std::shared_ptr<DynamicOptimizer> optimizer)
    : config_(config),
      risk_manager_(std::move(risk_manager)),
      optimizer_(std::move(optimizer)) {}

Result<void> BacktestPortfolioConstraints::apply_constraints(
    const std::vector<Bar>& bars,
    std::map<std::string, Position>& current_positions,
    std::vector<RiskResult>& risk_metrics) {
    try {
        // Apply risk management if enabled
        if (is_risk_management_enabled()) {
            auto risk_result = apply_risk_management(bars, current_positions);
            if (risk_result.is_error()) {
                return make_error<void>(risk_result.error()->code(),
                    risk_result.error()->what(), "BacktestPortfolioConstraints");
            }

            risk_metrics.push_back(risk_result.value());

            // Scale positions if risk limits exceeded
            if (risk_result.value().risk_exceeded) {
                double scale = risk_result.value().recommended_scale;
                WARN("Risk limits exceeded: scaling positions by " + std::to_string(scale));

                for (auto& [symbol, pos] : current_positions) {
                    pos.quantity = Quantity(static_cast<double>(pos.quantity) * scale);
                }
            }
        }

        // Apply optimization if enabled
        if (is_optimization_enabled() && current_positions.size() > 1) {
            auto opt_result = apply_optimization(current_positions);
            if (opt_result.is_error()) {
                WARN("Optimization failed: " + std::string(opt_result.error()->what()));
                // Don't fail, just warn
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
            std::string("Error applying portfolio constraints: ") + e.what(),
            "BacktestPortfolioConstraints");
    }
}

Result<RiskResult> BacktestPortfolioConstraints::apply_risk_management(
    const std::vector<Bar>& bars,
    const std::map<std::string, Position>& positions) {
    if (!risk_manager_) {
        return make_error<RiskResult>(ErrorCode::INVALID_DATA,
            "Risk manager not configured", "BacktestPortfolioConstraints");
    }

    MarketData market_data = risk_manager_->create_market_data(bars);

    // Convert map to unordered_map for risk manager compatibility
    std::unordered_map<std::string, Position> positions_for_risk(
        positions.begin(), positions.end());

    auto risk_result = risk_manager_->process_positions(positions_for_risk, market_data);
    if (risk_result.is_error()) {
        return make_error<RiskResult>(risk_result.error()->code(),
            risk_result.error()->what(), "BacktestPortfolioConstraints");
    }

    return risk_result;
}

Result<void> BacktestPortfolioConstraints::apply_optimization(
    std::map<std::string, Position>& current_positions) {
    if (!optimizer_) {
        return make_error<void>(ErrorCode::INVALID_DATA,
            "Optimizer not configured", "BacktestPortfolioConstraints");
    }

    // Prepare inputs for optimization
    std::vector<std::string> symbols;
    std::vector<double> current_pos, target_pos, costs, weights;

    for (const auto& [symbol, pos] : current_positions) {
        symbols.push_back(symbol);
        current_pos.push_back(static_cast<double>(pos.quantity));
        target_pos.push_back(static_cast<double>(pos.quantity));
        // Costs are handled by TransactionCostManager; keep optimizer penalty neutral.
        costs.push_back(0.0);
        weights.push_back(1.0);  // Equal weights
    }

    // Calculate covariance matrix
    auto covariance = calculate_covariance_matrix(symbols);

    // If covariance calculation failed, use default diagonal
    if (covariance.empty() || covariance.size() != symbols.size()) {
        WARN("Covariance calculation failed, using default diagonal matrix");
        covariance.resize(symbols.size(), std::vector<double>(symbols.size(), 0.0));
        for (size_t i = 0; i < symbols.size(); ++i) {
            covariance[i][i] = config_.default_variance;
        }
    }

    // Run optimization
    auto opt_result = optimizer_->optimize(current_pos, target_pos, costs, weights, covariance);

    if (opt_result.is_error()) {
        return make_error<void>(opt_result.error()->code(),
            opt_result.error()->what(), "BacktestPortfolioConstraints");
    }

    // Apply optimized positions
    const auto& optimized = opt_result.value().positions;
    for (size_t i = 0; i < symbols.size(); ++i) {
        current_positions[symbols[i]].quantity = Quantity(optimized[i]);
    }

    DEBUG("Positions optimized with tracking error: " +
          std::to_string(opt_result.value().tracking_error));

    return Result<void>();
}

void BacktestPortfolioConstraints::update_historical_returns(const std::vector<Bar>& bars) {
    if (bars.empty()) return;

    // Update price history for each symbol
    for (const auto& bar : bars) {
        price_history_[bar.symbol].push_back(static_cast<double>(bar.close));

        // Limit history length
        if (price_history_[bar.symbol].size() > config_.max_history_length) {
            price_history_[bar.symbol].erase(
                price_history_[bar.symbol].begin(),
                price_history_[bar.symbol].begin() +
                    (price_history_[bar.symbol].size() - config_.max_history_length));
        }
    }

    // Calculate returns for each symbol that has price history
    for (const auto& [symbol, prices] : price_history_) {
        if (prices.size() < 2) continue;

        // Clear previous returns
        historical_returns_[symbol].clear();

        // Calculate returns
        for (size_t i = 1; i < prices.size(); ++i) {
            double prev_price = prices[i - 1];
            double curr_price = prices[i];

            if (prev_price <= 0.0) continue;

            double ret = (curr_price - prev_price) / prev_price;
            if (std::isfinite(ret)) {
                historical_returns_[symbol].push_back(ret);
            }
        }

        // Limit history length
        if (historical_returns_[symbol].size() > config_.max_history_length) {
            size_t excess = historical_returns_[symbol].size() - config_.max_history_length;
            historical_returns_[symbol].erase(
                historical_returns_[symbol].begin(),
                historical_returns_[symbol].begin() + excess);
        }
    }
}

std::vector<std::vector<double>> BacktestPortfolioConstraints::calculate_covariance_matrix(
    const std::vector<std::string>& symbols) const {
    size_t num_assets = symbols.size();

    if (num_assets == 0) {
        return std::vector<std::vector<double>>();
    }

    // Get returns for these symbols
    auto returns_map = get_returns_for_symbols(symbols);

    // Find minimum length of return series
    size_t min_periods = SIZE_MAX;
    for (const auto& symbol : symbols) {
        auto it = returns_map.find(symbol);
        if (it == returns_map.end() || it->second.empty()) continue;
        min_periods = std::min(min_periods, it->second.size());
    }

    if (min_periods == SIZE_MAX) min_periods = 0;

    // Need sufficient data
    if (min_periods < config_.min_periods_for_covariance) {
        std::vector<std::vector<double>> default_cov(num_assets,
            std::vector<double>(num_assets, 0.0));
        for (size_t i = 0; i < num_assets; ++i) {
            default_cov[i][i] = config_.default_variance;
        }
        return default_cov;
    }

    // Create aligned returns matrix
    std::vector<std::vector<double>> aligned_returns(min_periods,
        std::vector<double>(num_assets, 0.0));

    for (size_t i = 0; i < num_assets; ++i) {
        auto it = returns_map.find(symbols[i]);
        if (it == returns_map.end()) continue;

        const auto& returns = it->second;
        size_t start_idx = returns.size() - min_periods;
        for (size_t j = 0; j < min_periods; ++j) {
            aligned_returns[j][i] = returns[start_idx + j];
        }
    }

    // Calculate means
    std::vector<double> means(num_assets, 0.0);
    for (size_t i = 0; i < num_assets; ++i) {
        for (size_t t = 0; t < min_periods; ++t) {
            means[i] += aligned_returns[t][i];
        }
        means[i] /= min_periods;
    }

    // Calculate covariance matrix
    std::vector<std::vector<double>> covariance(num_assets,
        std::vector<double>(num_assets, 0.0));

    double divisor = (min_periods > 1) ? (min_periods - 1) : 1.0;

    for (size_t i = 0; i < num_assets; ++i) {
        for (size_t j = 0; j < num_assets; ++j) {
            double cov_sum = 0.0;
            for (size_t t = 0; t < min_periods; ++t) {
                cov_sum += (aligned_returns[t][i] - means[i]) *
                          (aligned_returns[t][j] - means[j]);
            }
            covariance[i][j] = cov_sum / divisor;
        }
    }

    return covariance;
}

void BacktestPortfolioConstraints::reset() {
    price_history_.clear();
    historical_returns_.clear();
}

size_t BacktestPortfolioConstraints::get_history_length(const std::string& symbol) const {
    auto it = historical_returns_.find(symbol);
    if (it == historical_returns_.end()) {
        return 0;
    }
    return it->second.size();
}

std::unordered_map<std::string, std::vector<double>>
BacktestPortfolioConstraints::get_returns_for_symbols(
    const std::vector<std::string>& symbols) const {
    std::unordered_map<std::string, std::vector<double>> result;
    for (const auto& symbol : symbols) {
        auto it = historical_returns_.find(symbol);
        if (it != historical_returns_.end()) {
            result[symbol] = it->second;
        }
    }
    return result;
}

} // namespace backtest
} // namespace trade_ngin
