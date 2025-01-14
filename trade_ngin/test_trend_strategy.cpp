#include "system/trend_strategy.hpp"
#include "data/dataframe.hpp"
#include "data/database_interface.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>

struct PerformanceMetrics {
    double total_return;
    double annualized_return;
    double volatility;
    double sharpe_ratio;
    double max_drawdown;
    int winning_trades;
    int losing_trades;
    double avg_win;
    double avg_loss;
    double profit_factor;
};

PerformanceMetrics calculatePerformance(const std::vector<double>& positions, 
                                      const std::vector<double>& prices) {
    PerformanceMetrics metrics{};
    std::vector<double> returns;
    std::vector<double> strategy_returns;
    std::vector<double> equity_curve;
    
    // Calculate returns and strategy returns
    returns.reserve(prices.size() - 1);
    strategy_returns.reserve(prices.size() - 1);
    equity_curve.push_back(1.0);
    
    for (size_t i = 1; i < prices.size(); ++i) {
        double ret = (prices[i] / prices[i-1]) - 1.0;
        returns.push_back(ret);
        double strat_ret = positions[i-1] * ret;
        strategy_returns.push_back(strat_ret);
        equity_curve.push_back(equity_curve.back() * (1.0 + strat_ret));
    }
    
    // Calculate total return
    metrics.total_return = equity_curve.back() - 1.0;
    
    // Calculate annualized return
    metrics.annualized_return = std::pow(1.0 + metrics.total_return, 252.0/returns.size()) - 1.0;
    
    // Calculate volatility
    double sum_sq = 0.0;
    for (double ret : strategy_returns) {
        sum_sq += ret * ret;
    }
    metrics.volatility = std::sqrt(sum_sq / strategy_returns.size() * 252.0);
    
    // Calculate Sharpe ratio
    metrics.sharpe_ratio = metrics.annualized_return / metrics.volatility;
    
    // Calculate max drawdown
    double peak = equity_curve[0];
    metrics.max_drawdown = 0.0;
    for (double equity : equity_curve) {
        peak = std::max(peak, equity);
        metrics.max_drawdown = std::max(metrics.max_drawdown, (peak - equity) / peak);
    }
    
    // Calculate trade statistics
    double total_wins = 0.0;
    double total_losses = 0.0;
    metrics.winning_trades = 0;
    metrics.losing_trades = 0;
    
    for (double ret : strategy_returns) {
        if (ret > 0) {
            metrics.winning_trades++;
            total_wins += ret;
        } else if (ret < 0) {
            metrics.losing_trades++;
            total_losses -= ret;
        }
    }
    
    metrics.avg_win = metrics.winning_trades > 0 ? total_wins / metrics.winning_trades : 0;
    metrics.avg_loss = metrics.losing_trades > 0 ? total_losses / metrics.losing_trades : 0;
    metrics.profit_factor = total_losses > 0 ? total_wins / total_losses : 0;
    
    return metrics;
}

void printPerformanceReport(const std::string& symbol, const PerformanceMetrics& metrics) {
    std::cout << "\nPerformance Report for " << symbol << ":\n";
    std::cout << "==========================================\n";
    std::cout << std::fixed << std::setprecision(2);
    
    std::cout << "Returns:\n";
    std::cout << "  Total Return: " << metrics.total_return * 100 << "%\n";
    std::cout << "  Annualized Return: " << metrics.annualized_return * 100 << "%\n";
    std::cout << "  Annualized Volatility: " << metrics.volatility * 100 << "%\n";
    std::cout << "  Sharpe Ratio: " << metrics.sharpe_ratio << "\n";
    std::cout << "  Maximum Drawdown: " << metrics.max_drawdown * 100 << "%\n\n";
    
    std::cout << "Trade Statistics:\n";
    std::cout << "  Winning Trades: " << metrics.winning_trades << "\n";
    std::cout << "  Losing Trades: " << metrics.losing_trades << "\n";
    std::cout << "  Win Rate: " << 
        (double)metrics.winning_trades / (metrics.winning_trades + metrics.losing_trades) * 100 << "%\n";
    std::cout << "  Average Win: " << metrics.avg_win * 100 << "%\n";
    std::cout << "  Average Loss: " << metrics.avg_loss * 100 << "%\n";
    std::cout << "  Profit Factor: " << metrics.profit_factor << "\n";
}

void testStrategyConfig(const std::string& name,
                       const std::string& symbol,
                       const DataFrame& market_data,
                       const TrendStrategy::StrategyConfig& config,
                       const std::unordered_map<std::string, std::string>& params) {
    std::cout << "\nTesting " << name << " on " << symbol << "\n";
    std::cout << "==========================================\n";
    
    TrendStrategy strategy(config.capital, config);
    strategy.configure(params);
    
    std::cout << "Strategy configured with:\n"
              << "- Short MA: " << params.at("short_span") << " days\n"
              << "- Long MA: " << params.at("long_span") << " days\n"
              << "- Volatility Window: " << params.at("vol_window") << " days\n"
              << "- Regime Windows: " << params.at("regime_fast_window") << "/"
              << params.at("regime_slow_window") << " days\n\n";
    
    strategy.update(market_data);
    auto positions = strategy.positions();
    auto prices = market_data.get_column("close");
    
    auto metrics = calculatePerformance(positions.get_column("position"), prices);
    printPerformanceReport(symbol, metrics);
}

int main() {
    try {
        std::cout << "Starting Trend Strategy Test\n";
        std::cout << "===========================\n\n";

        // Initialize database interface
        DatabaseInterface db;
        
        // Get earliest and latest dates
        std::string start_date = db.getEarliestDate();
        std::string end_date = db.getLatestDate();
        std::cout << "Testing period: " << start_date << " to " << end_date << "\n\n";
        
        // List of futures to test
        std::vector<std::string> symbols = {"ES", "NQ", "CL", "GC", "ZN"};
        
        // Base configuration
        TrendStrategy::StrategyConfig base_config{
            1'000'000,  // capital
            2.0,        // max_leverage
            1.0,        // position_limit
            0.1         // risk_limit
        };
        
        // Strategy parameter sets
        std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>> strategies = {
            {"Fast Moving Strategy", {
                {"short_span", "5"},
                {"long_span", "20"},
                {"vol_window", "10"},
                {"regime_fast_window", "5"},
                {"regime_slow_window", "20"}
            }},
            {"Medium-term Strategy", {
                {"short_span", "20"},
                {"long_span", "50"},
                {"vol_window", "20"},
                {"regime_fast_window", "10"},
                {"regime_slow_window", "40"}
            }},
            {"Long-term Strategy", {
                {"short_span", "50"},
                {"long_span", "200"},
                {"vol_window", "50"},
                {"regime_fast_window", "20"},
                {"regime_slow_window", "100"}
            }}
        };
        
        // Test each symbol with each strategy configuration
        for (const auto& symbol : symbols) {
            std::cout << "\nTesting " << symbol << "\n";
            std::cout << "==========================================\n";
            
            // Get market data from database
            auto ohlcv_table = db.getOHLCVArrowTable(start_date, end_date, {symbol});
            if (!ohlcv_table || ohlcv_table->num_rows() == 0) {
                std::cout << "No data available for " << symbol << "\n";
                continue;
            }
            
            // Convert Arrow table to DataFrame
            DataFrame market_data;
            // Extract OHLCV columns from Arrow table
            auto close_array = std::static_pointer_cast<arrow::DoubleArray>(ohlcv_table->column(5)->chunk(0));
            std::vector<double> close_prices;
            close_prices.reserve(close_array->length());
            for (int64_t i = 0; i < close_array->length(); ++i) {
                close_prices.push_back(close_array->Value(i));
            }
            market_data.add_column("close", close_prices);
            
            std::cout << "Got " << market_data.rows() << " days of data\n";
            
            // Test each strategy configuration
            for (const auto& [name, params] : strategies) {
                testStrategyConfig(name, symbol, market_data, base_config, params);
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
} 