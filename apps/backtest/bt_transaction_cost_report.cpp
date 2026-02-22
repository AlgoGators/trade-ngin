/**
 * Transaction cost report for all futures contracts
 *
 * Outputs the exact transaction cost as calculated by TransactionCostManager
 * (same logic as backtest and live trading) for every futures contract in the
 * universe, broken down by:
 *   - commissions_fees
 *   - implicit_price_impact (price units per contract)
 *   - slippage_market_impact (dollars)
 *   - total_transaction_costs (dollars)
 *
 * Costs are computed per 1 contract at the latest available reference price.
 * Run from repo root so config path "./config" and DB connection work.
 */

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "trade_ngin/backtest/backtest_execution_manager.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;

static constexpr double DEFAULT_REFERENCE_PRICE = 100.0;  // fallback when no price in DB
static constexpr double REPORT_QUANTITY = 1.0;             // cost per 1 contract

int main() {
    try {
        LoggerConfig logger_config;
        logger_config.min_level = LogLevel::INFO;
        logger_config.destination = LogDestination::BOTH;
        logger_config.log_directory = "logs";
        logger_config.filename_prefix = "bt_transaction_cost_report";
        Logger::instance().initialize(logger_config);

        // Load config for DB (try ./config from repo root, then ../config from build/)
        auto app_config_result = ConfigLoader::load("./config", "base");
        if (app_config_result.is_error()) {
            app_config_result = ConfigLoader::load("../config", "base");
        }
        if (app_config_result.is_error()) {
            std::cerr << "Failed to load config: " << app_config_result.error()->what()
                      << std::endl;
            std::cerr << "Run from repo root or from build/ so config is at ./config or ../config"
                      << std::endl;
            return 1;
        }
        auto& app_config = app_config_result.value();

        auto pool_result =
            DatabasePool::instance().initialize(app_config.database.get_connection_string(),
                                                app_config.database.num_connections);
        if (pool_result.is_error()) {
            std::cerr << "Failed to init DB pool: " << pool_result.error()->what() << std::endl;
            return 1;
        }

        auto db_guard = DatabasePool::instance().acquire_connection();
        auto db = db_guard.get();
        if (!db || !db->is_connected()) {
            std::cerr << "Failed to acquire DB connection" << std::endl;
            return 1;
        }

        // Same universe as bt_portfolio: all futures symbols, minus a few exclusions
        auto symbols_result = db->get_symbols(AssetClass::FUTURES);
        if (symbols_result.is_error()) {
            std::cerr << "Failed to get symbols: " << symbols_result.error()->what() << std::endl;
            return 1;
        }
        std::vector<std::string> symbols = symbols_result.value();
        auto remove_it = std::remove_if(symbols.begin(), symbols.end(), [](const std::string& s) {
            return s.find(".c.0") != std::string::npos ||
                   s.find("MES.c.0") != std::string::npos ||
                   s.find("ES.v.0") != std::string::npos;
        });
        symbols.erase(remove_it, symbols.end());

        // Latest close prices for reference (market impact scales with price)
        std::unordered_map<std::string, double> latest_prices;
        auto prices_result = db->get_latest_prices(
            symbols, AssetClass::FUTURES, DataFrequency::DAILY, "ohlcv");
        if (prices_result.is_ok()) {
            latest_prices = prices_result.value();
        }

        // Same TransactionCostManager setup as backtest/live (BacktestExecutionConfig default)
        backtest::BacktestExecutionConfig exec_config;
        // Optional: if you add explicit_fee_per_contract to config, set it here:
        // exec_config.explicit_fee_per_contract = app_config.execution.explicit_fee_per_contract;
        transaction_cost::TransactionCostManager::Config tc_config;
        tc_config.explicit_fee_per_contract = exec_config.explicit_fee_per_contract;
        transaction_cost::TransactionCostManager cost_manager(tc_config);

        // Output paths
        const std::string csv_path = "transaction_cost_report_all_contracts.csv";
        std::ofstream csv(csv_path);
        if (!csv.is_open()) {
            std::cerr << "Failed to open " << csv_path << " for writing" << std::endl;
            return 1;
        }

        // CSV header: symbol, reference_price, quantity, commissions_fees, implicit_price_impact,
        // slippage_market_impact, total_transaction_costs
        csv << "symbol,reference_price,quantity,commissions_fees,implicit_price_impact,"
               "slippage_market_impact,total_transaction_costs\n";

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "Transaction cost report (per " << static_cast<int>(REPORT_QUANTITY)
                  << " contract(s)) – same model as backtest & live\n";
        std::cout << "Explicit fee per contract: $" << cost_manager.get_explicit_fee_per_contract()
                  << "\n\n";
        std::cout << std::setw(20) << "symbol" << std::setw(14) << "ref_price" << std::setw(10)
                  << "comm_fees" << std::setw(18) << "implicit_px_impact" << std::setw(18)
                  << "slippage_mkt_impact" << std::setw(18) << "total_txn_costs" << "\n";
        std::cout << std::string(96, '-') << "\n";

        for (const auto& symbol : symbols) {
            double ref_price = DEFAULT_REFERENCE_PRICE;
            auto pit = latest_prices.find(symbol);
            if (pit != latest_prices.end() && pit->second > 0.0) {
                ref_price = pit->second;
            }

            auto result = cost_manager.calculate_costs(symbol, REPORT_QUANTITY, ref_price);

            csv << symbol << "," << ref_price << "," << REPORT_QUANTITY << ","
                << result.commissions_fees << "," << result.implicit_price_impact << ","
                << result.slippage_market_impact << "," << result.total_transaction_costs << "\n";

            std::cout << std::setw(20) << symbol << std::setw(14) << ref_price << std::setw(10)
                      << result.commissions_fees << std::setw(18) << result.implicit_price_impact
                      << std::setw(18) << result.slippage_market_impact << std::setw(18)
                      << result.total_transaction_costs << "\n";
        }

        csv.close();
        std::cout << "\nWrote " << symbols.size() << " rows to " << csv_path << "\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
