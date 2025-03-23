#include "test_trend_strategy_paper_trade.hpp"
#include <cmath>
#include <algorithm>
#include <spdlog/spdlog.h>

TrendStrategyPaperTrader::TradeStats TrendStrategyPaperTrader::runSimulation(
    const std::vector<std::string>& symbols,
    const std::string& start_date,
    const std::string& end_date,
    bool use_real_time
) {
    current_capital_ = initial_capital_;
    equity_curve_.clear();
    equity_curve_.push_back(current_capital_);
    
    stats_ = TradeStats{};
    current_positions_.clear();

    try {
        // Authenticate with IBKR
        if (!ibkr_->authenticate()) {
            throw std::runtime_error("Failed to authenticate with IBKR");
        }

        // Get list of trading days
        std::string query = fmt::format(
            "SELECT DISTINCT time::date as date FROM futures_data.ohlcv_1d "
            "WHERE time BETWEEN '{}' AND '{}' "
            "ORDER BY date",
            start_date, end_date
        );
        
        auto result = db_client_->executeQuery(query);
        std::vector<std::string> trading_days;
        for (const auto& row : result) {
            trading_days.push_back(row[0].as<std::string>());
        }

        // Process each trading day
        for (const auto& date : trading_days) {
            processTradingDay(date, symbols);
            updatePerformanceMetrics(date);
        }

        // Calculate final statistics
        stats_.sharpe_ratio = calculateSharpeRatio(stats_.daily_returns);
        stats_.max_drawdown = calculateMaxDrawdown(equity_curve_);

        return stats_;

    } catch (const std::exception& e) {
        spdlog::error("Error in paper trading simulation: {}", e.what());
        throw;
    }
}

void TrendStrategyPaperTrader::processTradingDay(
    const std::string& date,
    const std::vector<std::string>& symbols
) {
    std::unordered_map<std::string, double> target_positions;

    for (const auto& symbol : symbols) {
        // Fetch market data
        json market_data;
        if (ibkr_->isConnected()) {
            market_data = ibkr_->getMarketData(symbol, {"last", "volume", "high", "low"});
        } else {
            // Fallback to database
            std::string query = fmt::format(
                "SELECT * FROM market_data WHERE symbol = '{}' AND date = '{}'",
                symbol, date
            );
            auto result = db_client_->executeQuery(query);
            if (!result.empty()) {
                market_data["last"] = result[0]["close"].as<double>();
                market_data["volume"] = result[0]["volume"].as<double>();
                market_data["high"] = result[0]["high"].as<double>();
                market_data["low"] = result[0]["low"].as<double>();
            }
        }

        if (!market_data.empty()) {
            // Generate signals
            auto signals = generateSignals(symbol, market_data);
            
            // Calculate position size
            for (const auto& [sym, signal] : signals) {
                double position_size = calculatePositionSize(sym, signal, market_data);
                target_positions[sym] = position_size;
            }
        }
    }

    // Update portfolio
    updatePortfolio(target_positions);
}

std::unordered_map<std::string, double> TrendStrategyPaperTrader::generateSignals(
    const std::string& symbol,
    const json& market_data
) {
    std::unordered_map<std::string, double> signals;

    try {
        // Fetch historical data for signal generation
        auto historical_data = fetchHistoricalData(
            symbol,
            "DATE_SUB(CURRENT_DATE, INTERVAL 1 YEAR)",
            "CURRENT_DATE"
        );

        if (historical_data.empty()) {
            return signals;
        }

        // Calculate moving averages
        std::vector<double> prices;
        for (const auto& data : historical_data) {
            prices.push_back(data["close"].get<double>());
        }

        // Short MA
        double ma_short = 0;
        int short_period = strategy_params_["ma_short"];
        for (int i = std::max(0, (int)prices.size() - short_period); i < prices.size(); ++i) {
            ma_short += prices[i];
        }
        ma_short /= short_period;

        // Long MA
        double ma_long = 0;
        int long_period = strategy_params_["ma_long"];
        for (int i = std::max(0, (int)prices.size() - long_period); i < prices.size(); ++i) {
            ma_long += prices[i];
        }
        ma_long /= long_period;

        // Calculate momentum
        int momentum_window = strategy_params_["momentum_window"];
        double momentum = (prices.back() - prices[prices.size() - momentum_window]) / prices[prices.size() - momentum_window];

        // Calculate volatility
        int vol_window = strategy_params_["volatility_window"];
        std::vector<double> returns;
        for (size_t i = 1; i < prices.size(); ++i) {
            returns.push_back((prices[i] - prices[i-1]) / prices[i-1]);
        }
        double volatility = calculateVolatility(returns);

        // Generate signal (-1 to 1)
        double trend_signal = (ma_short - ma_long) / ma_long;
        double momentum_signal = momentum;
        double vol_adjustment = std::exp(-volatility * 2); // Reduce position size in high volatility

        // Combine signals
        double final_signal = (trend_signal * 0.4 + momentum_signal * 0.3) * vol_adjustment;
        final_signal = std::max(std::min(final_signal, 1.0), -1.0);

        signals[symbol] = final_signal;

    } catch (const std::exception& e) {
        spdlog::error("Error generating signals for {}: {}", symbol, e.what());
    }

    return signals;
}

double TrendStrategyPaperTrader::calculatePositionSize(
    const std::string& symbol,
    double signal_strength,
    const json& market_data
) {
    try {
        double price = market_data["last"].get<double>();
        double volatility = calculateVolatility(stats_.daily_returns);
        
        // Base position size as percentage of capital
        double base_size = current_capital_ * risk_target_;
        
        // Adjust for volatility
        double vol_scaling = 0.2 / (volatility + 1e-6); // Target 20% annualized vol
        base_size *= vol_scaling;
        
        // Scale by signal strength
        double position_value = base_size * std::abs(signal_strength);
        
        // Convert to number of contracts/shares
        double position_size = position_value / price;
        
        // Apply leverage limit
        double leverage = position_value / current_capital_;
        if (leverage > leverage_limit_) {
            position_size *= leverage_limit_ / leverage;
        }
        
        // Return signed position size
        return position_size * (signal_strength > 0 ? 1 : -1);

    } catch (const std::exception& e) {
        spdlog::error("Error calculating position size for {}: {}", symbol, e.what());
        return 0.0;
    }
}

void TrendStrategyPaperTrader::updatePortfolio(
    const std::unordered_map<std::string, double>& target_positions
) {
    try {
        for (const auto& [symbol, target_pos] : target_positions) {
            double current_pos = current_positions_[symbol];
            double pos_diff = target_pos - current_pos;
            
            if (std::abs(pos_diff) > 0.01) { // Minimum trade size
                // Place order
                bool is_buy = pos_diff > 0;
                auto order = ibkr_->placeOrder(
                    symbol,
                    std::abs(pos_diff),
                    0.0, // Market order
                    is_buy
                );
                
                if (!order.empty() && order["status"] == "submitted") {
                    current_positions_[symbol] = target_pos;
                    stats_.total_trades++;
                }
            }
        }
        
        // Update position history
        for (const auto& [symbol, pos] : current_positions_) {
            stats_.position_history[symbol].push_back(pos);
        }

    } catch (const std::exception& e) {
        spdlog::error("Error updating portfolio: {}", e.what());
    }
}

void TrendStrategyPaperTrader::updatePerformanceMetrics(const std::string& date) {
    try {
        double portfolio_value = current_capital_;
        
        // Add market value of positions
        for (const auto& [symbol, quantity] : current_positions_) {
            auto market_data = ibkr_->getMarketData(symbol, {"last"});
            if (!market_data.empty()) {
                double price = market_data["last"].get<double>();
                portfolio_value += quantity * price;
            }
        }
        
        // Calculate daily return
        if (!equity_curve_.empty()) {
            double daily_return = (portfolio_value - equity_curve_.back()) / equity_curve_.back();
            stats_.daily_returns.push_back(daily_return);
            
            if (daily_return > 0) {
                stats_.winning_trades++;
            }
        }
        
        equity_curve_.push_back(portfolio_value);
        stats_.total_pnl = portfolio_value - initial_capital_;

    } catch (const std::exception& e) {
        spdlog::error("Error updating performance metrics: {}", e.what());
    }
}

double TrendStrategyPaperTrader::calculateVolatility(const std::vector<double>& returns) {
    if (returns.empty()) return 0.0;
    
    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double sq_sum = std::inner_product(returns.begin(), returns.end(), returns.begin(), 0.0);
    double variance = sq_sum / returns.size() - mean * mean;
    
    return std::sqrt(variance * 252); // Annualized
}

double TrendStrategyPaperTrader::calculateSharpeRatio(const std::vector<double>& returns) {
    if (returns.empty()) return 0.0;
    
    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double volatility = calculateVolatility(returns);
    
    return mean * std::sqrt(252) / (volatility + 1e-6);
}

double TrendStrategyPaperTrader::calculateMaxDrawdown(const std::vector<double>& equity_curve) {
    if (equity_curve.empty()) return 0.0;
    
    double max_drawdown = 0.0;
    double peak = equity_curve[0];
    
    for (double value : equity_curve) {
        if (value > peak) {
            peak = value;
        }
        double drawdown = (peak - value) / peak;
        max_drawdown = std::max(max_drawdown, drawdown);
    }
    
    return max_drawdown;
}

std::vector<json> TrendStrategyPaperTrader::fetchHistoricalData(
    const std::string& symbol,
    const std::string& start_date,
    const std::string& end_date
) {
    std::vector<json> data;
    
    try {
        std::string query = 
            "SELECT time::date as date, open, high, low, close, volume "
            "FROM futures_data.ohlcv_1d "
            "WHERE symbol = $1 "
            "AND time BETWEEN $2 AND $3 "
            "ORDER BY time";
        
        auto result = db_client_->executeQuery(
            "SELECT time, open, high, low, close, volume FROM futures_data.ohlcv_1d "
            "WHERE symbol = '" + symbol + "' AND time BETWEEN '" + start_date + "' AND '" + end_date + "' "
            "ORDER BY time ASC");
        
        for (const auto& row : result) {
            json bar = {
                {"date", row["date"].as<std::string>()},
                {"open", row["open"].as<double>()},
                {"high", row["high"].as<double>()},
                {"low", row["low"].as<double>()},
                {"close", row["close"].as<double>()},
                {"volume", row["volume"].as<int>()}
            };
            data.push_back(bar);
        }
    } catch (const std::exception& e) {
        spdlog::error("Error fetching historical data for {}: {}", symbol, e.what());
    }
    
    return data;
}
