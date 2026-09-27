// STAGED mechanical MODEL tail; pending compiler and complete raw parity gates.
#pragma once
#include <ctime>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/apps/run_consumption.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/csv_exporter.hpp"
#include "trade_ngin/live/live_trading_coordinator.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"

namespace trade_ngin {
class ExecutionManager;
class MarginManager;

struct BookTailForecastDisplay {
    double forecast;
    double position;
};

// Strategy creation, forecasting and lifecycle stay in the named caller.
// Each method executes at the exact former statement location.
class BookTailCallbacks {
public:
    virtual ~BookTailCallbacks() = default;
    virtual void run_counterfactual_book() = 0;
    virtual BookTailForecastDisplay get_primary_forecast_display(const std::string& symbol) = 0;
    virtual void stop_primary_strategy() = 0;
};

// All storage/managers/maps/evidence are borrowed. The caller keeps its existing
// database guard, coordinator and publication abandonment guard alive across
// this call. This legacy MODEL seam does not authorize a desk decision/runner.
struct BookTailInputs {
    AppConfig& app_config;
    const double& strategy_max_leverage;
    std::shared_ptr<PostgresDatabase>& db;
    std::string& combined_strategy_id;
    std::string& portfolio_id;
    std::vector<std::string>& strategy_names;
    std::unordered_map<std::string, double>& strategy_allocations;
    StrategyPositionsMap& strategy_positions_map;
    std::unordered_map<std::string, Position>& positions;
    Timestamp& now;
    std::tm*& now_tm;
    int& day_of_week;
    bool& use_override_date;
    bool& send_email;
    bool& skip_strategy_processing;
    double& initial_capital;
    Timestamp& start_date;
    Timestamp& end_date;
    Result<nlohmann::json>& trading_snapshot;
    std::vector<std::string>& symbols;
    std::vector<Bar>& all_bars;
    RiskConfig& risk_config;
    PortfolioConfig& portfolio_config;
    LiveTradingConfig& coordinator_config;
    LiveDataLoader* data_loader;
    LiveMetricsCalculator* metrics_calculator;
    LiveResultsManager* results_manager;
    LivePriceManager* price_manager;
    LivePnLManager* pnl_manager;
    std::unique_ptr<ExecutionManager>& execution_manager;
    std::unique_ptr<MarginManager>& margin_manager;
    std::unique_ptr<CSVExporter>& csv_exporter;
    StrategyInstancesMap& strategy_instances_map;
    RunConsumption& run_consumption;
    PublicationEvidenceToken& evidence_token;
    const char* qt_stream;
};

int run_book_tail(BookTailInputs& inputs, BookTailCallbacks& callbacks);
}  // namespace trade_ngin
