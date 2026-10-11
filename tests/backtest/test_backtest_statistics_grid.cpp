// tests/backtest/test_backtest_statistics_grid.cpp
//
// T-8D R1, R2, R3 and T-8D-2 R76, R79, the backtest half: a run's statistics are taken on the
// statistics grid of its own bars and annualised with the sessions a year the app hands the
// coordinator. A futures run's grid is the Sunday-to-Friday dates on which at least nine distinct
// stored symbols printed; a Saturday point and a thin date stay rows of the equity curve and are
// no observations of the statistics. An equity run's grid is its own rows.
//
// The run tests drive a whole backtest (run_portfolio) on bars the test controls: ten symbols,
// one of them held, with a Saturday bar of the held symbol alone and a weekday on which only
// three symbols print.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../core/test_base.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "cost_basis_test_helpers.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/listing_dates.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"
// The run marks its held contract with the registry's multiplier: the test registers its ten
// (the registry's map has no public writer).
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;
using namespace trade_ngin::testing::cost_basis;

namespace {

constexpr double kFuturesK = 311.0574;
constexpr int kLastDay = 19;      // four weeks of weekdays from Monday 2025-01-06
constexpr int kThinDay = 8;       // Thu 2025-01-16: only three symbols print
constexpr int kSaturdayOf = 10;   // the Saturday before trading day 10: Sat 2025-01-18, held symbol only
constexpr int kSundayOf = 15;     // the Sunday before trading day 15: Sun 2025-01-26, every symbol

std::string symbol_of(int i) { return "G" + std::to_string(i) + ".v.0"; }
const std::string kHeld = "G0.v.0";

std::vector<std::string> universe() {
    std::vector<std::string> out;
    for (int i = 0; i < 10; ++i) out.push_back(symbol_of(i));
    return out;
}

// A close that moves every bar, so every curve row after the first fill carries P&L.
double close_of(int i, double step) { return 100.0 + 3.0 * i + 1.7 * std::sin(1.1 * step + i); }

std::vector<Row> rows() {
    std::vector<Row> out;
    for (int d = 0; d <= kLastDay; ++d) {
        for (int i = 0; i < 10; ++i) {
            if (d == kThinDay && i >= 3) continue;  // the thin date: G0, G1, G2 only
            out.push_back({symbol_of(i), d, close_of(i, d), 100000.0});
        }
    }
    // The Saturday bar of the held symbol alone, and a full Sunday session.
    Row saturday{kHeld, kSaturdayOf, close_of(0, kSaturdayOf - 0.5) - 2.0, 100000.0};
    saturday.weekend_before = 2;
    out.push_back(saturday);
    for (int i = 0; i < 10; ++i) {
        Row sunday{symbol_of(i), kSundayOf, close_of(i, kSundayOf - 0.5), 100000.0};
        sunday.weekend_before = 1;
        out.push_back(sunday);
    }
    return out;
}

const std::string kThinDate = "2025-01-16";
const std::string kSaturdayDate = "2025-01-18";
const std::string kSundayDate = "2025-01-26";

std::map<Timestamp, std::vector<Bar>> bars_of(const std::vector<Row>& rows) {
    std::map<Timestamp, std::vector<Bar>> out;
    for (const auto& r : rows) {
        Bar b;
        b.symbol = r.symbol;
        b.timestamp = row_time(r);
        b.close = Decimal(r.close);
        out[b.timestamp].push_back(b);
    }
    return out;
}

}  // namespace

// ===== the grid of a run's bars =====

TEST(BacktestStatisticsGridOfBars, AFuturesRunsGridIsItsSundayToFridayDatesWithNineSymbolsPrinting) {
    ASSERT_EQ(ymd(kThinDay), kThinDate);
    const auto grouped = bars_of(rows());
    ASSERT_EQ(grouped.size(), 22u) << "20 weekdays, the Saturday and the Sunday";
    const auto grid =
        BacktestCoordinator::statistics_grid(grouped, AssetClass::FUTURES, kFuturesK);
    EXPECT_FALSE(grid.own_rows);
    EXPECT_DOUBLE_EQ(grid.sessions_per_year, kFuturesK);
    const std::set<std::string> dates(grid.dates.begin(), grid.dates.end());
    EXPECT_EQ(dates.size(), 20u) << "22 bar dates less the thin date and the Saturday";
    EXPECT_EQ(dates.count(kThinDate), 0u) << "three symbols printing is no session";
    EXPECT_EQ(dates.count(kSaturdayDate), 0u) << "a Saturday is never a grid date";
    EXPECT_EQ(dates.count(kSundayDate), 1u) << "a Sunday session is its own observation";
    EXPECT_EQ(dates.count("2025-01-06"), 1u);
}

TEST(BacktestStatisticsGridOfBars, AnEquityRunsGridIsItsOwnRows) {
    const auto grid =
        BacktestCoordinator::statistics_grid(bars_of(rows()), AssetClass::EQUITIES, 252.0);
    EXPECT_TRUE(grid.own_rows);
    EXPECT_TRUE(grid.dates.empty());
    EXPECT_DOUBLE_EQ(grid.sessions_per_year, 252.0);
}

// A window that trades before a listing date holds the predecessor (ES for MES) as a symbol of
// its own, on copies of the listed contract's bars. The predicate counts the STORED symbols: the
// copy is not a second symbol printing.
//   | date       | bars                                  | distinct stored | grid date |
//   | 2025-01-06 | MES + its copy ES + seven others      | 8               | no        |
//   | 2025-01-07 | MES + its copy ES + eight others      | 9               | yes       |
TEST(BacktestStatisticsGridOfBars, APredecessorsCopiedBarsAreNotCountedASecondSymbol) {
    ListingDates::instance().set({{"MES", "ES", "2019-05-06", 10.0}});
    std::map<Timestamp, std::vector<Bar>> grouped;
    for (int d = 0; d < 2; ++d) {
        std::vector<Bar> bars;
        for (const std::string symbol : {"MES.v.0", "ES.v.0"}) {
            Bar b;
            b.symbol = symbol;
            b.timestamp = trading_day(d);
            bars.push_back(b);
        }
        for (int i = 0; i < 7 + d; ++i) {
            Bar b;
            b.symbol = symbol_of(i);
            b.timestamp = trading_day(d);
            bars.push_back(b);
        }
        grouped[trading_day(d)] = bars;
    }
    const auto grid =
        BacktestCoordinator::statistics_grid(grouped, AssetClass::FUTURES, kFuturesK);
    ListingDates::instance().clear();
    EXPECT_EQ(grid.dates, (std::vector<std::string>{"2025-01-07"}));
    // Without a listing date ES is a symbol like any other: both dates reach nine.
    EXPECT_EQ(BacktestCoordinator::statistics_grid(grouped, AssetClass::FUTURES, kFuturesK).dates.size(),
              2u);
}

// ===== a whole run =====

class BacktestStatisticsGridRun : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        MarketDataBus::instance().set_publish_enabled(true);
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::ERR;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
        auto& registry = InstrumentRegistry::instance();
        for (int i = 0; i < 10; ++i) {
            const std::string root = "G" + std::to_string(i);
            FuturesSpec spec;
            spec.root_symbol = root;
            spec.exchange = "CME";
            spec.currency = "USD";
            spec.multiplier = 50.0;
            spec.tick_size = 0.25;
            spec.commission_per_contract = 0.0;
            spec.initial_margin = 1.0;
            spec.maintenance_margin = 1.0;
            spec.weight = 1.0;
            spec.trading_hours = "09:30-16:00";
            registry.instruments_[root] = std::make_shared<FuturesInstrument>(root, spec);
        }
        registry.initialized_ = true;
        db_ = std::make_shared<ServingDb>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }

    void TearDown() override {
        MarketDataBus::instance().set_publish_enabled(true);
        (void)MarketDataBus::instance().unsubscribe("PORTFOLIO_MANAGER");
        coord_.reset();
        strat_.reset();
        pm_.reset();
        db_.reset();
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_.clear();
        registry.initialized_ = false;
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    Result<BacktestResults> run(AssetClass asset_class, double sessions_per_year,
                                bool store = false) {
        static int n = 0;
        ++n;
        db_->rows = rows_;

        BacktestCoordinatorConfig cc;
        cc.initial_capital = 1'000'000.0;
        cc.store_results = false;
        cc.store_trade_details = store;
        cc.portfolio_id = "STAT_GRID_TEST";
        cc.csv_output_path = temp_csv_dir("stat_grid_" + std::to_string(n));
        cc.statistics_sessions_per_year = sessions_per_year;
        coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
        EXPECT_TRUE(coord_->initialize().is_ok());

        PortfolioConfig pc{1'000'000.0, 1.0, 0.0, /*optimization=*/false};
        pc.opt_config.capital = 1'000'000.0;
        pc.risk_config.capital = 1'000'000.0;
        pc.risk_modules = {test_none_module()};
        pm_ = std::make_shared<PortfolioManager>(pc, "PM_STAT_GRID_" + std::to_string(n));

        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {asset_class};
        sc.frequencies = {DataFrequency::DAILY};
        for (const auto& s : universe()) {
            sc.trading_params[s] = 1.0;
            sc.position_limits[s] = 1.0e6;
        }
        strat_ = std::make_shared<ScheduledStrategy>("STAT_GRID_S", sc, db_);
        strat_->targets = targets_;
        EXPECT_TRUE(strat_->initialize().is_ok());
        EXPECT_TRUE(strat_->start().is_ok());
        EXPECT_TRUE(pm_->add_strategy(strat_, 1.0, false).is_ok());

        return coord_->run_portfolio(pm_, universe(), trading_day(0), trading_day(kLastDay),
                                     asset_class, DataFrequency::DAILY);
    }

    // The run's bars and the sleeve's schedule; a test may replace either before run().
    std::vector<Row> rows_ = rows();
    std::map<std::string, std::map<int, double>> targets_ = {
        {kHeld, {{0, 100.0}}}};  // held from the first cycle to the end

    std::shared_ptr<ServingDb> db_;
    std::unique_ptr<BacktestCoordinator> coord_;
    std::shared_ptr<PortfolioManager> pm_;
    std::shared_ptr<ScheduledStrategy> strat_;
};

namespace {

struct Hand {
    int n{0};
    double volatility{0.0};
    double sharpe{0.0};
    double downside{0.0};
    double max_drawdown{0.0};
};

// The statistics of a list of levels (the base first), by the ruled formulas, written out here
// so the run is not compared with the calculator's own code.
Hand hand(const std::vector<double>& levels, double k) {
    Hand h;
    std::vector<double> r;
    for (size_t i = 1; i < levels.size(); ++i) r.push_back(levels[i] / levels[i - 1] - 1.0);
    h.n = static_cast<int>(r.size());
    double mean = 0.0, squares = 0.0, shortfall = 0.0;
    for (double x : r) mean += x;
    mean /= static_cast<double>(r.size());
    for (double x : r) {
        squares += (x - mean) * (x - mean);
        if (x < 0.0) shortfall += x * x;
    }
    h.volatility = std::sqrt(squares / static_cast<double>(r.size() - 1)) * std::sqrt(k);
    h.sharpe = mean * k / h.volatility;
    h.downside = std::sqrt(shortfall / static_cast<double>(r.size())) * std::sqrt(k);
    double peak = levels.front();
    for (double level : levels) {
        peak = std::max(peak, level);
        h.max_drawdown = std::max(h.max_drawdown, (peak - level) / peak);
    }
    return h;
}

}  // namespace

// T-8D-2 R79, executed through run_portfolio: the stored curve keeps the Saturday point and the
// thin date as rows; the statistics read the grid (those two rows folded into the next grid
// return) at the K the coordinator was handed; total_return reads every row.
TEST_F(BacktestStatisticsGridRun, AFuturesRunFoldsItsSaturdayPointAndItsThinDateIntoTheNextGridReturn) {
    auto result = run(AssetClass::FUTURES, kFuturesK);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    const BacktestResults& r = result.value();

    // The curve: the seed and one point per bar date (the seed shares the first bar's date).
    std::map<std::string, double> level_of;
    for (const auto& [timestamp, level] : r.equity_curve) level_of[core::format_utc_date(timestamp)] = level;
    ASSERT_EQ(level_of.size(), 22u);
    ASSERT_EQ(level_of.count(kThinDate), 1u) << "the thin date is a row of the curve";
    ASSERT_EQ(level_of.count(kSaturdayDate), 1u) << "the Saturday point is a row of the curve";

    std::vector<double> every_row, on_the_grid;
    std::string previous;
    for (const auto& [date, level] : level_of) {
        every_row.push_back(level);
        if (date != kThinDate && date != kSaturdayDate) on_the_grid.push_back(level);
        if (date == kThinDate || date == kSaturdayDate) {
            EXPECT_NE(level, level_of.at(previous))
                << date << ": the scenario needs P&L on the row that is no observation";
        }
        previous = date;
    }
    ASSERT_NE(every_row[5], every_row[4]) << "the held contract is marked on an ordinary day";
    const Hand grid = hand(on_the_grid, kFuturesK);
    const Hand rows_k = hand(every_row, kFuturesK);
    ASSERT_GT(std::abs(grid.volatility - rows_k.volatility), 1e-6)
        << "the scenario must tell the grid from the rows";

    EXPECT_EQ(r.statistics_days, 19) << "22 rows: the base, 19 grid returns, two folded rows";
    EXPECT_EQ(grid.n, 19);
    EXPECT_DOUBLE_EQ(r.statistics_sessions_per_year, kFuturesK);
    EXPECT_NEAR(r.volatility, grid.volatility, 1e-12);
    EXPECT_NEAR(r.sharpe_ratio, grid.sharpe, 1e-9);
    EXPECT_NEAR(r.downside_volatility, grid.downside, 1e-12);
    EXPECT_NEAR(r.max_drawdown, grid.max_drawdown, 1e-12);
    EXPECT_NEAR(r.total_return, every_row.back() / every_row.front() - 1.0, 1e-12)
        << "a level, on every row";
    // Not the rows at 252 with the population deviation (the calculator before T-8b).
    const Hand rows_252 = hand(every_row, 252.0);
    EXPECT_GT(std::abs(r.volatility - rows_252.volatility * std::sqrt(20.0 / 21.0)), 1e-6);
}

// The same bars as an equity run: its grid is its own rows, at the K handed in.
TEST_F(BacktestStatisticsGridRun, AnEquityRunReadsEveryRowAtItsOwnSessionsAYear) {
    auto result = run(AssetClass::EQUITIES, 252.0);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    const BacktestResults& r = result.value();
    std::map<std::string, double> level_of;
    for (const auto& [timestamp, level] : r.equity_curve) level_of[core::format_utc_date(timestamp)] = level;
    std::vector<double> every_row;
    for (const auto& [date, level] : level_of) every_row.push_back(level);
    const Hand rows_252 = hand(every_row, 252.0);
    EXPECT_EQ(r.statistics_days, static_cast<int>(every_row.size()) - 1);
    EXPECT_DOUBLE_EQ(r.statistics_sessions_per_year, 252.0);
    EXPECT_NEAR(r.volatility, rows_252.volatility, 1e-12);
    EXPECT_NEAR(r.sharpe_ratio, rows_252.sharpe, 1e-9);
    EXPECT_NEAR(r.max_drawdown, rows_252.max_drawdown, 1e-12);
}

// The factor has one source. A run that was handed none is refused before it loads a bar: it
// does not fall back on a constant.
TEST_F(BacktestStatisticsGridRun, ARunWithoutSessionsAYearIsRefused) {
    for (const double k : {0.0, -252.0}) {
        auto result = run(AssetClass::FUTURES, k);
        ASSERT_TRUE(result.is_error()) << k;
        EXPECT_NE(std::string(result.error()->what()).find("STATISTICS STOP"), std::string::npos)
            << result.error()->what();
        EXPECT_TRUE(db_->market_calls.empty()) << "the run loaded bars before it was refused";
    }
}

// ===== STAT_QUALITY (T-8D F10, T-8D-2 R82): the summary's lines of a whole run =====

class BacktestStatQualityRun : public BacktestStatisticsGridRun {
protected:
    const std::string kSecond = "G5.v.0";
    static constexpr int kGapDay = 12;  // Wed 2025-01-22: nine symbols print, G5 does not

    // G0 and G5 are held from the first fed cycle (Tue 2025-01-07) to the end. G5 prints no bar
    // on three stored dates: the thin date (Thu 2025-01-16) and the Saturday point (2025-01-18),
    // neither a date of the futures grid, and Wed 2025-01-22, which is one.
    void SetUp() override {
        BacktestStatisticsGridRun::SetUp();
        targets_[kSecond] = {{0, 100.0}};
        std::vector<Row> kept;
        for (const auto& row : rows_) {
            if (row.symbol == kSecond && row.day == kGapDay && row.weekend_before == 0) continue;
            kept.push_back(row);
        }
        rows_ = kept;
    }

    // The STAT_QUALITY lines of the run's log, without the logger's prefix.
    std::vector<std::string> stat_quality_lines_of_run(AssetClass asset_class, double k) {
        Logger::instance().set_level(LogLevel::INFO);
        ::testing::internal::CaptureStdout();
        auto result = run(asset_class, k);
        const std::string log = ::testing::internal::GetCapturedStdout();
        Logger::instance().set_level(LogLevel::ERR);
        EXPECT_TRUE(result.is_ok());
        if (result.is_ok()) {
            std::set<std::string> dates;
            for (const auto& [timestamp, level] : result.value().equity_curve) {
                (void)level;
                dates.insert(core::format_utc_date(timestamp));
            }
            EXPECT_EQ(dates.size(), 22u) << "20 weekdays, the Saturday point and the Sunday";
        }
        std::vector<std::string> out;
        size_t at = 0;
        while ((at = log.find("STAT_QUALITY", at)) != std::string::npos) {
            const size_t end = log.find('\n', at);
            out.push_back(log.substr(at, end == std::string::npos ? end : end - at));
            at = end == std::string::npos ? log.size() : end;
        }
        return out;
    }
};

// 22 stored dates; the book holds nothing on the first (Mon 2025-01-06: no signal bar yet) and
// two symbols on the other 21. Three of the 21 hold G5 with no bar, one of them a grid date.
TEST_F(BacktestStatQualityRun, AFuturesRunsSummaryNamesEachStoredDateAHeldSymbolPrintedNoBarOn) {
    const auto lines = stat_quality_lines_of_run(AssetClass::FUTURES, kFuturesK);
    ASSERT_EQ(lines.size(), 4u) << "the run line and one line a date";
    EXPECT_EQ(lines[0],
              "STAT_QUALITY run no_bar_days=3 of 21 held days; grid_days=1 rows=3 symbols=1: the "
              "stored dates on which a held symbol printed no bar (held, marked at last close), "
              "and how many of them are dates of the statistics grid");
    EXPECT_EQ(lines[1], "STAT_QUALITY date=2025-01-16 held=2 no_bar=1 grid=no: G5.v.0");
    EXPECT_EQ(lines[2], "STAT_QUALITY date=2025-01-18 held=2 no_bar=1 grid=no: G5.v.0");
    EXPECT_EQ(lines[3], "STAT_QUALITY date=2025-01-22 held=2 no_bar=1 grid=yes: G5.v.0");
}

// The same bars as an equity run: a cash book's held symbol with no bar is named too (its row
// carries no mark), and every row is a date of its grid.
TEST_F(BacktestStatQualityRun, AnEquityRunsSummaryNamesThemTooAndEveryDateIsOnItsGrid) {
    const auto lines = stat_quality_lines_of_run(AssetClass::EQUITIES, 252.0);
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0].rfind("STAT_QUALITY run no_bar_days=3 of 21 held days; grid_days=3 rows=3 symbols=1:", 0),
              0u)
        << lines[0];
    EXPECT_EQ(lines[1], "STAT_QUALITY date=2025-01-16 held=2 no_bar=1 grid=yes: G5.v.0");
    EXPECT_EQ(lines[2], "STAT_QUALITY date=2025-01-18 held=2 no_bar=1 grid=yes: G5.v.0");
    EXPECT_EQ(lines[3], "STAT_QUALITY date=2025-01-22 held=2 no_bar=1 grid=yes: G5.v.0");
}

// A run whose held symbol prints on every stored date: the run line alone, with zeros.
TEST_F(BacktestStatisticsGridRun, ARunWithEveryHeldSymbolPrintingLogsTheStatQualityRunLineAlone) {
    Logger::instance().set_level(LogLevel::INFO);
    ::testing::internal::CaptureStdout();
    auto result = run(AssetClass::FUTURES, kFuturesK);
    const std::string log = ::testing::internal::GetCapturedStdout();
    Logger::instance().set_level(LogLevel::ERR);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    const size_t first = log.find("STAT_QUALITY");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(log.find("STAT_QUALITY", first + 1), std::string::npos) << "one line";
    const std::string expected =
        "STAT_QUALITY run no_bar_days=0 of 21 held days; grid_days=0 rows=0 symbols=0:";
    EXPECT_EQ(log.substr(first, expected.size()), expected);
}

// Migration 031 through a whole run and its save: the statistics are taken on the same grid
// series as the others (the Saturday point and the thin date folded), in fractions, and reach
// the summary writer as typed cells.
TEST_F(BacktestStatisticsGridRun, AFuturesRunTakesTheStatisticsOfMigration031OnItsGridAndStoresThem) {
    auto result = run(AssetClass::FUTURES, kFuturesK, /*store=*/true);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    const BacktestResults& r = result.value();

    std::map<std::string, double> level_of;
    for (const auto& [timestamp, level] : r.equity_curve) level_of[core::format_utc_date(timestamp)] = level;
    // The grid returns by hand: the smallest, its date, and the smallest of every ROW's return.
    double previous = 0.0, worst = 0.0, worst_row = 0.0, row_before = 0.0;
    std::string worst_date;
    bool first = true;
    for (const auto& [date, level] : level_of) {
        if (!first) worst_row = std::min(worst_row, level / row_before - 1.0);
        row_before = level;
        if (date == kThinDate || date == kSaturdayDate) continue;
        if (!first && level / previous - 1.0 < worst) {
            worst = level / previous - 1.0;
            worst_date = date;
        }
        previous = level;
        first = false;
    }
    ASSERT_LT(worst, 0.0);
    const BacktestStatisticsColumns& c = r.statistics_columns;
    ASSERT_TRUE(c.worst_day);
    EXPECT_NEAR(*c.worst_day, worst, 1e-12);
    EXPECT_EQ(c.worst_day_date.value_or(""), worst_date);
    EXPECT_NE(worst_date, kThinDate);
    EXPECT_NE(worst_date, kSaturdayDate);
    EXPECT_EQ(c.worst_day_symbol.value_or(""), kHeld) << "the one contract the book holds lost";
    // No overlay sleeve in this book: no risk_detail, no sizing capital, the three NULL.
    EXPECT_FALSE(c.volatility_sizing);
    EXPECT_FALSE(c.max_drawdown_sizing);
    EXPECT_FALSE(c.worst_day_sizing);
    EXPECT_FALSE(c.monthly_skew) << "four weeks: no twelve complete months";
    ASSERT_TRUE(c.calendar_year_returns);
    const auto& year = (*c.calendar_year_returns)["2025"];
    EXPECT_TRUE(year["partial"].get<bool>());
    std::vector<double> on_the_grid;
    for (const auto& [date, level] : level_of) {
        if (date != kThinDate && date != kSaturdayDate) on_the_grid.push_back(level);
    }
    EXPECT_NEAR(year["return"].get<double>(), on_the_grid.back() / on_the_grid.front() - 1.0, 1e-12);
    EXPECT_EQ(c.losing_years.value_or(-1), 0);
    EXPECT_EQ(c.total_strategy_fills.value_or(-1), 1) << "the one opening fill";
    EXPECT_EQ(r.total_trades, 0) << "nothing was reduced: no round trip";

    auto saved = coord_->save_portfolio_results_to_db(r, {"STAT_GRID_S"}, {{"STAT_GRID_S", 1.0}},
                                                      pm_, nlohmann::json::object());
    ASSERT_TRUE(saved.is_ok()) << saved.error()->what();
    ASSERT_EQ(db_->call_count("store_backtest_summary"), 1);
    std::map<std::string, std::optional<std::string>> cell;
    for (const auto& stored : db_->last_backtest_summary_cells) cell[stored.column] = stored.value;
    ASSERT_EQ(cell.size(), 13u);
    ASSERT_TRUE(cell.at("worst_day"));
    EXPECT_NEAR(std::stod(*cell.at("worst_day")), worst, 1e-9 * std::abs(worst));
    EXPECT_EQ(cell.at("worst_day_date").value_or(""), worst_date);
    EXPECT_EQ(cell.at("worst_day_symbol").value_or(""), kHeld);
    EXPECT_EQ(cell.at("total_strategy_fills").value_or(""), "1");
    EXPECT_EQ(cell.at("losing_years").value_or(""), "0");
    EXPECT_FALSE(cell.at("volatility_sizing")) << "a NULL cell";
    EXPECT_FALSE(cell.at("monthly_skew"));
    // This book names no overlay sleeve: no row has a risk_detail, so the curve write names
    // neither that column nor the six overlay columns (the statement an equity run writes).
    ASSERT_EQ(db_->call_count("store_backtest_equity_curve_batch"), 1);
    EXPECT_TRUE(r.equity_overlay.empty());
    EXPECT_TRUE(db_->last_equity_risk_detail.empty());
    EXPECT_TRUE(db_->last_equity_curve_cells.empty());
    // The older columns travel as before, and none of the new ones is among them.
    EXPECT_EQ(db_->last_backtest_summary_metrics.count("total_trades"), 1u);
    EXPECT_EQ(db_->last_backtest_summary_metrics.count("worst_day"), 0u);
}

// The same bars as an equity run: every row is a return, so the Saturday row can be the worst
// day; the three sizing-capital cells are NULL on an equity run.
TEST_F(BacktestStatisticsGridRun, AnEquityRunTakesThemOnItsOwnRowsWithNoSizingCapital) {
    auto result = run(AssetClass::EQUITIES, 252.0);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    const BacktestResults& r = result.value();
    std::map<std::string, double> level_of;
    for (const auto& [timestamp, level] : r.equity_curve) level_of[core::format_utc_date(timestamp)] = level;
    double previous = 0.0, worst = 0.0;
    std::string worst_date;
    bool first = true;
    for (const auto& [date, level] : level_of) {
        if (!first && level / previous - 1.0 < worst) {
            worst = level / previous - 1.0;
            worst_date = date;
        }
        previous = level;
        first = false;
    }
    const BacktestStatisticsColumns& c = r.statistics_columns;
    ASSERT_TRUE(c.worst_day);
    EXPECT_NEAR(*c.worst_day, worst, 1e-12);
    EXPECT_EQ(c.worst_day_date.value_or(""), worst_date);
    EXPECT_FALSE(c.volatility_sizing);
    EXPECT_FALSE(c.max_drawdown_sizing);
    EXPECT_FALSE(c.worst_day_sizing);
    EXPECT_FALSE(c.worst_day_sizing_date);
    EXPECT_FALSE(c.worst_day_sizing_symbol);
}

// ===== a run whose id is already stored (T-8D-2 R49) =====
//
// The run id is the strategy ids and the window's end instant, so a second run of the same
// strategies to the same end date carries the first run's id. A storing run asks, before its
// first cycle, whether backtest.results or backtest.final_positions holds a row of its id, and is
// refused if either does. These drive a whole run (run_portfolio) on a database that answers the
// two counts.
//
//   | case | results rows | final_positions rows | the run stores | outcome            | positions writes |
//   | 1    | 1            | 0                    | yes            | refused, an error  | 0                |
//   | 2    | 0            | 3                    | yes            | refused, an error  | 0                |
//   | 3    | 0            | 0                    | yes            | runs               | one a stored day |
//   | 4    | 1            | 3                    | no             | runs, asks nothing | 0                |
//   | 5    | unreadable   | unreadable           | yes            | runs (not refused) | one a stored day |
class BacktestStoredRunRefusal : public BacktestStatisticsGridRun {
protected:
    std::shared_ptr<StoredRunDb> use_stored_run_db() {
        auto db = std::make_shared<StoredRunDb>("mock://testdb");
        EXPECT_TRUE(db->connect().is_ok());
        db_ = db;
        return db;
    }
};

TEST_F(BacktestStoredRunRefusal, ARunWhoseIdHasAResultsRowIsRefusedBeforeItsFirstCycleAndStoresNothing) {
    auto db = use_stored_run_db();
    db->results_rows = 1;
    auto result = run(AssetClass::FUTURES, kFuturesK, /*store=*/true);
    ASSERT_TRUE(result.is_error()) << "the run went on over a stored run of the same id";
    const std::string what = result.error()->what();
    EXPECT_NE(what.find("BACKTEST REFUSED: run_id STAT_GRID_S_"), std::string::npos) << what;
    EXPECT_NE(what.find("backtest.results rows: 1, backtest.final_positions rows: 0"),
              std::string::npos)
        << what;
    EXPECT_EQ(db->call_count("store_backtest_positions"), 0) << "a refused run stores no position";
    EXPECT_EQ(db->count_queries.size(), 2u);
}

TEST_F(BacktestStoredRunRefusal, ARunWhoseIdHasFinalPositionsRowsIsRefusedAndStoresNothing) {
    auto db = use_stored_run_db();
    db->final_positions_rows = 3;
    auto result = run(AssetClass::FUTURES, kFuturesK, /*store=*/true);
    ASSERT_TRUE(result.is_error()) << "the run went on over a stored run's positions";
    const std::string what = result.error()->what();
    EXPECT_NE(what.find("backtest.results rows: 0, backtest.final_positions rows: 3"),
              std::string::npos)
        << what;
    EXPECT_EQ(db->call_count("store_backtest_positions"), 0);
}

TEST_F(BacktestStoredRunRefusal, ARunWhoseIdIsNotStoredAsksBothTablesAndRuns) {
    auto db = use_stored_run_db();
    auto result = run(AssetClass::FUTURES, kFuturesK, /*store=*/true);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    ASSERT_EQ(db->count_queries.size(), 2u) << "one count a table, asked once";
    EXPECT_NE(db->count_queries[0].find("FROM backtest.results WHERE run_id = 'STAT_GRID_S_"),
              std::string::npos)
        << db->count_queries[0];
    EXPECT_NE(db->count_queries[1].find("FROM backtest.final_positions WHERE run_id = 'STAT_GRID_S_"),
              std::string::npos)
        << db->count_queries[1];
    EXPECT_GT(db->call_count("store_backtest_positions"), 0) << "the run stored its daily positions";
}

TEST_F(BacktestStoredRunRefusal, ARunThatStoresNothingAsksNothingAndIsNotRefused) {
    auto db = use_stored_run_db();
    db->results_rows = 1;
    db->final_positions_rows = 3;
    auto result = run(AssetClass::FUTURES, kFuturesK, /*store=*/false);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    EXPECT_TRUE(db->count_queries.empty());
    EXPECT_EQ(db->call_count("store_backtest_positions"), 0);
}

TEST_F(BacktestStoredRunRefusal, ACountThatCannotBeReadDoesNotRefuseTheRun) {
    auto db = use_stored_run_db();
    db->counts_fail = true;
    auto result = run(AssetClass::FUTURES, kFuturesK, /*store=*/true);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    EXPECT_EQ(db->count_queries.size(), 1u) << "the first count failed; the second is not asked";
    EXPECT_GT(db->call_count("store_backtest_positions"), 0);
}
