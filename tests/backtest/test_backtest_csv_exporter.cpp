#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
// The registry opened (after everything it includes), so that the fixture can empty and restore the
// process singleton.
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument.hpp"
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#undef private
#include "../core/test_base.hpp"
#include "../strategy/csv_test_sleeve.hpp"
#include "trade_ngin/backtest/backtest_csv_exporter.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

Position make_pos(const std::string& sym, double qty, double avg_price = 100.0) {
    return Position(sym, Quantity(qty), Price(avg_price), Decimal(0.0), Decimal(0.0),
                    Timestamp{});
}

Timestamp date_at(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;  // mid-day to avoid timezone day shifts in format_date
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

// A book of one sleeve.
SleevePositions one_sleeve(std::unordered_map<std::string, Position> positions) {
    return SleevePositions{{"SLEEVE", std::move(positions)}};
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        out.push_back(line);
    return out;
}

std::vector<std::string> cells_of(const std::string& line) {
    std::vector<std::string> out;
    std::string cell;
    std::istringstream in(line);
    while (std::getline(in, cell, ','))
        out.push_back(cell);
    if (!line.empty() && line.back() == ',')
        out.push_back("");
    return out;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::stringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

}  // namespace

class BacktestCSVExporterTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        // These cases assert the exporter's output with NO instrument registered (notional =
        // quantity x price). The registry is a process singleton: in one process an earlier
        // suite's instruments would change the rows, so it is emptied here and put back after.
        {
            auto& registry = InstrumentRegistry::instance();
            saved_instruments_ = registry.instruments_;
            saved_initialized_ = registry.initialized_;
            registry.instruments_.clear();
        }
        // Per-test unique temp dir to keep tests isolated.
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        out_dir_ = std::filesystem::temp_directory_path() /
                   ("trade_ngin_csv_exporter_" + std::string(info->name()));
        std::filesystem::remove_all(out_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(out_dir_);
        {
            auto& registry = InstrumentRegistry::instance();
            registry.instruments_ = saved_instruments_;
            registry.initialized_ = saved_initialized_;
        }
        TestBase::TearDown();
    }

    // A sleeve's row as the run holds it after the day's P&L: quantity, the price it is marked
    // at, and the realized P&L the daily save stores.
    static Position stored_row(const std::string& sym, double qty, double mark,
                               double realized = 0.0) {
        Position p = make_pos(sym, qty, mark);
        p.realized_pnl = Decimal(realized);
        return p;
    }

    static ExecutionReport fill_of(const std::string& sym, Side side, double qty, double price) {
        ExecutionReport fill;
        fill.symbol = sym;
        fill.side = side;
        fill.filled_quantity = Quantity(qty);
        fill.fill_price = Price(price);
        return fill;
    }

    // position_changes.csv after one day of a one-sleeve book ("SLEEVE"): the day's rows, the
    // rows of the day before, the sleeve's fills of the day, and whether the sleeve stores a
    // row closed to zero (a cash book does).
    std::string changes_body(std::unordered_map<std::string, Position> current,
                             std::unordered_map<std::string, Position> previous,
                             std::vector<ExecutionReport> fills, bool keeps_closed_rows = false) {
        BacktestCSVExporter exporter(out_dir_.string());
        EXPECT_TRUE(exporter.initialize_files().is_ok());
        const auto changes = BacktestCSVExporter::position_changes(
            one_sleeve(std::move(current)), one_sleeve(std::move(previous)),
            BacktestCSVExporter::fill_prices({{"SLEEVE", std::move(fills)}}),
            [keeps_closed_rows](const std::string&) { return keeps_closed_rows; });
        EXPECT_TRUE(exporter.append_position_changes(date_at(2026, 1, 5), changes).is_ok());
        exporter.finalize();
        return slurp(out_dir_ / "position_changes.csv");
    }

    std::filesystem::path out_dir_;
    std::unordered_map<std::string, std::shared_ptr<Instrument>> saved_instruments_;
    bool saved_initialized_{false};
};

TEST_F(BacktestCSVExporterTest, InitializeCreatesDirectoryAndAllThreeFilesWithHeaders) {
    BacktestCSVExporter exporter(out_dir_.string());
    auto r = exporter.initialize_files();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_TRUE(std::filesystem::exists(out_dir_ / "positions.csv"));
    EXPECT_TRUE(std::filesystem::exists(out_dir_ / "position_changes.csv"));
    EXPECT_FALSE(std::filesystem::exists(out_dir_ / "finalized_positions.csv"))
        << "the file is named for what it holds: the day's position changes";
    EXPECT_TRUE(std::filesystem::exists(out_dir_ / "equity_curve.csv"));

    // finalize() flushes & closes the streams so the headers land on disk.
    exporter.finalize();
    EXPECT_EQ(slurp(out_dir_ / "positions.csv"),
              "date,strategy,symbol,quantity,market_price,notional,pct_of_gross_notional,"
              "pct_of_portfolio_value,forecast,sizing_vol,ema_8,ema_32,ema_64,ema_256\n");
    EXPECT_EQ(slurp(out_dir_ / "position_changes.csv"),
              "date,strategy,symbol,quantity,previous_mark,fill_price,realized_pnl\n");
    EXPECT_EQ(slurp(out_dir_ / "equity_curve.csv"), "date,portfolio_value\n");
}

TEST_F(BacktestCSVExporterTest, InitializeFailsOnUnwritablePath) {
    // The path has an embedded null which std::filesystem rejects.
    BacktestCSVExporter exporter(std::string("/proc/this_path_does_not_exist_anywhere\0bad", 45));
    auto r = exporter.initialize_files();
    EXPECT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::CONVERSION_ERROR);
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsWithoutInitializeReturnsError) {
    BacktestCSVExporter exporter(out_dir_.string());
    auto r = exporter.append_daily_positions(date_at(2026, 1, 5), SleevePositions{}, {}, 1.0e6, 1.0,
                                             1.0, {});
    EXPECT_TRUE(r.is_error());
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsWritesPortfolioHeaderEvenWithNoPositions) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    ASSERT_TRUE(exporter
                    .append_daily_positions(date_at(2026, 1, 5), SleevePositions{}, {}, 1'000'000.0,
                                            0.0, 0.0, {})
                    .is_ok());
    std::string body = slurp(out_dir_ / "positions.csv");
    EXPECT_NE(body.find("# Portfolio Value: 1e+06"), std::string::npos);
    EXPECT_NE(body.find("Date: 2026-01-05"), std::string::npos);
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsSkipsZeroQuantityPositions) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 0.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    ASSERT_TRUE(exporter
                    .append_daily_positions(date_at(2026, 1, 5), one_sleeve(pos), prices,
                                            1'000'000.0, 100.0, 100.0, {})
                    .is_ok());
    std::string body = slurp(out_dir_ / "positions.csv");
    // Header + portfolio comment, but no data row for ES.
    int line_count = 0;
    for (char c : body) if (c == '\n') ++line_count;
    EXPECT_EQ(line_count, 2);  // header + portfolio comment
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsRowsContainSymbolAndComputedFields) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 5.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    // No instrument registry → notional = qty * price = 20000
    // gross_notional=20000 → pct_gross = 1.0
    // portfolio_value=1e6 → pct_portfolio = 0.02
    ASSERT_TRUE(exporter
                    .append_daily_positions(date_at(2026, 1, 5), one_sleeve(pos), prices,
                                            1'000'000.0, 20000.0, 20000.0, {})
                    .is_ok());
    std::string body = slurp(out_dir_ / "positions.csv");
    EXPECT_NE(body.find("2026-01-05,SLEEVE,ES,5,4000,20000,1,0.02,0,0,0,0,0,0\n"),
              std::string::npos)
        << body;
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsHandlesZeroPortfolioValueAndGrossNotional) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 1.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 100.0}};
    // gross_notional=0 → pct_gross=0; portfolio_value=0 → pct_portfolio=0
    ASSERT_TRUE(
        exporter
            .append_daily_positions(date_at(2026, 1, 5), one_sleeve(pos), prices, 0.0, 0.0, 0.0, {})
            .is_ok());
    std::string body = slurp(out_dir_ / "positions.csv");
    EXPECT_NE(body.find("SLEEVE,ES,1,100"), std::string::npos);
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsHandlesMissingMarketPrice) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 5.0)}};
    std::unordered_map<std::string, double> prices;  // empty
    ASSERT_TRUE(exporter
                    .append_daily_positions(date_at(2026, 1, 5), one_sleeve(pos), prices, 1.0e6,
                                            1.0, 1.0, {})
                    .is_ok());
    std::string body = slurp(out_dir_ / "positions.csv");
    EXPECT_NE(body.find("SLEEVE,ES,5,0"), std::string::npos);  // price defaulted to 0
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsAcceptsEmptyStrategyVector) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 3.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    std::vector<std::shared_ptr<StrategyInterface>> strategies;
    auto r = exporter.append_daily_positions(date_at(2026, 1, 5), one_sleeve(pos), prices, 1.0e6,
                                             1.0, 1.0, strategies);
    EXPECT_TRUE(r.is_ok());
}

TEST_F(BacktestCSVExporterTest, AppendEquityCurveWithoutInitializeReturnsError) {
    BacktestCSVExporter exporter(out_dir_.string());
    auto r = exporter.append_equity_curve(date_at(2026, 1, 5), 1'000'000.0);
    EXPECT_TRUE(r.is_error());
}

TEST_F(BacktestCSVExporterTest, AppendEquityCurveWritesDateAndValue) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    ASSERT_TRUE(exporter.append_equity_curve(date_at(2026, 1, 5), 1'234'567.0).is_ok());
    ASSERT_TRUE(exporter.append_equity_curve(date_at(2026, 1, 6), 1'250'000.0).is_ok());
    std::string body = slurp(out_dir_ / "equity_curve.csv");
    EXPECT_NE(body.find("2026-01-05,1.23457e+06"), std::string::npos);
    EXPECT_NE(body.find("2026-01-06,1.25e+06"), std::string::npos);
}

// ===== position_changes.csv (was finalized_positions.csv) ========================================
// One row per sleeve and symbol whose quantity changed that day. Nothing in the row is computed
// by the exporter: previous_mark is the previous day's row's price (empty on a new position),
// fill_price the price the day's fills traded at (empty with no fill), realized_pnl the realized
// P&L of the row the run stores for the day (empty when it stores none). The file used to print
// "entry_price" (that same previous mark, or the day's close on a new position), "exit_price"
// (the day's close) and a realized P&L of its own arithmetic, closed quantity x (close - previous
// mark) x multiplier, which no stored row held.

TEST_F(BacktestCSVExporterTest, PositionChangesWithoutInitializeReturnsError) {
    BacktestCSVExporter exporter(out_dir_.string());
    auto r = exporter.append_position_changes(date_at(2026, 1, 5), {});
    EXPECT_TRUE(r.is_error());
}

TEST_F(BacktestCSVExporterTest, PositionChangesSkipsUnchangedPositions) {
    const std::string body = changes_body({{"ES", stored_row("ES", 5.0, 4010.0, 250.0)}},
                                          {{"ES", stored_row("ES", 5.0, 4000.0)}}, {});
    EXPECT_EQ(body, "date,strategy,symbol,quantity,previous_mark,fill_price,realized_pnl\n");
}

TEST_F(BacktestCSVExporterTest, PositionChangesNewPositionHasItsFillPriceAndNoPreviousMark) {
    // Opened today: 5 bought at 4000 (the fill), marked at today's close 4010. There is no
    // previous mark; the file used to print the day's close as the "entry price".
    const std::string body = changes_body({{"ES", stored_row("ES", 5.0, 4010.0, 0.0)}}, {},
                                          {fill_of("ES", Side::BUY, 5.0, 4000.0)});
    EXPECT_NE(body.find("2026-01-05,SLEEVE,ES,5,,4000,0\n"), std::string::npos) << body;
}

TEST_F(BacktestCSVExporterTest, PositionChangesClosedPositionPrintsTheStoredRealizedOrNone) {
    // A futures sleeve stores no row for a position closed to zero: no realized P&L to print.
    const std::string futures = changes_body({{"ES", stored_row("ES", 0.0, 0.0, 0.0)}},
                                             {{"ES", stored_row("ES", 5.0, 4000.0)}},
                                             {fill_of("ES", Side::SELL, 5.0, 4000.0)});
    EXPECT_NE(futures.find("2026-01-05,SLEEVE,ES,0,4000,4000,\n"), std::string::npos) << futures;
    // A cash book keeps the close-day row with its realized flow: that flow is printed.
    const std::string cash = changes_body({{"ES", stored_row("ES", 0.0, 0.0, 50.0)}},
                                          {{"ES", stored_row("ES", 5.0, 4000.0)}},
                                          {fill_of("ES", Side::SELL, 5.0, 4010.0)}, true);
    EXPECT_NE(cash.find("2026-01-05,SLEEVE,ES,0,4000,4010,50\n"), std::string::npos) << cash;
}

TEST_F(BacktestCSVExporterTest, PositionChangesReducedPositionPrintsTheStoredRealized) {
    // 5 reduced to 2 at 4000, the day's close 4010. The stored row's realized P&L is printed as
    // it is (-17.25 here: a figure no arithmetic of these prices gives); the file's own
    // arithmetic printed 3 x (4010 - 4000) = 30.
    const std::string body = changes_body({{"ES", stored_row("ES", 2.0, 4010.0, -17.25)}},
                                          {{"ES", stored_row("ES", 5.0, 4000.0)}},
                                          {fill_of("ES", Side::SELL, 3.0, 4000.0)});
    EXPECT_NE(body.find("2026-01-05,SLEEVE,ES,2,4000,4000,-17.25\n"), std::string::npos) << body;
}

TEST_F(BacktestCSVExporterTest, PositionChangesIncreasedPositionPrintsTheStoredRealized) {
    // 5 increased to 8: the stored row carries the day's P&L (80); the file printed 0 on every
    // increase.
    const std::string body = changes_body({{"ES", stored_row("ES", 8.0, 4010.0, 80.0)}},
                                          {{"ES", stored_row("ES", 5.0, 4000.0)}},
                                          {fill_of("ES", Side::BUY, 3.0, 4000.0)});
    EXPECT_NE(body.find("2026-01-05,SLEEVE,ES,8,4000,4000,80\n"), std::string::npos) << body;
}

TEST_F(BacktestCSVExporterTest, PositionChangesCoversSymbolsOnlyInPrevious) {
    // The symbol left the book entirely: no current row, so no stored realized; no fill given.
    const std::string body = changes_body({}, {{"NQ", stored_row("NQ", 3.0, 15000.0)}}, {});
    EXPECT_NE(body.find("2026-01-05,SLEEVE,NQ,0,15000,,\n"), std::string::npos) << body;
}

TEST_F(BacktestCSVExporterTest, PositionChangesWithNoFillThatDayLeavesTheFillPriceEmpty) {
    const std::string body = changes_body({{"ES", stored_row("ES", 3.0, 4010.0, 30.0)}},
                                          {{"ES", stored_row("ES", 5.0, 4000.0)}}, {});
    EXPECT_NE(body.find("2026-01-05,SLEEVE,ES,3,4000,,30\n"), std::string::npos)
        << "the day's close is not a fill price: " << body;
}

// The price the day's fills traded at: the quantity-weighted mean of the sleeve's own fills of the
// symbol; a roll leg and a borrow row change no position and are left out; one fill's price is
// printed as it is.
TEST_F(BacktestCSVExporterTest, FillPricesAreTheWeightedMeanOfTheSleevesOwnFills) {
    ExecutionReport roll_close = fill_of("ES", Side::SELL, 2.0, 90.0);
    roll_close.execution_type = ExecutionType::ROLL;
    ExecutionReport roll_open = fill_of("ES", Side::BUY, 2.0, 95.0);
    roll_open.execution_type = ExecutionType::ROLL;
    ExecutionReport borrow = fill_of("NQ", Side::SELL, 9.0, 1.0);
    borrow.execution_type = ExecutionType::BORROW;
    const SleeveFillPrices prices = BacktestCSVExporter::fill_prices(
        {{"TREND",
          {fill_of("ES", Side::BUY, 1.0, 100.0), fill_of("ES", Side::BUY, 3.0, 104.0), roll_close,
           roll_open, fill_of("NQ", Side::SELL, 3.0, 0.1), borrow}},
         {"FAST", {fill_of("ES", Side::SELL, 2.0, 100.0)}},
         {"ROLLS_ONLY", {roll_close, roll_open}}});
    ASSERT_EQ(prices.count("TREND"), 1u);
    EXPECT_DOUBLE_EQ(prices.at("TREND").at("ES"), (1.0 * 100.0 + 3.0 * 104.0) / 4.0);
    EXPECT_EQ(prices.at("TREND").at("NQ"), 0.1) << "one fill: its own price, to the bit";
    EXPECT_EQ(prices.at("FAST").at("ES"), 100.0) << "each sleeve's own fills";
    EXPECT_EQ(prices.count("ROLLS_ONLY"), 0u) << "a roll changes no position";
}

// Two sleeves: a row per sleeve whose own quantity changed, by sleeve then symbol.
TEST_F(BacktestCSVExporterTest, PositionChangesAreTheSleevesOwn) {
    SleevePositions current{{"TREND", {{"ES", stored_row("ES", 1.0, 4010.0, 10.0)}}},
                            {"FAST",
                             {{"ES", stored_row("ES", -2.0, 4010.0, -20.0)},
                              {"CL", stored_row("CL", 1.0, 70.0, 0.0)}}}};
    SleevePositions previous{{"TREND", {{"ES", stored_row("ES", 1.0, 4000.0)}}},
                             {"FAST", {{"ES", stored_row("ES", -1.0, 4000.0)}}}};
    const auto changes = BacktestCSVExporter::position_changes(
        current, previous,
        BacktestCSVExporter::fill_prices(
            {{"FAST",
              {fill_of("ES", Side::SELL, 1.0, 4000.0), fill_of("CL", Side::BUY, 1.0, 69.5)}}}),
        nullptr);
    // The account's quantity of ES changed (0 to -1), but only FAST's row did: TREND has no row.
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].strategy, "FAST");
    EXPECT_EQ(changes[0].symbol, "CL");
    EXPECT_FALSE(changes[0].previous_mark.has_value());
    EXPECT_EQ(changes[0].fill_price, 69.5);
    EXPECT_EQ(changes[0].realized_pnl, 0.0);
    EXPECT_EQ(changes[1].strategy, "FAST");
    EXPECT_EQ(changes[1].symbol, "ES");
    EXPECT_EQ(changes[1].quantity, -2.0);
    EXPECT_EQ(changes[1].previous_mark, 4000.0);
    EXPECT_EQ(changes[1].fill_price, 4000.0);
    EXPECT_EQ(changes[1].realized_pnl, -20.0);
}

TEST_F(BacktestCSVExporterTest, FinalizeIsIdempotentAndSafeAfterDestructor) {
    {
        BacktestCSVExporter exporter(out_dir_.string());
        ASSERT_TRUE(exporter.initialize_files().is_ok());
        exporter.finalize();
        // Second call should be a no-op without crashing.
        exporter.finalize();
    }  // destructor runs finalize() again
    EXPECT_TRUE(std::filesystem::exists(out_dir_ / "positions.csv"));
}

TEST_F(BacktestCSVExporterTest, AppendsAfterFinalizeReturnError) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    exporter.finalize();
    EXPECT_TRUE(
        exporter
            .append_daily_positions(date_at(2026, 1, 5), SleevePositions{}, {}, 1.0, 1.0, 1.0, {})
            .is_error());
    EXPECT_TRUE(exporter.append_equity_curve(date_at(2026, 1, 5), 1.0).is_error());
    EXPECT_TRUE(exporter.append_position_changes(date_at(2026, 1, 5), {}).is_error());
}

// ===== A short row through the backtest CSV writers ==============================================
// Two of the writers dropped or mis-handled a short row's sign.

namespace {

// A futures instrument with multiplier 5, registered under a test-only symbol: with it the
// exporter goes through FuturesInstrument::get_notional_value, which returns a MAGNITUDE.
void register_short_test_future(const std::string& symbol) {
    FuturesSpec spec;
    spec.root_symbol = symbol;
    spec.exchange = "CME";
    spec.currency = "USD";
    spec.multiplier = 5.0;
    spec.tick_size = 0.25;
    spec.commission_per_contract = 1.0;
    spec.initial_margin = 1000.0;
    spec.maintenance_margin = 900.0;
    spec.weight = 1.0;
    InstrumentRegistry::instance().register_instrument(
        symbol, std::make_shared<FuturesInstrument>(symbol, spec));
}

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

}  // namespace

// Sign-dropper 1 of 2 (backtest_csv_exporter.cpp, append_daily_positions): the row's notional was
// get_notional_value(qty, price) = |qty| x price x multiplier, so a short -2 at 100 printed +1000
// and its pct_portfolio +0.001. The quantity column was always signed.
TEST_F(BacktestCSVExporterTest, ShortSignDailyRowNotionalKeepsTheShortsSign) {
    register_short_test_future("SHORT_FUT_NOTIONAL_SIGN");
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> pos = {
        {"SHORT_FUT_NOTIONAL_SIGN", make_pos("SHORT_FUT_NOTIONAL_SIGN", -2.0)}};
    std::unordered_map<std::string, double> prices = {{"SHORT_FUT_NOTIONAL_SIGN", 100.0}};
    ASSERT_TRUE(exporter
                    .append_daily_positions(date_at(2026, 1, 5), one_sleeve(pos), prices,
                                            1'000'000.0, 1000.0, -1000.0, {})
                    .is_ok());
    exporter.finalize();
    const std::string body = slurp(out_dir_ / "positions.csv");
    // symbol, qty, price, notional (-2 x 100 x 5), pct_gross (|n| / gross), pct_portfolio (n / pv)
    EXPECT_NE(body.find("SHORT_FUT_NOTIONAL_SIGN,-2,100,-1000,1,-0.001,"), std::string::npos)
        << body;
}

// Sign-dropper 2 of 2 (backtest_coordinator.cpp, the daily CSV's gross and net notional): the same
// magnitude was summed into net_notional, so a short counted as long and the header's net equalled
// its gross. The coordinator's loop cannot be driven here without a database, so its source is
// checked: the notional it sums must carry the position's sign.
TEST_F(BacktestCSVExporterTest, ShortSignCoordinatorNetNotionalCarriesTheSign) {
    const auto path = find_repo_file("src/backtest/backtest_coordinator.cpp");
    ASSERT_FALSE(path.empty());
    const std::string src = slurp(path);
    const auto at = src.find("net_notional += notional;");
    ASSERT_NE(at, std::string::npos);
    const std::string loop = src.substr(src.rfind("double gross_notional = 0.0;", at), 1200);
    EXPECT_NE(loop.find("std::copysign(instrument->get_notional_value(qty, price), qty)"),
              std::string::npos)
        << "the net notional sums get_notional_value, a magnitude, without the position's sign";
}

// A flip through zero. The file's own arithmetic realized the closed leg (prev x (close - previous
// mark): +2 -> -3 at a close of 4010 printed 20). The row now prints what the run stores for the
// sleeve's row of that day, whatever the two quantities are, and the fill price of the one trade.
TEST_F(BacktestCSVExporterTest, PositionChangesFlipPrintsTheStoredRowsRealized) {
    const std::string sym = "FLIP_PRINTS_STORED_ROW";
    // long +2 marked 4000 flips to -3: 5 sold at 4000, the row stored for the day realized 12.5
    const std::string to_short = changes_body({{sym, stored_row(sym, -3.0, 4010.0, 12.5)}},
                                              {{sym, stored_row(sym, 2.0, 4000.0)}},
                                              {fill_of(sym, Side::SELL, 5.0, 4000.0)});
    EXPECT_NE(to_short.find(sym + ",-3,4000,4000,12.5\n"), std::string::npos) << to_short;
    // short -3 marked 4000 flips to +2: 5 bought at 4000, the stored row realized -7.5
    const std::string to_long = changes_body({{sym, stored_row(sym, 2.0, 3990.0, -7.5)}},
                                             {{sym, stored_row(sym, -3.0, 4000.0)}},
                                             {fill_of(sym, Side::BUY, 5.0, 4000.0)});
    EXPECT_NE(to_long.find(sym + ",2,4000,4000,-7.5\n"), std::string::npos) << to_long;
}

// ===== positions.csv: one row per sleeve, each with its own readings ============================
// The file printed 0 under `volatility` on every row, and the forecast and averages of the first
// trend sleeve on every row of the account. Each row is now a sleeve's, and carries that sleeve's
// forecast, the volatility it sized on (sizing_vol) and its averages, read through the function
// the live positions file reads a sleeve through. The sleeves below are real trend sleeves fed a
// price walk; no database.

class BacktestCsvSleeveRows : public BacktestCSVExporterTest {
protected:
    void SetUp() override {
        BacktestCSVExporterTest::SetUp();
        trend_ = std::make_unique<CsvTestSleeve>("TREND_FOLLOWING", kSym, TrendFollowingConfig{});
        fast_ = std::make_unique<CsvTestSleeve>("TREND_FOLLOWING_FAST", kSym,
                                                fast_trend_following_config());
    }
    void TearDown() override {
        trend_.reset();
        fast_.reset();
        BacktestCSVExporterTest::TearDown();
    }

    // The day's rows of a book whose two sleeves both hold kSym (multiplier 5): +1 and -2 at 100.
    std::vector<std::vector<std::string>> rows() {
        BacktestCSVExporter exporter(out_dir_.string());
        EXPECT_TRUE(exporter.initialize_files().is_ok());
        SleevePositions book{{"TREND_FOLLOWING_FAST", {{kSym, make_pos(kSym, -2.0)}}},
                             {"TREND_FOLLOWING", {{kSym, make_pos(kSym, 1.0)}}}};
        std::vector<std::shared_ptr<StrategyInterface>> strategies{trend_->strategy,
                                                                   fast_->strategy};
        EXPECT_TRUE(exporter
                        .append_daily_positions(date_at(2026, 1, 5), book, {{kSym, 100.0}},
                                                1'000'000.0, 1500.0, -500.0, strategies)
                        .is_ok());
        exporter.finalize();
        std::vector<std::vector<std::string>> out;
        for (const auto& line : lines_of(slurp(out_dir_ / "positions.csv"))) {
            if (line.rfind("2026-01-05,", 0) == 0)
                out.push_back(cells_of(line));
        }
        return out;
    }

    const std::string kSym = "CSVBT";
    std::unique_ptr<CsvTestSleeve> trend_;
    std::unique_ptr<CsvTestSleeve> fast_;
};

TEST_F(BacktestCsvSleeveRows, EachSleeveHasItsOwnRowBySleeveThenSymbol) {
    const auto r = rows();
    ASSERT_EQ(r.size(), 2u) << "one account row where two sleeves hold the symbol";
    ASSERT_EQ(r[0].size(), 14u);
    EXPECT_EQ(r[0][1], "TREND_FOLLOWING");
    EXPECT_EQ(r[1][1], "TREND_FOLLOWING_FAST");
    EXPECT_EQ(r[0][2], kSym);
    // The sleeve's own quantity and notional (quantity x price x 5); the shares are of the
    // account's gross notional (1500) and portfolio value.
    EXPECT_EQ(r[0][3], "1");
    EXPECT_EQ(r[0][5], "500");
    EXPECT_NEAR(std::stod(r[0][6]), 500.0 / 1500.0, 1e-6);
    EXPECT_EQ(r[1][3], "-2");
    EXPECT_EQ(r[1][5], "-1000");
    EXPECT_DOUBLE_EQ(std::stod(r[1][7]), -0.001);
}

TEST_F(BacktestCsvSleeveRows, ARowCarriesItsSleevesSizingVolatilityForecastAndAverages) {
    const auto r = rows();
    ASSERT_EQ(r.size(), 2u);
    const CsvTestSleeve* sleeves[] = {trend_.get(), fast_.get()};
    for (size_t i = 0; i < 2; ++i) {
        ASSERT_EQ(r[i].size(), 14u);
        const InstrumentData* data = sleeves[i]->strategy->get_instrument_data(kSym);
        ASSERT_NE(data, nullptr);
        ASSERT_NE(data->current_volatility, 0.01) << "the test sleeve computed no volatility";
        const auto emas = sleeves[i]->strategy->get_ema_values(kSym, {8, 32, 64, 256});
        // Printed figures against the sleeve's own, to the printed digits.
        const auto near = [&](const std::string& cell, double want, const char* what) {
            EXPECT_NEAR(std::stod(cell), want, std::abs(want) * 1e-5 + 1e-12)
                << r[i][1] << " " << what << " printed " << cell;
        };
        EXPECT_NE(std::stod(r[i][9]), 0.0) << r[i][1] << ": the sizing volatility column is 0";
        near(r[i][8], data->current_forecast, "forecast");
        near(r[i][9], data->current_volatility, "sizing_vol");
        near(r[i][10], emas.at(8), "ema_8");
        near(r[i][11], emas.at(32), "ema_32");
        near(r[i][12], emas.at(64), "ema_64");
        near(r[i][13], emas.at(256), "ema_256");
    }
    EXPECT_NE(r[0][8], r[1][8]) << "both rows carry one sleeve's forecast";
    EXPECT_NE(r[0][9], r[1][9]) << "both rows carry one sleeve's sizing volatility";
}

TEST_F(BacktestCsvSleeveRows, ASleeveWithoutATrendStrategyPrintsNoReadings) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    // The book names a sleeve the strategy list does not hold: its row has no readings, and it
    // does not borrow the trend sleeve's.
    SleevePositions book{{"OTHER_SLEEVE", {{kSym, make_pos(kSym, 3.0)}}}};
    std::vector<std::shared_ptr<StrategyInterface>> strategies{trend_->strategy};
    ASSERT_TRUE(exporter
                    .append_daily_positions(date_at(2026, 1, 5), book, {{kSym, 100.0}}, 1'000'000.0,
                                            1500.0, 1500.0, strategies)
                    .is_ok());
    exporter.finalize();
    EXPECT_NE(slurp(out_dir_ / "positions.csv")
                  .find("2026-01-05,OTHER_SLEEVE,CSVBT,3,100,1500,1,0.0015,0,0,0,0,0,0\n"),
              std::string::npos)
        << slurp(out_dir_ / "positions.csv");
}
