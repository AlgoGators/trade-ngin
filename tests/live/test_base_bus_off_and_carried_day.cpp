// tests/live/test_base_bus_off_and_carried_day.cpp
//
// T-7a commit 3b (T-BASE B1, HD 2026-09-18 §28c item 3 and the Sunday positions CSV).
//
// 1. The BASE runner (apps/strategies/live_portfolio.cpp) loaded its 730-day bar window with the
//    MarketDataBus ON. PostgresDatabase::get_market_data publishes every row it reads as a BAR
//    event and the PortfolioManager subscribes to BAR with a full process_market_data per event,
//    so each BASE run day was ~21,000 one-bar optimise-and-risk rebalances (execution generation
//    on, Sundays included) before the runner's own call (T-BASE §3.1). The CONSERVATIVE twin has
//    always disabled the bus around the load and around its direct call; BASE now carries the
//    same two guards, byte for byte. The PM then gets exactly one rebalance a day: the runner's.
// 2. With the replay gone, a day with no session (the previous day a Saturday or a holiday) feeds
//    the strategies nothing, so their forecasts, volatilities and EMAs are empty that day. HD
//    (2026-09-18): the per-strategy positions file keeps the real details, held quantities, last
//    marks and the last computed forecasts from the previous session, with a note that no session
//    occurred and the values are carried, not computed. Never empty rows. Both twins.
//
// The runners are `main()`s, so their placement is tested in their source (as
// tests/live/test_live_run_refusal_arms.cpp does); the bus, the file writer and the signals read
// are run. The database test goes only through TRADE_NGIN_TEST_DSN and removes its probe rows.

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <cstdlib>
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
#include "../risk/risk_module_test_helpers.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/carried_day.hpp"
#include "trade_ngin/live/csv_exporter.hpp"
#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

constexpr auto npos = std::string::npos;

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_source(const std::string& relative) {
    auto path = find_repo_file(relative);
    if (path.empty()) return {};
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cell;
    std::istringstream in(line);
    while (std::getline(in, cell, ',')) out.push_back(cell);
    if (!line.empty() && line.back() == ',') out.push_back("");
    return out;
}

const char* const kConservative = "apps/strategies/live_portfolio_conservative.cpp";
const char* const kBase = "apps/strategies/live_portfolio.cpp";
const char* const kFuturesRunners[] = {kConservative, kBase};

// The text between two markers, both included in the search, [from, to).
std::string between(const std::string& src, const std::string& from, const std::string& to) {
    const auto a = src.find(from);
    if (a == npos) return {};
    const auto b = src.find(to, a);
    if (b == npos) return {};
    return src.substr(a, b - a);
}

}  // namespace

// =============================================================================================
// 1. The bus is off around the BASE load and the BASE direct call, exactly as on CONSERVATIVE.
// =============================================================================================

TEST(BaseBusOffSource, TheBaseLoadIsBracketedByTheSameGuardAsConservative) {
    const std::string base = read_source(kBase);
    const std::string cons = read_source(kConservative);
    if (base.empty() || cons.empty()) GTEST_SKIP() << "runner source not found";

    const std::string from = "        // Load market data for daily processing\n";
    const std::string to = "        if (market_data_result.is_error()) {";
    const std::string base_load = between(base, from, to);
    const std::string cons_load = between(cons, from, to);
    ASSERT_FALSE(cons_load.empty());
    ASSERT_FALSE(base_load.empty());
    EXPECT_NE(cons_load.find("MarketDataBus::instance().set_publish_enabled(false);"), npos)
        << "the CONSERVATIVE reference guard is gone";
    EXPECT_EQ(base_load, cons_load)
        << "the BASE bar load is not bracketed by CONSERVATIVE's guard: every loaded row is "
           "published as a BAR event and the PortfolioManager runs a full rebalance per row "
           "(T-BASE: 21,972 rebalances on 2026-04-24)";

    const auto off = base_load.find("MarketDataBus::instance().set_publish_enabled(false);");
    const auto load = base_load.find("db->get_market_data(");
    const auto on = base_load.find("MarketDataBus::instance().set_publish_enabled(true);");
    ASSERT_NE(load, npos);
    EXPECT_TRUE(off != npos && off < load) << "publishing must be disabled BEFORE the load";
    EXPECT_TRUE(on != npos && load < on) << "and re-enabled only after it";
    EXPECT_NE(base.find("#include \"trade_ngin/data/market_data_bus.hpp\""), npos);
}

TEST(BaseBusOffSource, TheBaseDirectCallIsBracketedByTheSameGuardAsConservative) {
    const std::string base = read_source(kBase);
    const std::string cons = read_source(kConservative);
    if (base.empty() || cons.empty()) GTEST_SKIP() << "runner source not found";
    const std::string from =
        "            INFO(\"Processing data through portfolio manager (optimization + risk)...\");";
    const std::string to = "            // T-RISK-ARCH Q2 (ruled yes)";
    const std::string base_call = between(base, from, to);
    const std::string cons_call = between(cons, from, to);
    ASSERT_FALSE(cons_call.empty());
    ASSERT_FALSE(base_call.empty());
    EXPECT_EQ(base_call, cons_call)
        << "the BASE direct process_market_data call is not bracketed as CONSERVATIVE's is";
    // One direct call a day, and it is the only feed the PM gets.
    size_t calls = 0;
    for (auto at = base.find("portfolio->process_market_data("); at != npos;
         at = base.find("portfolio->process_market_data(", at + 1))
        ++calls;
    EXPECT_EQ(calls, 1u);
    size_t loads = 0;
    for (auto at = base.find("db->get_market_data("); at != npos;
         at = base.find("db->get_market_data(", at + 1))
        ++loads;
    // Two loads since T-ROLLX-FIX commit 4 (N-2): the window, and the session classifier's history
    // before it. Each is bracketed by the bus-off guard.
    EXPECT_EQ(loads, 2u) << "every bar load in the runner must sit inside the guard";
    for (auto at = base.find("db->get_market_data("); at != npos;
         at = base.find("db->get_market_data(", at + 1)) {
        const auto off = base.rfind("MarketDataBus::instance().set_publish_enabled(false);", at);
        const auto on_before = base.rfind("MarketDataBus::instance().set_publish_enabled(true);", at);
        const auto on_after = base.find("MarketDataBus::instance().set_publish_enabled(true);", at);
        ASSERT_NE(off, npos);
        ASSERT_NE(on_after, npos);
        EXPECT_TRUE(on_before == npos || on_before < off) << "a bar load outside the bus-off guard";
    }
}

// The mechanism the guard relies on, run: a PortfolioManager subscribed to BAR rebalances once per
// published event while the bus publishes, never while it does not, and once for a direct call.
namespace {

class CountingStrategy : public BaseStrategy {
public:
    CountingStrategy(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)) {
        metadata_.name = "Counting Strategy";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        ++calls_;
        bars_ += data.size();
        return Result<void>();
    }
    std::unordered_map<std::string, Position> get_target_positions() const override { return {}; }
    size_t calls_{0};
    size_t bars_{0};
};

class BusFixture : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        MarketDataBus::instance().set_publish_enabled(true);
        mock_db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(mock_db_->connect().is_ok());
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::ERR;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
    }
    void TearDown() override {
        MarketDataBus::instance().set_publish_enabled(true);
        // The PM's constructor subscribed its `this`; nothing unsubscribes it on destruction.
        (void)MarketDataBus::instance().unsubscribe("PORTFOLIO_MANAGER");
        pm_.reset();
        mock_db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }
    std::shared_ptr<MockPostgresDatabase> mock_db_;
    std::shared_ptr<PortfolioManager> pm_;
};

MarketDataEvent bar_event(const std::string& symbol, int d, double close) {
    MarketDataEvent e;
    e.type = MarketDataEventType::BAR;
    e.symbol = symbol;
    e.timestamp = Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d));
    e.numeric_fields["open"] = close;
    e.numeric_fields["high"] = close;
    e.numeric_fields["low"] = close;
    e.numeric_fields["close"] = close;
    e.numeric_fields["volume"] = 1000.0;
    return e;
}

}  // namespace

TEST_F(BusFixture, WithTheGuardThePmGetsExactlyOneRebalanceTheRunnersDirectCall) {
    PortfolioConfig pc{1000.0, 1.0, 0.0, /*optimization=*/false};
    pc.risk_modules = {test_none_module()};
    pm_ = std::make_shared<PortfolioManager>(pc, "PM_C3B_BUS");
    StrategyConfig sc;
    sc.capital_allocation = 1000.0;
    sc.max_leverage = 10.0;
    sc.asset_classes = {AssetClass::FUTURES};
    sc.frequencies = {DataFrequency::DAILY};
    auto s = std::make_shared<CountingStrategy>("C3B_COUNT", sc, mock_db_);
    ASSERT_TRUE(s->initialize().is_ok());
    ASSERT_TRUE(s->start().is_ok());
    ASSERT_TRUE(pm_->add_strategy(s, 1.0, false).is_ok());

    std::vector<Bar> all_bars;
    std::vector<MarketDataEvent> events;
    for (int d = 1; d <= 5; ++d) {
        for (const char* sym : {"ZZA", "ZZB"}) {
            events.push_back(bar_event(sym, d, 100.0 + d));
            Bar b;
            b.symbol = sym;
            b.timestamp = events.back().timestamp;
            b.open = b.high = b.low = b.close = Decimal(100.0 + d);
            b.volume = 1000.0;
            all_bars.push_back(b);
        }
    }

    // The runner's sequence with the guard: publishing off, the load (each row is published by
    // get_market_data), publishing on, ..., off, the direct call, on.
    MarketDataBus::instance().set_publish_enabled(false);
    for (const auto& e : events) MarketDataBus::instance().publish(e);
    MarketDataBus::instance().set_publish_enabled(true);
    EXPECT_EQ(s->calls_, 0u) << "a row published during the guarded load reached the PM";
    MarketDataBus::instance().set_publish_enabled(false);
    ASSERT_TRUE(pm_->process_market_data(all_bars).is_ok());
    MarketDataBus::instance().set_publish_enabled(true);
    EXPECT_EQ(s->calls_, 1u) << "the PM must rebalance exactly once: the runner's direct call";
    EXPECT_EQ(s->bars_, all_bars.size());

    // The unguarded sequence (the BASE runner before this commit): one rebalance per row, then one.
    s->calls_ = 0;
    for (const auto& e : events) MarketDataBus::instance().publish(e);
    ASSERT_TRUE(pm_->process_market_data(all_bars).is_ok());
    EXPECT_EQ(s->calls_, events.size() + 1) << "control: the bus replays each published row";
}

// =============================================================================================
// 2. The positions file on a day with no session carries the real details, with the note.
// =============================================================================================

namespace {

FuturesSpec spec(double multiplier) {
    FuturesSpec s;
    s.root_symbol = "ZZ";
    s.exchange = "CME";
    s.currency = "USD";
    s.multiplier = multiplier;
    s.tick_size = 0.25;
    s.commission_per_contract = 2.0;
    s.initial_margin = 1000.0;
    s.maintenance_margin = 900.0;
    s.weight = 1.0;
    s.trading_hours = "17:00-16:00";
    return s;
}

Position held(const std::string& symbol, double qty, double price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(price);
    p.last_update = std::chrono::system_clock::now();
    return p;
}

std::chrono::system_clock::time_point utc(int y, int m, int d, int h) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

class CarriedFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::INFO;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
        auto& registry = InstrumentRegistry::instance();
        // The exporter strips ".v.N" before the registry lookup.
        registry.register_instrument("ZC3M", std::make_shared<FuturesInstrument>("ZC3M", spec(500000.0)));
        registry.register_instrument("ZC3C", std::make_shared<FuturesInstrument>("ZC3C", spec(100000.0)));
        registry.register_instrument("ZC3B", std::make_shared<FuturesInstrument>("ZC3B", spec(0.1)));
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path() / ("c3b_carried_" + std::string(info->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }
    std::filesystem::path dir_;
};

}  // namespace

TEST_F(CarriedFileTest, ASundayFileCarriesHeldQuantitiesLastMarksAndLastForecastsWithTheNote) {
    // BASE on Sunday 2026-04-26: the held book is Saturday's rows, the marks are Friday's closes,
    // the forecasts are Saturday's stored signals (computed from Friday's bars).
    StrategyPositionsMap held_book{
        {"TREND_FOLLOWING", {{"ZC3M.v.0", held("ZC3M.v.0", 1.0, 0.05749)}}},
        {"TREND_FOLLOWING_FAST",
         {{"ZC3M.v.0", held("ZC3M.v.0", 1.0, 0.05749)}, {"ZC3C.v.0", held("ZC3C.v.0", 1.0, 0.73325)}}}};
    std::unordered_map<std::string, CarriedMark> marks{
        {"ZC3M.v.0", {0.05749, "2026-04-24"}},
        {"ZC3C.v.0", {0.73325, "2026-04-24"}},
        {"ZC3B.v.0", {77960.0, "2026-04-24"}}};
    std::unordered_map<std::string, CarriedForecasts> forecasts{
        {"TREND_FOLLOWING",
         {"2026-04-25", {{"ZC3M.v.0", 12.834572}, {"ZC3C.v.0", 9.030285}, {"ZC3B.v.0", 1.470563}}}},
        {"TREND_FOLLOWING_FAST",
         {"2026-04-25", {{"ZC3M.v.0", 7.5}, {"ZC3C.v.0", 4.25}, {"ZC3B.v.0", -2.0}}}}};

    CSVExporter exporter(dir_.string());
    ::testing::internal::CaptureStdout();
    auto r = exporter.export_carried_positions(utc(2026, 4, 26, 5), held_book, marks, forecasts,
                                               502843.64, 393308.0, 393308.0,
                                               "no session on 2026-04-25 (a Saturday)");
    const std::string log = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(std::filesystem::path(r.value()).filename(), "2026-04-26_positions.csv")
        << "the carried file replaces the day's positions file, same name";
    const auto lines = split_lines(slurp(r.value()));
    ASSERT_GE(lines.size(), 3u);

    // The note: no session, carried, not computed.
    EXPECT_EQ(lines[0].rfind("# Portfolio Value: 502843.64", 0), 0u);
    EXPECT_EQ(lines[1].rfind("# NO SESSION: no session on 2026-04-25 (a Saturday).", 0), 0u)
        << "the file does not say that no session occurred: " << lines[1];
    EXPECT_NE(lines[1].find("CARRIED, not computed"), npos);

    // The rows: one per (sleeve, symbol the sleeve holds or last forecast), each with its real
    // quantity, its last mark and its own sleeve's last forecast.
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> rows;
    for (size_t i = 3; i < lines.size(); ++i) {
        auto cells = split_csv(lines[i]);
        ASSERT_EQ(cells.size(), 15u) << lines[i];
        rows[{cells[0], cells[1]}] = cells;
    }
    EXPECT_EQ(rows.size(), 6u) << "two sleeves x three symbols";
    const auto& fast_c = rows.at({"Trend Following Fast", "ZC3C.v.0"});
    EXPECT_DOUBLE_EQ(std::stod(fast_c[2]), 1.0) << "held quantity";
    EXPECT_DOUBLE_EQ(std::stod(fast_c[3]), 0.73325) << "last mark, not 0";
    EXPECT_DOUBLE_EQ(std::stod(fast_c[4]), 73325.0) << "notional at the last mark";
    EXPECT_DOUBLE_EQ(std::stod(fast_c[7]), 4.25) << "FAST's own last forecast, not 0 and not TF's";
    EXPECT_EQ(fast_c[13], "2026-04-24");
    EXPECT_EQ(fast_c[14], "2026-04-25");
    for (int col = 8; col <= 12; ++col) EXPECT_EQ(fast_c[col], "") << "volatility/EMAs not stored";
    const auto& tf_b = rows.at({"Trend Following", "ZC3B.v.0"});
    EXPECT_DOUBLE_EQ(std::stod(tf_b[2]), 0.0) << "not held: quantity 0";
    EXPECT_DOUBLE_EQ(std::stod(tf_b[3]), 77960.0) << "an unheld symbol still carries its last mark";
    EXPECT_DOUBLE_EQ(std::stod(tf_b[7]), 1.470563);
    const auto& tf_m = rows.at({"Trend Following", "ZC3M.v.0"});
    EXPECT_DOUBLE_EQ(std::stod(tf_m[7]), 12.834572);

    // Never an empty row: every row has a mark and a forecast.
    for (const auto& [key, cells] : rows) {
        EXPECT_NE(std::stod(cells[3]), 0.0) << key.first << " " << key.second << ": mark is 0";
        EXPECT_FALSE(cells[7].empty()) << key.first << " " << key.second << ": no forecast";
    }
    EXPECT_NE(log.find("CSVExporter: no session on 2026-04-26"), npos);
    EXPECT_NE(log.find("CSVExporter: Carried positions saved to "), npos);
}

TEST_F(CarriedFileTest, AHeldSymbolWithoutAStoredForecastKeepsItsRowAndSaysNothingWasCarried) {
    StrategyPositionsMap held_book{{"TREND_FOLLOWING", {{"ZC3C.v.0", held("ZC3C.v.0", -2.0, 0.7300)}}}};
    std::unordered_map<std::string, CarriedMark> marks;  // no loaded bar: the stored row's price
    std::unordered_map<std::string, CarriedForecasts> forecasts;  // no stored signals at all
    CSVExporter exporter(dir_.string());
    ::testing::internal::CaptureStdout();
    auto r = exporter.export_carried_positions(utc(2026, 5, 3, 5), held_book, marks, forecasts,
                                               500000.0, 146000.0, -146000.0,
                                               "no session on 2026-05-02 (a Saturday)");
    ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(r.is_ok());
    const auto lines = split_lines(slurp(r.value()));
    ASSERT_EQ(lines.size(), 4u);
    auto cells = split_csv(lines[3]);
    ASSERT_EQ(cells.size(), 15u);
    EXPECT_DOUBLE_EQ(std::stod(cells[2]), -2.0);
    EXPECT_DOUBLE_EQ(std::stod(cells[3]), 0.73);
    EXPECT_EQ(cells[13], "stored row");
    EXPECT_EQ(cells[7], "") << "no stored forecast: left empty, never a computed-looking 0";
    EXPECT_EQ(cells[14], "");
}

// Both twins write the carried file on a day with no session, through the same block.
TEST(CarriedDaySource, BothTwinsWriteTheCarriedFileOnADayWithNoSession) {
    std::vector<std::string> blocks;
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const std::string block =
            between(src, "        // Export current positions with per-strategy breakdown\n",
                    "        if (qt_sends && qt_desk_editable && current_export_result.is_ok() &&");
        ASSERT_FALSE(block.empty());
        EXPECT_NE(block.find("if (!skip_strategy_processing) {"), npos)
            << "the file does not tell a day with no session apart";
        EXPECT_NE(block.find("csv_exporter->export_carried_positions("), npos)
            << "a day with no session writes the unfed strategies' empty forecasts";
        EXPECT_NE(block.find("data_loader->load_last_signals_before("), npos)
            << "the carried forecasts must come from the sleeve's stored signals";
        EXPECT_NE(block.find("latest_bars_per_symbol"), npos) << "the last marks";
        blocks.push_back(block);
    }
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0], blocks[1]) << "the twins' positions-file blocks must be byte-identical";
}

// =============================================================================================
// The last computed forecasts: the sleeve's signals of the latest stored run date before today.
// =============================================================================================

namespace {

std::string test_dsn() {
    const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
    return (dsn && *dsn) ? std::string(dsn) : std::string();
}
bool require_db() {
    const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
    return v && std::string(v) == "1";
}
constexpr const char* kProbePortfolio = "C3B_CARRY_PROBE_PORTFOLIO";
constexpr const char* kProbeStrategy = "C3B_CARRY_PROBE_ID";

}  // namespace

TEST(CarriedForecastsDb, TheLatestStoredRunBeforeTodayOfThatSleeveIsRead) {
    const std::string dsn = test_dsn();
    if (dsn.empty()) {
        if (require_db()) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set";
        GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
    }
    auto purge = [&]() {
        try {
            pqxx::connection c(dsn);
            pqxx::work txn(c);
            txn.exec("DELETE FROM trading.signals WHERE portfolio_id = " + txn.quote(kProbePortfolio));
            txn.commit();
        } catch (const std::exception&) {
        }
    };
    purge();
    {
        pqxx::connection c(dsn);
        pqxx::work txn(c);
        auto ins = [&](const char* name, const char* sym, double v, const char* ts) {
            txn.exec("INSERT INTO trading.signals (strategy_id, symbol, signal_value, \"timestamp\", "
                     "portfolio_id, strategy_name) VALUES (" +
                     txn.quote(kProbeStrategy) + ", " + txn.quote(sym) + ", " + txn.quote(v) +
                     ", " + txn.quote(ts) + "::timestamptz, " + txn.quote(kProbePortfolio) + ", " +
                     txn.quote(name) + ")");
        };
        ins("SLEEVE_A", "ZC3M.v.0", 11.0, "2026-04-24 05:00:00+00");  // older session
        ins("SLEEVE_A", "ZC3M.v.0", 12.834572, "2026-04-25 05:00:00+00");  // the last one
        ins("SLEEVE_A", "ZC3C.v.0", -3.5, "2026-04-25 05:00:00+00");
        ins("SLEEVE_A", "ZC3M.v.0", 99.0, "2026-04-26 05:00:00+00");  // today: not "before"
        ins("SLEEVE_B", "ZC3M.v.0", 7.5, "2026-04-23 05:00:00+00");   // the other sleeve's own
        txn.commit();
    }
    auto db = std::make_shared<PostgresDatabase>(dsn);
    ASSERT_TRUE(db->connect().is_ok());
    LiveDataLoader loader(db, "trading");
    const auto today = utc(2026, 4, 26, 5);

    auto a = loader.load_last_signals_before(kProbeStrategy, "SLEEVE_A", kProbePortfolio, today);
    ASSERT_TRUE(a.is_ok()) << a.error()->what();
    EXPECT_EQ(a.value().session_date, "2026-04-25");
    ASSERT_EQ(a.value().forecasts.size(), 2u) << "only the latest run date's rows";
    EXPECT_DOUBLE_EQ(a.value().forecasts.at("ZC3M.v.0"), 12.834572);
    EXPECT_DOUBLE_EQ(a.value().forecasts.at("ZC3C.v.0"), -3.5);

    auto b = loader.load_last_signals_before(kProbeStrategy, "SLEEVE_B", kProbePortfolio, today);
    ASSERT_TRUE(b.is_ok());
    EXPECT_EQ(b.value().session_date, "2026-04-23") << "each sleeve reads its own rows";
    EXPECT_DOUBLE_EQ(b.value().forecasts.at("ZC3M.v.0"), 7.5);

    auto none = loader.load_last_signals_before(kProbeStrategy, "SLEEVE_C", kProbePortfolio, today);
    ASSERT_TRUE(none.is_ok());
    EXPECT_TRUE(none.value().session_date.empty());
    EXPECT_TRUE(none.value().forecasts.empty());
    purge();
}
