#include "trade_ngin/statistics/state_estimation/msar.hpp"
#include "trade_ngin/statistics/state_estimation/markov_switching.hpp"
#include "trade_ngin/core/logger.hpp"

#include <Eigen/Dense>
#include <pqxx/pqxx>

#include <cmath>
#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

Eigen::VectorXd closes_to_log_returns(const std::vector<double>& closes) {
    if (closes.size() < 2) {
        throw std::invalid_argument("Need at least 2 close prices to compute returns.");
    }

    Eigen::VectorXd returns(static_cast<Eigen::Index>(closes.size() - 1));
    for (std::size_t i = 1; i < closes.size(); ++i) {
        returns(static_cast<Eigen::Index>(i - 1)) = std::log(closes[i] / closes[i - 1]);
    }
    return returns;
}

}

int main() {
    using namespace trade_ngin::statistics;

    std::map<std::string, std::string> database = {
        {"host", "13.58.153.216"},
        {"port", "5432"},
        {"user", "postgres"},
        {"password", "algogators"},
        {"dbname", "new_algo_data"}
    };

    try {
        auto& logger = trade_ngin::Logger::instance();
        trade_ngin::LoggerConfig log_config;
        log_config.min_level = trade_ngin::LogLevel::DEBUG;
        log_config.destination = trade_ngin::LogDestination::CONSOLE;
        logger.initialize(log_config);
        trade_ngin::Logger::register_component("autoregressive_debug_main");

        pqxx::connection c(
            "dbname=" + database["dbname"] +
            " user=" + database["user"] +
            " password=" + database["password"] +
            " hostaddr=" + database["host"] +
            " port=" + database["port"]
        );

        if (!c.is_open()) {
            throw std::runtime_error("Failed to open PostgreSQL connection.");
        }

        pqxx::work txn(c);

        constexpr int lag = 5;
        constexpr std::size_t total_points = 1000;
        const std::size_t min_train_size = 252;

        std::vector<double> close_prices;
        close_prices.reserve(total_points);

        pqxx::result r = txn.exec(
            "SELECT close "
            "FROM futures_data.new_data_ohlcv_1d "
            "WHERE symbol = 'NG' "
            "ORDER BY \"time\" ASC "
            "LIMIT " + std::to_string(total_points) + ";"
        );

        for (const pqxx::row& row : r) {
            if (!row["close"].is_null()) {
                close_prices.push_back(row["close"].as<double>());
            }
        }

        txn.commit();

        Eigen::VectorXd returns = closes_to_log_returns(close_prices);
        if (returns.size() <= static_cast<Eigen::Index>(min_train_size)) {
            throw std::invalid_argument("Not enough return observations for requested backtest.");
        }

        MarkovSwitchingConfig ms_config;
        ms_config.n_states = 3;
        ms_config.max_iterations = 100;
        ms_config.tolerance = 1e-4;

        MSARBacktestResult result = historical_backtest_market_msar(
            returns,
            lag,
            min_train_size,
            ms_config,
            false
        );

        std::cout << "Historical MSAR backtest finished.\n";
        std::cout << "Total forecasts: " << result.points.size() << "\n\n";

        for (const auto& point : result.points) {
            std::cout
                << "t=" << point.t
                << "  train_size=" << point.train_size
                << "  pred=" << point.prediction
                << "  actual=" << point.actual
                << "  abs_err=" << point.abs_error
                << "  sq_err=" << point.sq_error
                << '\n';
        }

        std::cout << "\n========== Summary ==========\n";
        std::cout << "MAE:  " << result.mae << '\n';
        std::cout << "RMSE: " << result.rmse << '\n';

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}