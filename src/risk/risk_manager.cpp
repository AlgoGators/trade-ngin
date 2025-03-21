#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <map>

namespace trade_ngin {

RiskManager::RiskManager(RiskConfig config)
    : config_(std::move(config)) {
    // Initialize logger
    LoggerConfig logger_config;
    logger_config.min_level = LogLevel::DEBUG;
    logger_config.destination = LogDestination::BOTH;
    logger_config.log_directory = "logs";
    logger_config.filename_prefix = "risk_manager";
    Logger::instance().initialize(logger_config);
    }

Result<RiskResult> RiskManager::process_positions(
    const std::unordered_map<std::string, Position>& positions) {        
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
                ERROR("RiskManager: No positions provided for risk calculation");
                return make_error<RiskResult>(
                    ErrorCode::INVALID_DATA,
                    "No positions provided",
                    "RiskManager"
                );
            }

            // Add mutex lock for thread safety
            std::lock_guard<std::mutex> lock(mutex_);

            // Check if we have market data available
            if (market_data_.returns.empty() || market_data_.covariance.empty()) {
                ERROR("RiskManager: Market data not available for risk calculation");
                return make_error<RiskResult>(
                    ErrorCode::MARKET_DATA_ERROR,
                    "Market data not available for risk calculation",
                    "RiskManager"
                );
            }

            double total_value = 0.0;
            for (const auto& [symbol, pos] : positions) {
                total_value += std::abs(pos.quantity * pos.average_price);
            }
            const auto weights = calculate_weights(positions);
            
            // Calculate all risk multipliers and store metrics
            result.portfolio_multiplier = calculate_portfolio_multiplier(weights, result);
            result.jump_multiplier = calculate_jump_multiplier(weights, result);
            result.correlation_multiplier = calculate_correlation_multiplier(weights, result);
            result.leverage_multiplier = calculate_leverage_multiplier(weights, total_value, result);
            
            // Overall scale is minimum of all multipliers
            result.recommended_scale = std::min({
                result.portfolio_multiplier,
                result.jump_multiplier,
                result.correlation_multiplier,
                result.leverage_multiplier
            });
            
            result.risk_exceeded = result.recommended_scale < 1.0;
            return Result<RiskResult>(result);
            
        } catch(const std::exception& e) {
            ERROR("RiskManager: Risk calculation failed: " + std::string(e.what()));
            return make_error<RiskResult>(
                ErrorCode::INVALID_RISK_CALCULATION,
                std::string("Risk calculation failed: ") + e.what(),
                "RiskManager"
            );
        }
    }

std::vector<double> RiskManager::calculate_weights(
    const std::unordered_map<std::string, Position>& positions) const 
{
    std::vector<double> weights;
    double total_value = 0.0;

    // Calculate total portfolio value
    for(const auto& [symbol, pos] : positions) {
        total_value += std::abs(pos.quantity * pos.average_price);
    }

    // Calculate position weights
    if(total_value > 0.0) {
        for(const auto& [symbol, pos] : positions) {
            weights.push_back((pos.quantity * pos.average_price) / total_value);
        }
    } else {
        weights.resize(positions.size(), 0.0);
    }

    return weights;
}

double RiskManager::calculate_portfolio_multiplier(
    const std::vector<double>& weights,
    RiskResult& result) const 
{
    if (market_data_.covariance.empty() || weights.empty()) {
        result.portfolio_var = 0.0;
        return 1.0;
    }

    // Calculate portfolio variance using covariance matrix
    double variance = 0.0;
    for(size_t i = 0; i < weights.size(); ++i) {
        for(size_t j = 0; j < weights.size(); ++j) {
            variance += weights[i] * market_data_.covariance[i][j] * weights[j];
        }
    }
    result.portfolio_var = std::sqrt(variance);
    if (result.portfolio_var <= 0.0) {
        return 1.0;
    }
    
    // Calculate historical VaR
    std::vector<double> historical_var;
    for(const auto& daily_returns : market_data_.returns) {
        double port_return = std::inner_product(
            daily_returns.begin(), daily_returns.end(),
            weights.begin(), 0.0
        );
        historical_var.push_back(std::abs(port_return));
    }
    
    result.max_portfolio_risk = calculate_99th_percentile(historical_var);
    return std::min(1.0, config_.var_limit / result.portfolio_var);
}

double RiskManager::calculate_jump_multiplier(
    const std::vector<double>& weights,
    RiskResult& result) const 
{
    std::vector<double> jump_risks;
    for(const auto& daily_returns : market_data_.returns) {
        double jump_risk = 0.0;
        for(size_t i = 0; i < weights.size(); ++i) {
            jump_risk += std::abs(weights[i] * daily_returns[i]);
        }
        jump_risks.push_back(jump_risk);
    }
    
    result.jump_risk = calculate_99th_percentile(jump_risks);

    if (result.jump_risk <= 0.0) {
        return 1.0;
    }

    result.max_jump_risk = std::max(result.jump_risk, result.max_jump_risk);
    return std::min(1.0, config_.jump_risk_limit / result.jump_risk);
}

double RiskManager::calculate_correlation_multiplier(
    const std::vector<double>& weights,
    RiskResult& result) const 
{
    double max_corr = 0.0;
    const size_t n = weights.size();
    
    for(size_t i = 0; i < n; ++i) {
        for(size_t j = i+1; j < n; ++j) {
            double sum_ij = 0.0, sum_i2 = 0.0, sum_j2 = 0.0;
            for(const auto& daily_ret : market_data_.returns) {
                sum_ij += daily_ret[i] * daily_ret[j];
                sum_i2 += daily_ret[i] * daily_ret[i];
                sum_j2 += daily_ret[j] * daily_ret[j];
            }
            double corr = sum_ij / (std::sqrt(sum_i2) * std::sqrt(sum_j2));
            max_corr = std::max(max_corr, corr);
        }
    }
    
    result.correlation_risk = max_corr;
    return std::min(1.0, config_.max_correlation / max_corr);
}

double RiskManager::calculate_leverage_multiplier(
    const std::vector<double>& weights,
    double total_value,
    RiskResult& result) const {
    // Calculate gross and net leverage
    double gross = total_value;
    
    double net = std::accumulate(weights.begin(), weights.end(), 0.0) * total_value;
    
    result.gross_leverage = gross / config_.capital;
    result.net_leverage = std::abs(net) / config_.capital;
    
    // Historical leverage calculation
    std::vector<double> historical_leverage;
    for(const auto& daily_returns : market_data_.returns) {
        double lev = std::accumulate(daily_returns.begin(), daily_returns.end(), 0.0);
        historical_leverage.push_back(std::abs(lev) / config_.capital);
    }
    
    result.max_leverage_risk = calculate_99th_percentile(historical_leverage);
    
    // Only scale down positions if leverage exceeds limits
    double gross_multiplier = result.gross_leverage > config_.max_gross_leverage 
        ? config_.max_gross_leverage / result.gross_leverage 
        : 1.0;
        
    double net_multiplier = result.net_leverage > config_.max_net_leverage 
        ? config_.max_net_leverage / result.net_leverage 
        : 1.0;

    return std::min({1.0, gross_multiplier, net_multiplier});
}

Result<void> RiskManager::update_market_data(const std::vector<Bar>& data) {
    try {
        // Check for empty data
        if(data.empty()) {
            return make_error<void>(
                ErrorCode::MARKET_DATA_ERROR,
                "Empty market data provided",
                "RiskManager"
            );
        }

        // Calculate returns and covariance
        std::vector<std::vector<double>> new_returns;
        std::vector<std::vector<double>> new_covariance;
        std::unordered_map<std::string, size_t> new_symbol_indices;

        try {
            new_returns = calculate_returns(data);
            if (!new_returns.empty()) {
                new_covariance = calculate_covariance(new_returns);
            }
            
            // Update symbol indices mapping
            size_t idx = 0;
            for (const auto& bar : data) {
                if(new_symbol_indices.find(bar.symbol) == new_symbol_indices.end()) {
                    new_symbol_indices[bar.symbol] = idx++;
                }
            }


        } catch (const std::exception& e) {
            ERROR("Failed to calculate returns or covariance: " + std::string(e.what()));
            return make_error<void>(
                ErrorCode::MARKET_DATA_ERROR,
                "Failed to calculate returns or covariance: " + std::string(e.what()),
                "RiskManager"
            );
        }

        // Only lock when updating shared data
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!new_returns.empty()) {
                market_data_.returns = new_returns;
                market_data_.covariance = new_covariance;
                market_data_.symbol_indices = new_symbol_indices;
            }
        }
        
        return Result<void>();
        
    } catch(const std::exception& e) {
        ERROR("Failed to update market data: " + std::string(e.what()));
        return make_error<void>(
            ErrorCode::MARKET_DATA_ERROR,
            "Failed to update market data: " + std::string(e.what()),
            "RiskManager"
        );
    }
}

Result<void> RiskManager::update_config(const RiskConfig& config) {
    // Validate configuration parameters
    if(config.capital <= 0.0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Capital must be positive"
        );
    }
    
    if(config.confidence_level <= 0.0 || config.confidence_level >= 1.0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Confidence level must be between 0 and 1"
        );
    }

    if(config.var_limit <= 0.0 || config.jump_risk_limit <= 0.0 ||
       config.max_correlation <= 0.0 || config.max_gross_leverage <= 0.0 ||
       config.max_net_leverage <= 0.0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "All risk limits must be positive"
        );
    }
    
    config_ = config;
    return Result<void>();
}

std::vector<std::vector<double>> RiskManager::calculate_returns(
    const std::vector<Bar>& data) const 
{
    std::map<std::string, std::map<Timestamp, double>> prices_by_symbol;
    
    // Organize data by symbol and timestamp
    for(const auto& bar : data) {
        prices_by_symbol[bar.symbol][bar.timestamp] = bar.close;
    }
    
    // Convert to chronologically ordered timeseries
    std::map<Timestamp, std::map<std::string, double>> prices_by_time;
    for(const auto& [symbol, prices] : prices_by_symbol) {
        for(const auto& [ts, price] : prices) {
            prices_by_time[ts][symbol] = price;
        }
    }
    
    // Calculate returns between consecutive timestamps
    std::vector<std::vector<double>> returns;
    auto it = prices_by_time.begin();
    
    if(it != prices_by_time.end()) {
        auto prev = it++;
        
        while(it != prices_by_time.end()) {
            std::vector<double> daily_returns;
            
            for(const auto& [symbol, price] : it->second) {
                if(prev->second.count(symbol)) {
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
    const std::vector<std::vector<double>>& returns) const 
{
    if(returns.empty()) return {};
    
    const size_t num_assets = returns[0].size();
    const size_t num_days = returns.size();
    
    // Calculate means
    std::vector<double> means(num_assets, 0.0);
    for(const auto& daily_returns : returns) {
        for(size_t i = 0; i < num_assets; ++i) {
            means[i] += daily_returns[i];
        }
    }
    
    for(auto& mean : means) {
        mean /= num_days;
    }
    
    // Calculate covariance matrix
    std::vector<std::vector<double>> covariance(
        num_assets, std::vector<double>(num_assets, 0.0));
    
    for(const auto& daily_returns : returns) {
        for(size_t i = 0; i < num_assets; ++i) {
            for(size_t j = 0; j < num_assets; ++j) {
                double dev_i = daily_returns[i] - means[i];
                double dev_j = daily_returns[j] - means[j];
                covariance[i][j] += dev_i * dev_j;
            }
        }
    }
    
    // Normalize and annualize
    const double annualization = 252.0;  // Trading days per year
    for(auto& row : covariance) {
        for(auto& val : row) {
            val = (val / (num_days - 1)) * annualization;
        }
    }
    
    return covariance;
}

double RiskManager::calculate_var(
    const std::unordered_map<std::string, Position>& positions,
    const std::vector<std::vector<double>>& returns) const 
{
    const auto weights = calculate_weights(positions);
    std::vector<double> portfolio_returns;
    
    for(const auto& daily_returns : returns) {
        double daily_return = 0.0;
        for(size_t i = 0; i < weights.size(); ++i) {
            daily_return += weights[i] * daily_returns[i];
        }
        portfolio_returns.push_back(daily_return);
    }
    
    std::sort(portfolio_returns.begin(), portfolio_returns.end());
    size_t var_index = static_cast<size_t>(
        (1.0 - config_.confidence_level) * portfolio_returns.size()
    );
    
    return -portfolio_returns[std::min(var_index, portfolio_returns.size() - 1)] * 
           std::sqrt(252.0);  // Annualized VaR
}

double RiskManager::calculate_99th_percentile(
    const std::vector<double>& data) const 
{
    if(data.empty()) return 0.0;
    
    std::vector<double> sorted_data = data;
    std::sort(sorted_data.begin(), sorted_data.end());
    
    size_t index = static_cast<size_t>(config_.confidence_level * sorted_data.size());
    return sorted_data[std::min(index, sorted_data.size() - 1)];
}

} // namespace trade_ngin