#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {
namespace testing {

class MockStrategy : public BaseStrategy {
public:
    MockStrategy(std::string id,
                StrategyConfig config,
                std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config), std::move(db)) {
        
        // Initialize metadata
        metadata_.name = "Mock Strategy";
        metadata_.description = "Simple strategy for testing";
        metadata_.sharpe_ratio = 1.5;
        metadata_.sortino_ratio = 1.2;
        metadata_.max_drawdown = 0.1;
        metadata_.win_rate = 0.6;
    }

    Result<void> on_data(const std::vector<Bar>& data) override {
        auto base_result = BaseStrategy::on_data(data);
        if (base_result.is_error()) return base_result;

        try {
            // Generate positions with varying sizes to test optimization
            for (const auto& bar : data) {
                // Calculate position size based on price volatility
                double price_change = bar.high - bar.low;
                double volatility = price_change / bar.close;
                
                // Base position size - varies with price to test optimization
                double base_size = (config_.capital_allocation * 0.1) / bar.close;
                
                // Modify size based on volatility
                double position_size = base_size * (1.0 + volatility * 10.0);
                
                // Add some randomization to create diverse positions
                position_size *= (0.8 + (static_cast<double>(rand()) / RAND_MAX) * 0.4);
                
                // Generate signal between -1 and 1
                double signal = 2.0 * (bar.close - 100.0) / 100.0;
                signal = std::max(-1.0, std::min(1.0, signal));
                
                Position pos;
                pos.symbol = bar.symbol;
                pos.quantity = position_size * signal;
                pos.average_price = bar.close;
                pos.last_update = bar.timestamp;
                pos.unrealized_pnl = (bar.close - pos.average_price) * pos.quantity;
                
                // For risk management testing, add some positions that might exceed limits
                if (rand() % 10 == 0) {  // 10% chance of a large position
                    pos.quantity *= 5.0;  // Make position significantly larger
                }
                
                positions_[bar.symbol] = pos;
                last_signals_[bar.symbol] = signal;
            }

            // Update metrics for risk management
            metrics_.total_pnl = 0.0;
            metrics_.volatility = 0.0;
            
            for (const auto& [symbol, pos] : positions_) {
                metrics_.total_pnl += pos.unrealized_pnl;
                
                // Calculate simple volatility metric
                auto it = std::find_if(data.begin(), data.end(),
                    [&symbol](const Bar& bar) { return bar.symbol == symbol; });
                
                if (it != data.end()) {
                    double daily_return = (it->close - it->open) / it->open;
                    metrics_.volatility += daily_return * daily_return;
                }
            }
            
            metrics_.volatility = std::sqrt(metrics_.volatility / positions_.size());
            metrics_.win_rate = metrics_.total_pnl > 0 ? 0.6 : 0.4;

            return Result<void>();
        } catch (const std::exception& e) {
            return make_error<void>(
                ErrorCode::STRATEGY_ERROR,
                std::string("Error processing data in mock strategy: ") + e.what(),
                "MockStrategy"
            );
        }
    }

    Result<void> check_risk_limits() override {
        // Calculate total exposure for risk checks
        double total_exposure = 0.0;
        double max_position_size = 0.0;
        
        for (const auto& [symbol, pos] : positions_) {
            double position_value = std::abs(pos.quantity * pos.average_price);
            total_exposure += position_value;
            max_position_size = std::max(max_position_size, position_value);
        }
        
        // Check against risk limits
        if (total_exposure > config_.capital_allocation * config_.max_leverage) {
            return make_error<void>(
                ErrorCode::RISK_LIMIT_EXCEEDED,
                "Total exposure exceeds maximum leverage",
                "MockStrategy"
            );
        }
        
        double drawdown = -metrics_.total_pnl / config_.capital_allocation;
        if (drawdown > risk_limits_.max_drawdown) {
            return make_error<void>(
                ErrorCode::RISK_LIMIT_EXCEEDED,
                "Drawdown exceeds limit",
                "MockStrategy"
            );
        }

        return Result<void>();
    }
};

} // namespace testing
} // namespace trade_ngin