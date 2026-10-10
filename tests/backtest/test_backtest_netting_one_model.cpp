// tests/backtest/test_backtest_netting_one_model.cpp
//
// T-7b-3 (b), code review R-2: one cost model per backtest fill.
//
// A portfolio backtest stores, in backtest.executions, the PortfolioManager's own per-sleeve
// reports (save_portfolio_results_to_db hands get_strategy_executions() to the writer). The
// PortfolioManager prices them with its cost manager and nets each bar's sleeve rows of one symbol
// with the same manager (K3), so the stored net costs of a symbol-day sum to C(Q) under the model
// whose costs are stored. The parent also re-priced a COPY of every fill with the execution
// manager's cost model and put the copies in BacktestResults::executions (the rows the trade
// statistics of backtest.results and the strategies' on_execution read), keeping the PM's
// netting_adjustment beside the other model's cost: with the two models configured apart the
// reported rows' net no longer summed to C(Q) and no longer matched the stored rows. The
// coordinator now reports the stored rows as they are.
//
// The fixture runs a whole futures backtest (run_portfolio) with two sleeves trading one symbol on
// the same cycles, the PortfolioManager's cost model charging a 1.50 fee per contract and the
// execution manager's 3.00 (the metadata "Fee Per Contract" each manager reads), then saves it
// through save_portfolio_results_to_db into a database double that records the rows it is given.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "cost_basis_test_helpers.hpp"
// LateRowStrategy writes one row into the PortfolioManager's stored executions from inside the
// manager's own cycle (its lock held by the calling thread), which no public call can do.
#define private public
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#undef private
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;
using namespace trade_ngin::testing::cost_basis;

namespace {

const std::string kX = "XA.v.0";  // both sleeves trade it
const std::string kY = "XB.v.0";  // only sleeve A trades it: a single row, never netted
constexpr int kLastDay = 40;
constexpr int kSameDirection = 21;  // A buys 2, B buys 1: Q = +3
constexpr int kFullCross = 31;      // A sells 2, B buys 2: Q = 0
constexpr int kPartialCross = 40;   // A buys 3, B sells 1: Q = +2 (the last cycle)

double close_of(const std::string& symbol, int d) {
    const double phase = symbol == kX ? 0.0 : 1.7;
    return 100.0 * (1.0 + 0.02 * std::sin(0.9 * d + phase) + 0.001 * d);
}

std::vector<Row> rows() {
    std::vector<Row> out;
    for (int d = 0; d <= kLastDay; ++d) {
        for (const auto& s : {kX, kY}) out.push_back({s, d, close_of(s, d), 200000.0});
    }
    return out;
}

// The metadata spec each manager reads: the same contract, a different "Fee Per Contract".
transaction_cost::ContractCostSpecSource spec_with_fee(double fee) {
    return [fee](const std::string& symbol) -> std::optional<transaction_cost::ContractCostSpec> {
        if (symbol.rfind("XA", 0) != 0 && symbol.rfind("XB", 0) != 0) return std::nullopt;
        transaction_cost::ContractCostSpec spec;
        spec.point_value = 50.0;
        spec.tick_size = 0.25;
        spec.fee_per_contract = fee;
        return spec;
    };
}

// Records the rows save_portfolio_results_to_db stores per strategy.
class RecordingDb : public ServingDb {
public:
    using ServingDb::ServingDb;
    std::map<std::string, std::vector<ExecutionReport>> stored;

    Result<void> store_backtest_executions_with_strategy(
        const std::vector<ExecutionReport>& executions, const std::string& run_id,
        const std::string& strategy_id, const std::string& portfolio_id,
        const std::string& table_name) override {
        (void)run_id;
        (void)portfolio_id;
        (void)table_name;
        auto& out = stored[strategy_id];
        out.insert(out.end(), executions.begin(), executions.end());
        return Result<void>();
    }
};

using SymbolDay = std::tuple<std::string, Timestamp>;

// T-NETTING fix round 2: a sleeve that, when it is told of its first fill of `inject_day`, appends
// to its own stored executions a row no code of the engine can make: a ROLL leg (or a BORROW row)
// CARRYING a netting adjustment. The coordinator feeds a cycle's fills to the strategies before it
// adds up the cycle's cost, so the row is in that cycle's sum.
class InjectingStrategy : public ScheduledStrategy {
public:
    using ScheduledStrategy::ScheduledStrategy;
    std::weak_ptr<PortfolioManager> pm;
    int inject_day{-1};
    ExecutionType inject_type{ExecutionType::ROLL};
    bool injected{false};

    Result<void> on_execution(const ExecutionReport& report) override {
        if (!injected && inject_day >= 0 && report.fill_time == trading_day(inject_day)) {
            injected = true;
            ExecutionReport bad;
            bad.exec_id = "RL-INJECTED-0";
            bad.order_id = bad.exec_id;
            bad.symbol = kX;
            bad.side = Side::SELL;
            bad.filled_quantity = Quantity(1.0);
            bad.fill_price = report.fill_price;
            bad.fill_time = report.fill_time;
            bad.total_transaction_costs = Decimal(2.0);
            bad.netting_adjustment = Decimal(0.5);
            bad.execution_type = inject_type;
            if (auto p = pm.lock()) p->append_synthetic_execution(get_metadata().id, bad);
        }
        return ScheduledStrategy::on_execution(report);
    }
};

// T-8a (7), the netting audits' follow-up: a sleeve whose bad row is met by the END-OF-RUN totals
// only. On the last cycle the row (a ROLL leg or a BORROW row CARRYING a netting adjustment) is
// put into the sleeve's stored executions while the PortfolioManager processes the cycle (on_data
// runs inside the manager's cycle, on its thread, under its lock: the row is written to the
// manager's list directly), so the coordinator collects it with the cycle's fills into the run's
// executions; when the sleeve is told of the cycle's first fill, before the cycle's cost is added
// up, the stored lists are emptied, so the day's sum never reads the row. No code of the engine
// can make such a row; the run's own list still holds it when the totals are taken.
class LateRowStrategy : public ScheduledStrategy {
public:
    using ScheduledStrategy::ScheduledStrategy;
    std::weak_ptr<PortfolioManager> pm;
    ExecutionType type{ExecutionType::ROLL};
    bool appended{false};
    bool withdrawn{false};

    Result<void> on_data(const std::vector<Bar>& data) override {
        Timestamp latest{};
        for (const auto& b : data) latest = std::max(latest, b.timestamp);
        if (!appended && latest == trading_day(kLastDay - 1)) {  // the signal bars of the last cycle
            appended = true;
            ExecutionReport bad;
            bad.exec_id = "RL-LATE-0";
            bad.order_id = bad.exec_id;
            bad.symbol = kX;
            bad.side = Side::SELL;
            bad.filled_quantity = Quantity(type == ExecutionType::BORROW ? 0.0 : 1.0);
            bad.fill_price = Price(100.0);
            bad.fill_time = trading_day(kLastDay);
            bad.total_transaction_costs = Decimal(2.0);
            bad.netting_adjustment = Decimal(0.5);
            bad.execution_type = type;
            if (auto p = pm.lock()) p->strategy_executions_[get_metadata().id].push_back(bad);
        }
        return ScheduledStrategy::on_data(data);
    }

    Result<void> on_execution(const ExecutionReport& report) override {
        if (appended && !withdrawn && report.fill_time == trading_day(kLastDay)) {
            withdrawn = true;
            if (auto p = pm.lock()) p->clear_all_executions();
        }
        return ScheduledStrategy::on_execution(report);
    }
};

double signed_qty(const ExecutionReport& r) {
    const double q = static_cast<double>(r.filled_quantity);
    return r.side == Side::SELL ? -q : q;
}

}  // namespace

class BacktestNettingOneModelTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        MarketDataBus::instance().set_publish_enabled(true);
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::INFO;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
        db_ = std::make_shared<RecordingDb>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }

    void TearDown() override {
        MarketDataBus::instance().set_publish_enabled(true);
        (void)MarketDataBus::instance().unsubscribe("PORTFOLIO_MANAGER");
        coord_.reset();
        a_.reset();
        b_.reset();
        late_.reset();
        pm_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    std::shared_ptr<ScheduledStrategy> sleeve(const std::string& id,
                                              std::map<std::string, std::map<int, double>> t) {
        StrategyConfig sc;
        sc.capital_allocation = 500'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        for (const auto& s : {kX, kY}) {
            sc.trading_params[s] = 1.0;
            sc.position_limits[s] = 1.0e6;
        }
        std::shared_ptr<ScheduledStrategy> s;
        if (id == "NET_A" && inject_day_ >= 0) {
            auto injecting = std::make_shared<InjectingStrategy>(id, sc, db_);
            injecting->inject_day = inject_day_;
            injecting->inject_type = inject_type_;
            injector_ = injecting;
            s = injecting;
        } else if (id == "NET_A" && late_row_) {
            late_ = std::make_shared<LateRowStrategy>(id, sc, db_);
            late_->type = inject_type_;
            s = late_;
        } else {
            s = std::make_shared<ScheduledStrategy>(id, sc, db_);
        }
        s->targets = std::move(t);
        EXPECT_TRUE(s->initialize().is_ok());
        EXPECT_TRUE(s->start().is_ok());
        return s;
    }

    void run() {
        auto result = run_raw();
        ASSERT_TRUE(result.is_ok()) << result.error()->what();
        results_ = result.value();

        auto saved = coord_->save_portfolio_results_to_db(
            results_, {"NET_A", "NET_B"}, {{"NET_A", 0.5}, {"NET_B", 0.5}}, pm_,
            nlohmann::json::object());
        ASSERT_TRUE(saved.is_ok()) << saved.error()->what();
    }

    // The run itself, its result returned as it is (a refused run is an error result).
    Result<BacktestResults> run_raw() {
        db_->rows = rows();

        BacktestCoordinatorConfig cc;
        cc.initial_capital = 1'000'000.0;
        cc.store_results = false;
        cc.store_trade_details = true;  // save_portfolio_results_to_db writes the executions
        cc.portfolio_id = "NET_ONE_MODEL_TEST";
        cc.csv_output_path = temp_csv_dir("net_one_model");
        coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
        EXPECT_TRUE(coord_->initialize().is_ok());

        PortfolioConfig pc{1'000'000.0, 1.0, 0.0, /*optimization=*/false};
        pc.opt_config.capital = 1'000'000.0;
        pc.risk_config.capital = 1'000'000.0;
        pc.risk_modules = {test_none_module()};
        pm_ = std::make_shared<PortfolioManager>(pc, "PM_NET_ONE_MODEL");

        // Signal day d's target is traded on the cycle stamped d + 1.
        if (opposite_opens_) {
            // The sleeves open against each other on one cycle and hold: A +2, B -2.
            a_ = sleeve("NET_A", {{kX, {{kSameDirection - 1, 2.0}}}});
            b_ = sleeve("NET_B", {{kX, {{kSameDirection - 1, -2.0}}}});
        } else {
            a_ = sleeve("NET_A", {{kX, {{kSameDirection - 1, 2.0}, {kFullCross - 1, 0.0},
                                        {kPartialCross - 1, 3.0}}},
                                  {kY, {{kSameDirection - 1, 1.0}}}});
            b_ = sleeve("NET_B", {{kX, {{kSameDirection - 1, 1.0}, {kFullCross - 1, 3.0},
                                        {kPartialCross - 1, 2.0}}}});
        }
        EXPECT_TRUE(pm_->add_strategy(a_, 0.5, false).is_ok());
        EXPECT_TRUE(pm_->add_strategy(b_, 0.5, false).is_ok());
        if (injector_) injector_->pm = pm_;
        if (late_) late_->pm = pm_;

        // The two managers disagree on the fee: the PM's 1.50, the execution manager's 3.00.
        pm_->get_transaction_cost_manager().set_contract_spec_source(spec_with_fee(1.50));
        coord_->get_execution_manager()->get_transaction_cost_manager().set_contract_spec_source(
            spec_with_fee(3.00));

        ::testing::internal::CaptureStdout();
        auto result = coord_->run_portfolio(pm_, {kX, kY}, trading_day(0), trading_day(kLastDay),
                                            AssetClass::FUTURES, DataFrequency::DAILY);
        log_ = ::testing::internal::GetCapturedStdout();
        return result;
    }

    // The stored rows (backtest.executions) by symbol-day.
    std::map<SymbolDay, std::vector<ExecutionReport>> stored_by_symbol_day() const {
        std::map<SymbolDay, std::vector<ExecutionReport>> out;
        for (const auto& [sid, rows] : db_->stored) {
            (void)sid;
            for (const auto& r : rows) out[{r.symbol, r.fill_time}].push_back(r);
        }
        return out;
    }

    std::map<SymbolDay, std::vector<ExecutionReport>> reported_by_symbol_day() const {
        std::map<SymbolDay, std::vector<ExecutionReport>> out;
        for (const auto& r : results_.executions) out[{r.symbol, r.fill_time}].push_back(r);
        return out;
    }

    // C(Q) of the last cycle under the PortfolioManager's model: nothing feeds either manager after
    // the last cycle's fills, so its state is the one those fills and their netting read.
    Decimal pm_cost(double q, double px) {
        return Decimal(pm_->get_transaction_cost_manager()
                           .calculate_costs(kX, q, px)
                           .total_transaction_costs);
    }

    std::shared_ptr<RecordingDb> db_;
    std::unique_ptr<BacktestCoordinator> coord_;
    std::shared_ptr<PortfolioManager> pm_;
    std::shared_ptr<ScheduledStrategy> a_, b_;
    std::shared_ptr<InjectingStrategy> injector_;
    std::shared_ptr<LateRowStrategy> late_;
    bool late_row_{false};
    bool opposite_opens_{false};
    int inject_day_{-1};
    ExecutionType inject_type_{ExecutionType::ROLL};
    BacktestResults results_;
    std::string log_;
};

// The stored rows are the PortfolioManager's, priced and netted with its own model: the stored net
// of a netted symbol-day sums to C(Q) under that model (the full cross to 0), whatever the
// execution manager's fee, and the NETTING line prints the stored costs. Holds on the parent too:
// the parent's re-pricing never touched a stored row.
TEST_F(BacktestNettingOneModelTest, TheStoredNetOfASymbolDaySumsToCOfQUnderTheStoredModel) {
    run();
    const auto stored = stored_by_symbol_day();
    ASSERT_FALSE(stored.empty()) << "nothing was stored";

    const auto cross = stored.find({kX, trading_day(kFullCross)});
    ASSERT_NE(cross, stored.end());
    ASSERT_EQ(cross->second.size(), 2u);
    Decimal cross_net;
    for (const auto& r : cross->second) {
        EXPECT_GT(r.total_transaction_costs, Decimal()) << "each row keeps its own cost";
        cross_net += r.total_transaction_costs - r.netting_adjustment;
    }
    EXPECT_EQ(cross_net, Decimal()) << "a full cross nets to C(0) = 0";

    const auto last = stored.find({kX, trading_day(kPartialCross)});
    ASSERT_NE(last, stored.end());
    ASSERT_EQ(last->second.size(), 2u);
    double q = 0.0;
    Decimal net;
    for (const auto& r : last->second) {
        q += signed_qty(r);
        net += r.total_transaction_costs - r.netting_adjustment;
        EXPECT_EQ(r.total_transaction_costs,
                  pm_cost(signed_qty(r), static_cast<double>(r.fill_price)))
            << r.exec_id << ": the stored own cost is the PortfolioManager's model";
    }
    ASSERT_DOUBLE_EQ(q, 2.0);
    EXPECT_EQ(net, pm_cost(q, static_cast<double>(last->second.front().fill_price)))
        << "the stored net of the symbol-day sums to C(Q) under the stored model";
    for (const auto& r : last->second) {
        EXPECT_NE(log_.find(" C=" + r.total_transaction_costs.to_string() +
                            " adj=" + r.netting_adjustment.to_string()),
                  std::string::npos)
            << r.exec_id << ": the NETTING line prints the stored cost and adjustment";
    }

    const auto single = stored.find({kY, trading_day(kSameDirection)});
    ASSERT_NE(single, stored.end());
    ASSERT_EQ(single->second.size(), 1u);
    EXPECT_EQ(single->second[0].netting_adjustment, Decimal()) << "one row: nothing to net";
}

// The rows the backtest reports (BacktestResults::executions: the trade statistics stored in
// backtest.results and the strategies' on_execution read them) are the stored rows, costs and
// netting adjustment alike, so their net of a symbol-day sums to C(Q) under the model whose costs
// are stored. RED on f2932054: the coordinator re-priced its copies with the execution manager's
// 3.00 fee and kept the PM's 1.50-model adjustment, so the full cross reported a non-zero net.
TEST_F(BacktestNettingOneModelTest, TheReportedRowsAreTheStoredRowsAndTheirNetSumsToCOfQ) {
    run();
    std::map<std::string, ExecutionReport> stored_by_id;
    size_t stored_rows = 0;
    for (const auto& [sid, rows] : db_->stored) {
        (void)sid;
        for (const auto& r : rows) {
            stored_by_id[r.exec_id] = r;
            ++stored_rows;
        }
    }
    ASSERT_GT(stored_rows, 0u);
    ASSERT_EQ(results_.executions.size(), stored_rows)
        << "every stored fill is reported once";
    for (const auto& r : results_.executions) {
        const auto it = stored_by_id.find(r.exec_id);
        ASSERT_NE(it, stored_by_id.end()) << r.exec_id;
        const auto& s = it->second;
        EXPECT_EQ(r.commissions_fees, s.commissions_fees) << r.exec_id;
        EXPECT_EQ(r.implicit_price_impact, s.implicit_price_impact) << r.exec_id;
        EXPECT_EQ(r.slippage_market_impact, s.slippage_market_impact) << r.exec_id;
        EXPECT_EQ(r.total_transaction_costs, s.total_transaction_costs) << r.exec_id;
        EXPECT_EQ(r.netting_adjustment, s.netting_adjustment) << r.exec_id;
        EXPECT_EQ(r.fill_time, s.fill_time) << r.exec_id;
    }

    const auto reported = reported_by_symbol_day();
    const auto cross = reported.find({kX, trading_day(kFullCross)});
    ASSERT_NE(cross, reported.end());
    ASSERT_EQ(cross->second.size(), 2u);
    Decimal cross_net;
    for (const auto& r : cross->second) cross_net += r.total_transaction_costs - r.netting_adjustment;
    EXPECT_EQ(cross_net, Decimal()) << "the reported full cross nets to C(0) = 0";

    const auto last = reported.find({kX, trading_day(kPartialCross)});
    ASSERT_NE(last, reported.end());
    ASSERT_EQ(last->second.size(), 2u);
    double q = 0.0;
    Decimal net;
    for (const auto& r : last->second) {
        q += signed_qty(r);
        net += r.total_transaction_costs - r.netting_adjustment;
    }
    EXPECT_EQ(net, pm_cost(q, static_cast<double>(last->second.front().fill_price)))
        << "the reported net of the symbol-day sums to C(Q) under the stored model";

    const auto same = reported.find({kX, trading_day(kSameDirection)});
    ASSERT_NE(same, reported.end());
    ASSERT_EQ(same->second.size(), 2u);
    for (const auto& r : same->second) {
        EXPECT_LT(r.netting_adjustment, Decimal())
            << r.exec_id << ": one order of 3 costs more than orders of 2 and 1, a debit";
    }
}

// The cost after netting (HD 2026-10-09): the equity curve charges each cycle the NET cost of its
// fills (own cost minus the signed adjustment), and the run's transaction_costs is their sum. The
// fixture's sleeves book no position P&L (the equity curve moves on cycles with fills only), so a
// cycle's equity change is exactly minus what it was charged:
//
//   | cycle | NET_A  | NET_B  | account | charged                                    |
//   | 21    | BUY 2  | BUY 1  | BUY 3   | C(3): MORE than C(2) + C(1), plus kY's row |
//   | 31    | SELL 2 | BUY 2  | none    | 0                                          |
//   | 40    | BUY 3  | SELL 1 | BUY 2   | C(2): less than C(3) + C(1)                |
//
// RED when calculate_period_transaction_costs or the results' total goes back to the rows' own
// costs, and (cycle 21) when a negative adjustment is dropped, clamped or made positive.
TEST_F(BacktestNettingOneModelTest, TheEquityCurveAndTheResultsChargeTheCostAfterNetting) {
    run();
    std::map<Timestamp, std::vector<ExecutionReport>> fills_of;
    for (const auto& r : results_.executions) fills_of[r.fill_time].push_back(r);
    std::map<Timestamp, double> equity_of;
    for (const auto& [t, e] : results_.equity_curve) equity_of[t] = e;

    double total_net = 0.0, total_own = 0.0, previous = 1'000'000.0;
    for (int d = 0; d <= kLastDay; ++d) {
        const auto at = equity_of.find(trading_day(d));
        if (at == equity_of.end()) continue;
        double own = 0.0, net = 0.0, adjustment = 0.0;
        const auto fills = fills_of.find(trading_day(d));
        if (fills != fills_of.end()) {
            for (const auto& r : fills->second) {
                own += static_cast<double>(r.total_transaction_costs);
                net += static_cast<double>(r.total_transaction_costs - r.netting_adjustment);
                adjustment += static_cast<double>(r.netting_adjustment);
            }
        }
        EXPECT_NEAR(at->second - previous, -net, 1e-9)
            << "cycle " << d << ": the account moved by minus the net cost of the cycle's fills"
            << " (own " << own << ", net " << net << ")";
        if (d == kSameDirection) {
            EXPECT_LT(adjustment, -1e-6) << "the same-side cycle carries a NEGATIVE adjustment";
            EXPECT_GT(net, own + 1e-6) << "and is charged MORE than the rows' own costs";
            EXPECT_GT(std::abs((at->second - previous) + own), 1e-6);
        }
        if (d == kFullCross) {
            EXPECT_GT(own, 1.0) << "the crossing rows keep their own costs";
            EXPECT_NEAR(net, 0.0, 1e-9);
            EXPECT_DOUBLE_EQ(at->second, previous) << "a full cross costs the account nothing";
        }
        if (d == kPartialCross) {
            EXPECT_LT(net, own - 1e-6);
            EXPECT_NEAR(net, static_cast<double>(pm_cost(2.0, close_of(kX, kPartialCross - 1))), 1e-6)
                << "the partial offset is charged the account's BUY 2";
        }
        total_net += net;
        total_own += own;
        previous = at->second;
    }
    EXPECT_GT(std::abs(total_own - total_net), 1.0) << "the fixture must tell own from net";
    EXPECT_NEAR(results_.transaction_costs, total_net, 1e-9)
        << "backtest.results.transaction_costs is the sum of the net costs";
    EXPECT_NEAR(results_.equity_curve.back().second, 1'000'000.0 - total_net, 1e-9);
    EXPECT_DOUBLE_EQ(results_.roll_costs, 0.0);
}

// T-NETTING fix round 2 (audit A of the fix round, S2): the refusal of a ROLL or BORROW row that
// carries a netting adjustment STOPS THE RUN ON THE DAY IT FIRES. The row is put into the first
// trading cycle (21) through the real bar loop; the run must come back as an error naming the row
// and the day, and must not have gone on: the fixture's later cycles (31 and 40) trade nothing.
//
//   | cycle | before this round                              | now                         |
//   | 21    | WARN, equity carried flat                      | NETTING STOP, the run fails |
//   | 31    | traded (the run went on)                       | never reached               |
//   | 40    | traded; the run failed only in the end totals  | never reached               |
//
// RED on 4398b2c6: the run went on to the last day (fills on cycles 31 and 40) before it failed.
TEST_F(BacktestNettingOneModelTest, ARollRowWithAnAdjustmentStopsTheRunOnTheDayItFires) {
    for (const ExecutionType type : {ExecutionType::ROLL, ExecutionType::BORROW}) {
        TearDown();
        SetUp();
        inject_day_ = kSameDirection;
        inject_type_ = type;
        injector_.reset();
        auto result = run_raw();
        ASSERT_TRUE(injector_ && injector_->injected) << "the bad row never reached the bar loop";
        ASSERT_TRUE(result.is_error()) << "a run holding such a row must fail";
        const std::string what = result.error()->what();
        EXPECT_NE(what.find("NETTING STOP on " + ymd(kSameDirection)), std::string::npos) << what;
        EXPECT_NE(what.find("NETTING_REFUSED"), std::string::npos) << what;
        EXPECT_NE(what.find("RL-INJECTED-0"), std::string::npos) << "the refusal names the row: " << what;
        EXPECT_NE(what.find(type == ExecutionType::ROLL ? "the ROLL row" : "the BORROW row"),
                  std::string::npos)
            << what;

        // It stopped where it fired: nothing was traded after cycle 21.
        size_t on_the_day = 0, after_the_day = 0;
        for (const auto& [sleeve, rows] : pm_->get_strategy_executions()) {
            (void)sleeve;
            for (const auto& r : rows) {
                if (r.fill_time == trading_day(kSameDirection)) ++on_the_day;
                if (r.fill_time > trading_day(kSameDirection)) ++after_the_day;
            }
        }
        EXPECT_GE(on_the_day, 3u) << "the cycle's own fills and the injected row";
        EXPECT_EQ(after_the_day, 0u) << "the run went on after the refusal";
        EXPECT_EQ(log_.find("Portfolio data processing failed"), std::string::npos)
            << "the refusal was downgraded to a warning and the day carried flat";
        inject_day_ = -1;
    }
}

// T-8a (7), ledger G3 (the netting audits' follow-up): the END-OF-RUN refusal, driven through
// run_portfolio. A ROLL or BORROW row carrying a netting adjustment that no day's sum met (in a
// real run a BORROW row is appended after its cycle's sum) is met where the run's totals are
// taken, before the metrics: the run comes back as an error result naming the row, not as an
// exception out of run_portfolio and not as a results row.
//
//   | where the row is met     | the error                                             |
//   | a day's sum (cycle 21)   | NETTING STOP on <date>: NETTING_REFUSED: ...          |
//   | the totals (this test)   | NETTING STOP: NETTING_REFUSED: ... Failing the run    |
//
// RED when the totals' try and catch are removed (the refusal escapes run_portfolio) or the
// totals are taken after the metrics.
TEST_F(BacktestNettingOneModelTest, ARollOrBorrowRowWithAnAdjustmentMetAtTheEndFailsTheRunInItsTotals) {
    for (const ExecutionType type : {ExecutionType::ROLL, ExecutionType::BORROW}) {
        TearDown();
        SetUp();
        late_row_ = true;
        inject_type_ = type;
        Result<BacktestResults> result = make_error<BacktestResults>(ErrorCode::UNKNOWN_ERROR, "not run");
        ASSERT_NO_THROW(result = run_raw()) << "the refusal escaped run_portfolio";
        ASSERT_TRUE(late_ && late_->appended && late_->withdrawn)
            << "the row never reached the last cycle, or stayed in the day's sum";
        ASSERT_TRUE(result.is_error()) << "a run holding such a row must fail";
        const std::string what = result.error()->what();
        EXPECT_NE(what.find("NETTING STOP: NETTING_REFUSED"), std::string::npos) << what;
        EXPECT_EQ(what.find("NETTING STOP on "), std::string::npos)
            << "the day's sum met the row first: " << what;
        EXPECT_NE(what.find("RL-LATE-0"), std::string::npos) << "the refusal names the row: " << what;
        EXPECT_NE(what.find(type == ExecutionType::ROLL ? "the ROLL row" : "the BORROW row"),
                  std::string::npos)
            << what;
        EXPECT_NE(what.find("Failing the run"), std::string::npos) << what;
        EXPECT_NE(log_.find("NETTING STOP at the end of the run"), std::string::npos);
        late_row_ = false;
    }
}

// T-8a (7), LEAD_RULINGS_IN_SESSION R-G: the trade statistics of a run are the BOOK's. Two sleeves
// OPEN against each other on one cycle (A buys 2, B sells 2: a full cross, net cost 0) and hold to
// the end: the account sent no order, so the run has no trade.
//
//   | reading                       | cycle 21                                         | total_trades |
//   | every sleeve row is a fill    | the second row "closes" the first: a trade of 0  | 1            |
//   | the account's fills           | a full cross: no fill                            | 0            |
//
// RED on the parent: total_trades 1.
TEST_F(BacktestNettingOneModelTest, AFullCrossIsNoTradeInTheRunsTradeStatistics) {
    opposite_opens_ = true;
    run();
    size_t fills = 0;
    for (const auto& r : results_.executions) {
        ASSERT_EQ(r.fill_time, trading_day(kSameDirection)) << r.exec_id;
        EXPECT_EQ(r.total_transaction_costs, r.netting_adjustment) << "a full cross: net cost 0";
        ++fills;
    }
    ASSERT_EQ(fills, 2u) << "the run's executions keep one row per sleeve";
    EXPECT_EQ(results_.total_trades, 0) << "a crossed row was scored as a trade";
    EXPECT_DOUBLE_EQ(results_.win_rate, 0.0);
    EXPECT_DOUBLE_EQ(results_.max_win, 0.0);
    EXPECT_DOUBLE_EQ(results_.max_loss, 0.0);
    EXPECT_TRUE(results_.actual_trades.empty());
}
