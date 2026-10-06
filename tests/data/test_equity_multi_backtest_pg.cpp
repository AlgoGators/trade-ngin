#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;

namespace {

constexpr std::array<const char*, 6> kSymbols{
    "P1AAA", "P1BBB", "P1CCC", "P1DDD", "P1EEE", "P1FFF"};
constexpr const char* kBookA = "BACKTEST_P3_A";
constexpr const char* kBookB = "BACKTEST_P3_B";
constexpr const char* kLong = "P1_LONG";
constexpr const char* kShort = "P1_SHORT";

std::string owned_dsn() {
    const char* raw = std::getenv("TRADE_NGIN_121124_TEST_DSN");
    if (raw == nullptr)
        throw std::runtime_error("TRADE_NGIN_121124_TEST_DSN is required");
    const std::string dsn(raw);
    if (!dsn.starts_with("host=/tmp/algolens-repair-pg-") ||
        dsn.find(" dbname=trade_ngin_121124_p2_test ") == std::string::npos ||
        dsn.find("hostaddr") != std::string::npos ||
        dsn.find("service=") != std::string::npos) {
        throw std::runtime_error(
            "TRADE_NGIN_121124_TEST_DSN is not the owned disposable P1/P3 database");
    }
    return dsn;
}

Timestamp day(const char* value) {
    Timestamp parsed;
    if (!core::parse_utc_date(value, parsed))
        throw std::runtime_error(std::string("invalid test date: ") + value);
    return parsed;
}

void expect_ok(const Result<void>& result) {
    ASSERT_TRUE(result.is_ok()) << (result.is_error() ? result.error()->what() : "");
}

double signed_quantity(const pqxx::row& row) {
    const double quantity = row["quantity"].as<double>();
    return row["side"].as<std::string>() == "BUY" ? quantity : -quantity;
}

class DeterministicEquityStrategy final : public BaseStrategy {
public:
    DeterministicEquityStrategy(std::string id, StrategyConfig config,
                                std::shared_ptr<PostgresDatabase> db, double target)
        : BaseStrategy(std::move(id), std::move(config), std::move(db)), target_(target) {
        metadata_.name = "Issue 121 deterministic equity sleeve";
    }

    Result<void> on_data(const std::vector<Bar>& bars,
                         StrategyConsumptionTrace* trace = nullptr) override {
        if (trace) *trace = {};
        for (const auto& bar : bars) {
            Position position;
            position.symbol = bar.symbol;
            position.quantity = Quantity(target_);
            position.average_price = bar.close;
            position.last_update = bar.timestamp;
            positions_[bar.symbol] = position;
        }
        return Result<void>();
    }

    Result<void> on_execution(const ExecutionReport&) override { return Result<void>(); }

private:
    double target_;
};

struct RunEvidence {
    std::string run_id;
    double final_equity;
    double total_costs;
};

class EquityMultiBacktestPg : public ::testing::Test {
protected:
    void SetUp() override {
        dsn_ = owned_dsn();
        prepare_substrate();
        db_ = std::make_shared<PostgresDatabase>(dsn_);
        expect_ok(db_->connect());
        expect_ok(InstrumentRegistry::instance().initialize(db_));
        expect_ok(InstrumentRegistry::instance().load_equity_instruments(symbols()));
    }

    void TearDown() override {
        db_.reset();
        StateManager::instance().reset_instance();
    }

    static std::vector<std::string> symbols() {
        return {kSymbols.begin(), kSymbols.end()};
    }

    void prepare_substrate() {
        pqxx::connection connection(dsn_);
        pqxx::work tx(connection);
        tx.exec(R"SQL(
            CREATE SCHEMA IF NOT EXISTS equities_data;
            CREATE TABLE IF NOT EXISTS equities_data.ohlcv_1d(
                symbol text NOT NULL, time timestamptz NOT NULL,
                open double precision NOT NULL, high double precision NOT NULL,
                low double precision NOT NULL, close double precision NOT NULL,
                volume double precision NOT NULL, div_cash double precision,
                split_factor double precision, delisting_date date,
                PRIMARY KEY(symbol,time));

            DROP SCHEMA IF EXISTS backtest CASCADE;
            CREATE SCHEMA backtest;
            CREATE TABLE backtest.results(
                run_id text PRIMARY KEY, portfolio_id text NOT NULL,
                start_date timestamptz NOT NULL, end_date timestamptz NOT NULL,
                total_return double precision, sharpe_ratio double precision,
                sortino_ratio double precision, max_drawdown double precision,
                calmar_ratio double precision, volatility double precision,
                total_trades double precision, win_rate double precision,
                profit_factor double precision, avg_win double precision,
                avg_loss double precision, max_win double precision,
                max_loss double precision, avg_holding_period double precision,
                var_95 double precision, cvar_95 double precision,
                beta double precision, correlation double precision,
                downside_volatility double precision);
            CREATE TABLE backtest.equity_curve(
                run_id text NOT NULL, portfolio_id text NOT NULL,
                timestamp timestamptz NOT NULL, equity double precision NOT NULL,
                PRIMARY KEY(run_id,timestamp));
            CREATE TABLE backtest.final_positions(
                run_id text NOT NULL, portfolio_id text NOT NULL,
                strategy_id text NOT NULL, date date NOT NULL, symbol text NOT NULL,
                quantity double precision NOT NULL, average_price double precision NOT NULL,
                unrealized_pnl double precision NOT NULL DEFAULT 0,
                realized_pnl double precision NOT NULL DEFAULT 0,
                last_update timestamp NOT NULL, updated_at timestamp NOT NULL,
                PRIMARY KEY(run_id,strategy_id,date,symbol));
            CREATE TABLE backtest.executions(
                run_id text NOT NULL, portfolio_id text NOT NULL,
                strategy_id text NOT NULL, execution_id text NOT NULL,
                order_id text NOT NULL, timestamp timestamptz NOT NULL,
                symbol text NOT NULL, side text NOT NULL,
                quantity double precision NOT NULL, price double precision NOT NULL,
                commissions_fees double precision NOT NULL,
                implicit_price_impact double precision NOT NULL,
                slippage_market_impact double precision NOT NULL,
                total_transaction_costs double precision NOT NULL,
                is_partial boolean NOT NULL,
                PRIMARY KEY(run_id,strategy_id,execution_id));
            CREATE TABLE backtest.run_metadata(
                run_id text NOT NULL, portfolio_id text NOT NULL,
                portfolio_run_id text, strategy_id text NOT NULL,
                strategy_allocation double precision NOT NULL,
                portfolio_config jsonb NOT NULL, name text NOT NULL,
                description text NOT NULL, start_date timestamptz NOT NULL,
                end_date timestamptz NOT NULL, hyperparameters jsonb NOT NULL,
                PRIMARY KEY(run_id,strategy_id));
            CREATE TABLE backtest.signals(
                run_id text NOT NULL, portfolio_id text NOT NULL,
                strategy_id text NOT NULL, symbol text NOT NULL,
                signal_value double precision NOT NULL, timestamp timestamptz NOT NULL,
                portfolio_run_id text,
                PRIMARY KEY(run_id,strategy_id,symbol,timestamp));

            DELETE FROM equities_data.ohlcv_1d WHERE symbol LIKE 'P1___';
        )SQL");
        for (const char* symbol : kSymbols) {
            tx.exec(
                "INSERT INTO equities_data.ohlcv_1d "
                "(symbol,time,open,high,low,close,volume,div_cash,split_factor,delisting_date) "
                "VALUES($1,'2026-12-01T00:00:00Z',100,100,100,100,1000000,0,1,NULL),"
                "($1,'2026-12-02T00:00:00Z',101,101,101,101,1000000,0,1,NULL),"
                "($1,'2026-12-03T00:00:00Z',102,102,102,102,1000000,0,1,NULL)",
                pqxx::params{symbol});
        }
        tx.commit();
    }

    RunEvidence run_book(const std::string& portfolio_id, double capital) {
        PortfolioConfig portfolio_config{
            capital, 0.0, 1.0, 0.0, false, false};
        auto portfolio = std::make_shared<PortfolioManager>(
            portfolio_config, "PM_" + portfolio_id);

        StrategyConfig strategy_config;
        strategy_config.capital_allocation = capital * 0.5;
        strategy_config.max_leverage = 4.0;
        strategy_config.asset_classes = {AssetClass::EQUITIES};
        strategy_config.frequencies = {DataFrequency::DAILY};
        for (const auto& symbol : symbols()) {
            strategy_config.trading_params[symbol] = 1.0;
            strategy_config.position_limits[symbol] = 10000.0;
        }
        auto long_strategy = std::make_shared<DeterministicEquityStrategy>(
            kLong, strategy_config, db_, 7.0);
        auto short_strategy = std::make_shared<DeterministicEquityStrategy>(
            kShort, strategy_config, db_, -3.0);
        expect_ok(long_strategy->initialize());
        expect_ok(short_strategy->initialize());
        expect_ok(long_strategy->start());
        expect_ok(short_strategy->start());
        expect_ok(portfolio->add_strategy(long_strategy, 0.5, false, false));
        expect_ok(portfolio->add_strategy(short_strategy, 0.5, false, false));

        BacktestCoordinatorConfig coordinator_config;
        coordinator_config.initial_capital = capital;
        coordinator_config.use_risk_management = false;
        coordinator_config.use_optimization = false;
        coordinator_config.store_results = true;
        coordinator_config.store_trade_details = true;
        coordinator_config.portfolio_id = portfolio_id;
        coordinator_config.csv_output_path =
            "/tmp/trade-ngin-121124-" + portfolio_id;
        BacktestCoordinator coordinator(db_, &InstrumentRegistry::instance(),
                                        coordinator_config);
        auto results = coordinator.run_portfolio(
            portfolio, symbols(), day("2026-12-01"), day("2026-12-03"),
            AssetClass::EQUITIES, DataFrequency::DAILY);
        EXPECT_TRUE(results.is_ok())
            << (results.is_error() ? results.error()->what() : "");
        if (results.is_error()) return {};

        const std::unordered_map<std::string, double> allocations{
            {kLong, 0.5}, {kShort, 0.5}};
        const nlohmann::json config_json{
            {"portfolio_id", portfolio_id}, {"total_capital", capital}};
        auto saved = coordinator.save_portfolio_results_to_db(
            results.value(), {kLong, kShort}, allocations, portfolio, config_json);
        EXPECT_TRUE(saved.is_ok()) << (saved.is_error() ? saved.error()->what() : "");

        pqxx::connection connection(dsn_);
        pqxx::read_transaction tx(connection);
        const auto row = tx.exec(
            "SELECT run_id FROM backtest.results WHERE portfolio_id=$1",
            pqxx::params{portfolio_id});
        if (row.empty()) return {};
        double total_costs = 0.0;
        for (const auto& execution : results.value().executions)
            total_costs += execution.total_transaction_costs.as_double();
        return {row[0][0].as<std::string>(), results.value().equity_curve.back().second,
                total_costs};
    }

    std::string dsn_;
    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(EquityMultiBacktestPg,
       P1AndP3PersistGrossSleevesAccountNetAndDistinctPortfolioRuns) {
    const auto a = run_book(kBookA, 100000.0);
    ASSERT_FALSE(a.run_id.empty());
    const auto b = run_book(kBookB, 200000.0);
    ASSERT_FALSE(b.run_id.empty());
    EXPECT_NE(a.run_id, b.run_id);

    pqxx::connection connection(dsn_);
    pqxx::read_transaction tx(connection);
    const auto mappings = tx.exec(
        "SELECT run_id,count(DISTINCT portfolio_id) AS portfolios "
        "FROM backtest.results GROUP BY run_id ORDER BY run_id");
    ASSERT_EQ(mappings.size(), 2u);
    for (const auto& row : mappings) EXPECT_EQ(row["portfolios"].as<int>(), 1);

    for (const auto& [book, evidence, capital] :
         std::vector<std::tuple<std::string, RunEvidence, double>>{
             {kBookA, a, 100000.0}, {kBookB, b, 200000.0}}) {
        const auto positions = tx.exec(
            "SELECT strategy_id,symbol,quantity FROM backtest.final_positions "
            "WHERE run_id=$1 AND portfolio_id=$2 AND date='2026-12-03' "
            "ORDER BY strategy_id,symbol",
            pqxx::params{evidence.run_id, book});
        ASSERT_EQ(positions.size(), 12u);
        for (const auto& row : positions) {
            const double expected = row["strategy_id"].as<std::string>() == kLong ? 7.0 : -3.0;
            EXPECT_DOUBLE_EQ(row["quantity"].as<double>(), expected);
        }

        const auto executions = tx.exec(
            "SELECT strategy_id,symbol,side,quantity,total_transaction_costs "
            "FROM backtest.executions WHERE run_id=$1 AND portfolio_id=$2 "
            "ORDER BY strategy_id,symbol",
            pqxx::params{evidence.run_id, book});
        ASSERT_EQ(executions.size(), 12u);
        std::unordered_map<std::string, double> net;
        std::unordered_map<std::string, double> gross;
        double persisted_costs = 0.0;
        for (const auto& row : executions) {
            net[row["symbol"].as<std::string>()] += signed_quantity(row);
            gross[row["symbol"].as<std::string>()] += row["quantity"].as<double>();
            persisted_costs += row["total_transaction_costs"].as<double>();
        }
        for (const auto& symbol : symbols()) {
            EXPECT_DOUBLE_EQ(net.at(symbol), 4.0);
            EXPECT_DOUBLE_EQ(gross.at(symbol), 10.0);
        }
        EXPECT_NEAR(persisted_costs, evidence.total_costs, 1e-6);
        EXPECT_NEAR(evidence.final_equity, capital + 24.0 - evidence.total_costs, 1e-6);

        const auto metadata = tx.exec(
            "SELECT strategy_id,strategy_allocation FROM backtest.run_metadata "
            "WHERE run_id=$1 AND portfolio_id=$2 ORDER BY strategy_id",
            pqxx::params{evidence.run_id, book});
        ASSERT_EQ(metadata.size(), 2u);
        EXPECT_NE(metadata[0]["strategy_id"].as<std::string>(),
                  metadata[1]["strategy_id"].as<std::string>());
        EXPECT_NEAR(metadata[0]["strategy_allocation"].as<double>() +
                        metadata[1]["strategy_allocation"].as<double>(),
                    1.0, 1e-12);

        const double gross_notional = 10.0 * 6.0 * 102.0;
        EXPECT_NEAR(gross_notional / evidence.final_equity,
                    (6120.0 / evidence.final_equity), 1e-12);
    }
}

}  // namespace
