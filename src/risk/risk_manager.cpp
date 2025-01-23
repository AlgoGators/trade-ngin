//src/risk/risk_manager.cpp
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace trade_ngin {

RiskManager::RiskManager(RiskConfig config)
    : config_(std::move(config)) {}

Result<void> RiskManager::update_market_data(const std::vector<Bar>& data) {
    try {
        // Update returns
        market_data_.returns = calculate_returns(data);
        
        // Update covariance matrix
        market_data_.covariance = calculate_covariance(market_data_.returns);
        
        // Update symbol indices mapping
        market_data_.symbol_indices.clear();
        std::unordered_map<std::string, size_t> symbol_counts;
        
        for (const auto& bar : data) {
            if (market_data_.symbol_indices.count(bar.symbol) == 0) {
                market_data_.symbol_indices[bar.symbol] = symbol_counts.size();
            }
            symbol_counts[bar.symbol]++;
        }
        
        return Result<void>({});
        
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::INVALID_RISK_CALCULATION,
            std::string("Error updating market data: ") + e.what(),
            "RiskManager"
        );
    }
}

Result<RiskResult> RiskManager::process_positions(
    const std::unordered_map<std::string, Position>& positions) {
    
    try {
        RiskResult result;
        
        // Calculate individual risk multipliers
        result.portfolio_multiplier = calculate_portfolio_multiplier(
            positions, market_data_.returns);
        
        result.jump_multiplier = calculate_jump_multiplier(
            positions, market_data_.returns);
        
        result.correlation_multiplier = calculate_correlation_multiplier(
            positions, market_data_.returns);
        
        result.leverage_multiplier = calculate_leverage_multiplier(
            positions, market_data_.returns);
        
        // Final multiplier is minimum of all multipliers
        result.recommended_scale = std::min({
            result.portfolio_multiplier,
            result.jump_multiplier,
            result.correlation_multiplier,
            result.leverage_multiplier
        });
        
        // Set risk exceeded flag if any multiplier is less than 1
        result.risk_exceeded = result.recommended_scale < 1.0;
        
        return Result<RiskResult>(result);
        
    } catch (const std::exception& e) {
        return make_error<RiskResult>(
            ErrorCode::INVALID_RISK_CALCULATION,
            std::string("Error processing positions: ") + e.what(),
            "RiskManager"
        );
    }
}

double RiskManager::calculate_portfolio_multiplier(
    const std::unordered_map<std::string, Position>& positions,
    const std::vector<std::vector<double>>& returns) const {
    
    // Calculate current portfolio risk
    std::vector<double> weights = calculate_weights(positions);
    double current_risk = calculate_var(positions, returns);
    
    // Calculate historical portfolio risks
    std::vector<double> historical_risks;
    for (size_t t = 0; t < returns.size(); ++t) {
        double daily_return = 0.0;
        size_t i = 0;
        for (const auto& [symbol, pos] : positions) {
            if (market_data_.symbol_indices.count(symbol)) {
                size_t idx = market_data_.symbol_indices.at(symbol);
                daily_return += weights[i] * returns[t][idx];
            }
            ++i;
        }
        historical_risks.push_back(std::abs(daily_return));
    }
    
    // Get 99th percentile risk
    std::sort(historical_risks.begin(), historical_risks.end());
    size_t index = static_cast<size_t>((1.0 - config_.confidence_level) * historical_risks.size());
    double max_risk = historical_risks[historical_risks.size() - 1 - index];
    
    return std::min(1.0, max_risk / current_risk);
}

double RiskManager::calculate_jump_multiplier(
    const std::unordered_map<std::string, Position>& positions,
    const std::vector<std::vector<double>>& returns) const {
    
    // Calculate jump-adjusted standard deviations
    std::vector<double> jump_stdevs(market_data_.symbol_indices.size(), 0.0);
    
    for (const auto& [symbol, idx] : market_data_.symbol_indices) {
        std::vector<double> asset_returns;
        for (const auto& daily_return : returns) {
            asset_returns.push_back(std::abs(daily_return[idx]));
        }
        
        // Calculate 99th percentile of absolute returns
        std::sort(asset_returns.begin(), asset_returns.end());
        size_t percentile_idx = static_cast<size_t>(config_.confidence_level * asset_returns.size());
        jump_stdevs[idx] = asset_returns[percentile_idx] * std::sqrt(252.0);
    }
    
    // Calculate current jump risk
    double current_jump_risk = 0.0;
    std::vector<double> weights = calculate_weights(positions);
    
    size_t i = 0;
    for (const auto& [symbol, pos] : positions) {
        if (market_data_.symbol_indices.count(symbol)) {
            size_t idx = market_data_.symbol_indices.at(symbol);
            current_jump_risk += weights[i] * jump_stdevs[idx];
        }
        ++i;
    }
    
    // Calculate historical jump risks
    std::vector<double> jump_risks;
    for (const auto& daily_return : returns) {
        double daily_jump_risk = 0.0;
        i = 0;
        for (const auto& [symbol, pos] : positions) {
            if (market_data_.symbol_indices.count(symbol)) {
                size_t idx = market_data_.symbol_indices.at(symbol);
                daily_jump_risk += weights[i] * std::abs(daily_return[idx]);
            }
            ++i;
        }
        jump_risks.push_back(daily_jump_risk);
    }
    
    // Get 99th percentile jump risk
    std::sort(jump_risks.begin(), jump_risks.end());
    size_t index = static_cast<size_t>((1.0 - config_.confidence_level) * jump_risks.size());
    double max_jump_risk = jump_risks[jump_risks.size() - 1 - index];
    
    return std::min(1.0, max_jump_risk / current_jump_risk);
}

double RiskManager::calculate_correlation_multiplier(
    const std::unordered_map<std::string, Position>& positions,
    const std::vector<std::vector<double>>& returns) const {
    
    std::vector<double> weights = calculate_weights(positions);
    
    // Calculate current correlation risk (sum of absolute weighted returns)
    double current_correlation_risk = 0.0;
    size_t i = 0;
    for (const auto& [symbol, pos] : positions) {
        if (market_data_.symbol_indices.count(symbol)) {
            size_t idx = market_data_.symbol_indices.at(symbol);
            current_correlation_risk += std::abs(weights[i] * 
                returns.back()[idx] * std::sqrt(252.0));
        }
        ++i;
    }
    
    // Calculate historical correlation risks
    std::vector<double> correlation_risks;
    for (const auto& daily_return : returns) {
        double risk = 0.0;
        i = 0;
        for (const auto& [symbol, pos] : positions) {
            if (market_data_.symbol_indices.count(symbol)) {
                size_t idx = market_data_.symbol_indices.at(symbol);
                risk += std::abs(weights[i] * daily_return[idx]);
            }
            ++i;
        }
        correlation_risks.push_back(risk * std::sqrt(252.0));
    }
    
    // Get 99th percentile correlation risk
    std::sort(correlation_risks.begin(), correlation_risks.end());
    size_t index = static_cast<size_t>((1.0 - config_.confidence_level) * correlation_risks.size());
    double max_correlation_risk = correlation_risks[correlation_risks.size() - 1 - index];
    
    return std::min(1.0, max_correlation_risk / current_correlation_risk);
}

double RiskManager::calculate_leverage_multiplier(
    const std::unordered_map<std::string, Position>& positions,
    const std::vector<std::vector<double>>& returns) const {
    
    if (config_.capital <= 0.0) {
        WARN("Capital is zero or negative, returning minimum leverage multiplier");
        return 0.0;
    }

    // Calculate current leverage
    double gross_leverage = 0.0;
    double net_leverage = 0.0;
    
    for (const auto& [symbol, pos] : positions) {
        double position_value = std::abs(pos.quantity * pos.average_price);
        gross_leverage += position_value;
        net_leverage += pos.quantity * pos.average_price;
    }
    
    gross_leverage /= config_.capital;
    net_leverage = std::abs(net_leverage) / config_.capital;

    // Calculate historical leverage values
    std::vector<double> historical_leverage;
    for (const auto& daily_return : returns) {
        double daily_gross = 0.0;
        size_t i = 0;
        for (const auto& [symbol, pos] : positions) {
            if (market_data_.symbol_indices.count(symbol)) {
                size_t idx = market_data_.symbol_indices.at(symbol);
                daily_gross += std::abs(pos.quantity * pos.average_price * (1.0 + daily_return[idx]));
            }
            ++i;
        }
        historical_leverage.push_back(daily_gross / config_.capital);
    }

    // Get 99th percentile leverage
    std::sort(historical_leverage.begin(), historical_leverage.end());
    size_t index = static_cast<size_t>((1.0 - config_.confidence_level) * historical_leverage.size());
    double max_leverage = historical_leverage[historical_leverage.size() - 1 - index];

    // Calculate multiplier based on both gross and net leverage constraints
    double gross_multiplier = config_.max_gross_leverage / gross_leverage;
    double net_multiplier = config_.max_net_leverage / net_leverage;
    
    return std::min({1.0, gross_multiplier, net_multiplier});
}

Result<void> RiskManager::update_config(const RiskConfig& config) {
    if (config.portfolio_var_limit <= 0.0 || 
        config.max_drawdown <= 0.0 ||
        config.max_gross_leverage <= 0.0 ||
        config.max_net_leverage <= 0.0 ||
        config.capital <= 0.0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Invalid risk configuration parameters",
            "RiskManager"
        );
    }

    if (config.confidence_level <= 0.0 || config.confidence_level >= 1.0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Confidence level must be between 0 and 1",
            "RiskManager"
        );
    }

    config_ = config;
    return Result<void>({});
}

std::vector<std::vector<double>> RiskManager::calculate_returns(
    const std::vector<Bar>& data) const {
    
    // Group data by symbol and timestamp
    std::map<Timestamp, std::map<std::string, double>> time_series;
    for (const auto& bar : data) {
        time_series[bar.timestamp][bar.symbol] = bar.close;
    }

    // Calculate returns
    std::vector<std::vector<double>> returns;
    auto it = time_series.begin();
    auto prev = it++;

    for (; it != time_series.end(); ++it, ++prev) {
        std::vector<double> daily_returns;
        for (const auto& [symbol, price] : it->second) {
            if (prev->second.count(symbol)) {
                double ret = (price - prev->second.at(symbol)) / prev->second.at(symbol);
                daily_returns.push_back(ret);
            } else {
                daily_returns.push_back(0.0);
            }
        }
        returns.push_back(daily_returns);
    }

    return returns;
}

std::vector<std::vector<double>> RiskManager::calculate_covariance(
    const std::vector<std::vector<double>>& returns) const {
    
    size_t n = returns[0].size();
    std::vector<double> means(n, 0.0);
    size_t t = returns.size();

    // Calculate means
    for (const auto& daily_returns : returns) {
        for (size_t i = 0; i < n; ++i) {
            means[i] += daily_returns[i];
        }
    }
    for (auto& mean : means) {
        mean /= t;
    }

    // Calculate covariance
    std::vector<std::vector<double>> covariance(n, std::vector<double>(n, 0.0));
    for (const auto& daily_returns : returns) {
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) {
                covariance[i][j] += (daily_returns[i] - means[i]) * 
                                  (daily_returns[j] - means[j]);
            }
        }
    }

    // Normalize
    for (auto& row : covariance) {
        for (auto& val : row) {
            val /= (t - 1);
        }
    }

    return covariance;
}

double RiskManager::calculate_var(
    const std::unordered_map<std::string, Position>& positions,
    const std::vector<std::vector<double>>& returns) const {
    
    std::vector<double> portfolio_returns;
    std::vector<double> weights = calculate_weights(positions);

    // Calculate portfolio returns
    for (const auto& daily_returns : returns) {
        double port_return = 0.0;
        size_t i = 0;
        for (const auto& [symbol, pos] : positions) {
            if (market_data_.symbol_indices.count(symbol)) {
                size_t idx = market_data_.symbol_indices.at(symbol);
                port_return += weights[i] * daily_returns[idx];
            }
            ++i;
        }
        portfolio_returns.push_back(port_return);
    }

    // Calculate VaR
    std::sort(portfolio_returns.begin(), portfolio_returns.end());
    size_t var_index = static_cast<size_t>((1.0 - config_.confidence_level) * portfolio_returns.size());
    return -portfolio_returns[var_index] * std::sqrt(252.0);  // Annualized
}

std::vector<double> RiskManager::calculate_weights(
    const std::unordered_map<std::string, Position>& positions) const {
    
    std::vector<double> weights;
    double total_value = 0.0;

    // Calculate total portfolio value
    for (const auto& [symbol, pos] : positions) {
        double position_value = std::abs(pos.quantity * pos.average_price);
        total_value += position_value;
    }

    // Calculate weights
    if (total_value > 0.0) {
        for (const auto& [symbol, pos] : positions) {
            double weight = (pos.quantity * pos.average_price) / total_value;
            weights.push_back(weight);
        }
    } else {
        weights.resize(positions.size(), 0.0);
    }

    return weights;
}

} // namespace trade_ngin