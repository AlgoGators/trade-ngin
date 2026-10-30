// tests/backtest/test_backtest_no_bar_row.cpp
//
// The futures backtest's stored position row on a day the symbol prints no bar. The row's realized
// P&L is the day's booked P&L, and a day with no bar books nothing: 0, quantity and average price
// kept. The loop that stamps the row reaches a symbol with no close; skipping it before the stamp
// would leave the row the PortfolioManager rebuilt that cycle with the sleeve's own figure (its
// unrounded position times its last bar's move). The same holds on a bar with no previous close to
// book against. The equity curve does not read the cell.
//
// XA (M = 50) is held at 2 contracts; the sleeve's own target row carries a realized figure of
// 552.62, as a trend sleeve's does. XB is another symbol's bar, so a day XA does not print is still
// a cycle.
//
// Migration 038: the row of such a day is stored with row_mark = 'NO_BAR' ("no bar:
// held, marked at last close") and every other row with NULL. The mark is set where the row is
// zeroed, travels to the writer beside the day's rows, and is read back here from the writer's
// arguments (no database) and from backtest.final_positions (TRADE_NGIN_TEST_DSN).

#include <gtest/gtest.h>
#include <pqxx/pqxx>
#include "../risk/risk_module_test_helpers.hpp"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/backtest_position_row.hpp"
#include "trade_ngin/data/postgres_database.hpp"
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

constexpr double kSleeveFigure = 552.62;

Timestamp wday(int d) { return Timestamp(std::chrono::seconds(1767571200LL + 86400LL * d)); }

Bar bar(const std::string& symbol, int d, double close) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = wday(d);
    b.open = Decimal(close);
    b.high = Decimal(close * 1.01);
    b.low = Decimal(close * 0.99);
    b.close = Decimal(close);
    b.volume = 100000.0;
    b.instrument_id = symbol + "_A";
    return b;
}

class HoldTwoWithItsOwnFigure : public BaseStrategy {
public:
    HoldTwoWithItsOwnFigure(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)) {
        metadata_.name = "Hold Two";
        // A futures sleeve: the day's mark-to-market is the row's realized P&L.
        set_pnl_accounting_method(PnLAccountingMethod::REALIZED_ONLY);
    }
    Result<void> on_data(const std::vector<Bar>&) override { return Result<void>(); }
    std::unordered_map<std::string, Position> get_target_positions() const override {
        Position p;
        p.symbol = "XA";
        p.quantity = Decimal(2.0);
        p.average_price = Decimal(100.0);
        p.realized_pnl = Decimal(kSleeveFigure);
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

class BacktestNoBarRowTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto& registry = InstrumentRegistry::instance();
        for (const char* symbol : {"XA", "XB"}) {
            FuturesSpec spec;
            spec.root_symbol = symbol;
            spec.exchange = "CME";
            spec.currency = "USD";
            spec.multiplier = 50.0;
            spec.tick_size = 0.25;
            spec.commission_per_contract = 0.0;
            spec.initial_margin = 1.0;
            spec.maintenance_margin = 1.0;
            spec.weight = 1.0;
            spec.trading_hours = "09:30-16:00";
            registry.instruments_[symbol] = std::make_shared<FuturesInstrument>(symbol, spec);
        }
        registry.initialized_ = true;
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        BacktestCoordinatorConfig cc;
        cc.initial_capital = 1'000'000.0;
        cc.store_results = false;
        cc.store_trade_details = false;
        cc.portfolio_id = "NO_BAR_TEST";
        coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
        ASSERT_TRUE(coord_->initialize().is_ok());
        coord_->reset_portfolio_state();
        coord_->session_hold_enabled_ = true;
        static int n = 0;
        pm_ = std::make_shared<PortfolioManager>(plain_config(), "PM_NO_BAR_" + std::to_string(++n));
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto s = std::make_shared<HoldTwoWithItsOwnFigure>("NO_BAR_S", sc, db_);
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

    // One cycle on the day's bars; the stored row of XA after it and the cycle's equity move.
    struct Day {
        Position row;
        bool stored{false};  // the sleeve's book holds a row of XA after the cycle
        double equity_move{0.0};
    };
    Day run(int d, const std::vector<Bar>& bars) {
        const double before = equity_.empty() ? 1'000'000.0 : equity_.back().second;
        auto r = coord_->process_portfolio_day(wday(d), bars, pm_, execs_, equity_, risk_, false, 1'000'000.0);
        EXPECT_TRUE(r.is_ok()) << r.error()->what();
        Day out;
        const auto books = pm_->get_strategy_positions();
        const auto book = books.find("NO_BAR_S");
        if (book != books.end() && book->second.count("XA")) {
            out.row = book->second.at("XA");
            out.stored = true;
        }
        out.equity_move = equity_.back().second - before;
        return out;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<BacktestCoordinator> coord_;
    std::shared_ptr<PortfolioManager> pm_;
    std::vector<ExecutionReport> execs_;
    std::vector<std::pair<Timestamp, double>> equity_;
    std::vector<RiskResult> risk_;
};

TEST_F(BacktestNoBarRowTest, ARowOfADayTheSymbolPrintsNoBarBooksZeroAndKeepsItsQuantity) {
    run(0, {bar("XA", 0, 100.0), bar("XB", 0, 10.0)});
    run(1, {bar("XA", 1, 101.0), bar("XB", 1, 10.0)});
    const Day booked = run(2, {bar("XA", 2, 103.0), bar("XB", 2, 10.0)});
    ASSERT_TRUE(booked.stored) << "the book holds XA from the first fed cycle on";
    EXPECT_NEAR(static_cast<double>(booked.row.realized_pnl), 2.0 * (103.0 - 101.0) * 50.0, 1e-9)
        << "a bar day's row is stamped with the day's P&L";

    // XA prints no bar on days 3 and 4 (XB does, so each is a cycle).
    for (int d : {3, 4}) {
        const Day no_bar = run(d, {bar("XB", d, 10.0)});
        ASSERT_TRUE(no_bar.stored);
        EXPECT_EQ(static_cast<double>(no_bar.row.realized_pnl), 0.0)
            << "day " << d << ": the row of a day with no bar books nothing; it carried the sleeve's own "
            << kSleeveFigure << " (or the last bar day's stamp)";
        EXPECT_EQ(static_cast<double>(no_bar.row.quantity), 2.0) << "the quantity is kept";
        EXPECT_EQ(static_cast<double>(no_bar.row.average_price), 100.0) << "the average price is kept";
        EXPECT_NEAR(no_bar.equity_move, 0.0, 1e-9) << "the equity curve books nothing either";
    }

    // The next bar books against the last close before the gap, as before.
    const Day after = run(5, {bar("XA", 5, 104.0), bar("XB", 5, 10.0)});
    EXPECT_NEAR(static_cast<double>(after.row.realized_pnl), 2.0 * (104.0 - 103.0) * 50.0, 1e-9);
    EXPECT_NEAR(after.equity_move, 2.0 * (104.0 - 103.0) * 50.0, 1e-6);
}

// ===== migration 038: the mark of a no-bar row =====

// [the mark at the writer's door]
TEST_F(BacktestNoBarRowTest, TheWriterIsHandedTheMarkOfANoBarRowAndOfNoOtherRow) {
    const auto saved = [&](int d, const std::vector<Bar>& bars) {
        run(d, bars);
        EXPECT_TRUE(coord_->save_daily_positions(pm_, "NO_BAR_RUN", wday(d)).is_ok());
        return db_->last_backtest_no_bar_symbols;
    };
    // The first bar has no previous close to book against: the row books 0, and the symbol
    // printed a bar, so the row is not a no-bar row.
    EXPECT_TRUE(saved(0, {bar("XA", 0, 100.0), bar("XB", 0, 10.0)}).empty());
    EXPECT_TRUE(saved(1, {bar("XA", 1, 101.0), bar("XB", 1, 10.0)}).empty());
    EXPECT_TRUE(saved(2, {bar("XA", 2, 103.0), bar("XB", 2, 10.0)}).empty());

    for (int d : {3, 4}) {
        const auto marked = saved(d, {bar("XB", d, 10.0)});
        EXPECT_EQ(marked, std::unordered_set<std::string>({"XA"}))
            << "day " << d << ": XA printed no bar, its row is the marked one";
        ASSERT_EQ(db_->last_backtest_positions.size(), 1u);
        const Position& row = db_->last_backtest_positions.front();
        EXPECT_EQ(row.symbol, "XA");
        EXPECT_EQ(static_cast<double>(row.realized_pnl), 0.0) << "the marked row books nothing";
        EXPECT_EQ(static_cast<double>(row.quantity), 2.0);
        EXPECT_EQ(static_cast<double>(row.average_price), 100.0);
    }

    // The mark is the day's, not the symbol's: the next bar day's row carries none.
    EXPECT_TRUE(saved(5, {bar("XA", 5, 104.0), bar("XB", 5, 10.0)}).empty());
    ASSERT_EQ(db_->last_backtest_positions.size(), 1u);
    EXPECT_NEAR(static_cast<double>(db_->last_backtest_positions.front().realized_pnl),
                2.0 * (104.0 - 103.0) * 50.0, 1e-9);
}

// A cash book's row on a day its symbol prints no bar is not written here (its realised flow waits
// for the next written row), so it is not zeroed and carries no mark: the mark says "this
// row books 0 because no bar printed", which is true of a futures row only.
TEST_F(BacktestNoBarRowTest, ACashBooksRowOfANoBarDayCarriesNoMark) {
    for (const auto& s : pm_->get_strategies()) {
        std::dynamic_pointer_cast<BaseStrategy>(s)->set_pnl_accounting_method(
            PnLAccountingMethod::MIXED);
    }
    run(0, {bar("XA", 0, 100.0), bar("XB", 0, 10.0)});
    run(1, {bar("XA", 1, 101.0), bar("XB", 1, 10.0)});
    run(2, {bar("XB", 2, 10.0)});
    ASSERT_TRUE(coord_->save_daily_positions(pm_, "NO_BAR_RUN", wday(2)).is_ok());
    ASSERT_EQ(db_->last_backtest_positions.size(), 1u) << "the held row is handed to the writer";
    EXPECT_TRUE(db_->last_backtest_no_bar_symbols.empty());
}

TEST(BacktestPositionRowMark, TheCellIsTheQuotedMarkOnANoBarRowAndNullOnEveryOtherRow) {
    EXPECT_EQ(backtest_position_row_mark_value(true), "'NO_BAR'");
    EXPECT_EQ(backtest_position_row_mark_value(false), "NULL");
    EXPECT_STREQ(kRowMarkNoBar, "NO_BAR") << "the one value migration 038's CHECK admits";
}
// [end of the mark at the writer's door]

// The same days through the REAL writer into backtest.final_positions: the no-bar rows read
// realized_pnl 0 with row_mark 'NO_BAR', quantity and average price kept; the bar rows read their
// day's P&L with row_mark NULL. Database-backed: runs only with TRADE_NGIN_TEST_DSN (a database
// with migration 038), on a probe run id it deletes before and after.
class BacktestNoBarRowDbTest : public BacktestNoBarRowTest {
protected:
    static constexpr const char* kRun = "T8B_R50_PROBE_RUN";

    void SetUp() override {
        BacktestNoBarRowTest::SetUp();
        const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
        conn_ = dsn ? std::string(dsn) : std::string();
        if (conn_.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
        real_ = std::make_shared<PostgresDatabase>(conn_);
        if (real_->connect().is_error() || !real_->is_connected()) {
            conn_.clear();
            GTEST_SKIP() << "database unreachable; skipping";
        }
        purge();
        coord_->db_ = real_;  // the daily save writes through the real writer
    }
    void TearDown() override {
        if (!conn_.empty()) purge();
        if (real_ && real_->is_connected()) real_->disconnect();
        real_.reset();
        BacktestNoBarRowTest::TearDown();
    }
    void purge() {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        w.exec("DELETE FROM backtest.final_positions WHERE run_id = " + w.quote(kRun));
        w.commit();
    }
    void run_and_save(int d, const std::vector<Bar>& bars) {
        run(d, bars);
        ASSERT_TRUE(coord_->save_daily_positions(pm_, kRun, wday(d)).is_ok());
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> real_;
};

TEST_F(BacktestNoBarRowDbTest, AStoredNoBarRowReadsZeroWithTheMarkAndABarRowItsPnlWithNone) {
    run_and_save(0, {bar("XA", 0, 100.0), bar("XB", 0, 10.0)});
    run_and_save(1, {bar("XA", 1, 101.0), bar("XB", 1, 10.0)});
    run_and_save(2, {bar("XA", 2, 103.0), bar("XB", 2, 10.0)});
    run_and_save(3, {bar("XB", 3, 10.0)});
    run_and_save(4, {bar("XB", 4, 10.0)});
    run_and_save(5, {bar("XA", 5, 104.0), bar("XB", 5, 10.0)});

    pqxx::connection c(conn_);
    pqxx::work w(c);
    const auto rows = w.exec(
        "SELECT date::text, symbol, quantity, average_price, realized_pnl, "
        "coalesce(row_mark, '(null)') FROM backtest.final_positions WHERE run_id = " +
        w.quote(kRun) + " ORDER BY date");
    // wday(0) is 2026-01-05; the book holds XA from the first fed cycle, wday(1), on. That first
    // held bar has no earlier close to book against: it books 0 and, the symbol having printed,
    // carries no mark (a 0 is not what the mark says; no bar is).
    struct Want {
        const char* date;
        double realized;
        const char* mark;
    };
    const std::vector<Want> want = {{"2026-01-06", 0.0, "(null)"},   {"2026-01-07", 200.0, "(null)"},
                                    {"2026-01-08", 0.0, "NO_BAR"},   {"2026-01-09", 0.0, "NO_BAR"},
                                    {"2026-01-10", 100.0, "(null)"}};
    ASSERT_EQ(rows.size(), want.size()) << "one stored row of XA a day from the first held day";
    for (size_t i = 0; i < want.size(); ++i) {
        SCOPED_TRACE(want[i].date);
        EXPECT_EQ(rows[i][0].as<std::string>(), want[i].date);
        EXPECT_EQ(rows[i][1].as<std::string>(), "XA");
        EXPECT_EQ(rows[i][2].as<double>(), 2.0) << "the quantity is kept";
        EXPECT_EQ(rows[i][3].as<double>(), 100.0) << "the average price is kept";
        EXPECT_NEAR(rows[i][4].as<double>(), want[i].realized, 1e-9);
        EXPECT_EQ(rows[i][5].as<std::string>(), want[i].mark)
            << "row_mark: 'NO_BAR' on a row whose symbol printed no bar that date, NULL elsewhere";
    }
}
