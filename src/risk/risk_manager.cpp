#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <map>
#include <set>

namespace trade_ngin {

RiskManager::RiskManager(RiskConfig config)
    : config_(std::move(config)) {
        Logger::register_component("RiskManager");
    }

Result<RiskResult> RiskManager::process_positions(
    const std::unordered_map<std::string, Position>& positions,
    const MarketData& market_data) {        
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
        
        for (const auto& [symbol, pos] : positions) {
            // Only include positions with symbols in our market data
            auto it = market_data.symbol_indices.find(symbol);
            if (it != market_data.symbol_indices.end()) {
                size_t index = it->second;
                if (index < position_values.size()) {
                    double position_value = pos.quantity * pos.average_price;
                    position_values[index] = position_value;
                    total_value += std::abs(position_value);
                    position_symbols.push_back(symbol);
                }
            }
        }
        
        if (position_symbols.empty()) {
            WARN("No positions mapped to market data symbols, skipping risk calculation");
            return Result<RiskResult>(result);  // Return default result
        }
        
        // Calculate position weights
        std::vector<double> weights;
        weights.resize(position_values.size(), 0.0);
        
        if (total_value > 0.0) {
            for (size_t i = 0; i < position_values.size(); ++i) {
                weights[i] = position_values[i] / total_value;
            }
        }
        
        // Calculate all risk multipliers and store metrics
        result.portfolio_multiplier = calculate_portfolio_multiplier(market_data, weights, result);
        result.jump_multiplier = calculate_jump_multiplier(market_data, weights, result);
        result.correlation_multiplier = calculate_correlation_multiplier(market_data, weights, result);
        result.leverage_multiplier = calculate_leverage_multiplier(market_data, weights, total_value, result);
        
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
    const MarketData& market_data,
    const std::vector<double>& weights,
    RiskResult& result) const 
{
    if (market_data.covariance.empty() || weights.empty()) {
        result.portfolio_var = 0.0;
        return 1.0;
    }

    // Calculate portfolio variance using covariance matrix
    double variance = 0.0;
    for(size_t i = 0; i < weights.size(); ++i) {
        for(size_t j = 0; j < weights.size(); ++j) {
            variance += weights[i] * market_data.covariance[i][j] * weights[j];
        }
    }
    result.portfolio_var = std::sqrt(variance);
    if (result.portfolio_var <= 0.0) {
        return 1.0;
    }
    
    // Calculate historical VaR
    std::vector<double> historical_var;
    for(const auto& daily_returns : market_data.returns) {
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
    const MarketData& market_data,
    const std::vector<double>& weights,
    RiskResult& result) const 
{
    std::vector<double> jump_risks;
    for(const auto& daily_returns : market_data.returns) {
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
    const MarketData& market_data,
    const std::vector<double>& weights,
    RiskResult& result) const 
{
    double max_corr = 0.0;
    // Need to have the positions and their corresponding symbols
    // to correctly map to market data indices
    if (weights.empty() || market_data.ordered_symbols.empty() || 
        weights.size() > market_data.ordered_symbols.size()) {
        // Not enough data to calculate correlations
        result.correlation_risk = 0.0;
        return 1.0;  // Safe default, no scaling
    }
    
    const size_t n = std::min(weights.size(), market_data.ordered_symbols.size());
    
    if (market_data.returns.empty() || market_data.covariance.empty()) {
        // No return data available
        result.correlation_risk = 0.0;
        return 1.0;  // Safe default, no scaling
    }
    
    try {
        // Calculate correlation from covariance matrix
        for (size_t i = 0; i < n; ++i) {
            // Skip positions with zero weight
            if (std::abs(weights[i]) < 1e-10) continue;
            
            for (size_t j = i+1; j < n; ++j) {
                // Skip positions with zero weight
                if (std::abs(weights[j]) < 1e-10) continue;
                
                // Get standard deviations from diagonal of covariance matrix
                double var_i = market_data.covariance[i][i];
                double var_j = market_data.covariance[j][j];
                
                // Avoid division by zero
                if (var_i <= 0.0 || var_j <= 0.0) continue;
                
                double std_i = std::sqrt(var_i);
                double std_j = std::sqrt(var_j);
                
                // Calculate correlation coefficient
                double cov_ij = market_data.covariance[i][j];
                double corr = cov_ij / (std_i * std_j);
                
                // Ensure correlation is valid
                if (std::isnan(corr) || std::isinf(corr)) {
                    continue;
                }
                
                // Correlation must be between -1 and 1
                corr = std::max(-1.0, std::min(1.0, corr));
                
                // We're concerned with absolute correlation
                max_corr = std::max(max_corr, std::abs(corr));
            }
        }
    } catch (const std::exception& e) {
        ERROR("Exception in correlation calculation: " + std::string(e.what()));
        // Return safe value if calculation fails
        return 1.0;
    }
    
    result.correlation_risk = max_corr;
    
    // If max correlation exceeds limit, scale positions down
    if (max_corr > config_.max_correlation && max_corr > 0.0) {
        return config_.max_correlation / max_corr;
    }
    
    return 1.0;  // No scaling needed
}

double RiskManager::calculate_leverage_multiplier(
    const MarketData& market_data,
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
    for(const auto& daily_returns : market_data.returns) {
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

MarketData RiskManager::create_market_data(const std::vector<Bar>& data) {
    MarketData market_data;

    // Collect Unique Symbols and Order Them
    std::set<std::string> unique_symbols;
    for (const auto& bar : data) {
        unique_symbols.insert(bar.symbol);
    }
    market_data.ordered_symbols = std::vector<std::string>(unique_symbols.begin(), unique_symbols.end());
    std::sort(market_data.ordered_symbols.begin(), market_data.ordered_symbols.end());

    // Create Symbol Indices Mapping
    for (size_t i = 0; i < market_data.ordered_symbols.size(); ++i) {
        market_data.symbol_indices[market_data.ordered_symbols[i]] = i;
    }

    // Organize Prices by Symbol and Timestamp
    std::map<std::string, std::map<Timestamp, double>> prices_by_symbol;
    for (const auto& bar : data) {
        prices_by_symbol[bar.symbol][bar.timestamp] = bar.close;
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
                    daily_returns[i] = (it->second.at(symbol) - prev->second.at(symbol)) / prev->second.at(symbol);
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
                    market_data.covariance[i][j] += (market_data.returns[k][i] - means[i]) * (market_data.returns[k][j] - means[j]);
                }
                market_data.covariance[i][j] /= (num_periods - 1);
                market_data.covariance[i][j] *= 252.0; // Annualize
            }
        }
    }

    return market_data;
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