#include "trade_ngin/risk/risk_manager.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"

namespace trade_ngin {

RiskManager::RiskManager(RiskConfig config) : config_(std::move(config)) {
    Logger::register_component("RiskManager");
}

Result<RiskResult> RiskManager::process_positions(
    const std::unordered_map<std::string, Position>& positions, 
    const MarketData& market_data,
    const std::unordered_map<std::string, double>& current_prices) {
    try {
        RiskResult result;

        // Initialize with default values
        result.risk_exceeded = false;
        result.recommended_scale = 1.0;
        result.portfolio_multiplier = 1.0;
        result.jump_multiplier = 1.0;
        result.correlation_multiplier = 1.0;
        result.leverage_multiplier = 1.0;

        if (positions.empty()) {
            WARN("RiskManager: No positions provided for risk calculation");
            return Result<RiskResult>(result);  // Return default result
        }

        // Check if we have market data available - RETURN EARLY if not
        if (market_data.returns.empty() || market_data.covariance.empty() ||
            market_data.symbol_indices.empty() || market_data.ordered_symbols.empty()) {
            WARN("RiskManager: Market data not available, returning default result");
            return Result<RiskResult>(result);  // Return default instead of error
        }

        // Map positions to ordered indices for risk calculations
        std::vector<double> position_values;
        std::vector<std::string> position_symbols;
        double total_value = 0.0;

        // Create vectors that match the order in market_data_ structures
        position_values.resize(market_data.ordered_symbols.size(), 0.0);
        // Parallel values WITHOUT contract multipliers for volatility-only weighting
        std::vector<double> position_values_no_multiplier;
        position_values_no_multiplier.resize(market_data.ordered_symbols.size(), 0.0);

        for (const auto& [symbol, pos] : positions) {
            // Only include positions with symbols in our market data
            auto it = market_data.symbol_indices.find(symbol);
            if (it != market_data.symbol_indices.end()) {
                size_t index = it->second;
                if (index < position_values.size()) {
                    // Calculate position values for leverage
                    // For backtest: use average price (original logic)
                    // For live: use current price if available
                    double price_for_leverage = static_cast<double>(pos.average_price);

                    if (!current_prices.empty()) {
                        // Live trading: use current market price if available
                        auto price_it = current_prices.find(symbol);
                        if (price_it != current_prices.end()) {
                            price_for_leverage = price_it->second;
                        }
                    }

                    // Get contract multiplier from InstrumentRegistry for proper notional calculation
                    double contract_multiplier = 1.0;
                    try {
                        auto& registry = InstrumentRegistry::instance();
                        // Normalize variant-suffixed symbols for lookup (e.g., 6B.v.0 -> 6B)
                        std::string lookup_sym = symbol;
                        auto dotpos = lookup_sym.find(".v.");
                        if (dotpos != std::string::npos) {
                            lookup_sym = lookup_sym.substr(0, dotpos);
                        }
                        dotpos = lookup_sym.find(".c.");
                        if (dotpos != std::string::npos) {
                            lookup_sym = lookup_sym.substr(0, dotpos);
                        }
                        auto instrument = registry.get_instrument(lookup_sym);
                        if (instrument) {
                            contract_multiplier = instrument->get_multiplier();
                        }
                    } catch (...) {
                        // Use default multiplier if exception occurs
                    }

                    double signed_quantity = static_cast<double>(pos.quantity);
                    double position_value = signed_quantity * price_for_leverage * contract_multiplier;
                    position_values[index] = position_value;
                    total_value += std::abs(position_value);
                    position_symbols.push_back(symbol);

                    // Also capture position value WITHOUT multiplier (signed) for volatility weights
                    double position_value_no_mult = signed_quantity * price_for_leverage;
                    position_values_no_multiplier[index] = position_value_no_mult;
                }
            }
        }

        if (position_symbols.empty()) {
            WARN("No positions mapped to market data symbols, skipping risk calculation");
            return Result<RiskResult>(result);  // Return default result
        }

        // Calculate position weights (with multipliers) for general risk calcs
        std::vector<double> weights;
        weights.resize(position_values.size(), 0.0);
        // Volatility-specific weights (WITHOUT multipliers), signed numerators
        std::vector<double> vol_weights;
        vol_weights.resize(position_values.size(), 0.0);
        double total_value_no_multiplier_abs = 0.0;
        for (double v : position_values_no_multiplier) {
            total_value_no_multiplier_abs += std::abs(v);
        }

        if (total_value > 0.0) {
            for (size_t i = 0; i < position_values.size(); ++i) {
                weights[i] = position_values[i] / total_value;
            }
        }
        if (total_value_no_multiplier_abs > 0.0) {
            for (size_t i = 0; i < position_values.size(); ++i) {
                vol_weights[i] = position_values_no_multiplier[i] / total_value_no_multiplier_abs;
            }
        }

        // Calculate all risk multipliers and store metrics
        result.portfolio_multiplier = calculate_portfolio_multiplier(market_data, weights, result);
        result.jump_multiplier = calculate_jump_multiplier(market_data, weights, result);
        result.correlation_multiplier =
            calculate_correlation_multiplier(market_data, weights, result);
        result.leverage_multiplier =
            calculate_leverage_multiplier(market_data, weights, position_values, total_value, result);

        // Recompute portfolio_var for reporting using volatility-only weights (WITHOUT multipliers)
        if (!market_data.covariance.empty() && !vol_weights.empty()) {
            double variance = 0.0;
            for (size_t i = 0; i < vol_weights.size(); ++i) {
                for (size_t j = 0; j < vol_weights.size(); ++j) {
                    variance += vol_weights[i] * market_data.covariance[i][j] * vol_weights[j];
                }
            }
            result.portfolio_var = variance > 0.0 ? std::sqrt(variance) : 0.0;
        }

        // Overall scale is minimum of all multipliers
        result.recommended_scale =
            std::min({result.portfolio_multiplier, result.jump_multiplier,
                      result.correlation_multiplier, result.leverage_multiplier});

        result.risk_exceeded = result.recommended_scale < 1.0;

        // DEBUG: Log which constraint is binding when positions are heavily scaled
        if (result.recommended_scale < 0.5) {
            INFO("RISK_DEBUG: Heavy scaling detected! scale=" + std::to_string(result.recommended_scale));
            INFO("RISK_DEBUG:   portfolio_mult=" + std::to_string(result.portfolio_multiplier) +
                 " (VaR=" + std::to_string(result.portfolio_var) + ", limit=" + std::to_string(config_.var_limit) + ")");
            INFO("RISK_DEBUG:   jump_mult=" + std::to_string(result.jump_multiplier) +
                 " (jump_risk=" + std::to_string(result.jump_risk) + ", limit=" + std::to_string(config_.jump_risk_limit) + ")");
            INFO("RISK_DEBUG:   corr_mult=" + std::to_string(result.correlation_multiplier) +
                 " (max_corr=" + std::to_string(result.correlation_risk) + ", limit=" + std::to_string(config_.max_correlation) + ")");
            INFO("RISK_DEBUG:   lev_mult=" + std::to_string(result.leverage_multiplier) +
                 " (gross=" + std::to_string(result.gross_leverage) + ", net=" + std::to_string(result.net_leverage) + ")");
        }

        return Result<RiskResult>(result);

    } catch (const std::exception& e) {
        ERROR("RiskManager: Risk calculation failed: " + std::string(e.what()));
        return make_error<RiskResult>(ErrorCode::INVALID_RISK_CALCULATION,
                                      std::string("Risk calculation failed: ") + e.what(),
                                      "RiskManager");
    }
}

std::vector<double> RiskManager::calculate_weights(
    const std::unordered_map<std::string, Position>& positions) const {
    std::vector<double> weights;
    double total_value = 0.0;

    // Calculate total portfolio value
    for (const auto& [symbol, pos] : positions) {
        total_value +=
            std::abs(static_cast<double>(pos.quantity) * static_cast<double>(pos.average_price));
    }

    // Calculate position weights
    if (total_value > 0.0) {
        for (const auto& [symbol, pos] : positions) {
            weights.push_back(
                (static_cast<double>(pos.quantity) * static_cast<double>(pos.average_price)) /
                total_value);
        }
    } else {
        weights.resize(positions.size(), 0.0);
    }

    return weights;
}

double RiskManager::calculate_portfolio_multiplier(const MarketData& market_data,
                                                   const std::vector<double>& weights,
                                                   RiskResult& result) const {
    if (market_data.covariance.empty() || weights.empty()) {
        result.portfolio_var = 0.0;
        return 1.0;
    }

    const size_t n = weights.size();

    // --- Eigen Vectorized Variance Calculation ---
    // Convert weights to Eigen vector
    Eigen::VectorXd w(n);
    for (size_t i = 0; i < n; ++i) {
        w(i) = weights[i];
    }

    // Convert covariance to Eigen matrix
    Eigen::MatrixXd cov(n, n);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            cov(i, j) = market_data.covariance[i][j];
        }
    }

    // Calculate portfolio variance: w' * Cov * w (single vectorized operation)
    double variance = w.transpose() * cov * w;
    result.portfolio_var = std::sqrt(std::max(0.0, variance));

    if (result.portfolio_var <= 0.0) {
        return 1.0;
    }

    // Calculate historical VaR using Eigen dot product
    std::vector<double> historical_var;
    historical_var.reserve(market_data.returns.size());

    for (const auto& daily_returns : market_data.returns) {
        Eigen::VectorXd ret(n);
        for (size_t i = 0; i < n; ++i) {
            ret(i) = daily_returns[i];
        }
        double port_return = w.dot(ret);
        historical_var.push_back(std::abs(port_return));
    }

    result.max_portfolio_risk = calculate_99th_percentile(historical_var);
    return std::min(1.0, config_.var_limit / result.portfolio_var);
}

double RiskManager::calculate_jump_multiplier(const MarketData& market_data,
                                              const std::vector<double>& weights,
                                              RiskResult& result) const {
    // Carver's jump risk multiplier (Advanced Futures Trading Strategies, p.607-608):
    //   1. For each instrument, compute the 99th-percentile of historical rolling-window
    //      standard deviations (the "shocked" stdev = worst-case vol regime per instrument).
    //   2. Build covariance matrix Σ_jump using shocked stdevs on diagonal but original
    //      correlations on off-diagonals.
    //   3. Compute portfolio std under jump scenario: σ_jump = sqrt(w' × Σ_jump × w)
    //   4. If σ_jump > jump_shock_threshold (default 0.75 = 3.75 × τ for τ=0.20),
    //      scale by threshold / σ_jump.
    if (weights.empty() || market_data.covariance.empty() || market_data.returns.empty()) {
        result.jump_risk = 0.0;
        return 1.0;
    }

    const size_t n = std::min(weights.size(), market_data.covariance.size());
    if (n < 1) {
        result.jump_risk = 0.0;
        return 1.0;
    }

    try {
        // Current per-instrument stdevs (annualized) from covariance diagonal
        std::vector<double> current_stdevs(n);
        for (size_t i = 0; i < n; ++i) {
            current_stdevs[i] = std::sqrt(std::max(0.0, market_data.covariance[i][i]));
        }

        // Compute per-instrument 99th-percentile rolling stdev (the shocked stdev).
        // Use a 22-day rolling window over historical returns (~1 trading month).
        constexpr size_t window = 22;
        constexpr double trading_days_per_year = 252.0;
        const size_t T = market_data.returns.size();

        std::vector<double> shocked_stdevs(n);
        if (T < window + 1) {
            // Not enough history for a meaningful 99th-percentile shock — fall back to
            // current stdev × Carver's "extremely high" multiplier of 2.5x as a proxy.
            for (size_t i = 0; i < n; ++i) {
                shocked_stdevs[i] = current_stdevs[i] * 2.5;
            }
        } else {
            for (size_t i = 0; i < n; ++i) {
                std::vector<double> rolling_stdevs;
                rolling_stdevs.reserve(T - window);
                for (size_t t = window; t <= T; ++t) {
                    double mean = 0.0;
                    for (size_t k = t - window; k < t; ++k) {
                        if (i < market_data.returns[k].size()) mean += market_data.returns[k][i];
                    }
                    mean /= static_cast<double>(window);
                    double var = 0.0;
                    for (size_t k = t - window; k < t; ++k) {
                        if (i < market_data.returns[k].size()) {
                            double d = market_data.returns[k][i] - mean;
                            var += d * d;
                        }
                    }
                    var /= static_cast<double>(window - 1);
                    rolling_stdevs.push_back(std::sqrt(std::max(0.0, var)) *
                                              std::sqrt(trading_days_per_year));
                }
                if (rolling_stdevs.empty()) {
                    shocked_stdevs[i] = current_stdevs[i] * 2.5;
                    continue;
                }
                std::sort(rolling_stdevs.begin(), rolling_stdevs.end());
                size_t idx = static_cast<size_t>(rolling_stdevs.size() * 0.99);
                if (idx >= rolling_stdevs.size()) idx = rolling_stdevs.size() - 1;
                shocked_stdevs[i] = std::max(rolling_stdevs[idx], current_stdevs[i]);
            }
        }

        // Build Σ_jump: diagonal = shocked_σ_i^2, off-diagonal = shocked_σ_i × shocked_σ_j × ρ_ij_current
        // where ρ_ij_current = covariance[i][j] / (current_σ_i × current_σ_j).
        Eigen::MatrixXd Sigma_jump(n, n);
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) {
                if (i == j) {
                    Sigma_jump(i, j) = shocked_stdevs[i] * shocked_stdevs[i];
                } else {
                    double rho = 0.0;
                    if (current_stdevs[i] > 0 && current_stdevs[j] > 0) {
                        rho = market_data.covariance[i][j] /
                              (current_stdevs[i] * current_stdevs[j]);
                        if (std::isnan(rho) || std::isinf(rho)) rho = 0.0;
                        rho = std::max(-1.0, std::min(1.0, rho));
                    }
                    Sigma_jump(i, j) = shocked_stdevs[i] * shocked_stdevs[j] * rho;
                }
            }
        }

        // Portfolio std under jump scenario
        Eigen::VectorXd w(n);
        for (size_t i = 0; i < n; ++i) {
            w(i) = weights[i];
        }
        double variance_jump = w.transpose() * Sigma_jump * w;
        double risk_jump = std::sqrt(std::max(0.0, variance_jump));

        result.jump_risk = risk_jump;
        result.max_jump_risk = std::max(result.jump_risk, result.max_jump_risk);

        // Scale down if jump portfolio risk exceeds threshold
        if (risk_jump > config_.jump_shock_threshold && risk_jump > 0.0) {
            return config_.jump_shock_threshold / risk_jump;
        }

        return 1.0;
    } catch (const std::exception& e) {
        ERROR("Exception in jump shock calculation: " + std::string(e.what()));
        return 1.0;  // Safe default
    }
}

double RiskManager::calculate_correlation_multiplier(const MarketData& market_data,
                                                     const std::vector<double>& weights,
                                                     RiskResult& result) const {
    // Carver's correlation shock risk multiplier (Advanced Futures Trading Strategies, p.610-614):
    //   1. Build a "shocked" correlation matrix where every off-diagonal entry equals the
    //      99th-percentile of observed pair-wise correlations.
    //   2. Compute portfolio standard deviation under this shocked matrix:
    //        σ_shock = sqrt(w' × Σ_shock × w)
    //   3. If σ_shock > corr_shock_threshold (default 0.65 = 3.25 × τ for τ=0.20),
    //      scale positions by threshold / σ_shock.
    // This replaces the legacy hard pair-wise correlation cap which fired far too often
    // (60%+ vs Carver's design intent of 1-5%).
    if (weights.empty() || market_data.covariance.empty()) {
        result.correlation_risk = 0.0;
        return 1.0;
    }

    const size_t n = std::min(weights.size(), market_data.covariance.size());
    if (n < 2) {
        result.correlation_risk = 0.0;
        return 1.0;
    }

    try {
        // Per-instrument standard deviations from current covariance diagonal
        std::vector<double> stdevs(n);
        for (size_t i = 0; i < n; ++i) {
            stdevs[i] = std::sqrt(std::max(0.0, market_data.covariance[i][i]));
        }

        // Collect absolute pair-wise correlations from the current covariance matrix
        std::vector<double> abs_correlations;
        abs_correlations.reserve(n * (n - 1) / 2);
        for (size_t i = 0; i < n; ++i) {
            if (stdevs[i] <= 0) continue;
            for (size_t j = i + 1; j < n; ++j) {
                if (stdevs[j] <= 0) continue;
                double corr = market_data.covariance[i][j] / (stdevs[i] * stdevs[j]);
                if (std::isnan(corr) || std::isinf(corr)) continue;
                corr = std::max(-1.0, std::min(1.0, corr));
                abs_correlations.push_back(std::abs(corr));
            }
        }

        if (abs_correlations.empty()) {
            result.correlation_risk = 0.0;
            return 1.0;
        }

        // 99th percentile of pair-wise correlations = the shocked correlation value
        std::sort(abs_correlations.begin(), abs_correlations.end());
        size_t idx = static_cast<size_t>(abs_correlations.size() * 0.99);
        if (idx >= abs_correlations.size()) idx = abs_correlations.size() - 1;
        const double shocked_corr = abs_correlations[idx];

        // Build shocked covariance matrix:
        //   diagonal: σ_i^2 (unchanged)
        //   off-diagonal: shocked_corr × σ_i × σ_j (worst-case correlation)
        Eigen::MatrixXd Sigma_shock(n, n);
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) {
                if (i == j) {
                    Sigma_shock(i, j) = stdevs[i] * stdevs[i];
                } else {
                    Sigma_shock(i, j) = shocked_corr * stdevs[i] * stdevs[j];
                }
            }
        }

        // Portfolio standard deviation under the shocked correlation matrix
        Eigen::VectorXd w(n);
        for (size_t i = 0; i < n; ++i) {
            w(i) = weights[i];
        }
        const double variance_shock = w.transpose() * Sigma_shock * w;
        const double risk_shock = std::sqrt(std::max(0.0, variance_shock));

        result.correlation_risk = risk_shock;

        // Scale down if shocked portfolio risk exceeds Carver's threshold
        if (risk_shock > config_.corr_shock_threshold && risk_shock > 0.0) {
            return config_.corr_shock_threshold / risk_shock;
        }

        return 1.0;
    } catch (const std::exception& e) {
        ERROR("Exception in correlation shock calculation: " + std::string(e.what()));
        return 1.0;  // Safe default
    }
}

double RiskManager::calculate_leverage_multiplier(const MarketData& market_data,
                                                  const std::vector<double>& weights,
                                                  const std::vector<double>& position_values,
                                                  double total_value, RiskResult& result) const {
    (void)weights;
    // Calculate gross and net leverage
    double gross = total_value;
    result.gross_leverage = gross / static_cast<double>(config_.capital);

    // Net leverage should be the sum of signed position values (net exposure)
    // Calculate net from the position_values array (which preserves signs)
    double net = 0.0;
    for (size_t i = 0; i < position_values.size(); ++i) {
        net += position_values[i];  // This preserves the sign (long/short)
    }

    result.net_leverage = net / static_cast<double>(config_.capital);  // Preserve sign: positive = net long, negative = net short

    // Historical leverage calculation
    std::vector<double> historical_leverage;
    for (const auto& daily_returns : market_data.returns) {
        double lev = std::accumulate(daily_returns.begin(), daily_returns.end(), 0.0);
        historical_leverage.push_back(std::abs(lev) / static_cast<double>(config_.capital));
    }

    result.max_leverage_risk = calculate_99th_percentile(historical_leverage);

    // Scale down positions if gross or net leverage exceeds limits
    double gross_multiplier = result.gross_leverage > config_.max_gross_leverage
                                  ? config_.max_gross_leverage / result.gross_leverage
                                  : 1.0;
    double net_multiplier = result.net_leverage > config_.max_net_leverage
                                ? config_.max_net_leverage / result.net_leverage
                                : 1.0;

    return std::min({1.0, gross_multiplier, net_multiplier});
}

Result<void> RiskManager::update_config(const RiskConfig& config) {
    // Validate configuration parameters
    if (config.capital <= 0.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Capital must be positive");
    }

    if (config.confidence_level <= 0.0 || config.confidence_level >= 1.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Confidence level must be between 0 and 1");
    }

    if (config.var_limit <= 0.0 || config.jump_risk_limit <= 0.0 || config.max_correlation <= 0.0 ||
        config.max_gross_leverage <= 0.0 || config.max_net_leverage <= 0.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "All risk limits must be positive");
    }

    config_ = config;
    return Result<void>();
}

MarketData RiskManager::create_market_data(const std::vector<Bar>& data) {
    MarketData market_data;

    // Collect Unique Symbols and Order Them
    std::set<std::string> unique_symbols;
    for (const auto& bar : data) {
        unique_symbols.insert(bar.symbol);
    }
    market_data.ordered_symbols =
        std::vector<std::string>(unique_symbols.begin(), unique_symbols.end());
    std::sort(market_data.ordered_symbols.begin(), market_data.ordered_symbols.end());

    // Create Symbol Indices Mapping
    for (size_t i = 0; i < market_data.ordered_symbols.size(); ++i) {
        market_data.symbol_indices[market_data.ordered_symbols[i]] = i;
    }

    // Organize Prices by Symbol and Timestamp
    std::map<std::string, std::map<Timestamp, double>> prices_by_symbol;
    for (const auto& bar : data) {
        prices_by_symbol[bar.symbol][bar.timestamp] = static_cast<double>(bar.close);
    }

    // Calculate Returns
    std::map<Timestamp, std::map<std::string, double>> prices_by_time;
    for (const auto& [symbol, prices] : prices_by_symbol) {
        for (const auto& [ts, price] : prices) {
            prices_by_time[ts][symbol] = price;
        }
    }

    market_data.returns.clear();
    auto it = prices_by_time.begin();
    if (it != prices_by_time.end()) {
        auto prev = it++;
        while (it != prices_by_time.end()) {
            std::vector<double> daily_returns(market_data.ordered_symbols.size(), 0.0);
            for (size_t i = 0; i < market_data.ordered_symbols.size(); ++i) {
                const std::string& symbol = market_data.ordered_symbols[i];
                if (it->second.count(symbol) && prev->second.count(symbol)) {
                    daily_returns[i] =
                        (it->second.at(symbol) - prev->second.at(symbol)) / prev->second.at(symbol);
                }
            }
            market_data.returns.push_back(daily_returns);
            prev = it++;
        }
    }

    // Calculate Covariance Matrix
    market_data.covariance.clear();
    if (!market_data.returns.empty()) {
        size_t num_assets = market_data.ordered_symbols.size();
        size_t num_periods = market_data.returns.size();

        // Calculate means
        std::vector<double> means(num_assets, 0.0);
        for (const auto& daily_returns : market_data.returns) {
            for (size_t i = 0; i < num_assets; ++i) {
                means[i] += daily_returns[i];
            }
        }
        for (auto& mean : means) {
            mean /= num_periods;
        }

        // Calculate covariance
        market_data.covariance.resize(num_assets, std::vector<double>(num_assets, 0.0));
        for (size_t i = 0; i < num_assets; ++i) {
            for (size_t j = 0; j < num_assets; ++j) {
                for (size_t k = 0; k < num_periods; ++k) {
                    market_data.covariance[i][j] += (market_data.returns[k][i] - means[i]) *
                                                    (market_data.returns[k][j] - means[j]);
                }
                market_data.covariance[i][j] /= (num_periods - 1);
                market_data.covariance[i][j] *= 252.0;  // Annualize
            }
        }
    }

    return market_data;
}

std::vector<std::vector<double>> RiskManager::calculate_returns(
    const std::vector<Bar>& data) const {
    std::map<std::string, std::map<Timestamp, double>> prices_by_symbol;

    // Organize data by symbol and timestamp
    for (const auto& bar : data) {
        prices_by_symbol[bar.symbol][bar.timestamp] = static_cast<double>(bar.close);
    }

    // Convert to chronologically ordered timeseries
    std::map<Timestamp, std::map<std::string, double>> prices_by_time;
    for (const auto& [symbol, prices] : prices_by_symbol) {
        for (const auto& [ts, price] : prices) {
            prices_by_time[ts][symbol] = price;
        }
    }

    // Calculate returns between consecutive timestamps
    std::vector<std::vector<double>> returns;
    auto it = prices_by_time.begin();

    if (it != prices_by_time.end()) {
        auto prev = it++;

        while (it != prices_by_time.end()) {
            std::vector<double> daily_returns;

            for (const auto& [symbol, price] : it->second) {
                if (prev->second.count(symbol)) {
                    double prev_price = prev->second.at(symbol);
                    daily_returns.push_back((price - prev_price) / prev_price);
                } else {
                    daily_returns.push_back(0.0);
                }
            }

            returns.push_back(daily_returns);
            prev = it++;
        }
    }

    return returns;
}

std::vector<std::vector<double>> RiskManager::calculate_covariance(
    const std::vector<std::vector<double>>& returns) const {
    if (returns.empty())
        return {};

    const size_t num_assets = returns[0].size();
    const size_t num_days = returns.size();

    // --- Eigen Vectorized Covariance Calculation ---
    // Create returns matrix (days x assets)
    Eigen::MatrixXd R(num_days, num_assets);
    for (size_t t = 0; t < num_days; ++t) {
        for (size_t i = 0; i < num_assets; ++i) {
            R(t, i) = returns[t][i];
        }
    }

    // Calculate column means
    Eigen::VectorXd means = R.colwise().mean();

    // Center the data (subtract means from each row)
    Eigen::MatrixXd centered = R.rowwise() - means.transpose();

    // Calculate covariance: (centered' * centered) / (n-1), then annualize
    const double annualization = 252.0;
    Eigen::MatrixXd cov = (centered.transpose() * centered) / (num_days - 1) * annualization;

    // Convert back to std::vector<std::vector<double>>
    std::vector<std::vector<double>> covariance(num_assets, std::vector<double>(num_assets, 0.0));
    for (size_t i = 0; i < num_assets; ++i) {
        for (size_t j = 0; j < num_assets; ++j) {
            covariance[i][j] = cov(i, j);
        }
    }

    return covariance;
}

double RiskManager::calculate_var(const std::unordered_map<std::string, Position>& positions,
                                  const std::vector<std::vector<double>>& returns) const {
    const auto weights = calculate_weights(positions);
    std::vector<double> portfolio_returns;

    for (const auto& daily_returns : returns) {
        double daily_return = 0.0;
        for (size_t i = 0; i < weights.size(); ++i) {
            daily_return += weights[i] * daily_returns[i];
        }
        portfolio_returns.push_back(daily_return);
    }

    std::sort(portfolio_returns.begin(), portfolio_returns.end());
    size_t var_index =
        static_cast<size_t>((1.0 - config_.confidence_level) * portfolio_returns.size());

    return -portfolio_returns[std::min(var_index, portfolio_returns.size() - 1)] *
           std::sqrt(252.0);  // Annualized VaR
}

double RiskManager::calculate_99th_percentile(const std::vector<double>& data) const {
    if (data.empty())
        return 0.0;

    std::vector<double> sorted_data = data;
    std::sort(sorted_data.begin(), sorted_data.end());

    size_t index = static_cast<size_t>(config_.confidence_level * sorted_data.size());
    return sorted_data[std::min(index, sorted_data.size() - 1)];
}

}  // namespace trade_ngin