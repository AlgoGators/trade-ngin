// K3 wiring (T-7b-2 8b): the netting adjustment on the per-sleeve execution reports.
//
// Backtest: the PortfolioManager nets each bar's per-sleeve executions of one symbol (the account
// sends ONE order, the signed sum) with its own cost manager at that bar's price, and writes each
// report's netting_adjustment; a symbol only one sleeve trades gets 0, and an earlier bar's
// reports are never re-netted. Live: both futures runners call the same function on the day's
// per-sleeve executions after the STRICT assertion and before storing them (source check; the
// function itself is pinned in tests/transaction_cost/test_netting.cpp). A live runner's
// PortfolioManager (not marked backtest) does NOT net its own reports: in a fresh live process
// its filled ledger is empty, so those reports are each sleeve's whole held book, never an order
// and never stored (T-7b-2 C8b4).

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/transaction_cost/netting.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

using Book = std::unordered_map<std::string, Position>;

Timestamp day(int d) { return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d)); }

Bar make_bar(const std::string& symbol, int d, double close) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = day(d);
    b.open = Decimal(close);
    b.high = Decimal(close * 1.01);
    b.low = Decimal(close * 0.99);
    b.close = Decimal(close);
    b.volume = 100000.0;
    return b;
}

Position make_pos(const std::string& symbol, double qty, double price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(price);
    p.last_update = day(0);
    return p;
}

class ScriptedSleeve : public BaseStrategy {
public:
    ScriptedSleeve(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db,
                   std::vector<Book> script)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          script_(std::move(script)) {
        metadata_.name = "Scripted Sleeve";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        (void)data;
        ++calls_;
        return Result<void>();
    }
    Book get_target_positions() const override {
        if (calls_ == 0) return {};
        return script_[std::min(calls_, script_.size()) - 1];
    }

private:
    std::vector<Book> script_;
    size_t calls_{0};
};

PortfolioConfig base_config() {
    PortfolioConfig pc{1000000.0, 1.0, 0.0, /*optimization=*/false};
    pc.allow_fractional_positions = false;
    pc.risk_config.capital = 1000000.0;
    pc.risk_config.var_limit = 1e6;
    pc.risk_config.jump_risk_limit = 1e6;
    pc.risk_config.max_correlation = 1.0;
    pc.risk_config.max_gross_leverage = 1e6;
    pc.risk_config.max_net_leverage = 1e6;
    pc.risk_modules = {test_carver_module(pc.risk_config)};
    return pc;
}

std::string read_source(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) {
            std::ifstream in(dir / relative);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
        dir = dir.parent_path();
    }
    return {};
}

}  // namespace

class PmNettingTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::INFO;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
    }
    void TearDown() override {
        pm_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    void make_pm(std::vector<Book> a, std::vector<Book> b, bool backtest = true) {
        static int n = 0;
        pm_ = std::make_unique<PortfolioManager>(base_config(), "PM_NET_" + std::to_string(++n));
        if (backtest) pm_->set_backtest_mode(true);  // as BacktestCoordinator::run_portfolio does
        StrategyConfig sc;
        sc.capital_allocation = 1000000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        a_ = std::make_shared<ScriptedSleeve>("NET_A", sc, db_, std::move(a));
        b_ = std::make_shared<ScriptedSleeve>("NET_B", sc, db_, std::move(b));
        for (auto s : {a_, b_}) {
            ASSERT_TRUE(s->initialize().is_ok());
            ASSERT_TRUE(s->start().is_ok());
        }
        ASSERT_TRUE(pm_->add_strategy(a_, 0.5, false).is_ok());
        ASSERT_TRUE(pm_->add_strategy(b_, 0.5, false).is_ok());
    }

    std::vector<ExecutionReport> execs(const std::string& sleeve, const std::string& symbol) {
        std::vector<ExecutionReport> out;
        const auto all = pm_->get_strategy_executions();
        auto it = all.find(sleeve);
        if (it == all.end()) return out;
        for (const auto& r : it->second)
            if (r.symbol == symbol) out.push_back(r);
        return out;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::shared_ptr<ScriptedSleeve> a_, b_;
};

// Bar 1: both sleeves buy 1 ZZA (same direction, the account buys 2) and only A buys ZZB.
// Bar 2: the ZZA lot moves A -> B (A sells 1, B buys 1): the account sends nothing.
TEST_F(PmNettingTest, TheBacktestNetsEachBarsSleeveRowsAndLeavesSingleRowsAtZero) {
    make_pm({{{"ZZA", make_pos("ZZA", 1, 100)}, {"ZZB", make_pos("ZZB", 2, 50)}},
             {{"ZZA", make_pos("ZZA", 0, 100)}, {"ZZB", make_pos("ZZB", 2, 50)}}},
            {{{"ZZA", make_pos("ZZA", 1, 100)}},
             {{"ZZA", make_pos("ZZA", 2, 100)}}});
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 1, 100), make_bar("ZZB", 1, 50)}, false,
                                         day(2))
                    .is_ok());
    const std::string bar1_log = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(bar1_log.find("NETTING sym=ZZA"), std::string::npos)
        << "the backtest prints its NETTING line for the two-sleeve symbol";
    EXPECT_EQ(bar1_log.find("NETTING sym=ZZB"), std::string::npos);
    auto a1 = execs("NET_A", "ZZA"), b1 = execs("NET_B", "ZZA"), z1 = execs("NET_A", "ZZB");
    ASSERT_EQ(a1.size(), 1u);
    ASSERT_EQ(b1.size(), 1u);
    ASSERT_EQ(z1.size(), 1u);
    EXPECT_EQ(z1[0].netting_adjustment, Decimal()) << "one sleeve trades ZZB: nothing to net";
    EXPECT_LT(a1[0].netting_adjustment, Decimal())
        << "same direction: one order of 2 costs more than two of 1, a debit";
    EXPECT_EQ(a1[0].netting_adjustment.raw_value() - b1[0].netting_adjustment.raw_value() >= -1 &&
                  a1[0].netting_adjustment.raw_value() - b1[0].netting_adjustment.raw_value() <= 1,
              true)
        << "equal legs split equally (to one unit)";
    const Decimal bar1_a = a1[0].netting_adjustment, bar1_b = b1[0].netting_adjustment;

    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 2, 101), make_bar("ZZB", 2, 51)}, false,
                                         day(3))
                    .is_ok());
    auto a2 = execs("NET_A", "ZZA"), b2 = execs("NET_B", "ZZA");
    ASSERT_EQ(a2.size(), 2u);
    ASSERT_EQ(b2.size(), 2u);
    EXPECT_EQ(a2[1].side, Side::SELL);
    EXPECT_EQ(b2[1].side, Side::BUY);
    EXPECT_EQ(a2[1].netting_adjustment, a2[1].total_transaction_costs)
        << "a full cross credits each row its own cost";
    EXPECT_EQ(b2[1].netting_adjustment, b2[1].total_transaction_costs);
    EXPECT_GT(a2[1].total_transaction_costs, Decimal()) << "the row keeps its own cost";
    EXPECT_EQ(a2[0].netting_adjustment, bar1_a) << "an earlier bar's rows are never re-netted";
    EXPECT_EQ(b2[0].netting_adjustment, bar1_b);
}

// A live runner's PM pass (a fresh process: empty filled ledger, not marked backtest). Both sleeves
// hold 1 ZZA, so the PM's reports are the whole held book (A +1, B +1), not an order: nothing is
// netted, no NETTING line is printed, every report keeps netting_adjustment 0. RED on ac26b230,
// where the PM netted them (a debit on each report) and printed a NETTING line for ZZA.
TEST_F(PmNettingTest, ALiveRunnersPmPassDoesNotNetItsWholeBookReports) {
    make_pm({{{"ZZA", make_pos("ZZA", 1, 100)}, {"ZZB", make_pos("ZZB", 2, 50)}}},
            {{{"ZZA", make_pos("ZZA", 1, 100)}}}, /*backtest=*/false);
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 1, 100), make_bar("ZZB", 1, 50)}, false,
                                         day(2))
                    .is_ok());
    const std::string log = ::testing::internal::GetCapturedStdout();
    auto a1 = execs("NET_A", "ZZA"), b1 = execs("NET_B", "ZZA"), z1 = execs("NET_A", "ZZB");
    ASSERT_EQ(a1.size(), 1u) << "the PM still generates its per-sleeve reports";
    ASSERT_EQ(b1.size(), 1u);
    ASSERT_EQ(z1.size(), 1u);
    EXPECT_GT(a1[0].total_transaction_costs, Decimal()) << "each report keeps its own cost";
    EXPECT_EQ(a1[0].netting_adjustment, Decimal()) << "a live PM pass nets nothing";
    EXPECT_EQ(b1[0].netting_adjustment, Decimal());
    EXPECT_EQ(z1[0].netting_adjustment, Decimal());
    EXPECT_EQ(log.find(" NETTING"), std::string::npos)
        << "no NETTING line for reports that are the whole held book";
}

TEST(PmNettingSource, TheBacktestCoordinatorMarksItsPortfolioAsBacktest) {
    const std::string src = read_source("src/backtest/backtest_coordinator.cpp");
    ASSERT_FALSE(src.empty()) << "src/backtest/backtest_coordinator.cpp not found: a source-text test must be run from inside the source tree";
    const auto run = src.find("BacktestCoordinator::run_portfolio(");
    const auto mark = src.find("portfolio->set_backtest_mode(true);", run);
    ASSERT_NE(run, std::string::npos);
    ASSERT_NE(mark, std::string::npos)
        << "run_portfolio must mark its PortfolioManager, or the backtest stops netting";
}

TEST(PmNettingSource, BothFuturesRunnersNetTheDaysSleeveRowsBeforeStoringThem) {
    for (const char* f : {"apps/strategies/live_portfolio.cpp",
                          "apps/strategies/live_portfolio_conservative.cpp"}) {
        const std::string src = read_source(f);
        if (src.empty()) {
            ADD_FAILURE() << f << " not found: a source-text test must be run from inside the source tree";
            continue;
        }
        const auto strict = src.find("STRICT_ASSERTION failed");
        const auto net = src.find("transaction_cost::apply_netting_adjustments(");
        const auto store = src.find("db->store_executions(executions,");
        EXPECT_NE(strict, std::string::npos) << f;
        EXPECT_NE(net, std::string::npos) << f << ": the runner never nets its sleeve rows";
        EXPECT_NE(store, std::string::npos) << f;
        if (strict == std::string::npos || net == std::string::npos || store == std::string::npos) {
            continue;  // the next file is still read
        }
        EXPECT_LT(strict, net) << f << ": netting must see the final (post-rollback) rows";
        EXPECT_LT(net, store) << f << ": netting must happen before the rows are stored";
        EXPECT_NE(src.find("execution_manager->get_transaction_cost_manager().calculate_costs("),
                  std::string::npos)
            << f << ": C(Q) must be priced by the same cost manager the fills used";
    }
}

// The cost after netting (HD 2026-10-09): the backtest's cost totals read the one helper.
TEST(PmNettingSource, TheBacktestsCostTotalsReadTheHelper) {
    const std::vector<std::pair<const char*, const char*>> sites = {
        {"src/backtest/backtest_coordinator.cpp", "transaction_cost::add_net_costs(total_transaction_costs, execs, count_before)"},
        {"src/backtest/backtest_coordinator.cpp", "results.transaction_costs += static_cast<double>(transaction_cost::net_cost(e));"},
        {"apps/backtest/bt_equity_validation.cpp", "day_txn_costs += transaction_cost::net_cost(exec).as_double();"},
    };
    for (const auto& [f, text] : sites) {
        const std::string src = read_source(f);
        if (src.empty()) {
            ADD_FAILURE() << f << " not found: a source-text test must be run from inside the source tree";
            continue;
        }
        EXPECT_NE(src.find(text), std::string::npos) << f << " no longer reads: " << text;
    }
}

// The cost after netting (HD 2026-10-09), live: the day's transaction cost is added up AFTER the
// runner has written every fill's netting_adjustment, from the rows it stores, through the one
// helper; nothing adds a fill's own cost to the day's total.
TEST(PmNettingSource, BothFuturesRunnersAddUpTheDaysCostAfterNettingThroughTheHelper) {
    for (const char* f : {"apps/strategies/live_portfolio.cpp",
                          "apps/strategies/live_portfolio_conservative.cpp"}) {
        const std::string src = read_source(f);
        if (src.empty()) {
            ADD_FAILURE() << f << " not found: a source-text test must be run from inside the source tree";
            continue;
        }
        const auto last_net = src.rfind("transaction_cost::apply_netting_adjustments(");
        const auto sum = src.find("total_daily_transaction_costs = transaction_cost::add_net_costs(");
        const auto store = src.find("db->store_executions(executions,");
        // EXPECT, not ASSERT: a failure in one runner must not end the test before its twin is
        // read. The checks that need all three positions are skipped for that file only.
        EXPECT_NE(last_net, std::string::npos) << f;
        EXPECT_NE(sum, std::string::npos) << f << ": the day's cost is not the sum of the net costs";
        EXPECT_NE(store, std::string::npos) << f;
        EXPECT_EQ(src.find("total_daily_transaction_costs +="), std::string::npos)
            << f << ": something adds to the day's cost beside the helper";
        if (last_net == std::string::npos || sum == std::string::npos || store == std::string::npos) {
            continue;
        }
        EXPECT_LT(last_net, sum) << f << ": the sum is taken before the adjustment is on the fills";
        EXPECT_LT(sum, store) << f;
        size_t calls = 0;
        for (auto at = src.find("add_net_costs("); at != std::string::npos;
             at = src.find("add_net_costs(", at + 1)) {
            ++calls;
        }
        EXPECT_EQ(calls, 1u)
            << f << ": the day's cost is added up twice";
    }
}

// The equity runner (one strategy, never netted) adds its day's cost through the same helper.
TEST(PmNettingSource, TheEquityRunnerAddsUpTheDaysCostThroughTheHelper) {
    const char* f = "apps/strategies/live_equity_mean_reversion.cpp";
    const std::string src = read_source(f);
    ASSERT_FALSE(src.empty()) << f << " not found: a source-text test must be run from inside the source tree";
    EXPECT_NE(src.find("transaction_cost::add_net_costs(total_daily_commissions, daily_executions)"),
              std::string::npos);
    EXPECT_EQ(src.find("total_daily_commissions +="), std::string::npos);
}

// The live day, as the runners add it up: per-sleeve fills (a ROLL leg ahead of the sleeve's
// trades), netted by symbol, then summed sleeve by sleeve. The three symbol-days are real rows of
// the BASE lookback-3 backtest; the cost table is the cost model's own prices for them.
//
//   | contract | FAST   | TREND  | account | own costs   | adjustments     | charged |
//   | MES      | SELL 1 | BUY 1  | none    | 1.62 + 1.62 | 1.62 + 1.62     | 0.00    |
//   | MYM      | BUY 1  | SELL 2 | SELL 1  | 1.10 + 2.40 | 0.754 + 1.646   | 1.10    |
//   | MBT      | SELL 1 | SELL 1 | SELL 2  | 4.50 + 4.50 | -0.915 - 0.915  | 10.83   |
//   | ZN (roll)| -      | 2 legs | -       | 3.00 + 3.00 | never netted    | 6.00    |
TEST(LiveDayCost, TheDaysCostIsTheSumOfTheNetCostsOfTheStoredRows) {
    auto row = [](const std::string& symbol, Side side, double q, double px, double cost,
                  ExecutionType type = ExecutionType::STRATEGY) {
        ExecutionReport e;
        e.symbol = symbol;
        e.side = side;
        e.filled_quantity = Quantity(q);
        e.fill_price = Price(px);
        e.total_transaction_costs = Decimal(cost);
        e.execution_type = type;
        return e;
    };
    std::unordered_map<std::string, std::vector<ExecutionReport>> day;
    day["TREND_FOLLOWING"] = {row("ZN.v.0", Side::SELL, 2, 112.0, 3.00, ExecutionType::ROLL),
                              row("ZN.v.0", Side::BUY, 2, 112.5, 3.00, ExecutionType::ROLL),
                              row("MES.v.0", Side::BUY, 1, 6650, 1.62),
                              row("MYM.v.0", Side::SELL, 2, 49000, 2.40),
                              row("MBT.v.0", Side::SELL, 1, 115000, 4.50)};
    day["TREND_FOLLOWING_FAST"] = {row("MES.v.0", Side::SELL, 1, 6650, 1.62),
                                   row("MYM.v.0", Side::BUY, 1, 49000, 1.10),
                                   row("MBT.v.0", Side::SELL, 1, 115000, 4.50)};
    const std::vector<std::string> sleeves_filled{"TREND_FOLLOWING", "TREND_FOLLOWING_FAST"};

    std::map<std::pair<std::string, double>, double> price{
        {{"MYM.v.0", -1.0}, 1.10}, {{"MBT.v.0", -2.0}, 10.83}};
    std::vector<transaction_cost::SleeveExecution> rows;
    for (auto& [sleeve, execs] : day)
        for (auto& e : execs)
            if (e.execution_type == ExecutionType::STRATEGY) rows.push_back({sleeve, &e});
    transaction_cost::apply_netting_adjustments(
        rows, [&](const std::string& s, double q, double) { return price.at({s, q}); });

    double own = 0.0, charged = 0.0;
    for (const auto& sleeve : sleeves_filled) {
        for (const auto& e : day.at(sleeve)) own += static_cast<double>(e.total_transaction_costs);
        charged = transaction_cost::add_net_costs(charged, day.at(sleeve));
    }
    EXPECT_NEAR(own, 6.00 + 3.24 + 3.50 + 9.00, 1e-9);
    EXPECT_NEAR(charged, 6.00 + 0.00 + 1.10 + 10.83, 1e-9)
        << "the roll legs' own cost, nothing for the cross, the account's order for the other two";

    // The identity per symbol: the legs' net costs sum to the account order's cost.
    std::map<std::string, Decimal> net_of;
    for (const auto& [sleeve, execs] : day)
        for (const auto& e : execs)
            if (e.execution_type == ExecutionType::STRATEGY) net_of[e.symbol] += transaction_cost::net_cost(e);
    EXPECT_EQ(net_of["MES.v.0"], Decimal());
    EXPECT_EQ(net_of["MYM.v.0"], Decimal(1.10));
    EXPECT_EQ(net_of["MBT.v.0"], Decimal(10.83)) << "same side: above the 9.00 of the rows' own costs";
}

// TRADE_NGIN_EMAIL_BODY_DIR: the futures runners write the report body to a file and mail nothing.
// The one call of send_email sits in the else of the body-file branch, in both twins; every file
// is read and every failure reported.
TEST(EmailBodyFileSource, TheBodyFileBranchWritesTheBodyAndNeverSends) {
    for (const char* f : {"apps/strategies/live_portfolio.cpp",
                          "apps/strategies/live_portfolio_conservative.cpp"}) {
        const std::string src = read_source(f);
        if (src.empty()) {
            ADD_FAILURE() << f << " not found: run the tests from inside the source tree";
            continue;
        }
        const auto env = src.find("std::getenv(\"TRADE_NGIN_EMAIL_BODY_DIR\")");
        const auto gate = src.find("if (send_email || !email_body_dir.empty()) {");
        const auto file_branch =
            src.find("if (!email_body_dir.empty()) {\n                        const std::string body_path");
        const auto send_branch = src.find("} else {\n                        auto send_result =");
        const auto send = src.find("email_sender->send_email(");
        EXPECT_NE(env, std::string::npos) << f;
        EXPECT_NE(gate, std::string::npos) << f;
        EXPECT_NE(file_branch, std::string::npos) << f << ": no body-file branch";
        EXPECT_NE(send_branch, std::string::npos) << f << ": the send is not in the else";
        EXPECT_NE(send, std::string::npos) << f;
        if (file_branch == std::string::npos || send_branch == std::string::npos ||
            send == std::string::npos) {
            continue;
        }
        EXPECT_LT(file_branch, send_branch) << f;
        EXPECT_LT(send_branch, send) << f << ": a send outside the else of the body-file branch";
        EXPECT_EQ(src.find("email_sender->send_email(", send + 1), std::string::npos)
            << f << ": a second send";
    }
}

// T-NETTING fix round: the live log states the day's cost in one line that adds up (own costs,
// adjustments, cost after netting), printed after the day's sum in both twins.
TEST(PmNettingSource, BothFuturesRunnersLogTheDaysOwnCostsAdjustmentsAndCostAfterNetting) {
    for (const char* f : {"apps/strategies/live_portfolio.cpp",
                          "apps/strategies/live_portfolio_conservative.cpp"}) {
        const std::string src = read_source(f);
        if (src.empty()) {
            ADD_FAILURE() << f << " not found: a source-text test must be run from inside the source tree";
            continue;
        }
        const auto sum = src.find("total_daily_transaction_costs = transaction_cost::add_net_costs(");
        const auto line = src.find("INFO(\"DAY_COST own_costs=\" + day_own_costs.to_string() + \" netting_adjustments=\" +");
        EXPECT_NE(sum, std::string::npos) << f;
        EXPECT_NE(line, std::string::npos) << f << ": no DAY_COST line";
        if (sum == std::string::npos || line == std::string::npos) continue;
        EXPECT_LT(sum, line) << f << ": the line is printed before the day's sum";
        EXPECT_NE(src.find("\" cost_after_netting=\" +\n                 (day_own_costs - day_netting_adjustments).to_string());", line),
                  std::string::npos)
            << f << ": the line's last figure is not own costs minus adjustments";
        EXPECT_EQ(src.find("DAY_COST own_costs=", line + 40), std::string::npos) << f << ": printed twice";
    }
}
