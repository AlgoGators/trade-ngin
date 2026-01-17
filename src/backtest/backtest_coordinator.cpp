#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/backtest/slippage_models.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/run_id_generator.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/storage/backtest_results_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_fast.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace trade_ngin {
namespace backtest {

BacktestCoordinator::BacktestCoordinator(
    std::shared_ptr<PostgresDatabase> db,
    InstrumentRegistry* registry,
    const BacktestCoordinatorConfig& config)
    : config_(config),
      db_(std::move(db)),
      registry_(registry),
      current_portfolio_value_(config.initial_capital) {}

BacktestCoordinator::~BacktestCoordinator() = default;

Result<void> BacktestCoordinator::initialize() {
    if (is_initialized_) {
        return Result<void>();
    }

    // Validate database connection
    auto conn_result = validate_connection();
    if (conn_result.is_error()) {
        return conn_result;
    }

    // Create all components
    auto create_result = create_components();
    if (create_result.is_error()) {
        return create_result;
    }

    is_initialized_ = true;
    INFO("BacktestCoordinator initialized successfully");
    return Result<void>();
}

Result<void> BacktestCoordinator::create_components() {
    // Create data loader
    data_loader_ = std::make_unique<BacktestDataLoader>(db_);

    // Create metrics calculator (stateless)
    metrics_calculator_ = std::make_unique<BacktestMetricsCalculator>();

    // Create price manager
    price_manager_ = std::make_unique<BacktestPriceManager>();

    // Create PnL manager
    pnl_manager_ = std::make_unique<BacktestPnLManager>(config_.initial_capital, *registry_);

    // Create execution manager
    BacktestExecutionConfig exec_config;
    exec_config.commission_rate = config_.commission_rate;
    exec_config.slippage_bps = config_.slippage_bps;
    execution_manager_ = std::make_unique<BacktestExecutionManager>(exec_config);

    // Create portfolio constraints manager
    PortfolioConstraintsConfig constraints_config;
    constraints_config.use_risk_management = config_.use_risk_management;
    constraints_config.use_optimization = config_.use_optimization;
    constraints_config.commission_rate = config_.commission_rate;
    constraints_manager_ = std::make_unique<BacktestPortfolioConstraints>(constraints_config);

    // Initialize slippage model (matching BacktestEngine behavior)
    if (config_.slippage_bps > 0.0) {
        SpreadSlippageConfig slippage_config;
        slippage_config.min_spread_bps = config_.slippage_bps;
        slippage_config.spread_multiplier = 1.2;
        slippage_config.market_impact_multiplier = 1.5;
        slippage_model_ = SlippageModelFactory::create_spread_model(slippage_config);
        INFO("Created SpreadSlippageModel with min_spread_bps=" + std::to_string(config_.slippage_bps));
    }

    return Result<void>();
}

Result<void> BacktestCoordinator::validate_connection() const {
    if (!db_) {
        return make_error<void>(ErrorCode::CONNECTION_ERROR,
            "Database interface is null", "BacktestCoordinator");
    }

    if (!db_->is_connected()) {
        auto connect_result = db_->connect();
        if (connect_result.is_error()) {
            return make_error<void>(connect_result.error()->code(),
                "Failed to connect to database: " +
                    std::string(connect_result.error()->what()),
                "BacktestCoordinator");
        }
    }

    return Result<void>();
}

Result<BacktestResults> BacktestCoordinator::run_single_strategy(
    std::shared_ptr<StrategyInterface> strategy,
    const std::vector<std::string>& symbols,
    const Timestamp& start_date,
    const Timestamp& end_date,
    AssetClass asset_class,
    DataFrequency data_freq) {

    // Ensure initialized
    if (!is_initialized_) {
        auto init_result = initialize();
        if (init_result.is_error()) {
            return make_error<BacktestResults>(init_result.error()->code(),
                init_result.error()->what(), "BacktestCoordinator");
        }
    }

    // Reset state
    reset();

    // Load market data
    DataLoadConfig load_config;
    load_config.symbols = symbols;
    load_config.start_date = start_date;
    load_config.end_date = end_date;
    load_config.asset_class = asset_class;
    load_config.data_freq = data_freq;

    auto data_result = data_loader_->load_market_data(load_config);
    if (data_result.is_error()) {
        return make_error<BacktestResults>(data_result.error()->code(),
            data_result.error()->what(), "BacktestCoordinator");
    }

    auto& all_bars = data_result.value();
    auto grouped_bars = data_loader_->group_bars_by_timestamp(all_bars);

    // Initialize tracking
    std::vector<ExecutionReport> all_executions;
    std::vector<std::pair<Timestamp, double>> equity_curve;
    std::vector<RiskResult> risk_metrics;
    std::map<std::pair<Timestamp, std::string>, double> signals;

    // Process each day
    int day_count = 0;
    for (const auto& [timestamp, bars] : grouped_bars) {
        bool is_warmup = (day_count < config_.warmup_days);

        auto process_result = process_day(
            timestamp, bars, strategy,
            all_executions, equity_curve, risk_metrics, is_warmup);

        if (process_result.is_error()) {
            WARN("Error processing day: " + std::string(process_result.error()->what()));
        }

        day_count++;
    }

    // Calculate final metrics
    auto results = metrics_calculator_->calculate_all_metrics(
        equity_curve, all_executions, config_.warmup_days);

    // Copy executions to results
    results.executions = all_executions;
    results.equity_curve = equity_curve;

    // Copy final positions
    for (const auto& [symbol, pos] : current_positions_) {
        results.positions.push_back(pos);
    }

    INFO("Backtest completed: " + std::to_string(day_count) + " days processed, " +
         std::to_string(all_executions.size()) + " executions");

    return results;
}

Result<BacktestResults> BacktestCoordinator::run_portfolio(
    std::shared_ptr<PortfolioManager> portfolio,
    const std::vector<std::string>& symbols,
    const Timestamp& start_date,
    const Timestamp& end_date,
    AssetClass asset_class,
    DataFrequency data_freq) {

    // Validate portfolio
    if (!portfolio) {
        return make_error<BacktestResults>(ErrorCode::INVALID_ARGUMENT,
            "Null portfolio manager provided for backtest",
            "BacktestCoordinator");
    }

    // Ensure initialized
    if (!is_initialized_) {
        auto init_result = initialize();
        if (init_result.is_error()) {
            return make_error<BacktestResults>(init_result.error()->code(),
                init_result.error()->what(), "BacktestCoordinator");
        }
    }

    // Reset all state
    reset();
    reset_portfolio_state();

    // Store backtest dates for later use in save_portfolio_results_to_db
    backtest_start_date_ = start_date;
    backtest_end_date_ = end_date;

    // Share risk manager with portfolio if available
    if (risk_manager_ && portfolio) {
        portfolio->set_risk_manager(
            std::shared_ptr<RiskManager>(risk_manager_.get(), [](RiskManager*) {}));
    }

    // Disable MarketDataBus publishing during data loading
    INFO("Disabling MarketDataBus publishing during data loading");
    MarketDataBus::instance().set_publish_enabled(false);

    // Load market data
    DataLoadConfig load_config;
    load_config.symbols = symbols;
    load_config.start_date = start_date;
    load_config.end_date = end_date;
    load_config.asset_class = asset_class;
    load_config.data_freq = data_freq;

    auto data_result = data_loader_->load_market_data(load_config);

    // Re-enable publishing
    MarketDataBus::instance().set_publish_enabled(true);
    INFO("Re-enabled MarketDataBus publishing");

    if (data_result.is_error()) {
        return make_error<BacktestResults>(data_result.error()->code(),
            data_result.error()->what(), "BacktestCoordinator");
    }

    auto& all_bars = data_result.value();
    auto grouped_bars = data_loader_->group_bars_by_timestamp(all_bars);

    // Get portfolio config
    const auto& portfolio_config = portfolio->get_config();
    double initial_capital = static_cast<double>(portfolio_config.total_capital);

    // Initialize tracking
    std::vector<ExecutionReport> all_executions;
    std::vector<std::pair<Timestamp, double>> equity_curve;
    std::vector<RiskResult> risk_metrics;

    // Initialize equity curve with starting point
    equity_curve.emplace_back(start_date, initial_capital);

    // Generate run_id for position storage
    std::vector<std::string> strategy_names_for_id;
    for (const auto& strategy : portfolio->get_strategies()) {
        try {
            const auto& metadata = strategy->get_metadata();
            if (!metadata.id.empty()) {
                strategy_names_for_id.push_back(metadata.id);
            } else {
                strategy_names_for_id.push_back("TREND_FOLLOWING");
            }
        } catch (...) {
            strategy_names_for_id.push_back("TREND_FOLLOWING");
        }
    }

    current_run_id_ = generate_portfolio_run_id(strategy_names_for_id, end_date);
    INFO("Generated portfolio backtest run_id: " + current_run_id_);

    // Enable backtest mode on all strategies
    for (auto& strategy : portfolio->get_strategies()) {
        strategy->set_backtest_mode(true);
    }
    INFO("Backtest mode enabled on " + std::to_string(portfolio->get_strategies().size()) + " strategies");

    // Calculate warmup days dynamically from strategy lookbacks
    int calculated_warmup_days = calculate_warmup_days(portfolio->get_strategies());
    INFO("Calculated warmup days from strategies: " + std::to_string(calculated_warmup_days) +
         ", total available days: " + std::to_string(grouped_bars.size()));

    // Track last saved date
    std::string last_saved_date;

    // Process bars in chronological order
    int day_index = 0;
    for (const auto& [timestamp, bars] : grouped_bars) {
        bool is_warmup = (day_index < calculated_warmup_days);

        try {
            auto process_result = process_portfolio_day(
                timestamp, bars, portfolio, all_executions,
                equity_curve, risk_metrics, is_warmup, initial_capital);

            if (process_result.is_error()) {
                WARN("Portfolio data processing failed: " +
                     std::string(process_result.error()->what()));
                // Use previous value for equity curve
                if (!equity_curve.empty()) {
                    equity_curve.emplace_back(timestamp, equity_curve.back().second);
                }
            }
        } catch (const std::exception& e) {
            WARN("Exception processing portfolio data: " + std::string(e.what()));
            if (!equity_curve.empty()) {
                equity_curve.emplace_back(timestamp, equity_curve.back().second);
            }
        }

        // Save positions daily if storage is enabled (skip during warmup)
        if (!is_warmup && config_.store_trade_details && db_ && !bars.empty()) {
            auto time_t = std::chrono::system_clock::to_time_t(timestamp);
            std::stringstream date_ss;
            std::tm time_info;
            core::safe_gmtime(&time_t, &time_info);
            date_ss << std::put_time(&time_info, "%Y-%m-%d");
            std::string current_date = date_ss.str();

            if (current_date != last_saved_date) {
                auto save_result = save_daily_positions(portfolio, current_run_id_, timestamp);
                if (!save_result.is_error()) {
                    last_saved_date = current_date;
                }
            }
        }

        day_index++;
    }

    // Sort executions by timestamp
    std::sort(all_executions.begin(), all_executions.end(),
              [](const ExecutionReport& a, const ExecutionReport& b) {
                  return a.fill_time < b.fill_time;
              });

    // Calculate final metrics
    INFO("Calculating portfolio backtest metrics");
    auto results = metrics_calculator_->calculate_all_metrics(
        equity_curve, all_executions, calculated_warmup_days);
    results.warmup_days = calculated_warmup_days;

    // Add executions and equity curve to results
    results.executions = std::move(all_executions);
    results.equity_curve = std::move(equity_curve);

    // Get final portfolio positions
    try {
        auto portfolio_positions = portfolio->get_portfolio_positions();
        results.positions.reserve(portfolio_positions.size());
        for (const auto& [_, pos] : portfolio_positions) {
            results.positions.push_back(pos);
        }
    } catch (const std::exception& e) {
        WARN("Exception getting final portfolio positions: " + std::string(e.what()));
    }

    INFO("Portfolio backtest completed: " + std::to_string(day_index) + " days processed, " +
         std::to_string(results.executions.size()) + " executions");

    return results;
}

Result<void> BacktestCoordinator::process_day(
    const Timestamp& timestamp,
    const std::vector<Bar>& bars,
    std::shared_ptr<StrategyInterface> strategy,
    std::vector<ExecutionReport>& executions,
    std::vector<std::pair<Timestamp, double>>& equity_curve,
    std::vector<RiskResult>& risk_metrics,
    bool is_warmup) {

    try {
        // BEGINNING-OF-DAY MODEL:
        // 1. Use previous day's bars for signal generation
        // 2. Execute at previous day's close prices

        if (has_previous_bars_) {
            // Pass previous day's bars to strategy for signal generation
            auto data_result = strategy->on_data(previous_bars_);
            if (data_result.is_error()) {
                return data_result;
            }

            // Get new target positions from strategy (unordered_map)
            const auto& strategy_positions = strategy->get_positions();

            // Convert to std::map for execution manager
            std::map<std::string, Position> new_positions(
                strategy_positions.begin(), strategy_positions.end());

            // Generate executions at previous day's close prices
            auto new_executions = execution_manager_->generate_executions(
                current_positions_,
                new_positions,
                price_manager_->get_all_previous_day_prices(),
                bars,
                timestamp);

            // Update current positions
            for (const auto& [symbol, pos] : new_positions) {
                current_positions_[symbol] = pos;
            }

            // Notify strategy of fills
            for (const auto& exec : new_executions) {
                strategy->on_execution(exec);
                executions.push_back(exec);
            }
        }

        // Update prices with today's bars
        price_manager_->update_from_bars(bars);

        // Calculate portfolio value using today's close-to-previous-close PnL
        double portfolio_value = calculate_portfolio_value(current_positions_, bars);
        current_portfolio_value_ = portfolio_value;

        // Update equity curve
        equity_curve.emplace_back(timestamp, portfolio_value);

        // Apply portfolio constraints if enabled (updates current_positions_)
        if (constraints_manager_ &&
            (constraints_manager_->is_risk_management_enabled() ||
             constraints_manager_->is_optimization_enabled())) {
            constraints_manager_->update_historical_returns(bars);
            auto constraint_result = constraints_manager_->apply_constraints(
                bars, current_positions_, risk_metrics);
            if (constraint_result.is_error()) {
                WARN("Constraint application failed: " +
                     std::string(constraint_result.error()->what()));
            }
        }

        // Store previous bars for next iteration
        previous_bars_ = bars;
        has_previous_bars_ = true;

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
            std::string("Error processing day: ") + e.what(),
            "BacktestCoordinator");
    }
}

Result<void> BacktestCoordinator::process_portfolio_day(
    const Timestamp& timestamp,
    const std::vector<Bar>& bars,
    std::shared_ptr<PortfolioManager> portfolio,
    std::vector<ExecutionReport>& executions,
    std::vector<std::pair<Timestamp, double>>& equity_curve,
    std::vector<RiskResult>& risk_metrics,
    bool is_warmup,
    double initial_capital) {

    try {
        // BEGINNING-OF-DAY MODEL FOR PORTFOLIO BACKTEST:
        // - Use previous day's bars for signal generation via PortfolioManager
        // - Use today's bars for executions' slippage/valuation and equity curve

        // Check for empty data
        if (bars.empty()) {
            return make_error<void>(ErrorCode::MARKET_DATA_ERROR,
                "Empty market data provided for portfolio backtest",
                "BacktestCoordinator");
        }

        // If this is the first bar set, initialize previous_bars
        // We still continue processing - matching BacktestEngine behavior
        bool had_previous_bars = portfolio_has_previous_bars_;
        if (!portfolio_has_previous_bars_) {
            portfolio_previous_bars_ = bars;
            portfolio_has_previous_bars_ = true;
        }

        // Update slippage model if available
        if (slippage_model_) {
            for (const auto& bar : bars) {
                try {
                    slippage_model_->update(bar);
                } catch (const std::exception& e) {
                    WARN("Exception updating slippage model: " + std::string(e.what()));
                }
            }
        }

        // Track strategy execution counts BEFORE processing (for commission calculation)
        std::unordered_map<std::string, size_t> strategy_exec_counts_before;
        if (had_previous_bars && !is_warmup) {
            auto strategy_execs_before = portfolio->get_strategy_executions();
            for (const auto& [strategy_id, execs] : strategy_execs_before) {
                strategy_exec_counts_before[strategy_id] = execs.size();
            }
        }

        // Process market data through portfolio manager
        // Use previous day's bars for signal generation
        const auto& bars_for_signals = had_previous_bars ? portfolio_previous_bars_ : bars;

        auto data_result = portfolio->process_market_data(bars_for_signals, is_warmup, timestamp);
        if (data_result.is_error()) {
            return data_result;
        }

        // WARMUP HANDLING: keep equity flat, no executions
        if (is_warmup) {
            // Clear any executions that might have been generated
            portfolio->clear_all_executions();

            // Update previous close prices for first post-warmup day
            std::unordered_map<std::string, double> warmup_closes;
            for (const auto& bar : bars) {
                warmup_closes[bar.symbol] = static_cast<double>(bar.close);
            }
            pnl_manager_->update_previous_closes(warmup_closes);

            // Keep equity flat during warmup
            equity_curve.emplace_back(timestamp, initial_capital);

            // Update previous bars for next iteration
            portfolio_previous_bars_ = bars;
            price_manager_->update_from_bars(bars);

            return Result<void>();
        }

        // POST-WARMUP: Normal trading logic
        std::vector<ExecutionReport> period_executions;

        if (had_previous_bars) {
            try {
                period_executions = portfolio->get_recent_executions();
                portfolio->clear_execution_history();
            } catch (const std::exception& e) {
                WARN("Exception getting recent executions: " + std::string(e.what()));
                period_executions.clear();
            }
        }

        // Apply slippage and transaction costs to executions
        for (auto& exec : period_executions) {
            try {
                exec.fill_time = timestamp;

                // Apply slippage
                if (slippage_model_) {
                    auto symbol_bar = find_bar_for_symbol(bars, exec.symbol);
                    double adjusted_price = slippage_model_->calculate_slippage(
                        static_cast<double>(exec.fill_price),
                        static_cast<double>(exec.filled_quantity),
                        exec.side,
                        symbol_bar);
                    exec.fill_price = Price(adjusted_price);
                } else {
                    // Apply basic slippage model
                    double slip_factor = config_.slippage_bps / 10000.0;
                    exec.fill_price = exec.side == Side::BUY
                        ? Price(static_cast<double>(exec.fill_price) * (1.0 + slip_factor))
                        : Price(static_cast<double>(exec.fill_price) * (1.0 - slip_factor));
                }

                // Calculate and add commission
                exec.commission = Decimal(execution_manager_->calculate_transaction_costs(exec));

                executions.push_back(exec);
            } catch (const std::exception& e) {
                WARN("Exception processing execution for " + exec.symbol + ": " + std::string(e.what()));
            }
        }

        // Feed executions back to strategies
        for (const auto& exec : period_executions) {
            try {
                for (auto strategy_ptr : portfolio->get_strategies()) {
                    auto execution_result = strategy_ptr->on_execution(exec);
                    if (execution_result.is_error()) {
                        WARN("Failed to process execution for strategy: " +
                             execution_result.error()->to_string());
                    }
                }
            } catch (const std::exception& e) {
                WARN("Exception feeding execution to strategies: " + std::string(e.what()));
            }
        }

        // PNL CALCULATION (SINGLE SOURCE OF TRUTH via pnl_manager_)
        double total_portfolio_pnl = 0.0;

        // Build current close prices map from bars
        std::unordered_map<std::string, double> current_close_prices;
        for (const auto& bar : bars) {
            current_close_prices[bar.symbol] = static_cast<double>(bar.close);
        }

        // Calculate commissions from per-strategy executions
        double total_commissions = calculate_period_commissions(portfolio, strategy_exec_counts_before);

        // Calculate PnL for each strategy using its individual quantities
        auto strategy_positions = portfolio->get_strategy_positions();

        for (const auto& [strategy_id, positions_map] : strategy_positions) {
            for (const auto& [symbol, pos] : positions_map) {
                double qty = static_cast<double>(pos.quantity);

                // Skip zero quantity positions
                if (std::abs(qty) < 1e-8) continue;

                // Get current close price
                auto curr_it = current_close_prices.find(symbol);
                if (curr_it == current_close_prices.end()) {
                    continue;
                }
                double current_close = curr_it->second;

                // Check if we have previous close
                if (!pnl_manager_->has_previous_close(symbol)) {
                    pnl_manager_->set_previous_close(symbol, current_close);
                    continue;
                }

                double prev_close = pnl_manager_->get_previous_close(symbol);

                // Calculate PnL using BacktestPnLManager
                auto pnl_result = pnl_manager_->calculate_position_pnl(
                    symbol, qty, prev_close, current_close);

                if (pnl_result.valid) {
                    // Update this strategy's position with calculated PnL
                    Position updated_pos = pos;
                    updated_pos.realized_pnl = Decimal(pnl_result.daily_pnl);
                    updated_pos.unrealized_pnl = Decimal(0.0);

                    auto update_result = portfolio->update_strategy_position(
                        strategy_id, symbol, updated_pos);

                    if (!update_result.is_error()) {
                        total_portfolio_pnl += pnl_result.daily_pnl;
                    }
                }
            }
        }

        // Update previous closes for next iteration
        pnl_manager_->update_previous_closes(current_close_prices);

        // Calculate portfolio value: previous value + daily PnL - commissions
        double portfolio_value = equity_curve.empty() ? initial_capital : equity_curve.back().second;
        portfolio_value += (total_portfolio_pnl - total_commissions);

        // Add to equity curve
        equity_curve.emplace_back(timestamp, portfolio_value);

        // Get risk metrics if enabled
        if (config_.use_risk_management && risk_manager_) {
            try {
                auto portfolio_positions = portfolio->get_portfolio_positions();
                if (!portfolio_positions.empty()) {
                    MarketData market_data = risk_manager_->create_market_data(bars);
                    std::unordered_map<std::string, Position> positions_for_risk(
                        portfolio_positions.begin(), portfolio_positions.end());
                    auto risk_result = risk_manager_->process_positions(positions_for_risk, market_data);
                    if (risk_result.is_ok()) {
                        risk_metrics.push_back(risk_result.value());
                    }
                }
            } catch (const std::exception& e) {
                WARN("Exception calculating risk metrics: " + std::string(e.what()));
            }
        }

        // Update previous_bars for next day
        portfolio_previous_bars_ = bars;
        price_manager_->update_from_bars(bars);

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
            std::string("Error processing portfolio data: ") + e.what(),
            "BacktestCoordinator");
    }
}

void BacktestCoordinator::reset() {
    has_previous_bars_ = false;
    previous_bars_.clear();
    current_positions_.clear();
    current_portfolio_value_ = config_.initial_capital;

    if (price_manager_) {
        price_manager_->reset();
    }
    if (pnl_manager_) {
        pnl_manager_->reset();
    }
    if (execution_manager_) {
        execution_manager_->reset();
    }
    if (constraints_manager_) {
        constraints_manager_->reset();
    }
}

double BacktestCoordinator::calculate_portfolio_value(
    const std::map<std::string, Position>& positions,
    const std::vector<Bar>& bars) {

    // Start with last known portfolio value
    double portfolio_value = current_portfolio_value_;

    // Build price map from bars
    std::unordered_map<std::string, double> current_prices;
    for (const auto& bar : bars) {
        current_prices[bar.symbol] = static_cast<double>(bar.close);
    }

    // Calculate daily PnL for each position
    for (const auto& [symbol, pos] : positions) {
        double quantity = static_cast<double>(pos.quantity);
        if (std::abs(quantity) < 1e-6) continue;

        auto curr_it = current_prices.find(symbol);
        auto prev_result = price_manager_->get_previous_day_price(symbol);

        if (curr_it != current_prices.end() && !prev_result.is_error()) {
            double current_price = curr_it->second;
            double previous_price = prev_result.value();

            // Get point value from PnL manager
            double point_value = pnl_manager_ ?
                pnl_manager_->get_point_value(symbol) : 1.0;

            // Daily PnL = quantity * (current - previous) * point_value
            double daily_pnl = quantity * (current_price - previous_price) * point_value;
            portfolio_value += daily_pnl;
        }
    }

    return portfolio_value;
}

// ========== Portfolio Backtest Helpers ==========

int BacktestCoordinator::calculate_warmup_days(
    const std::vector<std::shared_ptr<StrategyInterface>>& strategies) {
    int max_lookback = 0;

    for (const auto& strat : strategies) {
        if (!strat) {
            continue;
        }

        // Try to cast to TrendFollowingStrategy
        auto trend_following = std::dynamic_pointer_cast<TrendFollowingStrategy>(strat);
        if (trend_following) {
            int strat_lookback = trend_following->get_max_required_lookback();
            max_lookback = std::max(max_lookback, strat_lookback);
            continue;
        }

        // Try to cast to TrendFollowingFastStrategy
        auto trend_following_fast = std::dynamic_pointer_cast<TrendFollowingFastStrategy>(strat);
        if (trend_following_fast) {
            int strat_lookback = trend_following_fast->get_max_required_lookback();
            max_lookback = std::max(max_lookback, strat_lookback);
            continue;
        }

        // For other strategy types, assume 0 (no warmup required)
    }

    return max_lookback;
}

void BacktestCoordinator::reset_portfolio_state() {
    portfolio_has_previous_bars_ = false;
    portfolio_previous_bars_.clear();
    current_run_id_.clear();
}

std::string BacktestCoordinator::generate_portfolio_run_id(
    const std::vector<std::string>& strategy_names,
    const Timestamp& end_date) {
    return RunIdGenerator::generate_portfolio_run_id(strategy_names, end_date);
}

Result<void> BacktestCoordinator::save_daily_positions(
    std::shared_ptr<PortfolioManager> portfolio,
    const std::string& run_id,
    const Timestamp& timestamp) {

    if (!db_) return Result<void>();

    auto strategy_positions = portfolio->get_strategy_positions();
    int total_positions_saved = 0;
    int strategies_with_positions = 0;

    for (const auto& [strategy_id, positions_map] : strategy_positions) {
        if (positions_map.empty()) {
            continue;
        }

        std::vector<Position> positions_vec;
        positions_vec.reserve(positions_map.size());

        for (const auto& [symbol, pos] : positions_map) {
            Position pos_with_date = pos;
            pos_with_date.last_update = timestamp;
            positions_vec.push_back(pos_with_date);
        }

        if (!positions_vec.empty()) {
            std::string composite_run_id = run_id + "|" + strategy_id;
            auto save_result = db_->store_backtest_positions(
                positions_vec,
                composite_run_id,
                config_.portfolio_id,
                "backtest.final_positions");

            if (save_result.is_error()) {
                WARN("Failed to save daily positions for strategy " + strategy_id +
                     ", error: " + std::string(save_result.error()->what()));
            } else {
                total_positions_saved += static_cast<int>(positions_vec.size());
                strategies_with_positions++;
            }
        }
    }

    if (strategies_with_positions > 0) {
        DEBUG("Saved " + std::to_string(total_positions_saved) +
              " positions across " + std::to_string(strategies_with_positions) + " strategies");
    }

    return Result<void>();
}

double BacktestCoordinator::calculate_period_commissions(
    std::shared_ptr<PortfolioManager> portfolio,
    const std::unordered_map<std::string, size_t>& exec_counts_before) {

    double total_commissions = 0.0;

    auto all_strategy_executions = portfolio->get_strategy_executions();

    for (const auto& [strategy_id, execs] : all_strategy_executions) {
        size_t count_before = exec_counts_before.count(strategy_id) > 0
                            ? exec_counts_before.at(strategy_id)
                            : 0;

        // Get only the new executions (those added after count_before)
        for (size_t i = count_before; i < execs.size(); ++i) {
            total_commissions += static_cast<double>(execs[i].commission);
        }
    }

    return total_commissions;
}

std::optional<Bar> BacktestCoordinator::find_bar_for_symbol(
    const std::vector<Bar>& bars,
    const std::string& symbol) {

    for (const auto& bar : bars) {
        if (bar.symbol == symbol) {
            return bar;
        }
    }
    return std::nullopt;
}

Result<void> BacktestCoordinator::save_portfolio_results_to_db(
    const BacktestResults& results,
    const std::vector<std::string>& strategy_names,
    const std::unordered_map<std::string, double>& strategy_allocations,
    std::shared_ptr<PortfolioManager> portfolio,
    const nlohmann::json& portfolio_config) const {

    if (!config_.store_trade_details) {
        return Result<void>();
    }

    INFO("Using BacktestResultsManager for portfolio-level storage");

    auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db_);
    if (!db_ptr) {
        ERROR("Database is not a PostgresDatabase instance");
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                               "Invalid database type", "BacktestCoordinator");
    }

    // Use the run_id from daily position storage if available, otherwise generate a new one
    std::string portfolio_run_id;
    if (!current_run_id_.empty()) {
        portfolio_run_id = current_run_id_;
        INFO("Using run_id from daily position storage: " + portfolio_run_id);
    } else {
        // Fallback: Generate portfolio run_id (combined strategy names)
        portfolio_run_id = RunIdGenerator::generate_portfolio_run_id(
            strategy_names, std::chrono::system_clock::now());
        INFO("Generated new portfolio run_id: " + portfolio_run_id);
    }

    // Create results manager for portfolio-level storage
    auto results_manager = std::make_unique<BacktestResultsManager>(
        db_ptr,
        config_.store_trade_details,
        portfolio_run_id,
        config_.portfolio_id
    );

    // Set metadata with portfolio configuration
    nlohmann::json hyperparameters;
    hyperparameters["initial_capital"] = config_.initial_capital;
    hyperparameters["commission_rate"] = config_.commission_rate;
    hyperparameters["slippage_bps"] = config_.slippage_bps;
    hyperparameters["use_risk_management"] = config_.use_risk_management;
    hyperparameters["use_optimization"] = config_.use_optimization;
    hyperparameters["portfolio_config"] = portfolio_config;

    // Use the actual backtest start/end dates that were stored in run_portfolio()
    results_manager->set_metadata(
        backtest_start_date_,
        backtest_end_date_,
        hyperparameters,
        "Portfolio Backtest Run: " + portfolio_run_id,
        "Multi-strategy portfolio backtest"
    );

    // Set performance metrics (portfolio-level)
    std::unordered_map<std::string, double> metrics = {
        {"total_return", results.total_return},
        {"sharpe_ratio", results.sharpe_ratio},
        {"sortino_ratio", results.sortino_ratio},
        {"max_drawdown", results.max_drawdown},
        {"calmar_ratio", results.calmar_ratio},
        {"volatility", results.volatility},
        {"total_trades", static_cast<double>(results.total_trades)},
        {"win_rate", results.win_rate},
        {"profit_factor", results.profit_factor},
        {"avg_win", results.avg_win},
        {"avg_loss", results.avg_loss},
        {"max_win", results.max_win},
        {"max_loss", results.max_loss},
        {"avg_holding_period", results.avg_holding_period},
        {"var_95", results.var_95},
        {"cvar_95", results.cvar_95},
        {"beta", results.beta},
        {"correlation", results.correlation},
        {"downside_volatility", results.downside_volatility}
    };
    results_manager->set_performance_metrics(metrics);

    // Set portfolio-level equity curve
    std::vector<std::pair<Timestamp, double>> equity_points;
    for (const auto& [timestamp, equity] : results.equity_curve) {
        equity_points.push_back({timestamp, equity});
    }
    results_manager->set_equity_curve(equity_points);

    // Collect per-strategy executions from PortfolioManager
    if (portfolio) {
        auto strategy_executions_map = portfolio->get_strategy_executions();

        // Process each strategy - only save executions, not positions (positions saved daily)
        for (const auto& [strategy_id, executions] : strategy_executions_map) {
            results_manager->set_strategy_executions(strategy_id, executions);
        }

        INFO("Skipping final positions save - positions already saved daily during backtest");
    }

    // Save portfolio-level results (summary, equity curve)
    auto save_result = results_manager->save_all_results(portfolio_run_id, backtest_end_date_);

    if (save_result.is_error()) {
        ERROR("Failed to save portfolio results: " + std::string(save_result.error()->what()));
        return save_result;
    }

    // Save per-strategy executions
    auto executions_result = results_manager->save_strategy_executions(portfolio_run_id);
    if (executions_result.is_error()) {
        WARN("Failed to save strategy executions: " + std::string(executions_result.error()->what()));
        // Non-fatal, continue
    }

    // Save per-strategy metadata
    auto metadata_result = results_manager->save_strategy_metadata(
        portfolio_run_id, strategy_allocations, portfolio_config);
    if (metadata_result.is_error()) {
        WARN("Failed to save strategy metadata: " + std::string(metadata_result.error()->what()));
        // Non-fatal, continue
    }

    INFO("Successfully saved portfolio backtest results");
    return Result<void>();
}

} // namespace backtest
} // namespace trade_ngin
