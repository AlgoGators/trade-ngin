// tests/backtest/test_backtest_change_bar_pnl.cpp
//
// LOOP_SPEC v6.1 sections 2.1, 6.6 and 7 in the futures backtest (T-ROLLX-FIX commit 2): the daily
// P&L of a held position is q x (close - previous CONSUMED close) x M on every consumed bar, 0 on a
// change bar (the splice's price gap) and 0 on a WITHHELD bar (K-01: never consumed, it moves no
// previous close, so the next consumed bar books against the last consumed close); every stored row
// carries the contract held after its bar.
//
// XA (M = 50) is held at 2 contracts from cycle 1. Closes: d0 100 A, d1 101 A, d2 103 A, d3 110 B
// (the change bar), d4 111 B (confirms), d5 112 B, d6 50 B locked (JUNK, withheld), d7 113 B.

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

Timestamp wday(int d) { return Timestamp(std::chrono::seconds(1767571200LL + 86400LL * d)); }

Bar bar(int d, double close, const std::string& id, bool locked = false) {
    Bar b;
    b.symbol = "XA";
    b.timestamp = wday(d);
    b.open = Decimal(close);
    b.high = Decimal(locked ? close : close * 1.01);
    b.low = Decimal(locked ? close : close * 0.99);
    b.close = Decimal(close);
    b.volume = 100000.0;
    b.instrument_id = id;
    return b;
}

class HoldTwo : public BaseStrategy {
public:
    HoldTwo(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)) {
        metadata_.name = "Hold Two";
    }
    Result<void> on_data(const std::vector<Bar>&) override { return Result<void>(); }
    std::unordered_map<std::string, Position> get_target_positions() const override {
        Position p;
        p.symbol = "XA";
        p.quantity = Decimal(2.0);
        p.average_price = Decimal(100.0);
        p.last_update = wday(0);
        return {{"XA", p}};
    }
};

PortfolioConfig plain_config() {
    PortfolioConfig c{1'000'000.0, 1.0, 0.0, false};
    c.opt_config.capital = 1'000'000.0;
    c.risk_config.capital = 1'000'000.0;
    c.risk_modules = {test_none_module()};
    return c;
}

}  // namespace

class BacktestChangeBarPnlTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto& registry = InstrumentRegistry::instance();
        FuturesSpec spec;
        spec.root_symbol = "XA";
        spec.exchange = "CME";
        spec.currency = "USD";
        spec.multiplier = 50.0;
        spec.tick_size = 0.25;
        spec.commission_per_contract = 0.0;
        spec.initial_margin = 1.0;
        spec.maintenance_margin = 1.0;
        spec.weight = 1.0;
        spec.trading_hours = "09:30-16:00";
        registry.instruments_["XA"] = std::make_shared<FuturesInstrument>("XA", spec);
        registry.initialized_ = true;
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        BacktestCoordinatorConfig cc;
        cc.initial_capital = 1'000'000.0;
        cc.store_results = false;
        cc.store_trade_details = false;
        cc.portfolio_id = "PNL_TEST";
        coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
        ASSERT_TRUE(coord_->initialize().is_ok());
        coord_->reset_portfolio_state();
        coord_->session_hold_enabled_ = true;
        static int n = 0;
        pm_ = std::make_shared<PortfolioManager>(plain_config(), "PM_PNL_" + std::to_string(++n));
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto s = std::make_shared<HoldTwo>("PNL_S", sc, db_);
        ASSERT_TRUE(s->initialize().is_ok());
        ASSERT_TRUE(s->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(s, 1.0, false).is_ok());
    }
    void TearDown() override {
        coord_.reset();
        pm_.reset();
        db_.reset();
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_.clear();
        registry.initialized_ = false;
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<BacktestCoordinator> coord_;
    std::shared_ptr<PortfolioManager> pm_;
    std::vector<ExecutionReport> execs_;
    std::vector<std::pair<Timestamp, double>> equity_;
    std::vector<RiskResult> risk_;
};

TEST_F(BacktestChangeBarPnlTest, ChangeAndWithheldBarsBookNothingAndRowsCarryTheHeldContract) {
    const std::vector<Bar> xa = {bar(0, 100, "A"), bar(1, 101, "A"), bar(2, 103, "A"), bar(3, 110, "B"),
                                 bar(4, 111, "B"), bar(5, 112, "B"), bar(6, 50, "B", true), bar(7, 113, "B")};
    std::vector<double> move;
    std::vector<double> roll_cost;
    std::vector<std::string> held;
    for (int d = 0; d < static_cast<int>(xa.size()); ++d) {
        auto r = coord_->process_portfolio_day(wday(d), {xa[d]}, pm_, execs_, equity_, risk_, false,
                                               1'000'000.0);
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
        if (d < 2) continue;
        // The equity move is the cycle's P&L less its costs; the only cost after cycle 1 is the
        // confirmed roll's two ROLL legs (section 6.5), booked on the cycle consuming d4.
        double cost = 0.0;
        for (const auto& e : execs_) {
            if (e.fill_time == wday(d) && e.execution_type == ExecutionType::ROLL) {
                cost += static_cast<double>(e.total_transaction_costs);
            }
        }
        roll_cost.push_back(cost);
        move.push_back(equity_.back().second - equity_[equity_.size() - 2].second + cost);
        held.push_back(coord_->row_held_id_["XA"]);
    }
    EXPECT_GT(roll_cost[3], 0.0) << "the legs are booked on the cycle that consumes the confirming bar";
    roll_cost[3] = 0.0;
    EXPECT_EQ(roll_cost, std::vector<double>(roll_cost.size(), 0.0));
    // The book is 2 contracts from cycle 1 on and never trades again, so each cycle's equity move plus
    // its roll cost is its P&L: d2 2 x (103 - 101) x 50; d3 the change bar 0 (the parent booked
    // 2 x 7 x 50 = 700); d4 against the change bar's close 110; d5 100; d6 withheld 0 (the parent
    // booked 2 x (50 - 112) x 50); d7 against d5's 112, the last CONSUMED close (not d6's 50).
    const std::vector<double> want{200.0, 0.0, 100.0, 100.0, 0.0, 100.0};
    ASSERT_EQ(move.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) EXPECT_NEAR(move[i], want[i], 1e-6) << "cycle " << i + 2;
    EXPECT_EQ(held, (std::vector<std::string>{"A", "A", "B", "B", "B", "B"}))
        << "the change bar's row is held in the outgoing contract, the confirming bar's in the new one";
}
