// The fix-up of the one pass (LOOP_SPEC v6.2 sections 1, 3.1, 4, 7.3 and 7.7), the parts that are
// not the pass's arithmetic: a futures sleeve's required keys, the starting drawdown of a seeded
// chain, the estimator-window and estimator-history log lines, the backtest's risk_detail column,
// and which sleeve the manager values a contract on. The manager's own behaviour is pinned in
// test_one_pass_book.cpp; the runners' main() cannot be driven from a unit test, so what they wire
// is pinned on their source text here.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/logging.h>
#include <nlohmann/json.hpp>

#define private public
#define protected public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#undef protected
#undef private
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "one_pass_test_fixture.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/live/live_estimator_history.hpp"
#include "trade_ngin/live/stored_book_ownership.hpp"
#include "trade_ngin/strategy/short_window_log.hpp"
#include "trade_ngin/strategy/sleeve_config.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

std::filesystem::path repo_root() {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / "apps/strategies/live_portfolio_conservative.cpp")) return dir;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_all(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

const char* const kFuturesRunners[] = {
    "apps/backtest/bt_portfolio_conservative.cpp", "apps/backtest/bt_portfolio.cpp",
    "apps/strategies/live_portfolio_conservative.cpp", "apps/strategies/live_portfolio.cpp"};

nlohmann::json sleeve(const nlohmann::json& config) {
    return {{"type", "TrendFollowingStrategy"}, {"config", config}};
}

void console_logger() {
    LoggerConfig lc;
    lc.destination = LogDestination::CONSOLE;
    lc.min_level = LogLevel::INFO;
    Logger::instance().initialize(lc);
}

}  // namespace

// ---- section 7.7: a futures sleeve's risk_target, idm and vol_lookback_short are required -------

TEST(RequiredSleeveKeys, AllThreeAreReadFromTheSleevesConfig) {
    TrendFollowingConfig config;
    const auto r = read_required_sleeve_keys(
        "TREND_FOLLOWING", sleeve({{"risk_target", 0.2}, {"idm", 2.5}, {"vol_lookback_short", 32}}),
        config);
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_DOUBLE_EQ(config.risk_target, 0.2);
    EXPECT_DOUBLE_EQ(config.idm, 2.5);
    EXPECT_EQ(config.vol_lookback_short, 32);
}

TEST(RequiredSleeveKeys, AMissingKeyRefusesAndNamesTheKey) {
    const nlohmann::json full = {{"risk_target", 0.2}, {"idm", 2.5}, {"vol_lookback_short", 32}};
    for (const char* key : {"risk_target", "idm", "vol_lookback_short"}) {
        nlohmann::json config = full;
        config.erase(key);
        TrendFollowingConfig out;
        out.risk_target = -1.0;
        const auto r = read_required_sleeve_keys("TREND_FOLLOWING", sleeve(config), out);
        ASSERT_TRUE(r.is_error()) << key << " missing was accepted (a default was used)";
        const std::string what = r.error()->what();
        EXPECT_NE(what.find(std::string("strategies.TREND_FOLLOWING.config.") + key + " is missing"),
                  std::string::npos)
            << what;
    }
    // no "config" object at all: nothing is defaulted either
    TrendFollowingConfig out;
    const auto r = read_required_sleeve_keys("FAST", {{"type", "TrendFollowingFastStrategy"}}, out);
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("strategies.FAST.config.risk_target is missing"),
              std::string::npos)
        << r.error()->what();
}

TEST(RequiredSleeveKeys, AValueThatIsNotAPositiveNumberRefuses) {
    const nlohmann::json full = {{"risk_target", 0.2}, {"idm", 2.5}, {"vol_lookback_short", 32}};
    const std::vector<std::pair<std::string, nlohmann::json>> bad = {
        {"risk_target", "0.2"}, {"risk_target", 0.0},  {"risk_target", -0.2},
        {"idm", nullptr},       {"idm", 0},            {"vol_lookback_short", 32.5},
        {"vol_lookback_short", 0}, {"vol_lookback_short", "32"}};
    for (const auto& [key, value] : bad) {
        nlohmann::json config = full;
        config[key] = value;
        TrendFollowingConfig out;
        const auto r = read_required_sleeve_keys("TREND_FOLLOWING", sleeve(config), out);
        ASSERT_TRUE(r.is_error()) << key << " = " << value.dump();
        EXPECT_NE(std::string(r.error()->what()).find(".config." + key + " must be"), std::string::npos)
            << r.error()->what();
    }
}

// The four futures runners read both sleeve types through the helper and carry no fallback value
// of their own (the backtest's 0.15 and the live 0.2 are gone).
TEST(RequiredSleeveKeys, TheFourFuturesRunnersCarryNoDefault) {
    const auto root = repo_root();
    ASSERT_FALSE(root.empty());
    for (const char* runner : kFuturesRunners) {
        const std::string src = read_all(root / runner);
        ASSERT_FALSE(src.empty()) << runner;
        EXPECT_EQ(count_of(src, "read_required_sleeve_keys("), 2u) << runner << ": TREND and FAST";
        for (const char* key : {"risk_target", "idm", "vol_lookback_short"}) {
            EXPECT_EQ(src.find(std::string("cfg.value(\"") + key + "\""), std::string::npos)
                << runner << " still reads " << key << " with an in-code default";
        }
    }
}

// The shipped templates carry the three keys on every futures sleeve.
TEST(RequiredSleeveKeys, TheFuturesTemplatesCarryThem) {
    const auto root = repo_root();
    ASSERT_FALSE(root.empty());
    int sleeves = 0;
    for (const char* book : {"conservative", "base"}) {
        std::ifstream in(root / "config_template" / "portfolios" / book / "portfolio.json");
        ASSERT_TRUE(in.good()) << book;
        const nlohmann::json portfolio = nlohmann::json::parse(in);
        for (const auto& [id, def] : portfolio.at("strategies").items()) {
            TrendFollowingConfig out;
            const auto r = read_required_sleeve_keys(id, def, out);
            EXPECT_TRUE(r.is_ok()) << book << " " << id << ": " << (r.is_error() ? r.error()->what() : "");
            ++sleeves;
        }
    }
    EXPECT_EQ(sleeves, 3) << "CONSERVATIVE's one sleeve and BASE's two";
}

// ---- sections 7.2 and 10: the live risk_scale is measured on the stored book -------------------

TEST(LiveRiskScaleWiring, TheLiveFuturesRunnersStoreTheDeliveredScaleOfTheBookTheyStore) {
    const auto root = repo_root();
    ASSERT_FALSE(root.empty());
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        const std::string src = read_all(root / runner);
        EXPECT_EQ(src.find("one_pass_day.stores_detail() ? one_pass_day.risk_scale : 1.0"), std::string::npos)
            << runner << " still stores the pass's own figure, measured before its STRICT step";
        const size_t at = src.find("double risk_scale =");
        ASSERT_NE(at, std::string::npos) << runner;
        const std::string statement = src.substr(at, src.find(';', at) - at);
        EXPECT_NE(statement.find("one_pass_day.stores_detail()"), std::string::npos) << statement;
        EXPECT_NE(statement.find("portfolio->delivered_scale_for_book("), std::string::npos) << statement;
        EXPECT_NE(statement.find("account_book_of(strategy_positions_map)"), std::string::npos) << statement;
        EXPECT_NE(statement.find(": 1.0"), std::string::npos) << statement;
        // the STRICT step is above it: the book it reads is the one the runner stores
        const size_t strict = src.find("STRICT_TRIPWIRE");
        ASSERT_NE(strict, std::string::npos);
        EXPECT_LT(strict, at) << runner;
    }
}

// ---- section 1: a short estimator window is counted on one line per run ------------------------

class ShortWindowLine : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        db_ = std::make_shared<MockPostgresDatabase>("mock://short_window");
        ASSERT_TRUE(db_->connect().is_ok());
    }
    void TearDown() override {
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_.erase("SWA");
        registry.instruments_.erase("SWB");
        db_.reset();
        TestBase::TearDown();
    }
    std::shared_ptr<TrendFollowingStrategy> make_sleeve(const std::string& id) {
        auto& registry = InstrumentRegistry::instance();
        for (const char* symbol : {"SWA", "SWB"}) {
            FuturesSpec spec;
            spec.root_symbol = symbol;
            spec.exchange = "CME";
            spec.currency = "USD";
            spec.multiplier = 5.0;
            spec.tick_size = 0.25;
            spec.commission_per_contract = 2.0;
            spec.initial_margin = 10000.0;
            spec.maintenance_margin = 8000.0;
            spec.trading_hours = "09:30-16:00";
            registry.instruments_[symbol] = std::make_shared<FuturesInstrument>(symbol, spec);
        }
        registry.initialized_ = true;
        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 100.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        for (const char* symbol : {"SWA", "SWB"}) {
            sc.trading_params[symbol] = 5.0;
            sc.position_limits[symbol] = 1000.0;
        }
        TrendFollowingConfig tc;
        tc.risk_target = 0.2;
        tc.idm = 2.5;
        tc.ema_windows = {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
        tc.vol_lookback_short = 32;
        tc.fdm = {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}};
        auto strategy = std::make_shared<TrendFollowingStrategy>(id, sc, tc, db_, registry_ptr);
        EXPECT_TRUE(strategy->initialize().is_ok());
        RiskLimits limits;
        limits.max_position_size = 1000.0;
        limits.max_notional_value = 1e9;
        limits.max_drawdown = 0.5;
        limits.max_leverage = 100.0;
        EXPECT_TRUE(strategy->update_risk_limits(limits).is_ok());
        EXPECT_TRUE(strategy->start().is_ok());
        return strategy;
    }
    static std::vector<Bar> bars(const std::string& symbol, int from, int to) {
        std::vector<Bar> out;
        double p = 100.0;
        for (int k = 0; k < to; ++k) {
            p *= 1.0 + 0.004 * ((k * 7) % 5 - 2);
            if (k < from) continue;
            Bar b;
            b.symbol = symbol;
            b.timestamp = Timestamp(std::chrono::hours(24 * (19000 + k)));
            b.open = Decimal(p);
            b.high = Decimal(p * 1.002);
            b.low = Decimal(p * 0.998);
            b.close = Decimal(p);
            b.volume = 100000.0;
            b.instrument_id = "A";
            out.push_back(b);
        }
        return out;
    }
    std::shared_ptr<MockPostgresDatabase> db_;
};

TEST_F(ShortWindowLine, CountsTheSymbolsAndRowsSizedOnAWindowShorterThanW) {
    EXPECT_EQ(estimator_short_window_line({}),
              "ESTIMATOR_SHORT_WINDOW window=3200 symbols=0 rows=0 names=[-]");

    auto sleeve = make_sleeve("SW_TREND");
    // 300 bars of SWA, then three more days of it and 300 bars of SWB on the last: every estimate
    // reads a window far shorter than 3,200 bars.
    ASSERT_TRUE(sleeve->on_data(bars("SWA", 0, 300)).is_ok());
    ASSERT_TRUE(sleeve->on_data(bars("SWA", 300, 301)).is_ok());
    ASSERT_TRUE(sleeve->on_data(bars("SWA", 301, 302)).is_ok());
    auto last = bars("SWA", 302, 303);
    const auto other = bars("SWB", 0, 300);
    last.insert(last.end(), other.begin(), other.end());
    ASSERT_TRUE(sleeve->on_data(last).is_ok());
    ASSERT_LT(sleeve->get_all_instrument_data().at("SWA").estimate.window_bars,
              trend_estimator::kWindowBars);
    EXPECT_EQ(estimator_short_window_line({sleeve}),
              "ESTIMATOR_SHORT_WINDOW window=3200 symbols=2 rows=5 names=[SWA:4 SWB:1]");
    sleeve->stop();
}

TEST(ShortWindowWiring, EveryFuturesRunnerPrintsTheLineOncePerRun) {
    const auto root = repo_root();
    ASSERT_FALSE(root.empty());
    for (const char* runner : kFuturesRunners) {
        EXPECT_EQ(count_of(read_all(root / runner), "INFO(trade_ngin::estimator_short_window_line(strategies));"),
                  1u)
            << runner;
    }
}

// ---- the estimator history's two silent paths --------------------------------------------------

TEST(EstimatorHistoryLines, AFailedIdReadIsWarnedAndAnEmptyHistoryIsNamed) {
    console_logger();
    const Timestamp window_start = Timestamp(std::chrono::hours(24 * 20000));
    const auto no_ids = make_error<std::vector<market_data_utils::FuturesInstrumentId>>(
        ErrorCode::DATABASE_ERROR, "canceling statement due to statement timeout", "PostgresDatabase");
    ::testing::internal::CaptureStdout();
    const auto consumed = estimator_history_consumed({}, window_start, no_ids);
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_TRUE(consumed.empty());
    ASSERT_EQ(count_of(out, "ESTIMATOR_HISTORY the instrument ids of the bars before the window could "
                            "not be read"), 1u) << out;
    const size_t at = out.find("ESTIMATOR_HISTORY the instrument ids");
    const size_t line_start = out.rfind('\n', at) == std::string::npos ? 0 : out.rfind('\n', at) + 1;
    EXPECT_NE(out.substr(line_start, at - line_start).find("[WARN"), std::string::npos) << out;
    EXPECT_EQ(count_of(out, "ESTIMATOR_HISTORY empty: no bar before the window"), 1u) << out;

    // ids read, bars present: neither line
    Bar b;
    b.symbol = "ES.v.0";
    b.timestamp = window_start - std::chrono::hours(48);
    b.open = b.high = b.low = b.close = Decimal(100.0);
    b.volume = 1000.0;
    ::testing::internal::CaptureStdout();
    (void)estimator_history_consumed(
        {b}, window_start, Result<std::vector<market_data_utils::FuturesInstrumentId>>({}));
    const std::string quiet = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(count_of(quiet, "ESTIMATOR_HISTORY"), 0u) << quiet;
}

TEST(EstimatorHistoryLines, TheBacktestNamesAnEmptyHistory) {
    const auto root = repo_root();
    ASSERT_FALSE(root.empty());
    const std::string src = read_all(root / "src/backtest/backtest_coordinator.cpp");
    const size_t at = src.find("what.find(\"No market data loaded\") != std::string::npos");
    ASSERT_NE(at, std::string::npos);
    const std::string branch = src.substr(at, src.find("return Result<void>();", at) - at);
    EXPECT_NE(branch.find("INFO(\"ESTIMATOR_HISTORY empty: no bar before the window"), std::string::npos)
        << "the empty-history return is silent: " << branch;
}

// ---- section 7.3: the backtest's risk_detail column is never dropped without a word ------------

TEST(EquityCurveRiskDetail, ALengthMismatchIsAnErrorNotADroppedColumn) {
    PostgresDatabase db("host=127.0.0.1 port=1 dbname=none_never_connected");
    const std::vector<std::pair<Timestamp, double>> points = {
        {Timestamp(std::chrono::hours(24 * 20000)), 500000.0},
        {Timestamp(std::chrono::hours(24 * 20001)), 500100.0}};
    const auto r = db.store_backtest_equity_curve_batch("RUN", points, "CONSERVATIVE_PORTFOLIO",
                                                        "backtest.equity_curve", {"{\"a\":1}"});
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT) << r.error()->what();
    const std::string what = r.error()->what();
    EXPECT_NE(what.find("1 risk_detail entries for 2 equity points"), std::string::npos) << what;
}

// ---- the comment of risk_detail.hpp names a test file that exists -------------------------------

TEST(RiskDetailHeader, NamesATestFileThatExists) {
    const auto root = repo_root();
    ASSERT_FALSE(root.empty());
    const std::string header = read_all(root / "include/trade_ngin/risk/risk_detail.hpp");
    int named = 0;
    for (size_t at = header.find("tests/"); at != std::string::npos; at = header.find("tests/", at + 1)) {
        const size_t end = header.find(".cpp", at);
        ASSERT_NE(end, std::string::npos);
        const std::string file = header.substr(at, end + 4 - at);
        EXPECT_TRUE(std::filesystem::exists(root / file)) << "risk_detail.hpp names " << file;
        ++named;
    }
    EXPECT_GE(named, 1);
}

// ---- which sleeve a contract is valued on ------------------------------------------------------

// The manager's notional per contract (the RISK_DELIVERED measurement) on a book that names an
// overlay sleeve reads THAT sleeve's row, whichever id it has: never the row of whichever sleeve an
// unordered walk happens to visit last.
class OverlaySleeveNotional : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://overlay_notional");
        ASSERT_TRUE(db_->connect().is_ok());
    }
    void TearDown() override {
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }
    double notional(const std::string& overlay_id, const std::string& other_id,
                    bool overlay_added_first = true) {
        static int n = 0;
        PortfolioManager pm(one_pass_config(overlay_id), "PM_OSN_" + std::to_string(++n));
        StrategyConfig sc;
        sc.capital_allocation = 250000.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto overlay = std::make_shared<TrendFollowingStrategy>(overlay_id, sc, TrendFollowingConfig{}, db_);
        auto other = std::make_shared<TrendFollowingStrategy>(other_id, sc, TrendFollowingConfig{}, db_);
        overlay->instrument_data_["ZZA"].contract_size = 5.0;  // 5 x 120 = 600
        overlay->instrument_data_["ZZA"].price_history = {100.0, 120.0};
        other->instrument_data_["ZZA"].contract_size = 1.0;    // a sleeve still in warm-up: 1 x 120
        other->instrument_data_["ZZA"].price_history = {100.0, 120.0};
        // either order of registration: an unordered walk visits the sleeves in an order that follows it
        EXPECT_TRUE(pm.add_strategy(overlay_added_first ? overlay : other, 0.5, false).is_ok());
        EXPECT_TRUE(pm.add_strategy(overlay_added_first ? other : overlay, 0.5, false).is_ok());
        return pm.delivered_notional_per_contract({"ZZA"}).at("ZZA");
    }
    std::shared_ptr<MockPostgresDatabase> db_;
};

TEST_F(OverlaySleeveNotional, IsTheOverlaySleevesWhicheverIdItHas) {
    EXPECT_EQ(notional("SLEEVE_A", "SLEEVE_B"), 600.0);
    EXPECT_EQ(notional("SLEEVE_B", "SLEEVE_A"), 600.0);
    EXPECT_EQ(notional("TREND_FOLLOWING", "TREND_FOLLOWING_FAST"), 600.0);
    EXPECT_EQ(notional("TREND_FOLLOWING_FAST", "TREND_FOLLOWING"), 600.0);
    for (const bool first : {true, false}) {
        EXPECT_EQ(notional("SLEEVE_A", "SLEEVE_B", first), 600.0) << first;
        EXPECT_EQ(notional("SLEEVE_B", "SLEEVE_A", first), 600.0) << first;
        EXPECT_EQ(notional("TREND_FOLLOWING", "TREND_FOLLOWING_FAST", first), 600.0) << first;
        EXPECT_EQ(notional("TREND_FOLLOWING_FAST", "TREND_FOLLOWING", first), 600.0) << first;
    }
}

// A book that holds a trend sleeve and names no overlay sleeve cannot be rebalanced by the one
// pass, and the generic step would optimise it on a placeholder weight and no cost: it is refused.
// A book of another strategy with no overlay sleeve (the equity book's shape) runs as before.
TEST_F(OverlaySleeveNotional, ATrendSleeveWithoutAnOverlaySleeveIsRefused) {
    PortfolioConfig plain{500000.0, 1.0, 0.0, /*optimization=*/true};
    plain.opt_config.capital = 500000.0;
    plain.risk_config.capital = 500000.0;
    plain.risk_modules = {test_none_module()};
    StrategyConfig sc;
    sc.capital_allocation = 500000.0;
    sc.max_leverage = 10.0;
    sc.asset_classes = {AssetClass::FUTURES};
    sc.frequencies = {DataFrequency::DAILY};
    {
        PortfolioManager pm(plain, "PM_NO_OVERLAY_TREND");
        auto trend = std::make_shared<TrendFollowingStrategy>("TREND", sc, TrendFollowingConfig{}, db_);
        pm.strategies_["TREND"] = PortfolioManager::StrategyInfo{trend, 1.0, true, {}, {}};
        const auto r = pm.process_market_data({one_pass_bar("ZZA", 1, 100.0)});
        ASSERT_TRUE(r.is_error()) << "a trend sleeve was optimised by the generic step";
        EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT);
        const std::string what = r.error()->what();
        EXPECT_NE(what.find("holds the trend sleeve TREND and names no overlay sleeve"), std::string::npos)
            << what;
    }
    {
        // the overlay sleeve named but not registered is the same book
        PortfolioConfig named = plain;
        named.overlay_sleeve = "MISSING";
        named.overlay_tau = 0.2;
        PortfolioManager pm(named, "PM_OVERLAY_NOT_REGISTERED");
        auto trend = std::make_shared<TrendFollowingStrategy>("TREND", sc, TrendFollowingConfig{}, db_);
        pm.strategies_["TREND"] = PortfolioManager::StrategyInfo{trend, 1.0, true, {}, {}};
        EXPECT_TRUE(pm.process_market_data({one_pass_bar("ZZA", 1, 100.0)}).is_error());
    }
    {
        PortfolioManager pm(plain, "PM_NO_OVERLAY_OTHER");
        auto other = make_overlay_stub("OTHER", 500000.0, db_);
        ASSERT_TRUE(other->initialize().is_ok());
        ASSERT_TRUE(other->start().is_ok());
        ASSERT_TRUE(pm.add_strategy(other, 1.0, true).is_ok());
        const auto r = pm.process_market_data({one_pass_bar("ZZA", 1, 100.0)});
        EXPECT_TRUE(r.is_ok()) << (r.is_error() ? r.error()->what() : "");
    }
}

// ---- a stored book the run will not load -------------------------------------------------------

namespace {

// The positions query's answer, every column a string as the generic converter builds it.
class StoredBookDatabase : public MockPostgresDatabase {
public:
    StoredBookDatabase() : MockPostgresDatabase("mock://stored_book") { (void)connect(); }
    std::vector<std::array<std::string, 5>> rows;  // date, strategy_id, sleeve, symbol, quantity
    bool fail = false;
    std::string last_query;
    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override {
        if (query.find(".positions") == std::string::npos) return MockPostgresDatabase::execute_query(query);
        last_query = query;
        if (fail) {
            return make_error<std::shared_ptr<arrow::Table>>(
                ErrorCode::DATABASE_ERROR, "server closed the connection unexpectedly", "PostgresDatabase");
        }
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        std::vector<std::shared_ptr<arrow::Field>> fields;
        const char* const names[] = {"stored_book_date", "strategy_id", "strategy_name", "symbol", "quantity"};
        for (int c = 0; c < 5; ++c) {
            arrow::StringBuilder b;
            for (const auto& row : rows) ARROW_CHECK_OK(b.Append(row[static_cast<size_t>(c)]));
            std::shared_ptr<arrow::Array> a;
            ARROW_CHECK_OK(b.Finish(&a));
            arrays.push_back(a);
            fields.push_back(arrow::field(names[c], arrow::utf8()));
        }
        return Result<std::shared_ptr<arrow::Table>>(arrow::Table::Make(arrow::schema(fields), arrays));
    }
};

const Timestamp kRunDate = Timestamp(std::chrono::seconds(1776988800LL));  // 2026-04-24
const char* const kBaseId = "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST";

}  // namespace

TEST(StoredBookOwnership, ABookTheRunLoadsInFullIsNotRefused) {
    StoredBookDatabase db;
    db.rows = {{"2026-04-23", kBaseId, "TREND_FOLLOWING", "MES.v.0", "1.0000"},
               {"2026-04-23", kBaseId, "TREND_FOLLOWING_FAST", "MES.v.0", "-2.0000"}};
    const auto r = stored_positions_outside_run(db, "BASE_PORTFOLIO", kBaseId,
                                                {"TREND_FOLLOWING", "TREND_FOLLOWING_FAST"}, kRunDate);
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_TRUE(r.value().empty());
    // the previous book date before the RUN date, this portfolio's, non-zero rows only
    EXPECT_NE(db.last_query.find("portfolio_id = 'BASE_PORTFOLIO'"), std::string::npos) << db.last_query;
    EXPECT_NE(db.last_query.find("quantity <> 0"), std::string::npos);
    EXPECT_NE(db.last_query.find("DATE(date) < '2026-04-24'"), std::string::npos) << db.last_query;
    // a portfolio with no stored book at all
    db.rows.clear();
    const auto first = stored_positions_outside_run(db, "BASE_PORTFOLIO", kBaseId, {"TREND_FOLLOWING"}, kRunDate);
    ASSERT_TRUE(first.is_ok());
    EXPECT_TRUE(first.value().empty());
}

// The adversary's construction: BASE with its first sleeve not loaded. The run's strategy id is
// then the FAST sleeve's alone, and BOTH sleeves' stored rows sit under the two-sleeve id.
TEST(StoredBookOwnership, StoredPositionsOfASleeveTheRunDoesNotLoadAreNamed) {
    StoredBookDatabase db;
    db.rows = {{"2026-04-23", kBaseId, "TREND_FOLLOWING", "6C.v.0", "1.0000"},
               {"2026-04-23", kBaseId, "TREND_FOLLOWING", "MES.v.0", "1.0000"},
               {"2026-04-23", kBaseId, "TREND_FOLLOWING_FAST", "MES.v.0", "1.0000"}};
    auto r = stored_positions_outside_run(db, "BASE_PORTFOLIO", "LIVE_TREND_FOLLOWING_FAST",
                                          {"TREND_FOLLOWING_FAST"}, kRunDate);
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 3u) << "another strategy id: no row of it is this run's";
    EXPECT_EQ(r.value()[0], std::string("2026-04-23 ") + kBaseId + "/TREND_FOLLOWING 6C.v.0 1.0000");
    const std::string line =
        stored_positions_outside_run_line("BASE_PORTFOLIO", "LIVE_TREND_FOLLOWING_FAST", r.value());
    EXPECT_EQ(line.rfind("STORED_BOOK_NOT_LOADED portfolio BASE_PORTFOLIO: 3 stored non-zero position(s)", 0), 0u)
        << line;
    EXPECT_NE(line.find("it runs as LIVE_TREND_FOLLOWING_FAST"), std::string::npos) << line;
    EXPECT_NE(line.find("TREND_FOLLOWING 6C.v.0 1.0000"), std::string::npos) << line;
    EXPECT_NE(line.find("Refusing to run"), std::string::npos) << line;

    // the same strategy id, one of its sleeves not loaded: that sleeve's rows only
    r = stored_positions_outside_run(db, "BASE_PORTFOLIO", kBaseId, {"TREND_FOLLOWING_FAST"}, kRunDate);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().size(), 2u);
}

TEST(StoredBookOwnership, AFailedReadIsAnErrorNotAnEmptyBook) {
    StoredBookDatabase db;
    db.fail = true;
    const auto r = stored_positions_outside_run(db, "BASE_PORTFOLIO", kBaseId, {"TREND_FOLLOWING"}, kRunDate);
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("previous stored book could not be read"), std::string::npos);
}

// Both live futures runners refuse before any row of the day is written: the check sits above the
// sizing read, which is itself above the first write (the live_run_metadata upsert).
TEST(StoredBookOwnership, BothLiveFuturesRunnersRefuseBeforeAnyRowIsWritten) {
    const auto root = repo_root();
    ASSERT_FALSE(root.empty());
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        const std::string src = read_all(root / runner);
        const size_t check = src.find("stored_positions_outside_run(*db, coordinator_config.portfolio_id,");
        ASSERT_NE(check, std::string::npos) << runner;
        const size_t refuse = src.find("return 1;", check);
        const size_t sizing = src.find("read_live_sizing_equity(", check);
        const size_t first_write = src.find("db->store_live_run_metadata(", check);
        ASSERT_NE(sizing, std::string::npos);
        ASSERT_NE(first_write, std::string::npos);
        EXPECT_LT(refuse, sizing) << runner;
        EXPECT_LT(sizing, first_write) << runner;
        EXPECT_EQ(src.substr(0, check).find("db->store_"), std::string::npos)
            << runner << " writes a row above the check";
        EXPECT_EQ(count_of(src, "WARN(sizing_capital_history_log_line("), 1u) << runner;
    }
    for (const char* runner : kFuturesRunners) {
        EXPECT_EQ(count_of(read_all(root / runner), "Logger::register_component(\"SleeveConfig\");"), 2u)
            << runner << ": the required-key refusal is logged under its own tag";
    }
}

// The generic optimiser step, which no book with a trend sleeve reaches after the one pass, reads
// no trend sleeve: one cast is left in the manager, the one above.
TEST(OverlaySleeveNotionalSource, TheUnreachedCastSitesAreGone) {
    const auto root = repo_root();
    ASSERT_FALSE(root.empty());
    const std::string src = read_all(root / "src/portfolio/portfolio_manager.cpp");
    // Two casts are left: the refusal of a trend sleeve with no overlay sleeve (it reads no trend
    // data), and the one reached site.
    EXPECT_EQ(count_of(src, "dynamic_pointer_cast<TrendFollowingStrategy>"), 2u);
    const size_t refusal = src.find("dynamic_pointer_cast<TrendFollowingStrategy>");
    ASSERT_NE(refusal, std::string::npos);
    EXPECT_NE(src.find("names no overlay sleeve the one pass can run on", refusal), std::string::npos);
    const size_t at = src.rfind("dynamic_pointer_cast<TrendFollowingStrategy>");
    ASSERT_NE(at, refusal);
    const size_t function = src.rfind("PortfolioManager::delivered_notional_per_contract(", at);
    ASSERT_NE(function, std::string::npos);
    EXPECT_EQ(src.find("\n}\n", function) > at, true) << "the remaining cast is not in that function";
    EXPECT_EQ(src.find("all_trading_data"), std::string::npos);
}
