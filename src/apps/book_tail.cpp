// STAGED: mechanical extraction only; no live integration or parity claim.
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/apps/qt_processed_report.hpp"
#include "trade_ngin/apps/live_runtime_invocation.hpp"
#include "trade_ngin/apps/consumption_projection.hpp"
#include "trade_ngin/apps/run_consumption.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/git_version.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/csv_exporter.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"
#include "trade_ngin/live/live_metrics_calculator.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/live/live_price_manager.hpp"
#include "trade_ngin/live/live_trading_coordinator.hpp"
#include "trade_ngin/live/margin_manager.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"

#include "trade_ngin/apps/book_tail.hpp"

using namespace trade_ngin;

namespace {
/// A ratio for a log line, or "n/a". Sharpe, Sortino and profit factor are all
/// empty where their denominator is zero, and std::to_string cannot say that.
std::string show_optional(const std::optional<double>& value) {
    return value ? std::to_string(*value) : std::string("n/a");
}
}  // namespace

int trade_ngin::run_book_tail(BookTailInputs& inputs, BookTailCallbacks& callbacks) {
    auto& app_config = inputs.app_config;
    auto& db = inputs.db;
    auto& combined_strategy_id = inputs.combined_strategy_id;
    auto& portfolio_id = inputs.portfolio_id;
    auto& strategy_names = inputs.strategy_names;
    auto& strategy_allocations = inputs.strategy_allocations;
    auto& strategy_positions_map = inputs.strategy_positions_map;
    auto& positions = inputs.positions;
    auto& now = inputs.now;
    auto& now_tm = inputs.now_tm;
    auto& day_of_week = inputs.day_of_week;
    auto& use_override_date = inputs.use_override_date;
    auto& send_email = inputs.send_email;
    auto& skip_strategy_processing = inputs.skip_strategy_processing;
    auto& initial_capital = inputs.initial_capital;
    auto& start_date = inputs.start_date;
    auto& end_date = inputs.end_date;
    auto& trading_snapshot = inputs.trading_snapshot;
    auto& symbols = inputs.symbols;
    auto& all_bars = inputs.all_bars;
    auto& risk_config = inputs.risk_config;
    auto& portfolio_config = inputs.portfolio_config;
    auto& coordinator_config = inputs.coordinator_config;
    auto& data_loader = inputs.data_loader;
    auto& metrics_calculator = inputs.metrics_calculator;
    auto& results_manager = inputs.results_manager;
    auto& price_manager = inputs.price_manager;
    auto& pnl_manager = inputs.pnl_manager;
    auto& execution_manager = inputs.execution_manager;
    auto& margin_manager = inputs.margin_manager;
    auto& csv_exporter = inputs.csv_exporter;
    auto& strategy_instances_map = inputs.strategy_instances_map;
    auto& run_consumption = inputs.run_consumption;
    auto& evidence_token = inputs.evidence_token;
    const char* const QT_STREAM = inputs.qt_stream;
        // Load previous day positions for PnL calculation
        INFO("Loading previous day positions for PnL calculation...");
        auto previous_date = now - std::chrono::hours(24);
        auto previous_positions_result =
            db->load_positions_by_date(combined_strategy_id, "", coordinator_config.portfolio_id,
                                       previous_date, "trading.positions", QT_STREAM);
        std::unordered_map<std::string, Position> previous_positions;

        if (previous_positions_result.is_ok()) {
            previous_positions = previous_positions_result.value();
            INFO("Loaded " + std::to_string(previous_positions.size()) + " previous day positions");
        } else {
            INFO("No previous day positions found (first run or no data): " +
                 std::string(previous_positions_result.error()->what()));
        }

        INFO("DEBUG: Previous date used for lookup: " +
             std::to_string(std::chrono::system_clock::to_time_t(previous_date)));
        INFO("DEBUG: Current date: " + std::to_string(std::chrono::system_clock::to_time_t(now)));
        INFO("DEBUG: Previous positions loaded: " + std::to_string(previous_positions.size()));
        for (const auto& [symbol, pos] : previous_positions) {
            INFO("DEBUG: Previous position - " + symbol + ": " +
                 std::to_string(pos.quantity.as_double()));
        }

        // Get market prices from PriceManager - already extracted from bars
        INFO("Getting market prices for PnL lag model from PriceManager...");

        // PriceManager has already extracted T-1 and T-2 prices from bars
        // Make copies since we need to use [] operator in many places
        std::unordered_map<std::string, double> previous_day_close_prices =
            price_manager->get_all_previous_day_prices();
        std::unordered_map<std::string, double> two_days_ago_close_prices =
            price_manager->get_all_two_days_ago_prices();

        INFO("Retrieved prices from PriceManager: " +
             std::to_string(previous_day_close_prices.size()) + " Day T-1, " +
             std::to_string(two_days_ago_close_prices.size()) + " Day T-2");

        // ========================================
        // MONDAY AGRICULTURAL FUTURES FIX
        // Agricultural futures don't trade Sunday evening, so on Monday they have
        // the same rolling window problem as other futures have on Sunday.
        // For agricultural symbols missing T-1 prices on Monday, reuse previous positions.
        // ========================================
        bool is_monday = (day_of_week == 1);

        // Agricultural futures that don't trade Sunday evening
        const std::set<std::string> AGRICULTURAL_FUTURES_BASE = {
            "ZC", "ZS", "ZW", "ZL", "ZM", "KE", "ZR",  // Grains
            "LE", "HE", "GF"                           // Livestock
        };

        // Helper lambda to check if a symbol is an agricultural future
        auto is_agricultural_future =
            [&AGRICULTURAL_FUTURES_BASE](const std::string& symbol) -> bool {
            // Extract base symbol (e.g., "ZC.v.0" -> "ZC")
            std::string base = symbol;
            auto dot_pos = symbol.find('.');
            if (dot_pos != std::string::npos) {
                base = symbol.substr(0, dot_pos);
            }
            return AGRICULTURAL_FUTURES_BASE.count(base) > 0;
        };

        if (is_monday && !skip_strategy_processing) {
            INFO("═══════════════════════════════════════════════════════════════");
            INFO("MONDAY AGRICULTURAL FUTURES CHECK");
            INFO("Agricultural futures don't trade Sunday - checking for missing T-1 prices");
            INFO("═══════════════════════════════════════════════════════════════");

            int ag_symbols_fixed = 0;

            // For each strategy, check agricultural symbols
            for (auto& [strategy_name, current_positions_map] : strategy_positions_map) {
                // Load previous positions for this strategy
                auto prev_strategy_result = db->load_positions_by_date(
                    combined_strategy_id, strategy_name, coordinator_config.portfolio_id,
                    previous_date, "trading.positions", QT_STREAM);

                std::unordered_map<std::string, Position> prev_strategy_positions;
                if (prev_strategy_result.is_ok()) {
                    prev_strategy_positions = prev_strategy_result.value();
                }

                // Check each position in the current strategy
                for (auto& [symbol, current_pos] : current_positions_map) {
                    if (is_agricultural_future(symbol)) {
                        // Check if T-1 price is missing for this symbol
                        bool has_t1_price = previous_day_close_prices.find(symbol) !=
                                            previous_day_close_prices.end();

                        if (!has_t1_price) {
                            // This agricultural future has no Sunday data
                            // Reuse Friday's position to avoid phantom execution
                            auto prev_it = prev_strategy_positions.find(symbol);
                            if (prev_it != prev_strategy_positions.end()) {
                                double prev_qty = prev_it->second.quantity.as_double();
                                double curr_qty = current_pos.quantity.as_double();

                                if (std::abs(curr_qty - prev_qty) > 1e-10) {
                                    INFO("Monday fix for " + symbol + " (" + strategy_name +
                                         "): " + "No Sunday data - reverting position from " +
                                         std::to_string(curr_qty) + " to " +
                                         std::to_string(prev_qty) + " (Friday's position)");

                                    // Override with previous position
                                    current_pos = prev_it->second;
                                    current_pos.last_update = now;  // Update timestamp
                                    ag_symbols_fixed++;
                                }
                            } else {
                                // No previous position exists - keep current (likely first time)
                                INFO("Monday check for " + symbol + " (" + strategy_name + "): " +
                                     "No Sunday data and no previous position - keeping current");
                            }
                        }
                    }
                }

                // Also update the combined positions map
                for (const auto& [symbol, pos] : current_positions_map) {
                    if (is_agricultural_future(symbol) &&
                        previous_day_close_prices.find(symbol) == previous_day_close_prices.end()) {
                        positions[symbol] = pos;
                    }
                }
            }

            if (ag_symbols_fixed > 0) {
                INFO("Monday agricultural fix: Reverted " + std::to_string(ag_symbols_fixed) +
                     " positions to Friday's values (no Sunday trading data)");
            } else {
                INFO("Monday agricultural check complete: No position reversions needed");
            }
            INFO("═══════════════════════════════════════════════════════════════");
        }

        // Verify we have prices for all required symbols
        std::set<std::string> all_symbols;
        for (const auto& [symbol, position] : positions) {
            if (position.quantity.as_double() != 0.0) {
                all_symbols.insert(symbol);
            }
        }
        for (const auto& [symbol, position] : previous_positions) {
            all_symbols.insert(symbol);
        }

        for (const auto& symbol : all_symbols) {
            if (previous_day_close_prices.find(symbol) == previous_day_close_prices.end()) {
                // On Monday, agricultural futures missing T-1 is expected (handled above)
                if (is_monday && is_agricultural_future(symbol)) {
                    INFO("Expected: Missing T-1 price for agricultural future " + symbol +
                         " on Monday");
                } else {
                    WARN("Missing T-1 price for symbol: " + symbol);
                }
            }
            if (two_days_ago_close_prices.find(symbol) == two_days_ago_close_prices.end() &&
                previous_positions.find(symbol) != previous_positions.end()) {
                WARN("Missing T-2 price for symbol: " + symbol + " (needed for PnL finalization)");
            }
        }

        // ========================================
        // PHASE 5: PER-STRATEGY DAY T-1 FINALIZATION
        // Finalize previous day positions FOR EACH STRATEGY
        // ========================================
        INFO("PHASE 5: Finalizing Day T-1 PnL per-strategy using PnLManager...");

        // Check if we have T-1 price data for finalization
        if (previous_day_close_prices.empty() && !previous_positions.empty()) {
            WARN(
                "No T-1 close prices available (likely weekend/holiday) - all positions will have "
                "0 PnL");
            INFO("This is expected behavior when Day T-1 (" +
                 std::to_string(std::chrono::system_clock::to_time_t(previous_date)) +
                 ") was a non-trading day");
        }

        INFO("PnLManager initialized with InstrumentRegistry access");

        double aggregate_yesterday_total_pnl = 0.0;

        const bool finalize_prior_day = !two_days_ago_close_prices.empty() && pnl_manager;
        run_consumption.pnl_path_decision_reached = true;
        run_consumption.pnl_path_eligible = finalize_prior_day;
        if (finalize_prior_day) {
            INFO("Finalizing Day T-1 positions per-strategy...");

            // Finalize for each strategy separately
            run_consumption.pnl_loop.reached = true;
            for (const auto& [strategy_name, current_positions_map] : strategy_positions_map) {
                // Load previous day positions for THIS strategy
                // Filter by BOTH combined_strategy_id AND individual strategy_name
                // to ensure we only get positions from this specific run
                auto prev_strategy_positions_result =
                    db->load_positions_by_date(combined_strategy_id,  // Combined strategy_id
                                               strategy_name,         // Individual strategy_name
                                               coordinator_config.portfolio_id,  // Portfolio ID
                                               previous_date, "trading.positions", QT_STREAM);

                if (prev_strategy_positions_result.is_error()) {
                    INFO("No previous positions found for strategy " + strategy_name +
                         " (first run or no data): " +
                         std::string(prev_strategy_positions_result.error()->what()));
                    continue;  // Skip this strategy
                }

                auto prev_strategy_positions_map = prev_strategy_positions_result.value();

                if (prev_strategy_positions_map.empty()) {
                    INFO("No previous positions to finalize for strategy: " + strategy_name);
                    continue;
                }

                INFO("DEBUG PHASE 5: Strategy '" + strategy_name + "' has " +
                     std::to_string(prev_strategy_positions_map.size()) +
                     " previous day positions to finalize");

                // Convert map to vector for PnLManager
                std::vector<Position> prev_positions_vec;
                prev_positions_vec.reserve(prev_strategy_positions_map.size());
                for (const auto& [symbol, pos] : prev_strategy_positions_map) {
                    prev_positions_vec.push_back(pos);
                }

                // Get this strategy's allocation for capital calculation
                double strategy_allocation = 1.0;  // Default to full allocation
                bool allocation_found = false;
                if (strategy_allocations.find(strategy_name) != strategy_allocations.end()) {
                    allocation_found = true;
                    strategy_allocation = strategy_allocations[strategy_name];
                }
                double strategy_capital = initial_capital * strategy_allocation;
                auto* pnl_evidence = run_consumption.append_pnl(strategy_name);
                if (pnl_evidence) {
                    pnl_evidence->record_operands(allocation_found, strategy_allocation,
                                                  initial_capital, strategy_capital);
                }

                // Use PnLManager to finalize previous day for this strategy
                auto finalization_result = invoke_run_result(
                    pnl_evidence ? &pnl_evidence->call : nullptr,
                    [&](RunEmptyObservation*) {
                        return pnl_manager->finalize_previous_day(prev_positions_vec,
                                                       previous_day_close_prices,  // T-1 prices
                                                       two_days_ago_close_prices,  // T-2 prices
                                                       strategy_capital,
                                                       0.0  // Commissions (will be handled later)
                        );
                    });

                if (finalization_result.is_ok()) {
                    auto& result = finalization_result.value();
                    double strategy_yesterday_pnl = result.finalized_daily_pnl;
                    aggregate_yesterday_total_pnl += strategy_yesterday_pnl;

                    INFO("DEBUG PHASE 5: Strategy '" + strategy_name +
                         "' finalized Day T-1 PnL: $" + std::to_string(strategy_yesterday_pnl));

                    // Log individual position PnLs for this strategy
                    for (const auto& [symbol, pnl] : result.position_realized_pnl) {
                        DEBUG("PHASE 5: " + strategy_name + " - Position " + symbol +
                              " finalized PnL: $" + std::to_string(pnl));
                    }

                    // Store updated positions for yesterday (Day T-1) in database FOR THIS STRATEGY
                    if (!result.finalized_positions.empty()) {
                        auto update_result =
                            db->store_positions(result.finalized_positions,
                                                combined_strategy_id,  // Combined strategy_id
                                                strategy_name,         // Individual strategy_name
                                                portfolio_id,          // Portfolio identifier
                                                "trading.positions");

                        if (update_result.is_error()) {
                            ERROR("Failed to update Day T-1 positions for strategy " +
                                  strategy_name + ": " +
                                  std::string(update_result.error()->what()));
                        } else {
                            INFO("Successfully updated " +
                                 std::to_string(result.finalized_positions.size()) +
                                 " Day T-1 positions with finalized PnL for strategy: " +
                                 strategy_name);
                        }
                    }
                } else {
                    ERROR("PnLManager failed to finalize Day T-1 for strategy " + strategy_name +
                          ": " + std::string(finalization_result.error()->what()));
                }
            }

            run_consumption.pnl_loop.completed = true;
            INFO("PHASE 5: Total finalized Day T-1 PnL across all strategies: $" +
                 std::to_string(aggregate_yesterday_total_pnl));
        } else {
            INFO("Skipping Day T-1 finalization (no two_days_ago prices or no PnLManager)");
        }

        // ========================================
        // STEP 2: CREATE TODAY'S (Day T) POSITIONS WITH ZERO PnL
        // ========================================
        INFO("STEP 2: Creating Day T positions with zero PnL (placeholders)...");

        double total_daily_transaction_costs = 0.0;  // Will be calculated from executions

        // Update all current positions to have:
        // - average_price = Day T-1 close (execution price)
        // - market_price = Day T-1 close (last known price)
        // - realized_pnl = 0 (placeholder, will be finalized tomorrow)
        // - unrealized_pnl = 0 (always 0 for futures)

        for (auto& [symbol, current_position] : positions) {
            // Get Day T-1 close price for this symbol
            double yesterday_close = current_position.average_price.as_double();  // Default
            if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                yesterday_close = previous_day_close_prices[symbol];
            }

            // Set position fields for Day T
            current_position.average_price = Decimal(yesterday_close);  // Entry at Day T-1 close
            current_position.realized_pnl =
                Decimal(0.0);  // PLACEHOLDER - will be finalized tomorrow
            current_position.unrealized_pnl = Decimal(0.0);  // Always 0 for futures
            current_position.last_update = now;              // Today's timestamp

            INFO("Day T position for " + symbol +
                 ": qty=" + std::to_string(current_position.quantity.as_double()) +
                 " entry_price=" + std::to_string(yesterday_close) +
                 " realized_pnl=0 (placeholder)");
        }

        // ========================================
        // PHASE 4: PER-STRATEGY EXECUTIONS GENERATION
        // Generate executions for each strategy based on their position changes
        // Load previous positions per-strategy (Option A)
        // ========================================
        INFO("PHASE 4: Generating per-strategy executions...");

        // Load previous day per-strategy positions
        INFO("DEBUG PHASE 4: Loading previous day positions per-strategy...");
        std::unordered_map<std::string, std::unordered_map<std::string, Position>>
            previous_strategy_positions;

        for (const auto& [strategy_name, _] : strategy_positions_map) {
            // Load previous positions filtering by BOTH combined_strategy_id AND individual
            // strategy_name to ensure we only get positions from this specific run
            auto prev_result =
                db->load_positions_by_date(combined_strategy_id,  // Combined strategy_id
                                           strategy_name,         // Individual strategy_name
                                           coordinator_config.portfolio_id,  // Portfolio ID
                                           previous_date, "trading.positions", QT_STREAM);

            if (prev_result.is_ok()) {
                previous_strategy_positions[strategy_name] = prev_result.value();
                INFO("DEBUG PHASE 4: Loaded " + std::to_string(prev_result.value().size()) +
                     " previous positions for strategy: " + strategy_name);

                // Log individual previous positions for debugging
                for (const auto& [symbol, pos] : prev_result.value()) {
                    DEBUG("DEBUG PHASE 4: Previous " + strategy_name + " - " + symbol +
                          " qty=" + std::to_string(pos.quantity.as_double()));
                }
            } else {
                INFO("No previous positions found for strategy: " + strategy_name +
                     " (first run or no data): " + std::string(prev_result.error()->what()));
                previous_strategy_positions[strategy_name] = {};
            }
        }

        // Generate executions for each strategy
        std::unordered_map<std::string, std::vector<ExecutionReport>> all_strategy_executions;
        int total_executions = 0;
        // total_daily_transaction_costs already declared earlier at line 881

        run_consumption.execution_loop.reached = true;
        for (const auto& [strategy_name, current_positions_map] : strategy_positions_map) {
            auto prev_positions_map = previous_strategy_positions[strategy_name];

            INFO("DEBUG PHASE 4: Generating executions for strategy '" + strategy_name +
                 "' (current=" + std::to_string(current_positions_map.size()) +
                 ", previous=" + std::to_string(prev_positions_map.size()) + ")");

            auto* batch = run_consumption.append_strategy(
                run_consumption.execution_batches, strategy_name);
            auto exec_result = invoke_run_result(batch ? &batch->call : nullptr,
                [&](DailyExecutionObservation* output) {
                    return execution_manager->generate_daily_executions(
                        current_positions_map, prev_positions_map,
                        previous_day_close_prices, now, "system", output);
                });
            run_consumption.observe_native_limits();

            if (exec_result.is_ok()) {
                std::vector<ExecutionReport> strategy_executions = exec_result.value();

                INFO("DEBUG PHASE 4: Strategy '" + strategy_name + "' generated " +
                     std::to_string(strategy_executions.size()) + " executions");

                // Log each execution for debugging
                for (const auto& exec : strategy_executions) {
                    INFO("DEBUG PHASE 4: " + strategy_name + " execution - " + exec.symbol + " " +
                         (exec.side == Side::BUY ? "BUY" : "SELL") + " " +
                         std::to_string(exec.filled_quantity.as_double()) + " @ " +
                         std::to_string(exec.fill_price) + " commission=$" +
                         std::to_string(exec.total_transaction_costs.as_double()));

                    total_daily_transaction_costs += exec.total_transaction_costs.as_double();
                }

                all_strategy_executions[strategy_name] = strategy_executions;
                total_executions += strategy_executions.size();
            } else {
                ERROR("Failed to generate executions for strategy " + strategy_name + ": " +
                      std::string(exec_result.error()->what()));
                all_strategy_executions[strategy_name] = {};
            }
        }
        run_consumption.execution_loop.completed = true;

        INFO("PHASE 4: Total executions across all strategies: " +
             std::to_string(total_executions));
        INFO("PHASE 4: Total daily transaction costs: $" +
             std::to_string(total_daily_transaction_costs));

        // Store executions for each strategy
        for (const auto& [strategy_name, executions] : all_strategy_executions) {
            if (!executions.empty()) {
                // Before inserting, delete any stale executions for today with the same order_ids
                try {
                    // Build unique order_id list
                    std::set<std::string> unique_order_ids;
                    for (const auto& exec : executions) {
                        unique_order_ids.insert(exec.order_id);
                    }

                    if (!unique_order_ids.empty()) {
                        // Convert set to vector for the delete method
                        std::vector<std::string> order_ids_vector(unique_order_ids.begin(),
                                                                  unique_order_ids.end());

                        INFO("Deleting stale executions for strategy " + strategy_name + " with " +
                             std::to_string(order_ids_vector.size()) + " order_ids");

                        // Use the delete_stale_executions method with strategy name
                        auto del_res = db->delete_stale_executions(
                            order_ids_vector, now, strategy_name, "trading.executions");
                        if (del_res.is_error()) {
                            WARN("Failed to delete stale executions for strategy " + strategy_name +
                                 ": " + std::string(del_res.error()->what()));
                        } else {
                            INFO("Stale executions (if any) deleted successfully for strategy: " +
                                 strategy_name);
                        }
                    }
                } catch (const std::exception& e) {
                    WARN("Exception while deleting stale executions for strategy " + strategy_name +
                         ": " + std::string(e.what()));
                }

                // Store executions with combined strategy_id and individual strategy_name
                auto save_result =
                    db->store_executions(executions,
                                         combined_strategy_id,  // Combined strategy_id for tier 2
                                         strategy_name,  // Individual strategy_name for tier 3
                                         portfolio_id,   // Portfolio identifier
                                         "trading.executions");

                if (save_result.is_error()) {
                    ERROR("Failed to store executions for strategy " + strategy_name + ": " +
                          std::string(save_result.error()->what()));
                } else {
                    INFO("Successfully stored " + std::to_string(executions.size()) +
                         " executions for strategy: " + strategy_name);
                }
            } else {
                INFO("No executions to store for strategy: " + strategy_name);
            }
        }

        std::cout << "\n======= Daily Position Report =======" << std::endl;
        std::cout << "Date: " << (now_tm->tm_year + 1900) << "-" << std::setfill('0')
                  << std::setw(2) << (now_tm->tm_mon + 1) << "-" << std::setfill('0')
                  << std::setw(2) << now_tm->tm_mday << std::endl;
        std::cout << "Total Positions: " << positions.size() << std::endl;
        std::cout << std::endl;

        // Add header for position table
        std::cout << std::setw(10) << "Symbol"
                  << " | " << std::setw(10) << "Quantity"
                  << " | " << std::setw(10) << "Mkt Price"
                  << " | " << std::setw(12) << "Notional"
                  << " | " << std::setw(10) << "Unreal PnL" << std::endl;
        std::cout << std::string(60, '-') << std::endl;

        // Use MarginManager for margin calculations
        INFO("Using MarginManager to calculate margin requirements...");

        auto margin_result = margin_manager->calculate_margin_requirements(
            positions, previous_day_close_prices, initial_capital);

        double gross_notional = 0.0;
        double net_notional = 0.0;
        double total_posted_margin = 0.0;  // Sum of per-contract initial margins times contracts
        double maintenance_requirement_today =
            0.0;  // Sum of per-contract maintenance margins times contracts
        int active_positions = 0;

        if (margin_result.is_ok()) {
            auto& metrics = margin_result.value();

            // Recompute notional AND margin from per-strategy positions. See
            // live_portfolio.cpp for the rationale: get_portfolio_positions() returns
            // Σ qᵢ × allocᵢ, double-applying the allocation factor that strategies
            // already account for when sizing. Per-strategy summation restores the
            // additive invariant for both notional and posted margin.
            gross_notional = 0.0;
            net_notional = 0.0;
            total_posted_margin = 0.0;
            maintenance_requirement_today = 0.0;
            int true_active_positions = 0;
            bool recompute_fallback = false;
            for (const auto& [strategy_id, pos_map] : strategy_positions_map) {
                if (recompute_fallback)
                    break;
                for (const auto& [symbol, pos] : pos_map) {
                    double qty = pos.quantity.as_double();
                    if (std::abs(qty) < 1e-6)
                        continue;
                    true_active_positions++;

                    double price = previous_day_close_prices.count(symbol)
                                       ? previous_day_close_prices.at(symbol)
                                       : pos.average_price.as_double();

                    auto notional_result =
                        margin_manager->calculate_position_notional(symbol, qty, price);
                    auto margin_result_per =
                        margin_manager->calculate_position_margin(symbol, qty, price);

                    if (notional_result.is_ok() && margin_result_per.is_ok()) {
                        double signed_notional = notional_result.value();
                        gross_notional += std::abs(signed_notional);
                        net_notional += signed_notional;
                        auto [initial_m, maint_m] = margin_result_per.value();
                        total_posted_margin += initial_m;
                        maintenance_requirement_today += maint_m;
                    } else {
                        WARN("Failed per-strategy notional/margin for " + symbol +
                             " in strategy " + strategy_id +
                             ", falling back to MarginManager combined values");
                        gross_notional = metrics.gross_notional;
                        net_notional = metrics.net_notional;
                        total_posted_margin = metrics.total_posted_margin;
                        maintenance_requirement_today = metrics.maintenance_requirement;
                        recompute_fallback = true;
                        break;
                    }
                }
            }
            active_positions = true_active_positions;

            INFO("Per-strategy recompute: gross=$" + std::to_string(gross_notional) +
                 ", net=$" + std::to_string(net_notional) +
                 ", posted_margin=$" + std::to_string(total_posted_margin) +
                 " (combined was gross=$" + std::to_string(metrics.gross_notional) +
                 ", posted_margin=$" + std::to_string(metrics.total_posted_margin) + ")");
            INFO("MarginManager calculated: gross_notional=$" + std::to_string(gross_notional) +
                 ", posted_margin=$" + std::to_string(total_posted_margin) +
                 ", active_positions=" + std::to_string(active_positions));
        } else {
            ERROR("MarginManager failed: " + std::string(margin_result.error()->what()));
            // No fallback - component is required to work
            throw std::runtime_error("MarginManager failed");
        }

        std::cout << std::endl;
        std::cout << "Active Positions: " << active_positions << std::endl;
        std::cout << "Gross Notional: $" << std::fixed << std::setprecision(2) << gross_notional
                  << std::endl;
        std::cout << "Net Notional: $" << std::fixed << std::setprecision(2) << net_notional
                  << std::endl;
        std::cout << "Gross Leverage: " << std::fixed << std::setprecision(2)
                  << (gross_notional / initial_capital) << "x" << std::endl;
        // Posted margin should never be zero if there are active positions; enforce and warn
        if (active_positions > 0 && total_posted_margin <= 0.0) {
            ERROR(
                "Computed posted margin is non-positive while positions are active. Check "
                "instrument metadata.");
        }
        // Equity-to-Margin Ratio = portfolio_equity / total_posted_margin.
        // Higher = safer (more equity per dollar of margin posted). The prior
        // formula here used gross_notional in the numerator, which is a
        // leverage-to-margin metric, not equity-to-margin. We use
        // initial_capital as the equity proxy (matches MarginManager's
        // gross_leverage convention).
        double equity_to_margin_ratio =
            (total_posted_margin > 0.0) ? (initial_capital / total_posted_margin) : 0.0;
        if (equity_to_margin_ratio <= 1.0 && active_positions > 0) {
            WARN("Equity-to-Margin Ratio is <= 1.0 (account equity at or below "
                 "posted margin); verify margins and sizing.");
        }

        // ========================================
        // PHASE 4: PER-STRATEGY POSITIONS STORAGE
        // Extract per-strategy positions from PortfolioManager
        // Each strategy's positions are stored separately with strategy_name tag
        // strategy_positions_map already extracted above for use by executions section
        // ========================================
        INFO("PHASE 4: Storing per-strategy positions to database...");

        // Store positions for each strategy with strategy_name tag
        int total_positions_saved = 0;
        for (const auto& [strategy_name, positions_map] : strategy_positions_map) {
            std::vector<Position> strategy_positions_vec;
            strategy_positions_vec.reserve(positions_map.size());

            INFO("DEBUG PHASE 4: Strategy '" + strategy_name + "' has " +
                 std::to_string(positions_map.size()) + " positions");

            for (const auto& [symbol, pos] : positions_map) {
                // Explicit zeros are closure evidence and remain part of this snapshot.

                // Create a new position with validated values
                Position validated_position;
                validated_position.symbol = pos.symbol;
                validated_position.quantity = pos.quantity;
                validated_position.last_update = now;  // Use current timestamp

                // CRITICAL: For PnL lag model, Day T positions must have ZERO PnL (placeholders)
                // The PnL will be finalized tomorrow when we run for Day T+1
                // Do NOT use pos.realized_pnl which contains calculated PnL from strategy
                // processing
                validated_position.realized_pnl =
                    Decimal(0.0);  // PLACEHOLDER - will be finalized tomorrow
                validated_position.unrealized_pnl = Decimal(0.0);  // Always 0 for futures

                // For Day T positions, average_price should be Day T-1 close (entry price)
                // This is the price at which positions were "executed" (opened at yesterday's
                // close)
                double avg_price_double =
                    previous_day_close_prices.find(symbol) != previous_day_close_prices.end()
                        ? previous_day_close_prices[symbol]
                        : static_cast<double>(pos.average_price);

                // Decimal limit is approximately 92,233,720,368,547.75807
                const double DECIMAL_MAX = 9.223372036854775807e13;  // INT64_MAX / SCALE
                if (avg_price_double > DECIMAL_MAX || avg_price_double < -DECIMAL_MAX) {
                    WARN("Position " + symbol + " has average_price " +
                         std::to_string(avg_price_double) +
                         " which exceeds Decimal limit, using Day T-1 close instead");
                    // Use Day T-1 close if available
                    if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                        validated_position.average_price =
                            Decimal(previous_day_close_prices[symbol]);
                    } else {
                        validated_position.average_price = Decimal(1.0);
                    }
                } else {
                    try {
                        validated_position.average_price = pos.average_price;
                    } catch (const std::exception& e) {
                        ERROR("Failed to validate average_price for " + symbol + ": " +
                              std::string(e.what()));
                        if (previous_day_close_prices.find(symbol) !=
                            previous_day_close_prices.end()) {
                            validated_position.average_price =
                                Decimal(previous_day_close_prices[symbol]);
                        } else {
                            validated_position.average_price = Decimal(1.0);
                        }
                    }
                }

                strategy_positions_vec.push_back(validated_position);

                INFO("DEBUG PHASE 4: " + strategy_name + " - " + symbol + " qty=" +
                     std::to_string(validated_position.quantity.as_double()) + " avg_price=" +
                     std::to_string(static_cast<double>(validated_position.average_price)) +
                     " realized_pnl=" +
                     std::to_string(static_cast<double>(validated_position.realized_pnl)));
            }

            if (!strategy_positions_vec.empty()) {
                INFO("Attempting to save " + std::to_string(strategy_positions_vec.size()) +
                     " positions for strategy: " + strategy_name);
                DEBUG("Database connection status: " +
                      std::string(db->is_connected() ? "connected" : "disconnected"));

                // Store with combined strategy_id and individual strategy_name
                auto save_result =
                    db->store_positions(strategy_positions_vec,
                                        combined_strategy_id,  // Combined strategy_id for tier 2
                                        strategy_name,  // Individual strategy_name for tier 3
                                        portfolio_id,   // Portfolio identifier
                                        "trading.positions");

                if (save_result.is_error()) {
                    ERROR("Failed to store positions for strategy " + strategy_name + ": " +
                          std::string(save_result.error()->what()));
                } else {
                    INFO("Successfully stored " + std::to_string(strategy_positions_vec.size()) +
                         " positions for strategy: " + strategy_name);
                    total_positions_saved += strategy_positions_vec.size();
                }
            } else {
                INFO("No non-zero positions to store for strategy: " + strategy_name);
            }
        }

        INFO("PHASE 4: Total positions saved across all strategies: " +
             std::to_string(total_positions_saved));

        // Register QT draft seeds against the pending MODEL publication.
        auto proposal_seed_result = seed_qt_proposal_positions(
            *db, combined_strategy_id, strategy_names, portfolio_id, now);
        if (proposal_seed_result.is_error()) {
            ERROR("MODEL_PUBLICATION_BLOCKED: " + std::string(proposal_seed_result.error()->what()));
            return 1;
        }

        // Carry QT state even when a strategy's system book has become flat.
        // A failed seed blocks distribution below, including apparently flat reports.
        auto qt_seed_result = seed_qt_report_positions(
            *db, combined_strategy_id, strategy_names, portfolio_id, now);
        if (qt_seed_result.is_error()) {
            ERROR("INVESTOR_REPORT_BLOCKED: " + std::string(qt_seed_result.error()->what()));
        }

        // Record the non-secret immutable replay snapshot with the final publication.
        const auto run_inputs_row = build_run_inputs_row(TRADE_NGIN_GIT_SHA,
            trading_snapshot.value(), symbols, all_bars, portfolio_config.benchmark_mode,
            start_date, end_date);
        if (db->store_live_run_inputs(combined_strategy_id, portfolio_id, now, run_inputs_row).is_error()) {
            ERROR("Replay snapshot publication refused");
            return 1;
        }

        callbacks.run_counterfactual_book();

        // Compute portfolio-level snapshot metrics using RiskManager on today's state
        INFO("Retrieving strategy metrics...");
        trade_ngin::RiskManager snapshot_rm(risk_config);
        auto market_data_snapshot = snapshot_rm.create_market_data(all_bars);
        run_consumption.diagnostics_reached = true;
        if (run_consumption.collecting()) run_consumption.snapshot_risk.emplace();
        auto risk_eval = invoke_run_result(
            run_consumption.snapshot_risk ? &run_consumption.snapshot_risk.value() : nullptr,
            [&](RiskConfigConsumption* output) {
                return snapshot_rm.process_positions(positions, market_data_snapshot, {}, output);
            });

        std::cout << "\n======= Strategy Metrics =======" << std::endl;
        if (risk_eval.is_ok()) {
            const auto& r = risk_eval.value();
            // Use portfolio_var as annualized volatility proxy
            std::cout << "Volatility: " << std::fixed << std::setprecision(2)
                      << (r.portfolio_var * 100.0) << "%" << std::endl;
            std::cout << "Gross Leverage (Risk): " << std::fixed << std::setprecision(2)
                      << r.gross_leverage << std::endl;
            std::cout << "Net Leverage: " << std::fixed << std::setprecision(2) << r.net_leverage
                      << std::endl;
            std::cout << "Max Correlation: " << std::fixed << std::setprecision(2)
                      << r.correlation_risk << std::endl;
            std::cout << "Jump Risk (99th): " << std::fixed << std::setprecision(2) << r.jump_risk
                      << std::endl;
            std::cout << "Risk Scale: " << std::fixed << std::setprecision(2) << r.recommended_scale
                      << std::endl;
        } else {
            std::cout << "Volatility: N/A" << std::endl;
            std::cout << "Gross Leverage (Risk): N/A" << std::endl;
            std::cout << "Net Leverage: N/A" << std::endl;
            std::cout << "Max Correlation: N/A" << std::endl;
            std::cout << "Jump Risk (99th): N/A" << std::endl;
            std::cout << "Risk Scale: N/A" << std::endl;
        }
        run_consumption.diagnostics_completed = true;
        // ========================================
        // STEP 3: CALCULATE TRANSACTION COSTS AND Day T PnL (ZERO)
        // ========================================
        INFO("STEP 3: Calculating transaction costs and Day T PnL...");

        // total_daily_transaction_costs already calculated in per-strategy executions loop above
        INFO("Total daily transaction costs (from per-strategy executions): $" +
             std::to_string(total_daily_transaction_costs));

        // Day T PnL is ZERO (placeholder) - positions were just opened at Day T-1 close
        // Update PnLManager with today's positions (all with 0 PnL as placeholders)
        for (const auto& [symbol, position] : positions) {
            pnl_manager->update_position_pnl(symbol, 0.0, 0.0);  // Zero PnL for Day T
        }

        double daily_realized_pnl = 0.0;
        double daily_unrealized_pnl = 0.0;
        double daily_pnl_for_today =
            -total_daily_transaction_costs;  // Only transaction costs on Day T

        INFO("Day T PnL (placeholder): $0.00");
        INFO("Day T transaction costs: $" + std::to_string(total_daily_transaction_costs));
        INFO("Day T total impact: $" + std::to_string(daily_pnl_for_today));

        // ========================================
        // STEP 4: UPDATE Day T-1 live_results AND equity_curve WITH FINALIZED PnL
        // ========================================
        // Skip if this is the first trading day (no previous positions to finalize)
        bool is_first_trading_day =
            previous_positions.empty() ||
            (previous_positions.size() > 0 &&
             std::all_of(previous_positions.begin(), previous_positions.end(),
                         [](const auto& p) { return p.second.quantity.as_double() == 0.0; }));

        // Declare yesterday's daily metrics outside the block so they're available for email
        double yesterday_daily_return_for_email = 0.0;
        double yesterday_daily_pnl_for_email = 0.0;
        double yesterday_realized_pnl_for_email = 0.0;
        double yesterday_unrealized_pnl_for_email = 0.0;

        if (!two_days_ago_close_prices.empty() && aggregate_yesterday_total_pnl != 0.0 &&
            !is_first_trading_day) {
            INFO("STEP 4: Updating Day T-1 live_results with finalized PnL: $" +
                 std::to_string(aggregate_yesterday_total_pnl));

            // Get yesterday's transaction costs and other existing metrics from database
            double yesterday_transaction_costs = 0.0;
            double yesterday_gross_notional = 0.0;
            double yesterday_margin_posted = 0.0;

            std::stringstream yesterday_date_ss;
            auto yesterday_time_t = std::chrono::system_clock::to_time_t(previous_date);
            yesterday_date_ss << std::put_time(std::gmtime(&yesterday_time_t), "%Y-%m-%d");

            // Use LiveDataLoader to get yesterday's metrics
            try {
                INFO("Using LiveDataLoader to query yesterday's metrics for date: " +
                     yesterday_date_ss.str());
                auto live_results = data_loader->load_live_results(
                    combined_strategy_id, coordinator_config.portfolio_id, previous_date);

                if (live_results.is_ok()) {
                    auto& row = live_results.value();
                    yesterday_transaction_costs = row.daily_transaction_costs;
                    yesterday_gross_notional = row.gross_notional;
                    yesterday_margin_posted = row.margin_posted;

                    INFO("Successfully loaded yesterday's metrics via LiveDataLoader:");
                    INFO("  yesterday_transaction_costs: $" +
                         std::to_string(yesterday_transaction_costs));
                    INFO("  yesterday_gross_notional: $" +
                         std::to_string(yesterday_gross_notional));
                    INFO("  yesterday_margin_posted: $" + std::to_string(yesterday_margin_posted));
                } else {
                    WARN("LiveDataLoader failed to get yesterday's metrics: " +
                         std::string(live_results.error()->what()));
                    INFO("Using default values (0) for yesterday's metrics");
                }
            } catch (const std::exception& e) {
                WARN("Failed to get yesterday's metrics: " + std::string(e.what()));
            }

            // Use the commission value already loaded from LiveDataLoader
            double yesterday_transaction_costs_for_calc = yesterday_transaction_costs;
            INFO("Using yesterday_transaction_costs_for_calc from LiveDataLoader: $" +
                 std::to_string(yesterday_transaction_costs_for_calc));

            // Use the queried value from earlier (which may be 0 if query failed)
            double yesterday_daily_pnl_finalized =
                aggregate_yesterday_total_pnl - yesterday_transaction_costs;

            INFO("Day T-1 PnL breakdown:");
            INFO("  Position PnL (aggregate_yesterday_total_pnl): $" +
                 std::to_string(aggregate_yesterday_total_pnl));
            INFO("  Transaction costs (yesterday_transaction_costs): $" +
                 std::to_string(yesterday_transaction_costs));
            INFO("  Net PnL (yesterday_daily_pnl_finalized): $" +
                 std::to_string(yesterday_daily_pnl_finalized));

            // Get the day BEFORE yesterday's portfolio value, total_pnl, and
            // total_transaction_costs
            double day_before_yesterday_portfolio_value = initial_capital;
            double day_before_aggregate_yesterday_total_pnl = 0.0;
            double day_before_yesterday_total_transaction_costs = 0.0;
            try {
                auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db);
                if (db_ptr) {
                    auto prev_agg = db_ptr->get_previous_live_aggregates(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date,
                        "trading.live_results");
                    if (prev_agg.is_ok()) {
                        std::tie(day_before_yesterday_portfolio_value,
                                 day_before_aggregate_yesterday_total_pnl,
                                 day_before_yesterday_total_transaction_costs) = prev_agg.value();
                        INFO("Loaded day-before-yesterday aggregates: portfolio=$" +
                             std::to_string(day_before_yesterday_portfolio_value) +
                             ", total_pnl=$" +
                             std::to_string(day_before_aggregate_yesterday_total_pnl) +
                             ", total_transaction_costs=$" +
                             std::to_string(day_before_yesterday_total_transaction_costs));
                    }
                }
            } catch (const std::exception& e) {
                INFO("Could not load day-before-yesterday aggregates: " + std::string(e.what()));
            }

            // Calculate yesterday's cumulative values
            // NOTE: Since we may not have correct transaction costs, the cumulative values will be
            // recalculated by SQL using the daily_pnl formula (daily_realized_pnl -
            // daily_transaction_costs)
            double aggregate_yesterday_total_pnl_cumulative =
                day_before_aggregate_yesterday_total_pnl + yesterday_daily_pnl_finalized;
            double yesterday_total_transaction_costs_cumulative =
                day_before_yesterday_total_transaction_costs + yesterday_transaction_costs;
            double yesterday_total_realized_pnl_cumulative =
                aggregate_yesterday_total_pnl_cumulative +
                yesterday_total_transaction_costs_cumulative;
            double yesterday_portfolio_value_finalized =
                day_before_yesterday_portfolio_value + yesterday_daily_pnl_finalized;

            // Note: Yesterday's metrics for email will be loaded from database after update

            // Calculate yesterday's total cumulative return (non-annualized)
            double yesterday_total_cumulative_return = metrics_calculator->calculate_total_return(
                yesterday_portfolio_value_finalized, initial_capital);

            double yesterday_total_return_decimal = 0.0;
            if (initial_capital > 0.0) {
                yesterday_total_return_decimal =
                    (yesterday_portfolio_value_finalized - initial_capital) / initial_capital;
            }
            double yesterday_total_cumulative_return_pct =
                yesterday_total_cumulative_return;  // Already in %

            // Get trading days count for annualization using PostgreSQL function
            // This avoids issues with row multiplication/duplication in the database
            // Uses trading.strategy_trading_days_metadata table for live_start_date
            int trading_days_count = 1;
            try {
                // Call PostgreSQL function to calculate trading days
                std::string trading_days_query = "SELECT trading.get_trading_days('" +
                                                 combined_strategy_id + "', DATE '" +
                                                 yesterday_date_ss.str() + "')";

                INFO("TRADING_DAYS_CALC [Day T-1]: Querying trading days...");
                INFO("TRADING_DAYS_CALC [Day T-1]: Query: " + trading_days_query);
                INFO("TRADING_DAYS_CALC [Day T-1]: Strategy ID: " + combined_strategy_id);
                INFO("TRADING_DAYS_CALC [Day T-1]: Target Date: " + yesterday_date_ss.str());

                auto trading_days_result = db->execute_query(trading_days_query);

                if (trading_days_result.is_ok()) {
                    auto table = trading_days_result.value();
                    if (table && table->num_rows() > 0 && table->num_columns() > 0) {
                        // execute_query returns StringArray for all columns
                        auto arr = std::static_pointer_cast<arrow::StringArray>(
                            table->column(0)->chunk(0));
                        if (arr && arr->length() > 0 && !arr->IsNull(0)) {
                            trading_days_count = std::max<int>(1, std::stoi(arr->GetString(0)));
                            INFO("TRADING_DAYS_CALC [Day T-1]: Result from DB: " +
                                 std::to_string(trading_days_count) + " trading days");
                            INFO(
                                "TRADING_DAYS_CALC [Day T-1]: This value comes from "
                                "strategy_trading_days_metadata.live_start_date");
                        }
                    }
                } else {
                    WARN("TRADING_DAYS_CALC [Day T-1]: Could not call get_trading_days function: " +
                         std::string(trading_days_result.error()->what()));
                }
            } catch (const std::exception& e) {
                WARN("TRADING_DAYS_CALC [Day T-1]: Failed to get trading days: " +
                     std::string(e.what()));
            }

            // Calculate yesterday's annualized return using LiveMetricsCalculator
            // Formula: annualized_return = ((1 + total_return)^(252/trading_days) - 1) * 100
            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Calculating annualized return...");
            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Input: total_return_decimal = " +
                 std::to_string(yesterday_total_return_decimal) + " (" +
                 std::to_string(yesterday_total_return_decimal * 100.0) + "%)");
            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Input: trading_days_count = " +
                 std::to_string(trading_days_count));
            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Formula: ((1 + " +
                 std::to_string(yesterday_total_return_decimal) + ")^(252/" +
                 std::to_string(trading_days_count) + ") - 1) * 100");

            double yesterday_total_return_annualized =
                metrics_calculator->calculate_annualized_return(yesterday_total_return_decimal,
                                                                trading_days_count);

            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Result: " +
                 std::to_string(yesterday_total_return_annualized) + "%");

            // Calculate yesterday's leverage and risk metrics
            // IMPORTANT: We MUST preserve existing values from the database
            // These were calculated correctly when Day T-1 was originally processed
            double yesterday_gross_leverage = 0.0;
            double yesterday_equity_to_margin_ratio = 0.0;

            // Load existing values from database using LiveDataLoader - DO NOT RECALCULATE
            try {
                auto margin_metrics = data_loader->load_margin_metrics(
                    combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                if (margin_metrics.is_ok() && margin_metrics.value().valid) {
                    auto& metrics = margin_metrics.value();
                    yesterday_gross_leverage = metrics.gross_leverage;
                    yesterday_equity_to_margin_ratio = metrics.equity_to_margin_ratio;

                    // Also update the gross_notional and margin_posted if available
                    yesterday_gross_notional = metrics.gross_notional;
                    yesterday_margin_posted = metrics.margin_posted;

                    INFO("Preserved existing metrics from database via LiveDataLoader: leverage=" +
                         std::to_string(yesterday_gross_leverage) +
                         ", equity_to_margin=" + std::to_string(yesterday_equity_to_margin_ratio) +
                         ", gross_notional=" + std::to_string(yesterday_gross_notional) +
                         ", margin_posted=" + std::to_string(yesterday_margin_posted));
                } else {
                    INFO("No existing margin metrics found for yesterday via LiveDataLoader");
                }
            } catch (const std::exception& e) {
                WARN("Failed to load existing metrics: " + std::string(e.what()));
            }

            // UPDATE yesterday's live_results with ALL recalculated metrics
            // Note: We calculate daily_pnl, total_pnl, and current_portfolio_value in SQL
            // to properly incorporate the EXISTING daily_transaction_costs value
            // IMPORTANT: Only update portfolio_leverage and equity_to_margin_ratio if they are NULL
            // or 0
            std::string update_query =
                "WITH day_before AS ("
                "  SELECT COALESCE(current_portfolio_value, " +
                std::to_string(initial_capital) +
                ") as portfolio, "
                "         COALESCE(total_pnl, 0.0) as total_pnl, "
                "         COALESCE(total_realized_pnl, 0.0) as total_realized_pnl_prev "
                "  FROM trading.live_results "
                "  WHERE portfolio_type = 'system' AND strategy_id = '" +
                combined_strategy_id + "' AND portfolio_id = '" + coordinator_config.portfolio_id +
                "' AND DATE(date) < '" + yesterday_date_ss.str() +
                "' "
                "  ORDER BY date DESC LIMIT 1"
                ") "
                "UPDATE trading.live_results SET "
                "daily_realized_pnl = " +
                std::to_string(aggregate_yesterday_total_pnl) +
                ", "
                "daily_pnl = " +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0), "
                "total_pnl = COALESCE((SELECT total_pnl FROM day_before), 0.0) + (" +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0)), "
                "total_realized_pnl = " +
                std::to_string(yesterday_total_realized_pnl_cumulative) +
                ", "
                "current_portfolio_value = COALESCE((SELECT portfolio FROM day_before), " +
                std::to_string(initial_capital) + ") + (" +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0)), "
                "daily_return = CASE WHEN COALESCE((SELECT portfolio FROM day_before), " +
                std::to_string(initial_capital) +
                ") > 0 "
                "               THEN ((" +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0)) / COALESCE((SELECT portfolio FROM "
                "day_before), " +
                std::to_string(initial_capital) +
                ")) * 100.0 "
                "               ELSE 0.0 END, "
                "total_cumulative_return = " +
                std::to_string(yesterday_total_cumulative_return_pct) +
                ", "
                "total_annualized_return = " +
                std::to_string(yesterday_total_return_annualized) +
                ", "
                "portfolio_leverage = CASE WHEN portfolio_leverage IS NULL OR portfolio_leverage = "
                "0 THEN " +
                std::to_string(yesterday_gross_leverage) +
                " ELSE portfolio_leverage END, "
                "equity_to_margin_ratio = CASE WHEN equity_to_margin_ratio IS NULL OR "
                "equity_to_margin_ratio = 0 THEN " +
                std::to_string(yesterday_equity_to_margin_ratio) +
                " ELSE equity_to_margin_ratio END, "
                "cash_available = COALESCE((SELECT portfolio FROM day_before), " +
                std::to_string(initial_capital) + ") + (" +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0)) - COALESCE(margin_posted, 0.0) "
                "WHERE portfolio_type = 'system' AND strategy_id = '" +
                combined_strategy_id + "' AND portfolio_id = '" + coordinator_config.portfolio_id +
                "' AND DATE(date) = '" + yesterday_date_ss.str() + "'";

            INFO("Executing UPDATE query for Day T-1 live_results...");
            INFO("UPDATE will set current_portfolio_value for date: " + yesterday_date_ss.str());

            auto update_result = db->execute_scoped_live_update(update_query, combined_strategy_id, coordinator_config.portfolio_id);
            if (update_result.is_error()) {
                ERROR("Failed to update Day T-1 live_results: " +
                      std::string(update_result.error()->what()));
            } else {
                INFO(
                    "Successfully updated Day T-1 live_results with finalized PnL and all metrics");

                // Log the expected value
                INFO(
                    "Expected current_portfolio_value calculation: day_before_portfolio + "
                    "(yesterday_pnl - commissions)");
                INFO("  aggregate_yesterday_total_pnl: $" +
                     std::to_string(aggregate_yesterday_total_pnl));
                INFO("  yesterday_transaction_costs: $" +
                     std::to_string(yesterday_transaction_costs_for_calc));
            }

            // UPDATE yesterday's equity_curve using LiveResultsManager
            INFO("Updating Day T-1 equity_curve...");

            // Query the current portfolio value from updated live_results
            std::string get_equity_query =
                "SELECT current_portfolio_value FROM trading.live_results "
                "WHERE portfolio_type = 'system' AND strategy_id = '" +
                combined_strategy_id + "' AND portfolio_id = '" + coordinator_config.portfolio_id +
                "' AND DATE(date) = '" + yesterday_date_ss.str() + "'";

            INFO("Querying for portfolio value with date: " + yesterday_date_ss.str());

            auto equity_result = db->execute_query(get_equity_query);
            if (equity_result.is_error()) {
                ERROR("Failed to get portfolio value for equity update: " +
                      std::string(equity_result.error()->what()));
            } else {
                auto table = equity_result.value();
                INFO("Query returned " + std::to_string(table->num_rows()) + " rows");

                if (table->num_rows() > 0) {
                    // NOTE: execute_query returns StringArray for all columns
                    auto array =
                        std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));

                    // Check for NULL value before reading
                    if (array->IsNull(0)) {
                        ERROR(
                            "Cannot update Day T-1 equity_curve: current_portfolio_value is NULL "
                            "for date " +
                            yesterday_date_ss.str());
                    } else {
                        // Parse string to double
                        double portfolio_value = std::stod(array->GetString(0));
                        INFO("Raw value read from database: " + std::to_string(portfolio_value));

                        // Validate the value before using it
                        if (portfolio_value <= 0.0 || std::isnan(portfolio_value) ||
                            std::isinf(portfolio_value) || portfolio_value < 1000.0) {
                            ERROR("Invalid portfolio value for Day T-1 equity update: " +
                                  std::to_string(portfolio_value) + " (date: " +
                                  yesterday_date_ss.str() + "). Skipping equity_curve update.");
                            ERROR("  Validation failed: <= 0.0? " +
                                  std::string(portfolio_value <= 0.0 ? "YES" : "NO") + ", isnan? " +
                                  std::string(std::isnan(portfolio_value) ? "YES" : "NO") +
                                  ", isinf? " +
                                  std::string(std::isinf(portfolio_value) ? "YES" : "NO") +
                                  ", < 1000? " +
                                  std::string(portfolio_value < 1000.0 ? "YES" : "NO"));
                        } else {
                            INFO("✓ Valid portfolio value for Day T-1: $" +
                                 std::to_string(portfolio_value));

                            // DEBUG: Log the exact timestamp being used for the update
                            auto prev_time_t = std::chrono::system_clock::to_time_t(previous_date);
                            std::stringstream prev_ts_ss;
                            prev_ts_ss
                                << std::put_time(std::gmtime(&prev_time_t), "%Y-%m-%d %H:%M:%S");
                            INFO("DEBUG: previous_date timestamp for equity curve update: " +
                                 prev_ts_ss.str());

                            // DEBUG: Query existing equity_curve timestamp for this date
                            std::string debug_eq_query =
                                "SELECT timestamp, equity FROM trading.equity_curve "
                                "WHERE strategy_id = '" +
                                combined_strategy_id + "' AND portfolio_id = '" +
                                coordinator_config.portfolio_id +
                                "' "
                                "AND DATE(timestamp) = '" +
                                yesterday_date_ss.str() +
                                "' "
                                "ORDER BY timestamp";
                            auto debug_result = db->execute_query(debug_eq_query);
                            if (debug_result.is_ok() && debug_result.value()->num_rows() > 0) {
                                INFO("DEBUG: Existing equity_curve entries for " +
                                     yesterday_date_ss.str() + ":");
                                auto debug_table = debug_result.value();
                                for (int64_t i = 0; i < debug_table->num_rows(); ++i) {
                                    auto ts_arr = std::static_pointer_cast<arrow::StringArray>(
                                        debug_table->column(0)->chunk(0));
                                    // execute_query returns StringArray for all columns
                                    auto eq_arr = std::static_pointer_cast<arrow::StringArray>(
                                        debug_table->column(1)->chunk(0));
                                    if (!ts_arr->IsNull(i) && !eq_arr->IsNull(i)) {
                                        INFO("DEBUG:   Existing row: timestamp=" +
                                             ts_arr->GetString(i) +
                                             ", equity=" + eq_arr->GetString(i));
                                    }
                                }
                            } else {
                                INFO("DEBUG: No existing equity_curve entry found for " +
                                     yesterday_date_ss.str());
                            }

                            // Create a temporary LiveResultsManager for Day T-1 equity update
                            auto yesterday_manager = std::make_unique<LiveResultsManager>(
                                db, true, combined_strategy_id, coordinator_config.portfolio_id);
                            yesterday_manager->set_equity(portfolio_value);

                            auto update_equity_result =
                                yesterday_manager->save_equity_curve(previous_date);
                            if (update_equity_result.is_error()) {
                                ERROR("Failed to update Day T-1 equity_curve: " +
                                      std::string(update_equity_result.error()->what()));
                            } else {
                                INFO("Successfully updated Day T-1 equity_curve with value: " +
                                     std::to_string(portfolio_value));

                                // DEBUG: Verify what was actually saved
                                auto verify_result = db->execute_query(debug_eq_query);
                                if (verify_result.is_ok() &&
                                    verify_result.value()->num_rows() > 0) {
                                    INFO("DEBUG: After update, equity_curve entries for " +
                                         yesterday_date_ss.str() + ":");
                                    auto verify_table = verify_result.value();
                                    for (int64_t i = 0; i < verify_table->num_rows(); ++i) {
                                        auto ts_arr = std::static_pointer_cast<arrow::StringArray>(
                                            verify_table->column(0)->chunk(0));
                                        // execute_query returns StringArray for all columns
                                        auto eq_arr = std::static_pointer_cast<arrow::StringArray>(
                                            verify_table->column(1)->chunk(0));
                                        if (!ts_arr->IsNull(i) && !eq_arr->IsNull(i)) {
                                            INFO("DEBUG:   Row after update: timestamp=" +
                                                 ts_arr->GetString(i) +
                                                 ", equity=" + eq_arr->GetString(i));
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    WARN("No live_results found for date " + yesterday_date_ss.str() +
                         ", skipping equity_curve update");
                }
            }

            // Recalculate historical performance metrics for Day T-1 and update live_results
            try {
                HistoricalMetrics yesterday_hist_metrics;

                if (data_loader && data_loader->is_connected()) {
                    auto returns_hist_res = data_loader->load_daily_returns_history(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                    auto pnl_hist_res = data_loader->load_daily_pnl_history(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                    auto equity_hist_res = data_loader->load_equity_curve_history(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                    auto trades_hist_res = data_loader->load_total_trades_count(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);

                    std::vector<double> returns_hist;
                    std::vector<double> pnl_hist;
                    std::vector<double> equity_hist;
                    int total_trades_hist = 0;

                    if (returns_hist_res.is_ok()) {
                        returns_hist = returns_hist_res.value();
                    }
                    if (pnl_hist_res.is_ok()) {
                        pnl_hist = pnl_hist_res.value();
                    }
                    if (equity_hist_res.is_ok()) {
                        equity_hist = equity_hist_res.value();
                    }
                    if (trades_hist_res.is_ok()) {
                        total_trades_hist = trades_hist_res.value();
                    }

                    LiveHistoricalMetricsCalculator hist_calc;
                    // NOTE: returns_hist is already loaded in PERCENT units (e.g., 0.11 = 0.11%,
                    // not 11%) because daily_return is computed in SQL as `... * 100.0`. Do NOT
                    // multiply by 100 here — that produced a 100x volatility / 100x lower sharpe.
                    yesterday_hist_metrics =
                        hist_calc.calculate(returns_hist, pnl_hist, equity_hist,
                                            yesterday_total_return_annualized, total_trades_hist);

                    // Override total_days with authoritative trading days count
                    yesterday_hist_metrics.total_days = trading_days_count;
                    if (trading_days_count > 0) {
                        yesterday_hist_metrics.win_rate =
                            static_cast<double>(yesterday_hist_metrics.winning_days) /
                            static_cast<double>(trading_days_count) * 100.0;
                    }

                    INFO("HIST_METRICS [Day T-1]: volatility=" +
                         std::to_string(yesterday_hist_metrics.volatility) +
                         " sharpe=" + show_optional(yesterday_hist_metrics.sharpe_ratio) +
                         " winning_days=" + std::to_string(yesterday_hist_metrics.winning_days) +
                         " losing_days=" + std::to_string(yesterday_hist_metrics.losing_days) +
                         " total_days=" + std::to_string(yesterday_hist_metrics.total_days) +
                         " best_day=" + std::to_string(yesterday_hist_metrics.best_day) +
                         " worst_day=" + std::to_string(yesterday_hist_metrics.worst_day) +
                         " avg_win=" + std::to_string(yesterday_hist_metrics.avg_win) +
                         " avg_loss=" + std::to_string(yesterday_hist_metrics.avg_loss) +
                         " gross_profit=" + std::to_string(yesterday_hist_metrics.gross_profit) +
                         " gross_loss=" + std::to_string(yesterday_hist_metrics.gross_loss));

                    std::unordered_map<std::string, double> metric_updates = {
                        {"max_drawdown", yesterday_hist_metrics.max_drawdown},
                        {"volatility", yesterday_hist_metrics.volatility},
                        {"downside_deviation", yesterday_hist_metrics.downside_deviation},
                        {"win_rate", yesterday_hist_metrics.win_rate},
                        {"avg_win", yesterday_hist_metrics.avg_win},
                        {"avg_loss", yesterday_hist_metrics.avg_loss},
                        {"best_day", yesterday_hist_metrics.best_day},
                        {"worst_day", yesterday_hist_metrics.worst_day},
                        {"gross_profit", yesterday_hist_metrics.gross_profit},
                        {"gross_loss", yesterday_hist_metrics.gross_loss},
                        // Note: total_trades column was dropped from trading.live_results;
                        // the count still lives on yesterday_hist_metrics for in-memory use.
                        {"winning_days", static_cast<double>(yesterday_hist_metrics.winning_days)},
                        {"losing_days", static_cast<double>(yesterday_hist_metrics.losing_days)},
                        // flat_days NOT written to DB — column doesn't exist on trading.live_results
                        // and is trivially derivable as total - winning - losing on read.
                        {"total_days", static_cast<double>(yesterday_hist_metrics.total_days)}};

                    // Only written when it is defined. A book with no losing day
                    // has no profit factor, and gross_profit and gross_loss are
                    // both in this same update for anyone who wants to say so.
                    if (yesterday_hist_metrics.sharpe_ratio) {
                        metric_updates["sharpe_ratio"] = *yesterday_hist_metrics.sharpe_ratio;
                    }
                    if (yesterday_hist_metrics.sortino_ratio) {
                        metric_updates["sortino_ratio"] = *yesterday_hist_metrics.sortino_ratio;
                    }
                    if (yesterday_hist_metrics.profit_factor) {
                        metric_updates["profit_factor"] = *yesterday_hist_metrics.profit_factor;
                    }

                    auto yesterday_metrics_manager = std::make_unique<LiveResultsManager>(
                        db, true, combined_strategy_id, coordinator_config.portfolio_id);
                    auto update_metrics_result = yesterday_metrics_manager->update_live_results(
                        previous_date, metric_updates);
                    if (update_metrics_result.is_error()) {
                        WARN("Failed to update historical performance metrics for Day T-1: " +
                             std::string(update_metrics_result.error()->what()));
                    } else {
                        INFO(
                            "Successfully updated historical performance metrics for Day T-1 in "
                            "trading.live_results");
                    }
                } else {
                    WARN(
                        "LiveDataLoader not available or not connected; skipping Day T-1 "
                        "historical metrics update.");
                }
            } catch (const std::exception& e) {
                WARN("Exception while updating historical performance metrics for Day T-1: " +
                     std::string(e.what()));
            }

            // Load updated metrics from database for email - MUST do this AFTER the UPDATE
            try {
                std::string metrics_query =
                    "SELECT daily_return, daily_pnl, daily_realized_pnl, daily_unrealized_pnl, "
                    "portfolio_leverage, equity_to_margin_ratio "
                    "FROM trading.live_results "
                    "WHERE portfolio_type = 'system' AND strategy_id = '" +
                    combined_strategy_id + "' AND portfolio_id = '" +
                    coordinator_config.portfolio_id + "' AND DATE(date) = '" +
                    yesterday_date_ss.str() + "'";

                INFO("Loading yesterday's metrics from database with query: " + metrics_query);
                auto metrics_result = db->execute_query(metrics_query);

                if (metrics_result.is_ok() && metrics_result.value()->num_rows() > 0) {
                    auto table = metrics_result.value();
                    if (table->num_columns() >= 4) {
                        auto daily_return_arr = std::static_pointer_cast<arrow::DoubleArray>(
                            table->column(0)->chunk(0));
                        auto daily_pnl_arr = std::static_pointer_cast<arrow::DoubleArray>(
                            table->column(1)->chunk(0));
                        auto daily_realized_arr = std::static_pointer_cast<arrow::DoubleArray>(
                            table->column(2)->chunk(0));
                        auto daily_unrealized_arr = std::static_pointer_cast<arrow::DoubleArray>(
                            table->column(3)->chunk(0));

                        if (daily_return_arr && daily_return_arr->length() > 0 &&
                            !daily_return_arr->IsNull(0)) {
                            yesterday_daily_return_for_email = daily_return_arr->Value(0);
                            INFO("Loaded yesterday's daily_return: " +
                                 std::to_string(yesterday_daily_return_for_email));
                        }
                        if (daily_pnl_arr && daily_pnl_arr->length() > 0 &&
                            !daily_pnl_arr->IsNull(0)) {
                            yesterday_daily_pnl_for_email = daily_pnl_arr->Value(0);
                            INFO("Loaded yesterday's daily_pnl: " +
                                 std::to_string(yesterday_daily_pnl_for_email));
                        }
                        if (daily_realized_arr && daily_realized_arr->length() > 0 &&
                            !daily_realized_arr->IsNull(0)) {
                            yesterday_realized_pnl_for_email = daily_realized_arr->Value(0);
                            INFO("Loaded yesterday's daily_realized_pnl: " +
                                 std::to_string(yesterday_realized_pnl_for_email));
                        } else {
                            // If daily_realized_pnl is null or 0, use aggregate_yesterday_total_pnl
                            // as fallback
                            yesterday_realized_pnl_for_email = aggregate_yesterday_total_pnl;
                            INFO(
                                "Using calculated aggregate_yesterday_total_pnl as realized PnL: " +
                                std::to_string(yesterday_realized_pnl_for_email));
                        }
                        if (daily_unrealized_arr && daily_unrealized_arr->length() > 0 &&
                            !daily_unrealized_arr->IsNull(0)) {
                            yesterday_unrealized_pnl_for_email = daily_unrealized_arr->Value(0);
                            INFO("Loaded yesterday's daily_unrealized_pnl: " +
                                 std::to_string(yesterday_unrealized_pnl_for_email));
                        }

                        // For futures, unrealized PnL should always be 0, realized PnL is the total
                        // daily PnL
                        yesterday_unrealized_pnl_for_email = 0.0;  // Futures have no unrealized PnL

                        INFO("Successfully loaded yesterday's metrics from database for email");
                    }
                } else {
                    WARN("No metrics found in database for yesterday, using calculated values");
                    // Use the calculated values as fallback
                    yesterday_realized_pnl_for_email = aggregate_yesterday_total_pnl;
                    yesterday_daily_pnl_for_email =
                        aggregate_yesterday_total_pnl;  // For futures, daily PnL = realized PnL
                    yesterday_unrealized_pnl_for_email = 0.0;  // No unrealized for futures
                }
            } catch (const std::exception& e) {
                WARN("Failed to load updated yesterday's metrics: " + std::string(e.what()));
                // Use calculated values as fallback
                yesterday_realized_pnl_for_email = aggregate_yesterday_total_pnl;
                yesterday_daily_pnl_for_email = aggregate_yesterday_total_pnl;
                yesterday_unrealized_pnl_for_email = 0.0;
            }
        } else {
            if (is_first_trading_day) {
                INFO(
                    "Skipping Day T-1 update (first trading day - no previous positions to "
                    "finalize)");
            } else {
                INFO("Skipping Day T-1 live_results update (no two_days_ago prices or zero PnL)");
            }
        }

        // ========================================
        // STEP 5: LOAD UPDATED PREVIOUS DAY AGGREGATES AND CALCULATE Day T CUMULATIVE VALUES
        // ========================================
        INFO(
            "STEP 5: Loading updated previous day aggregates and calculating Day T cumulative "
            "values...");

        // Load previous day's aggregates (portfolio value, total pnl, total transaction costs)
        // This is done AFTER updating Day T-1 live_results to ensure we get the finalized values
        double previous_portfolio_value = initial_capital;  // Default to initial capital
        double previous_total_pnl = 0.0;
        double previous_total_transaction_costs = 0.0;

        try {
            auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db);
            if (db_ptr) {
                auto prev_agg = db_ptr->get_previous_live_aggregates(
                    combined_strategy_id, coordinator_config.portfolio_id, now,
                    "trading.live_results");
                if (prev_agg.is_ok()) {
                    std::tie(previous_portfolio_value, previous_total_pnl,
                             previous_total_transaction_costs) = prev_agg.value();
                    INFO("Loaded updated previous aggregates - portfolio_value: $" +
                         std::to_string(previous_portfolio_value) + ", total_pnl: $" +
                         std::to_string(previous_total_pnl) + ", total_transaction_costs: $" +
                         std::to_string(previous_total_transaction_costs));
                } else {
                    INFO("No previous aggregates found: " + std::string(prev_agg.error()->what()));
                }
            }
        } catch (const std::exception& e) {
            INFO("Could not load previous day aggregates: " + std::string(e.what()));
        }

        // Calculate cumulative values for Day T
        double total_pnl = previous_total_pnl + daily_pnl_for_today;
        double current_portfolio_value = previous_portfolio_value + daily_pnl_for_today;
        double daily_pnl = daily_pnl_for_today;  // Only transaction costs on Day T
        double total_transaction_costs_cumulative =
            previous_total_transaction_costs + total_daily_transaction_costs;

        // Since it's futures, all PnL is realized
        // total_realized_pnl = total_pnl + total_transaction_costs (GROSS)
        double total_realized_pnl = total_pnl + total_transaction_costs_cumulative;
        double total_unrealized_pnl = 0.0;

        // Calculate returns using LiveMetricsCalculator
        double daily_return =
            metrics_calculator->calculate_daily_return(daily_pnl, previous_portfolio_value);

        // Calculate total cumulative return (non-annualized)
        double total_cumulative_return =
            metrics_calculator->calculate_total_return(current_portfolio_value, initial_capital);

        double total_return_decimal = 0.0;
        if (initial_capital > 0.0) {
            total_return_decimal = (current_portfolio_value - initial_capital) / initial_capital;
        }
        double total_cumulative_return_pct = total_cumulative_return;  // Already in %

        // Get n = number of trading days using PostgreSQL function (robust against row duplication)
        // Uses trading.strategy_trading_days_metadata table for live_start_date
        int trading_days_count = 1;  // Default to 1 to avoid division by zero on first day
        try {
            // Format today's date for SQL query
            auto now_time_t_for_query = std::chrono::system_clock::to_time_t(now);
            std::stringstream now_date_ss;
            now_date_ss << std::put_time(std::gmtime(&now_time_t_for_query), "%Y-%m-%d");

            // Call PostgreSQL function to calculate trading days
            std::string trading_days_query = "SELECT trading.get_trading_days('" +
                                             combined_strategy_id + "', DATE '" +
                                             now_date_ss.str() + "')";

            INFO("TRADING_DAYS_CALC [Day T]: Querying trading days...");
            INFO("TRADING_DAYS_CALC [Day T]: Query: " + trading_days_query);
            INFO("TRADING_DAYS_CALC [Day T]: Strategy ID: " + combined_strategy_id);
            INFO("TRADING_DAYS_CALC [Day T]: Target Date: " + now_date_ss.str());

            auto trading_days_result = db->execute_query(trading_days_query);

            if (trading_days_result.is_ok()) {
                auto table = trading_days_result.value();
                if (table && table->num_rows() > 0 && table->num_columns() > 0) {
                    // execute_query returns StringArray for all columns
                    auto arr =
                        std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));
                    if (arr && arr->length() > 0 && !arr->IsNull(0)) {
                        trading_days_count = std::max<int>(1, std::stoi(arr->GetString(0)));
                        INFO("TRADING_DAYS_CALC [Day T]: Result from DB: " +
                             std::to_string(trading_days_count) + " trading days");
                        INFO(
                            "TRADING_DAYS_CALC [Day T]: This value comes from "
                            "strategy_trading_days_metadata.live_start_date");
                    }
                }
            } else {
                WARN("TRADING_DAYS_CALC [Day T]: Could not call get_trading_days function: " +
                     std::string(trading_days_result.error()->what()));
            }
        } catch (const std::exception& e) {
            WARN("TRADING_DAYS_CALC [Day T]: Failed to get trading days: " + std::string(e.what()));
        }

        // Calculate annualized return using LiveMetricsCalculator
        // Formula: annualized_return = ((1 + total_return)^(252/trading_days) - 1) * 100
        INFO("ANNUALIZED_RETURN_CALC [Day T]: Calculating annualized return...");
        INFO("ANNUALIZED_RETURN_CALC [Day T]: Input: total_return_decimal = " +
             std::to_string(total_return_decimal) + " (" +
             std::to_string(total_return_decimal * 100.0) + "%)");
        INFO("ANNUALIZED_RETURN_CALC [Day T]: Input: trading_days_count = " +
             std::to_string(trading_days_count));
        INFO("ANNUALIZED_RETURN_CALC [Day T]: Formula: ((1 + " +
             std::to_string(total_return_decimal) + ")^(252/" + std::to_string(trading_days_count) +
             ") - 1) * 100");

        double total_return_annualized = metrics_calculator->calculate_annualized_return(
            total_return_decimal, trading_days_count);

        INFO("ANNUALIZED_RETURN_CALC [Day T]: Result: " + std::to_string(total_return_annualized) +
             "%");

        INFO("Portfolio value calculation:");
        INFO("  Previous portfolio value: $" + std::to_string(previous_portfolio_value));
        INFO("  Daily PnL: $" + std::to_string(daily_pnl));
        INFO("  Current portfolio value: $" + std::to_string(current_portfolio_value));
        INFO("  Total PnL: $" + std::to_string(total_pnl));
        INFO("  Daily return: " + std::to_string(daily_return) + "%");
        INFO("  Annualized return: " + std::to_string(total_return_annualized) + "%");

        std::cout << "Total P&L: $" << std::fixed << std::setprecision(2) << total_pnl << std::endl;
        std::cout << "Realized P&L: $" << std::fixed << std::setprecision(2) << total_realized_pnl
                  << std::endl;
        std::cout << "Unrealized P&L: $" << std::fixed << std::setprecision(2)
                  << total_unrealized_pnl << std::endl;
        std::cout << "Current Portfolio Value: $" << std::fixed << std::setprecision(2)
                  << current_portfolio_value << std::endl;
        std::cout << "Total Return (Cumulative): " << std::fixed << std::setprecision(2)
                  << total_cumulative_return_pct << "%" << std::endl;
        std::cout << "Total Return (Annualized): " << std::fixed << std::setprecision(2)
                  << total_return_annualized << "%" << std::endl;
        std::cout << "Daily Return: " << std::fixed << std::setprecision(2) << daily_return << "%"
                  << std::endl;
        std::cout << "Gross Leverage: " << std::fixed << std::setprecision(2)
                  << (gross_notional / current_portfolio_value) << "x" << std::endl;
        std::cout << "Posted Margin (Initial×Contracts): $" << std::fixed << std::setprecision(2)
                  << total_posted_margin << std::endl;
        std::cout << "Equity-to-Margin Ratio: " << std::fixed << std::setprecision(2)
                  << equity_to_margin_ratio << "x" << std::endl;
        double margin_cushion = 0.0;
        if (maintenance_requirement_today > 0.0) {
            // Correct formula: margin_cushion = (equity - maintenance) / equity
            // This shows how much cushion we have above maintenance margin requirements
            margin_cushion =
                (current_portfolio_value - maintenance_requirement_today) / current_portfolio_value;
        } else {
            margin_cushion = -1.0;  // Invalid if no maintenance requirement
        }

        // Warnings per thresholds
        if (total_posted_margin > current_portfolio_value) {
            WARN("Posted margin exceeds current portfolio value; check sizing and risk limits.");
        }
        if (margin_cushion < 0.20) {
            WARN("Margin cushion below 20%.");
        }
        // Old WARN fired on `e2m_ratio > 4.0` — sensible only under the prior
        // (buggy) gross_notional/margin formula where high = more leverage.
        // After the formula fix, high e2m means more equity per dollar of
        // posted margin (safer). Replaced with a low-floor alarm.
        if (equity_to_margin_ratio > 0.0 && equity_to_margin_ratio < 1.5) {
            WARN("Equity-to-Margin Ratio below 1.5x (low margin cushion).");
        }

        // Get forecasts for all symbols
        INFO("Retrieving current forecasts...");
        std::cout << "\n======= Current Forecasts =======" << std::endl;
        std::cout << std::setw(10) << "Symbol"
                  << " | " << std::setw(12) << "Forecast"
                  << " | " << std::setw(12) << "Position" << std::endl;
        std::cout << std::string(40, '-') << std::endl;

        // Collect signals for database storage
        std::unordered_map<std::string, double> signals_to_store;

        for (const auto& symbol : symbols) {
            const auto forecast_display = callbacks.get_primary_forecast_display(symbol);
            double forecast = forecast_display.forecast;
            double position = forecast_display.position;

            signals_to_store[symbol] = forecast;

            std::cout << std::setw(10) << symbol << " | " << std::setw(12) << std::fixed
                      << std::setprecision(4) << forecast << " | " << std::setw(12) << std::fixed
                      << std::setprecision(2) << position << std::endl;
        }

        // NOTE: Signals are already stored per-strategy in PHASE 4 above.
        // Do NOT call results_manager->set_signals() here as it would cause duplicate
        // storage with the combined_strategy_id as both strategy_id AND strategy_name,
        // which creates incorrect duplicate entries in the signals table.
        // The signals_to_store map is only used for display purposes above.

        // Save trading results to results table
        INFO("Saving trading results to database...");
        try {
            // Calculate current date for results (use override date if specified)
            auto current_date = now;

            // Use the calculated returns from above
            [[maybe_unused]] double volatility = 0.0;

            // Get volatility from risk evaluation if available
            if (risk_eval.is_ok()) {
                const auto& r = risk_eval.value();
                volatility = r.portfolio_var * 100.0;  // Convert to percentage
            }

            // Create configuration JSON
            nlohmann::json report_config_json;
            report_config_json["strategy_type"] = combined_strategy_id;  // From config (Phase 1)
            report_config_json["capital_allocation"] = initial_capital;
            report_config_json["max_leverage"] = inputs.strategy_max_leverage;
            report_config_json["weight"] = 0.03;      // Default weight
            report_config_json["risk_target"] = 0.2;  // Default risk target
            report_config_json["idm"] = 2.5;          // Default IDM
            report_config_json["active_positions"] = active_positions;
            report_config_json["gross_notional"] = gross_notional;
            report_config_json["net_notional"] = net_notional;
            report_config_json["gross_leverage"] = gross_notional / initial_capital;

            // Create SQL insert for live_results table with correct schema
            std::stringstream date_ss;
            auto time_t = std::chrono::system_clock::to_time_t(current_date);
            date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d %H:%M:%S");

            // Use calculated metrics from position analysis
            double portfolio_var = 0.0;
            double net_leverage = 0.0;
            double max_correlation = 0.0;
            double jump_risk = 0.0;
            double risk_scale = 1.0;

            if (risk_eval.is_ok()) {
                const auto& r = risk_eval.value();
                portfolio_var = r.portfolio_var;
                max_correlation = r.correlation_risk;
                jump_risk = r.jump_risk;
                risk_scale = r.recommended_scale;
            }

            // Use LiveMetricsCalculator for gross leverage (notional / portfolio value)
            double gross_leverage = metrics_calculator->calculate_gross_leverage(
                gross_notional, current_portfolio_value);
            // Net leverage: net_notional / current_portfolio_value (standard definition)
            net_leverage =
                (current_portfolio_value > 0.0) ? (net_notional / current_portfolio_value) : 0.0;
            // equity_to_margin_ratio and margin_cushion already computed above

            // Calculate since-inception performance metrics (Sharpe, Sortino, MaxDD, etc.)
            HistoricalMetrics historical_metrics;
            try {
                if (data_loader && data_loader->is_connected()) {
                    // Use previous_date so history comes from all fully finalized days (<= Day T-1)
                    auto returns_hist_res = data_loader->load_daily_returns_history(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                    auto pnl_hist_res = data_loader->load_daily_pnl_history(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                    auto equity_hist_res = data_loader->load_equity_curve_history(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                    auto trades_hist_res = data_loader->load_total_trades_count(
                        combined_strategy_id, coordinator_config.portfolio_id, current_date);

                    std::vector<double> returns_hist;
                    std::vector<double> pnl_hist;
                    std::vector<double> equity_hist;
                    int total_trades_hist = 0;

                    if (returns_hist_res.is_ok()) {
                        returns_hist = returns_hist_res.value();
                    }
                    if (pnl_hist_res.is_ok()) {
                        pnl_hist = pnl_hist_res.value();
                    }
                    if (equity_hist_res.is_ok()) {
                        equity_hist = equity_hist_res.value();
                    }
                    if (trades_hist_res.is_ok()) {
                        total_trades_hist = trades_hist_res.value();
                    }

                    // Append today's data so metrics are up-to-date as of Day T
                    // Debug: dump raw returns before ×100 conversion
                    {
                        std::string raw_dump = "RETURNS_RAW [Day T]: [";
                        for (size_t i = 0; i < returns_hist.size(); ++i) {
                            if (i > 0)
                                raw_dump += ", ";
                            raw_dump += std::to_string(returns_hist[i]);
                        }
                        raw_dump += "] (size=" + std::to_string(returns_hist.size()) + ")";
                        INFO(raw_dump);
                    }
                    // NOTE: returns_hist is already loaded in PERCENT units (daily_return SQL
                    // computes `... * 100.0`). Do NOT multiply by 100 here — that produced a
                    // 100x volatility / 100x lower sharpe. daily_return for today is also
                    // already in percent (per the SQL UPDATE), so it's appended as-is.
                    returns_hist.push_back(daily_return);
                    pnl_hist.push_back(daily_pnl);
                    equity_hist.push_back(current_portfolio_value);

                    LiveHistoricalMetricsCalculator hist_calc;
                    historical_metrics =
                        hist_calc.calculate(returns_hist, pnl_hist, equity_hist,
                                            total_return_annualized, total_trades_hist);

                    // Keep volatility variable aligned with return-volatility definition
                    volatility = historical_metrics.volatility;

                    // Override total_days with authoritative trading days count from
                    // get_trading_days() DB function, which uses strategy_trading_days_metadata
                    historical_metrics.total_days = trading_days_count;
                    // Recalculate win_rate using actual trading days as denominator
                    if (trading_days_count > 0) {
                        historical_metrics.win_rate =
                            static_cast<double>(historical_metrics.winning_days) /
                            static_cast<double>(trading_days_count) * 100.0;
                    }

                    INFO("HIST_METRICS [Day T]: volatility=" +
                         std::to_string(historical_metrics.volatility) +
                         " sharpe=" + show_optional(historical_metrics.sharpe_ratio) +
                         " winning_days=" + std::to_string(historical_metrics.winning_days) +
                         " losing_days=" + std::to_string(historical_metrics.losing_days) +
                         " total_days=" + std::to_string(historical_metrics.total_days) +
                         " best_day=" + std::to_string(historical_metrics.best_day) +
                         " worst_day=" + std::to_string(historical_metrics.worst_day) +
                         " avg_win=" + std::to_string(historical_metrics.avg_win) +
                         " avg_loss=" + std::to_string(historical_metrics.avg_loss) +
                         " gross_profit=" + std::to_string(historical_metrics.gross_profit) +
                         " gross_loss=" + std::to_string(historical_metrics.gross_loss));
                } else {
                    WARN(
                        "LiveDataLoader not available or not connected; historical performance "
                        "metrics will remain at default values for today.");
                }
            } catch (const std::exception& e) {
                WARN("Exception while calculating historical performance metrics for today: " +
                     std::string(e.what()));
            }

            // Use the LiveResultsManager
            INFO("Setting metrics in LiveResultsManager...");

            // Prepare metrics maps
            std::unordered_map<std::string, double> double_metrics = {
                {"total_cumulative_return", total_cumulative_return_pct},
                {"total_annualized_return", total_return_annualized},
                {"volatility", historical_metrics.volatility},
                {"downside_deviation", historical_metrics.downside_deviation},
                {"max_drawdown", historical_metrics.max_drawdown},
                {"win_rate", historical_metrics.win_rate},
                {"avg_win", historical_metrics.avg_win},
                {"avg_loss", historical_metrics.avg_loss},
                {"best_day", historical_metrics.best_day},
                {"worst_day", historical_metrics.worst_day},
                {"gross_profit", historical_metrics.gross_profit},
                {"gross_loss", historical_metrics.gross_loss},
                {"total_pnl", total_pnl},
                {"total_unrealized_pnl", total_unrealized_pnl},
                {"total_realized_pnl", total_realized_pnl},
                {"current_portfolio_value", current_portfolio_value},
                {"portfolio_var", portfolio_var},
                {"net_leverage", net_leverage},
                {"portfolio_leverage", gross_leverage},
                {"equity_to_margin_ratio", equity_to_margin_ratio},
                {"margin_cushion", margin_cushion},
                {"max_correlation", max_correlation},
                {"jump_risk", jump_risk},
                {"risk_scale", risk_scale},
                {"gross_notional", gross_notional},
                {"net_notional", net_notional},
                {"daily_return", daily_return},
                {"daily_pnl", daily_pnl},
                {"total_transaction_costs", total_transaction_costs_cumulative},
                {"daily_realized_pnl", daily_realized_pnl},
                {"daily_unrealized_pnl", daily_unrealized_pnl},
                {"daily_transaction_costs", total_daily_transaction_costs},
                {"margin_posted", total_posted_margin},
                {"cash_available", current_portfolio_value - total_posted_margin}};

            // Left out of the map when undefined, which stores the column NULL.
            // It used to be written as 999.99, a sentinel that no reader of
            // trading.live_results could tell apart from a real ratio.
            // Left out when undefined, which stores the column NULL. Volatility
            // and downside_deviation go in regardless, so a reader of a NULL
            // ratio has the reason in the same row.
            if (historical_metrics.sharpe_ratio) {
                double_metrics["sharpe_ratio"] = *historical_metrics.sharpe_ratio;
            }
            if (historical_metrics.sortino_ratio) {
                double_metrics["sortino_ratio"] = *historical_metrics.sortino_ratio;
            }
            if (historical_metrics.profit_factor) {
                double_metrics["profit_factor"] = *historical_metrics.profit_factor;
            }

            std::unordered_map<std::string, int> int_metrics = {
                {"active_positions", active_positions},
                // Removed total_trades - will be implemented properly later with closing trades logic
                {"winning_days", historical_metrics.winning_days},
                {"losing_days", historical_metrics.losing_days},
                {"total_days", historical_metrics.total_days}};

            // Set all metrics at once
            results_manager->set_metrics(double_metrics, int_metrics);

            // Set config
            results_manager->set_config(report_config_json);

            // Set equity for equity curve tracking
            results_manager->set_equity(current_portfolio_value);
        } catch (const std::exception& e) {
            ERROR("Exception while saving trading results: " + std::string(e.what()));
            return 1;
        }

        // Phase 4: Use CSVExporter for position export
        INFO("Using CSVExporter to save positions to file...");

        // Note: previously called LiveDataLoader::load_commissions_by_symbol here, but it
        // was dead code (result map was unused; csv_exporter explicitly does
        // (void)symbol_commissions) and the SQL referenced a column that no longer exists.
        // All real transaction-cost data flows from cost_manager_->calculate_costs() at
        // execution time and lives on trading.executions / trading.live_results directly.

        // Current-day writes have only been captured by value so far. The publisher
        // rechecks registry revision and commits positions/limits/results/QT seed
        // together before any report reader observes the new publication.
        if (results_manager->save_all_results(combined_strategy_id, now).is_error()) {
            ERROR("Current-day publication payload could not be completed");
            return 1;
        }
        auto final_consumption = project_run_consumption(run_consumption);
        if (db->attach_live_consumption(evidence_token, final_consumption).is_error()) {
            ERROR("Final consumption attachment refused; publication blocked");
            return 1;
        }
        if (db->publish_live_publication().is_error()) {
            ERROR("Current-day publication rolled back; investor report blocked");
            return 1;
        }

        StrategyPositionRows report_strategy_positions;
        std::unordered_map<std::string, Position> report_positions;
        std::optional<CurrentReportQuantityProjection> report_quantity_display;
        bool reporting_blocked = qt_seed_result.is_error();
        auto report_snapshot = load_qt_investor_report_snapshot(
            *db, combined_strategy_id, strategy_names, portfolio_id, now,
            strategy_positions_map);
        if (report_snapshot.is_error()) {
            reporting_blocked = true;
            ERROR("INVESTOR_REPORT_BLOCKED: " +
                  std::string(report_snapshot.error()->what()));
        } else {
            report_strategy_positions = report_snapshot.value().calculations.by_strategy;
            report_positions = report_snapshot.value().calculations.combined;
            report_quantity_display = report_snapshot.value().display;
        }

        // Export current QT report positions with per-strategy breakdown
        std::string today_filename;
        if (!reporting_blocked) {
            auto current_export_result = csv_exporter->export_current_positions(
                now, report_strategy_positions,
                previous_day_close_prices,  // Market prices (Day T-1 close)
                current_portfolio_value, gross_notional, net_notional, strategy_instances_map,
                true, report_quantity_display ? &*report_quantity_display : nullptr);

            if (current_export_result.is_ok()) {
                today_filename = current_export_result.value();
                INFO("Today's positions saved to " + today_filename);
            } else {
                reporting_blocked = true;
                ERROR("Failed to export current positions: " +
                      std::string(current_export_result.error()->what()));
            }
        }

        // Export yesterday's finalized positions with per-strategy breakdown (if not first trading
        // day)
        std::string yesterday_filename;
        if (!is_first_trading_day && !previous_strategy_positions.empty()) {
            INFO("Exporting yesterday's finalized positions with per-strategy breakdown...");

            auto yesterday_time = now - std::chrono::hours(24);

            auto finalized_export_result = csv_exporter->export_finalized_positions(
                now, yesterday_time, previous_strategy_positions,
                two_days_ago_close_prices,  // Entry prices (T-2)
                previous_day_close_prices   // Exit prices (T-1)
            );

            if (finalized_export_result.is_ok()) {
                yesterday_filename = finalized_export_result.value();
                INFO("Yesterday's finalized positions saved to " + yesterday_filename);
            } else {
                ERROR("Failed to export finalized positions: " +
                      std::string(finalized_export_result.error()->what()));
            }
        }
        callbacks.stop_primary_strategy();

        std::cout << "\n======= Daily Processing Complete =======" << std::endl;
        std::cout << "Today's positions file: " << today_filename << std::endl;
        // Removed yesterday finalized positions file output per request
        // Only show processing time for real-time runs, not historical
        if (!use_override_date) {
            std::cout << "Total processing time: "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now() - now)
                             .count()
                      << "ms" << std::endl;
        }

        INFO("Daily trend following position generation completed successfully");

        // Send email report with trading results (based on send_email flag)
        if (send_email && !reporting_blocked && !today_filename.empty()) {
            INFO("Sending email report...");
            try {
                EmailSenderConfig email_config;
                email_config.smtp_host = app_config.email.smtp_host;
                email_config.smtp_port = app_config.email.smtp_port;
                email_config.username = app_config.email.username;
                email_config.password = app_config.email.password;
                email_config.from_email = app_config.email.from_email;
                email_config.use_tls = app_config.email.use_tls;
                email_config.to_emails = app_config.email.to_emails;

                auto email_sender = std::make_shared<EmailSender>(email_config);
                auto email_init_result = email_sender->initialize();
                if (email_init_result.is_error()) {
                    ERROR("Failed to initialize email sender: " +
                          std::string(email_init_result.error()->what()));
                } else {
                    // Prepare email data
                    std::string date_str =
                        std::to_string(now_tm->tm_year + 1900) + "-" +
                        std::string(2 - std::to_string(now_tm->tm_mon + 1).length(), '0') +
                        std::to_string(now_tm->tm_mon + 1) + "-" +
                        std::string(2 - std::to_string(now_tm->tm_mday).length(), '0') +
                        std::to_string(now_tm->tm_mday);

                    std::string subject = "Daily Trading Report - " + date_str;

                    // Load yesterday's finalized positions for email display
                    std::unordered_map<std::string, Position> yesterday_positions_finalized;
                    std::map<std::string, double> yesterday_daily_metrics_final;
                    std::unordered_map<std::string, double>
                        yesterday_entry_prices;                                     // Day T-2 close
                    std::unordered_map<std::string, double> yesterday_exit_prices;  // Day T-1 close

                    // Calculate yesterday's date for email
                    auto yesterday_time_email = now - std::chrono::hours(24);
                    auto yesterday_time_t_email =
                        std::chrono::system_clock::to_time_t(yesterday_time_email);

                    // Load finalized positions from database for email
                    std::string yesterday_date_for_email;
                    std::ostringstream yss_email;
                    yss_email << std::put_time(std::gmtime(&yesterday_time_t_email), "%Y-%m-%d");
                    yesterday_date_for_email = yss_email.str();

                    INFO("Loading yesterday's finalized positions for email: " +
                         yesterday_date_for_email);

                    std::string positions_query_email =
                        "SELECT symbol, quantity, average_price, daily_realized_pnl, "
                        "daily_unrealized_pnl, last_update "
                        "FROM trading.positions "
                        "WHERE strategy_id = '" +
                        combined_strategy_id + "' AND portfolio_id = '" +
                        coordinator_config.portfolio_id +
                        "' AND DATE(last_update) = "
                        "'" +
                        yesterday_date_for_email + "'";

                    auto positions_result_email = db->execute_query(positions_query_email);

                    if (positions_result_email.is_ok() &&
                        positions_result_email.value()->num_rows() > 0) {
                        auto table_email = positions_result_email.value();
                        // All columns are StringArrays from generic converter
                        auto symbol_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(0)->chunk(0));
                        auto quantity_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(1)->chunk(0));
                        auto avg_price_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(2)->chunk(0));
                        auto realized_pnl_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(3)->chunk(0));

                        for (int64_t i = 0; i < table_email->num_rows(); ++i) {
                            if (!symbol_arr->IsNull(i) && !quantity_arr->IsNull(i)) {
                                std::string symbol = symbol_arr->GetString(i);
                                double quantity = std::stod(quantity_arr->GetString(i));
                                double avg_price = std::stod(avg_price_arr->GetString(i));
                                double realized_pnl = std::stod(realized_pnl_arr->GetString(i));

                                // Skip positions with zero quantity
                                if (std::abs(quantity) < 0.0001)
                                    continue;

                                // Create Position object for yesterday's finalized position
                                Position pos;
                                pos.symbol = symbol;
                                pos.quantity = Decimal(quantity);
                                pos.average_price = Decimal(avg_price);
                                pos.realized_pnl = Decimal(realized_pnl);

                                yesterday_positions_finalized[symbol] = pos;

                                // Populate entry and exit prices
                                if (two_days_ago_close_prices.find(symbol) !=
                                    two_days_ago_close_prices.end()) {
                                    yesterday_entry_prices[symbol] =
                                        two_days_ago_close_prices[symbol];
                                }
                                if (previous_day_close_prices.find(symbol) !=
                                    previous_day_close_prices.end()) {
                                    yesterday_exit_prices[symbol] =
                                        previous_day_close_prices[symbol];
                                }
                            }
                        }
                        INFO("Loaded " + std::to_string(yesterday_positions_finalized.size()) +
                             " finalized positions for email");

                        // Load yesterday's daily metrics from database for accurate display
                        std::string yesterday_metrics_query =
                            "SELECT daily_return, daily_unrealized_pnl, daily_realized_pnl, "
                            "daily_pnl, daily_transaction_costs "
                            "FROM trading.live_results "
                            "WHERE portfolio_type = 'system' AND strategy_id = '" +
                            combined_strategy_id + "' AND portfolio_id = '" +
                            coordinator_config.portfolio_id + "' AND date = '" +
                            yesterday_date_for_email +
                            "' "
                            "ORDER BY date DESC LIMIT 1";

                        INFO("Loading yesterday's daily metrics from live_results: " +
                             yesterday_metrics_query);
                        auto yesterday_metrics_result = db->execute_query(yesterday_metrics_query);

                        if (yesterday_metrics_result.is_ok() &&
                            yesterday_metrics_result.value()->num_rows() > 0) {
                            auto metrics_table = yesterday_metrics_result.value();
                            INFO("Retrieved " + std::to_string(metrics_table->num_rows()) +
                                 " rows from live_results");

                            auto daily_return_arr = std::static_pointer_cast<arrow::StringArray>(
                                metrics_table->column(0)->chunk(0));
                            auto daily_unrealized_arr =
                                std::static_pointer_cast<arrow::StringArray>(
                                    metrics_table->column(1)->chunk(0));
                            auto daily_realized_arr = std::static_pointer_cast<arrow::StringArray>(
                                metrics_table->column(2)->chunk(0));
                            auto daily_total_arr = std::static_pointer_cast<arrow::StringArray>(
                                metrics_table->column(3)->chunk(0));
                            auto daily_commissions_arr =
                                std::static_pointer_cast<arrow::StringArray>(
                                    metrics_table->column(4)->chunk(0));

                            if (!daily_return_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Return"] =
                                    std::stod(daily_return_arr->GetString(0));
                                INFO("Daily Return: " + daily_return_arr->GetString(0));
                            }
                            if (!daily_unrealized_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Unrealized PnL"] =
                                    std::stod(daily_unrealized_arr->GetString(0));
                                INFO("Daily Unrealized PnL: " + daily_unrealized_arr->GetString(0));
                            }
                            if (!daily_realized_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Realized PnL"] =
                                    std::stod(daily_realized_arr->GetString(0));
                                INFO("Daily Realized PnL: " + daily_realized_arr->GetString(0));
                            }
                            if (!daily_total_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Total PnL"] =
                                    std::stod(daily_total_arr->GetString(0));
                                INFO("Daily Total PnL: " + daily_total_arr->GetString(0));
                            }

                            if (!daily_commissions_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Transaction Costs"] =
                                    std::stod(daily_commissions_arr->GetString(0));
                                INFO("Daily Transaction Costs: " +
                                     daily_commissions_arr->GetString(0));
                            }

                            INFO("Successfully loaded yesterday's daily metrics from live_results");
                        } else {
                            if (yesterday_metrics_result.is_error()) {
                                ERROR("Failed to query live_results: " +
                                      std::string(yesterday_metrics_result.error()->what()));
                            } else {
                                WARN("No rows found in live_results for date: " +
                                     yesterday_date_for_email);
                            }
                            // Fallback: calculate from positions if database query fails
                            double yesterday_daily_realized = 0.0;
                            for (const auto& [symbol, pos] : yesterday_positions_finalized) {
                                yesterday_daily_realized += pos.realized_pnl.as_double();
                            }
                            yesterday_daily_metrics_final["Daily Realized PnL"] =
                                yesterday_daily_realized;
                            INFO(
                                "Calculated yesterday's metrics from positions (fallback) - Daily "
                                "Realized PnL: " +
                                std::to_string(yesterday_daily_realized));
                        }

                    } else {
                        INFO("No finalized positions found for yesterday's email table");
                    }

                    // Create strategy metrics map with all relevant metrics organized by category
                    std::map<std::string, double> strategy_metrics;

                    // Load today's stored metrics from live_results (including Sharpe, Sortino,
                    // etc.)
                    LiveResultsRow today_row;
                    bool has_today_row_metrics = false;
                    try {
                        auto today_row_result = data_loader->load_live_results(
                            combined_strategy_id, coordinator_config.portfolio_id, now);
                        if (today_row_result.is_ok()) {
                            today_row = today_row_result.value();
                            has_today_row_metrics = true;
                        } else {
                            WARN("Failed to load today's live_results row for email metrics: " +
                                 std::string(today_row_result.error()->what()));
                        }
                    } catch (const std::exception& e) {
                        WARN("Exception while loading today's live_results for email metrics: " +
                             std::string(e.what()));
                    }

                    // Performance Metrics
                    strategy_metrics["Daily Return"] = daily_return;
                    strategy_metrics["Daily Unrealized PnL"] = daily_unrealized_pnl;
                    strategy_metrics["Daily Realized PnL"] = daily_realized_pnl;
                    strategy_metrics["Daily Total PnL"] = daily_pnl;
                    strategy_metrics["Total Cumulative Return"] = total_cumulative_return_pct;
                    strategy_metrics["Total Annualized Return"] = total_return_annualized;
                    strategy_metrics["Total Unrealized PnL"] = total_unrealized_pnl;
                    strategy_metrics["Total Realized PnL"] = total_realized_pnl;
                    strategy_metrics["Total PnL"] = total_pnl;

                    if (has_today_row_metrics) {
                        // Risk-adjusted and distribution metrics from live_results
                        // Omitted when undefined, so the report shows no row
                        // rather than a confident 0.00.
                        if (today_row.sharpe_ratio) {
                            strategy_metrics["Sharpe Ratio"] = *today_row.sharpe_ratio;
                        }
                        if (today_row.sortino_ratio) {
                            strategy_metrics["Sortino Ratio"] = *today_row.sortino_ratio;
                        }
                        strategy_metrics["Max Drawdown"] = today_row.max_drawdown;
                        strategy_metrics["Volatility"] = today_row.volatility;
                        strategy_metrics["Win Rate"] = today_row.win_rate;
                        strategy_metrics["Average Win"] = today_row.avg_win;
                        strategy_metrics["Average Loss"] = today_row.avg_loss;
                        // Omitted when undefined, so the report shows no Profit
                        // Factor row rather than a confident "999.99".
                        if (today_row.profit_factor) {
                            strategy_metrics["Profit Factor"] = *today_row.profit_factor;
                        }
                        strategy_metrics["Best Day"] = today_row.best_day;
                        strategy_metrics["Worst Day"] = today_row.worst_day;
                        strategy_metrics["Downside Deviation"] = today_row.downside_deviation;
                        strategy_metrics["Gross Profit"] = today_row.gross_profit;
                        strategy_metrics["Gross Loss"] = today_row.gross_loss;
                        // Removed Total Trades - will be implemented properly later with closing trades logic
                        strategy_metrics["Winning Days"] =
                            static_cast<double>(today_row.winning_days);
                        strategy_metrics["Losing Days"] =
                            static_cast<double>(today_row.losing_days);
                        // Flat Days = total - winning - losing (Sat/Sun/holidays w/ zero PnL).
                        // Shown explicitly in email so the total math adds up cleanly.
                        strategy_metrics["Flat Days"] = static_cast<double>(
                            std::max(0, today_row.total_days - today_row.winning_days -
                                            today_row.losing_days));
                        strategy_metrics["Total Days"] = static_cast<double>(today_row.total_days);
                    }
                    // Portfolio VaR is always sourced from the live risk evaluation,
                    // separate from historical volatility stored in live_results
                    if (risk_eval.is_ok()) {
                        strategy_metrics["Portfolio VaR"] = risk_eval.value().portfolio_var * 100.0;
                    }
                    strategy_metrics["Total Transaction Costs"] =
                        total_transaction_costs_cumulative;
                    strategy_metrics["Current Portfolio Value"] = current_portfolio_value;

                    // Leverage Metrics - Calculate values from position analysis
                    double gross_leverage_calc = (current_portfolio_value != 0.0)
                                                     ? (gross_notional / current_portfolio_value)
                                                     : 0.0;
                    double net_leverage_calc = (current_portfolio_value != 0.0)
                                                   ? (net_notional / current_portfolio_value)
                                                   : 0.0;

                    strategy_metrics["Gross Leverage"] = gross_leverage_calc;
                    strategy_metrics["Net Leverage"] = net_leverage_calc;
                    strategy_metrics["Equity-to-Margin Ratio"] = equity_to_margin_ratio;

                    // Risk & Liquidity Metrics
                    strategy_metrics["Margin Cushion"] =
                        margin_cushion * 100.0;  // Convert to percentage
                    strategy_metrics["Margin Posted"] = total_posted_margin;
                    strategy_metrics["Cash Available"] =
                        current_portfolio_value - total_posted_margin;

                    // Note: yesterday_daily_metrics_final is now loaded AFTER database updates
                    // above So we don't need to create it here anymore

                    // Generate email body with is_daily_strategy flag set to true and current
                    // prices. Pass the QT report snapshot and all_strategy_executions for
                    // per-strategy tables.
                    std::string email_body = email_sender->generate_trading_report_body(
                        report_strategy_positions,  // Per-strategy QT report positions
                        report_positions,
                        risk_eval.is_ok() ? std::make_optional(risk_eval.value()) : std::nullopt,
                        strategy_metrics, all_strategy_executions, date_str,
                        portfolio_id,                 // Portfolio name for email header
                        true,                         // is_daily_strategy
                        previous_day_close_prices,    // Pass Day T-1 close prices for today's
                                                      // positions
                        db,                           // Pass database for symbols reference table
                        previous_strategy_positions,  // Per-strategy yesterday's positions for
                                                      // grouped tables
                        yesterday_exit_prices,   // Day T-1 close prices for yesterday's positions
                        yesterday_entry_prices,  // Day T-2 close prices for yesterday's positions
                        yesterday_daily_metrics_final,  // Yesterday's metrics
                        report_quantity_display ? &*report_quantity_display : nullptr
                    );

                    // Send email with CSV attachments: today's positions and yesterday's finalized
                    // (if available)
                    std::vector<std::string> attachments = {today_filename};
                    if (!yesterday_filename.empty()) {
                        attachments.push_back(yesterday_filename);
                    }

                    auto send_result =
                        email_sender->send_email(subject, email_body, true, attachments);
                    if (send_result.is_error()) {
                        ERROR("Failed to send email: " + std::string(send_result.error()->what()));
                    } else {
                        std::string attachment_list = today_filename;
                        if (!yesterday_filename.empty()) {
                            attachment_list += ", " + yesterday_filename;
                        }
                        INFO("Email report sent successfully with CSV attachments: " +
                             attachment_list);
                    }
                }
            } catch (const std::exception& e) {
                ERROR("Exception during email sending: " + std::string(e.what()));
            }
        } else if (!send_email) {
            INFO("Email reporting disabled");
        } else {
            INFO("Email reporting blocked because the current-position report was unavailable");
        }

        std::cerr << "At end of main: initialized=" << Logger::instance().is_initialized()
                  << std::endl;

        return 0;

}
