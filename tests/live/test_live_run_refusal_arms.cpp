// tests/live/test_live_run_refusal_arms.cpp
//
// T-7a commit 1 (S-1, S-4, T-RISK-ARCH Q2): what a refused or half-finished live run leaves
// in the database.
//
// S-1. The futures runners' day-classification refusal ("DATA ISSUE DETECTED - ABORTING",
//      no T-1 close on a day the calendar calls a trading day) sat BELOW the
//      trading.live_run_metadata upsert, so the refused 2026-05-18 replay (T-5 C-HEAD) exited 1
//      leaving a metadata row and no live_results row. scripts/check_live_trading.py reads that
//      table as proof the day's run happened. a117a8d9 moved the A3/A2/A1 guards above the
//      upsert and left this one below it.
// Q2.  A portfolio-scope risk REFUSE is found inside process_market_data, after the upsert. The
//      row records a run that did happen, so HD ruled it is marked, not deleted: the runner
//      writes the same row again with the refusal in its portfolio_config JSON.
// S-4. PostgresDatabase::execute_direct_query returned Result<void>, so the runners' Day T-1
//      live_results UPDATE that matched ZERO rows logged "Successfully updated Day T-1
//      live_results with finalized PnL and all metrics" (T-5 C-LAST, 2026-05-04 on a Mon-Fri
//      cron). It now reports the affected rows and the zero-row case warns.
//
// The runners are `main()`s and cannot be linked into this binary, so the placement of each
// arm is tested in the runner source, as tests/live/test_day_t_write_ordering.cpp does. The
// pieces that can run are run: the PortfolioManager is driven to a real REFUSE and the helper
// the runners call reads it; the database tests (TRADE_NGIN_TEST_DSN only) write the two
// upserts and read the row back, and count the rows a Day T-1 shaped UPDATE touches.

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/run_metadata_marks.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/basic_risk_modules.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

// ---------------------------------------------------------------------------------------------
// Runner source
// ---------------------------------------------------------------------------------------------

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

const char* const kFuturesRunners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                                       "apps/strategies/live_portfolio.cpp"};
const char* const kAllRunners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                                   "apps/strategies/live_portfolio.cpp",
                                   "apps/strategies/live_equity_mean_reversion.cpp"};

constexpr auto npos = std::string::npos;

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

// ---------------------------------------------------------------------------------------------
// A PortfolioManager that can be driven to a REFUSE (the shape of
// tests/portfolio/test_risk_module_loop.cpp's fixture, kept local).
// ---------------------------------------------------------------------------------------------

using Book = std::unordered_map<std::string, Position>;

class FixedBookStrategy : public BaseStrategy {
public:
    FixedBookStrategy(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db,
                      Book book)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          book_(std::move(book)) {
        metadata_.name = "Fixed Book Strategy";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        (void)data;
        ++calls_;
        return Result<void>();
    }
    std::unordered_map<std::string, Position> get_target_positions() const override {
        return calls_ == 0 ? Book{} : book_;
    }

private:
    Book book_;
    size_t calls_{0};
};

Timestamp day(int d) {
    return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d));
}

Bar make_bar(const std::string& symbol, int d, double close) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = day(d);
    b.open = b.high = b.low = b.close = Decimal(close);
    b.volume = 1000.0;
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

std::vector<Bar> three_days(const std::string& symbol) {
    return {make_bar(symbol, 1, 100.0), make_bar(symbol, 2, 102.0), make_bar(symbol, 3, 99.0)};
}

PortfolioConfig small_config() {
    PortfolioConfig pc{1000.0, 1.0, 0.0, /*optimization=*/false};
    pc.allow_fractional_positions = false;
    pc.risk_config.capital = 1000.0;
    pc.risk_config.var_limit = 1e6;
    pc.risk_config.jump_risk_limit = 1e6;
    pc.risk_config.max_correlation = 1.0;
    pc.risk_config.max_gross_leverage = 100.0;
    pc.risk_config.max_net_leverage = 100.0;
    pc.risk_modules = {test_carver_module(pc.risk_config)};
    return pc;
}

class RefusalPmFixture : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        mock_db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(mock_db_->connect().is_ok());
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::INFO;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
    }
    void TearDown() override {
        pm_.reset();
        strategies_.clear();
        mock_db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    std::shared_ptr<FixedBookStrategy> add(const std::string& id, Book book, double allocation) {
        StrategyConfig sc;
        sc.capital_allocation = 1000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto s = std::make_shared<FixedBookStrategy>(id, sc, mock_db_, std::move(book));
        EXPECT_TRUE(s->initialize().is_ok());
        EXPECT_TRUE(s->start().is_ok());
        EXPECT_TRUE(pm_->add_strategy(s, allocation, false).is_ok());
        strategies_.push_back(s);
        return s;
    }

    void make_pm(const std::string& id) {
        pm_ = std::make_unique<PortfolioManager>(small_config(), id);
    }

    std::shared_ptr<MockPostgresDatabase> mock_db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::vector<std::shared_ptr<FixedBookStrategy>> strategies_;
};

// The two JSON values the runner writes on its first upsert, built the way the runner builds
// them (live_portfolio_conservative.cpp, "STORE LIVE RUN METADATA").
nlohmann::json first_upsert_portfolio_config() {
    nlohmann::json j;
    j["total_capital"] = 500000.0;
    j["use_optimization"] = true;
    return j;
}

// ---------------------------------------------------------------------------------------------
// Database access, TRADE_NGIN_TEST_DSN only (never discovered from config/).
// ---------------------------------------------------------------------------------------------

std::string test_dsn() {
    const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
    return (dsn && *dsn) ? std::string(dsn) : std::string();
}

bool require_db() {
    const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
    return v && std::string(v) == "1";
}

constexpr const char* kProbeStrategy = "C1_REFUSAL_PROBE_ID";
constexpr const char* kProbePortfolio = "C1_REFUSAL_PROBE_PORTFOLIO";

Timestamp date_at(int year, int month, int d) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = d;
    tm.tm_hour = 12;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

// -1 when execute_direct_query cannot say how many rows it touched (its Result<void> form, as
// at 0738fd7e), else the count. Written generically so this file compiles against both forms
// and the parent source fails at RUN time, on the assertion, not at compile time.
template <typename R>
long long rows_reported(const R& result) {
    if constexpr (std::is_same_v<R, Result<void>>) {
        (void)result;
        return -1;
    } else {
        return static_cast<long long>(result.value());
    }
}

class RunMetadataDbFixture : public ::testing::Test {
protected:
    void SetUp() override {
        dsn_ = test_dsn();
        if (dsn_.empty()) {
            if (require_db()) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set";
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(dsn_);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            if (require_db()) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable";
            GTEST_SKIP() << "database unreachable; skipping";
        }
        purge();
    }
    void TearDown() override {
        if (db_ && db_->is_connected()) purge();
    }
    void purge() {
        try {
            pqxx::connection c(dsn_);
            pqxx::work txn(c);
            txn.exec("DELETE FROM trading.live_run_metadata WHERE strategy_id = " +
                     txn.quote(kProbeStrategy));
            txn.exec("DELETE FROM trading.live_results WHERE strategy_id = " +
                     txn.quote(kProbeStrategy));
            txn.commit();
        } catch (const std::exception&) {
            // Best effort; a failure here must not mask the assertion that ran.
        }
    }

    std::string dsn_;
    std::shared_ptr<PostgresDatabase> db_;
};

}  // namespace

// =============================================================================================
// S-1: the day-classification refusal precedes the metadata upsert, in both twins.
// =============================================================================================

TEST(RunRefusalArmsSource, DayClassificationRefusalIsAboveTheMetadataUpsertInBothTwins) {
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

        const auto refusal = src.find("ERROR(\"DATA ISSUE DETECTED - ABORTING\");");
        const auto upsert = src.find("db->store_live_run_metadata(");
        ASSERT_NE(refusal, npos) << "the day-classification refusal is gone entirely";
        ASSERT_NE(upsert, npos);
        EXPECT_EQ(count_of(src, "ERROR(\"DATA ISSUE DETECTED - ABORTING\");"), 1u);

        EXPECT_LT(refusal, upsert)
            << "the day-classification refusal runs AFTER the live_run_metadata upsert: a "
               "refused day (the 2026-05-18 replay) exits 1 leaving a metadata row and no "
               "live_results row, which the watchdog reads as a completed run (S-1)";

        // The refusal returns before the upsert: its `return 1` lies between the two.
        const auto ret = src.find("return 1;  // Fail fast on data issues", refusal);
        ASSERT_NE(ret, npos);
        EXPECT_LT(ret, upsert) << "the refusal must RETURN before the upsert, not fall through";
    }
}

TEST(RunRefusalArmsSource, TheTwinsCarryTheSameRefusalAndUpsertBlock) {
    std::vector<std::string> blocks;
    for (const char* runner : kFuturesRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const auto from = src.find("bool is_yesterday_holiday = holiday_checker.is_holiday(");
        const auto to = src.find("bool skip_strategy_processing = false;");
        ASSERT_NE(from, npos);
        ASSERT_NE(to, npos);
        ASSERT_LT(from, to);
        blocks.push_back(src.substr(from, to - from));
    }
    EXPECT_EQ(blocks[0], blocks[1])
        << "live_portfolio_conservative.cpp and live_portfolio.cpp are twins: the refusal and "
           "the upsert must be byte-identical in both";
    EXPECT_NE(blocks[0].find("DATA ISSUE DETECTED - ABORTING"), npos)
        << "the refusal must sit inside the classification-to-upsert block";
    EXPECT_NE(blocks[0].find("db->store_live_run_metadata("), npos);
}

// =============================================================================================
// Q2: the runners mark the metadata row when the portfolio risk step refused.
// =============================================================================================

TEST(RunRefusalArmsSource, BothTwinsMarkTheMetadataRowAfterProcessMarketData) {
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

        const auto process = src.find("portfolio->process_market_data(all_bars);");
        ASSERT_NE(process, npos);
        const auto detect =
            src.find("portfolio_risk_refusal(portfolio->last_risk_decisions())", process);
        const auto second_upsert = src.find("db->store_live_run_metadata(", process);
        const auto marked = src.find("mark_risk_refusal(", process);
        const auto error_check = src.find("if (port_process_result.is_error())", process);
        ASSERT_NE(error_check, npos);

        EXPECT_NE(detect, npos)
            << "the runner never asks the PM whether its risk step refused: a refused run "
               "leaves an unmarked metadata row that reads as a completed run (T-RISK-ARCH Q2)";
        EXPECT_NE(second_upsert, npos) << "no second live_run_metadata upsert after the PM call";
        EXPECT_NE(marked, npos) << "the second upsert does not carry the refusal";
        if (detect == npos || second_upsert == npos || marked == npos) continue;
        EXPECT_LT(detect, second_upsert);
        EXPECT_LT(second_upsert, marked) << "the marked JSON is the second upsert's argument";
        EXPECT_LT(marked, error_check)
            << "the mark must come before the error check, or a refusal that fails the call "
               "(an unseeded scope, exit 1) leaves its row unmarked";
    }
}

// =============================================================================================
// S-4: a zero-row Day T-1 UPDATE warns instead of logging success, in all three runners.
// =============================================================================================

TEST(RunRefusalArmsSource, AZeroRowDayTMinusOneUpdateWarnsInEveryRunner) {
    for (const char* runner : kAllRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

        const auto update = src.find("db->execute_direct_query(update_query);");
        ASSERT_NE(update, npos);
        const auto success = src.find(
            "Successfully updated Day T-1 live_results with finalized PnL and all metrics", update);
        ASSERT_NE(success, npos);
        EXPECT_EQ(count_of(src, "Successfully updated Day T-1 live_results with finalized PnL and "
                                "all metrics"),
                  1u);

        const auto zero = src.find("update_result.value() == 0", update);
        const auto warn = src.find("WARN(\"Day T-1 live_results UPDATE matched 0 rows for \"", update);
        EXPECT_TRUE(zero != npos && zero < success)
            << "the Day T-1 UPDATE's row count is never checked before the success line: an "
               "UPDATE that matched no row logs \"Successfully updated\" (S-4)";
        EXPECT_TRUE(warn != npos && warn < success)
            << "no WARN for the zero-row Day T-1 UPDATE";
    }
}

// =============================================================================================
// Q2, the part that runs: the PM's REFUSE, read by the helper the runners call.
// =============================================================================================

TEST_F(RefusalPmFixture, APortfolioRefuseIsReadWithItsModuleReasonAndLap) {
    make_pm("PM_C1_REFUSE");
    add("C1_S", {{"ZZA", make_pos("ZZA", 5.0, 100.0)}}, 1.0);
    // Seeded, as the runners seed yesterday's book before the call.
    ASSERT_TRUE(pm_->update_strategy_position("C1_S", "ZZA", make_pos("ZZA", 1.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules({std::make_shared<RefuseOnConditionRiskModule>(
                                          "stop", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0},
                                          "book over the stop")})
                    .is_ok());
    ::testing::internal::CaptureStdout();
    const auto result = pm_->process_market_data(three_days("ZZA"));
    ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(result.is_ok());
    // The PM did refuse: yesterday's book is what it holds.
    EXPECT_EQ(static_cast<double>(
                  pm_->get_strategy_positions().at("C1_S").at("ZZA").quantity),
              1.0);

    const auto refusal = portfolio_risk_refusal(pm_->last_risk_decisions());
    ASSERT_TRUE(refusal.has_value()) << "the helper did not see the PM's portfolio REFUSE";
    EXPECT_EQ((*refusal)["action"], "REFUSE");
    EXPECT_EQ((*refusal)["module"], "stop");
    EXPECT_EQ((*refusal)["reason"], "book over the stop");
    EXPECT_EQ((*refusal)["scope_id"], "PM_C1_REFUSE");
    EXPECT_EQ((*refusal)["phase"], "lap");
    EXPECT_EQ((*refusal)["lap"], 1);

    // The second upsert's JSON: the first upsert's keys kept verbatim, the mark added.
    const auto decisions = pm_->risk_decisions_json();
    const auto marked = mark_risk_refusal(first_upsert_portfolio_config(), *refusal, decisions);
    for (const auto& [k, v] : first_upsert_portfolio_config().items()) {
        EXPECT_EQ(marked.at(k), v) << k;
    }
    EXPECT_EQ(marked["risk_refusal"], *refusal);
    EXPECT_EQ(marked["risk_decisions"], decisions);
    EXPECT_EQ(marked["risk_decisions"]["outcome"]["refused"], true);
}

TEST_F(RefusalPmFixture, NoRefuseMeansNoMarkAndTheRowIsLeftAlone) {
    make_pm("PM_C1_NONE");
    add("C1_S", {{"ZZA", make_pos("ZZA", 2.0, 100.0)}}, 1.0);
    ASSERT_TRUE(pm_->update_strategy_position("C1_S", "ZZA", make_pos("ZZA", 1.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules({std::make_shared<WarnRiskModule>(
                                          "look", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0},
                                          "only a warning")})
                    .is_ok());
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days("ZZA")).is_ok());
    ::testing::internal::GetCapturedStdout();
    EXPECT_FALSE(pm_->last_risk_decisions().empty()) << "the module was evaluated";
    EXPECT_FALSE(portfolio_risk_refusal(pm_->last_risk_decisions()).has_value())
        << "a WARN is not a refusal: the runner must not write the second upsert";
}

TEST_F(RefusalPmFixture, ASleeveRefuseDoesNotMarkThePortfolioRun) {
    make_pm("PM_C1_SLEEVE");
    add("SA", {{"AAA", make_pos("AAA", 3.0, 100.0)}}, 0.5);
    add("SB", {{"BBB", make_pos("BBB", 4.0, 100.0)}}, 0.5);
    ASSERT_TRUE(pm_->update_strategy_position("SA", "AAA", make_pos("AAA", 1.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules(
                       {}, {{"SA", {std::make_shared<RefuseOnConditionRiskModule>(
                                       "sleeve_stop",
                                       RiskCondition{RiskCondition::Kind::ALWAYS, 0.0},
                                       "sleeve only")}}})
                    .is_ok());
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days("AAA")).is_ok());
    ::testing::internal::GetCapturedStdout();
    bool sleeve_refused = false;
    for (const auto& r : pm_->last_risk_decisions()) {
        if (r.scope == RiskScope::SLEEVE && r.applied_action == RiskAction::REFUSE) {
            sleeve_refused = true;
        }
    }
    ASSERT_TRUE(sleeve_refused) << "setup: the sleeve module did not refuse";
    EXPECT_FALSE(portfolio_risk_refusal(pm_->last_risk_decisions()).has_value())
        << "a sleeve REFUSE pins that sleeve only; the portfolio run did not refuse";
}

// =============================================================================================
// Database (TRADE_NGIN_TEST_DSN): the second upsert, and execute_direct_query's row count.
// =============================================================================================

TEST_F(RunMetadataDbFixture, TheSecondUpsertMarksTheSameRowWithTheReason) {
    const Timestamp run_date = date_at(2026, 5, 18);
    const nlohmann::json allocations = {{"TREND_FOLLOWING", 1.0}};
    const nlohmann::json strategy_configs = {{"TREND_FOLLOWING", {{"probe", true}}}};
    const nlohmann::json config = first_upsert_portfolio_config();

    // The first upsert, as every run writes it.
    ASSERT_FALSE(db_->store_live_run_metadata(run_date, kProbeStrategy, kProbePortfolio,
                                              allocations, config, strategy_configs)
                     .is_error());

    // The mark, from a refusal record shaped as the PM records one.
    RiskDecisionRecord rec;
    rec.phase = RiskPhase::LAP;
    rec.lap = 2;
    rec.scope = RiskScope::PORTFOLIO;
    rec.scope_id = kProbePortfolio;
    rec.module_id = "stop";
    rec.requested.action = RiskAction::REFUSE;
    rec.requested.module_id = "stop";
    rec.requested.reason = "gross leverage over the stop";
    rec.applied_action = RiskAction::REFUSE;
    const auto refusal = portfolio_risk_refusal({rec});
    ASSERT_TRUE(refusal.has_value());
    const nlohmann::json decisions = build_risk_decisions_json({}, {rec});
    ASSERT_FALSE(db_->store_live_run_metadata(run_date, kProbeStrategy, kProbePortfolio,
                                              allocations,
                                              mark_risk_refusal(config, *refusal, decisions),
                                              strategy_configs)
                     .is_error());

    pqxx::connection c(dsn_);
    pqxx::work txn(c);
    auto r = txn.exec(
        "SELECT portfolio_config::text, strategy_allocations::text, strategy_configs::text "
        "FROM trading.live_run_metadata WHERE strategy_id = " +
        txn.quote(kProbeStrategy) + " AND portfolio_id = " + txn.quote(kProbePortfolio) +
        " AND date = '2026-05-18'");
    ASSERT_EQ(r.size(), 1u) << "the mark must update the day's row, not add a second one";
    const auto stored = nlohmann::json::parse(r[0][0].as<std::string>());
    EXPECT_EQ(stored["risk_refusal"]["action"], "REFUSE");
    EXPECT_EQ(stored["risk_refusal"]["module"], "stop");
    EXPECT_EQ(stored["risk_refusal"]["reason"], "gross leverage over the stop");
    EXPECT_EQ(stored["risk_refusal"]["lap"], 2);
    EXPECT_EQ(stored["risk_decisions"]["outcome"]["refused"], true);
    EXPECT_EQ(stored["total_capital"], 500000.0);
    EXPECT_EQ(stored["use_optimization"], true);
    EXPECT_EQ(nlohmann::json::parse(r[0][1].as<std::string>()), allocations);
    EXPECT_EQ(nlohmann::json::parse(r[0][2].as<std::string>()), strategy_configs);
}

TEST_F(RunMetadataDbFixture, ExecuteDirectQueryReportsTheRowsItTouched) {
    // A statement whose command tag carries no row count (DDL): succeeds and reports 0. At
    // ed6c4a6e this threw "Could not convert '' to int" out of execute_direct_query, AFTER the
    // statement had committed (libpqxx's affected_rows() on an empty tag).
    auto created = db_->execute_direct_query("CREATE TEMP TABLE c1_s4_probe (id integer)");
    ASSERT_FALSE(created.is_error()) << created.error()->what();
    EXPECT_EQ(rows_reported(created), 0);

    auto inserted = db_->execute_direct_query("INSERT INTO c1_s4_probe VALUES (1), (2)");
    ASSERT_FALSE(inserted.is_error());
    EXPECT_EQ(rows_reported(inserted), 2)
        << "execute_direct_query does not report the rows a statement touched (S-4)";

    auto none = db_->execute_direct_query("UPDATE c1_s4_probe SET id = id WHERE id = 99");
    ASSERT_FALSE(none.is_error()) << "a statement that matches nothing is not an error";
    EXPECT_EQ(rows_reported(none), 0);

    auto one = db_->execute_direct_query("UPDATE c1_s4_probe SET id = id WHERE id = 1");
    ASSERT_FALSE(one.is_error());
    EXPECT_EQ(rows_reported(one), 1);

    // The runners' Day T-1 UPDATE opens with a CTE ("WITH day_before AS (...) UPDATE ...").
    auto with_update = db_->execute_direct_query(
        "WITH d AS (SELECT 2 AS k) UPDATE c1_s4_probe SET id = id WHERE id = (SELECT k FROM d)");
    ASSERT_FALSE(with_update.is_error());
    EXPECT_EQ(rows_reported(with_update), 1);

    auto deleted = db_->execute_direct_query("  -- leading comment\n DELETE FROM c1_s4_probe");
    ASSERT_FALSE(deleted.is_error());
    EXPECT_EQ(rows_reported(deleted), 2);

    // More statements with no row count: each succeeds, reports 0, and took effect.
    auto set = db_->execute_direct_query("/* c1 */ SET LOCAL statement_timeout = 0");
    ASSERT_FALSE(set.is_error()) << set.error()->what();
    EXPECT_EQ(rows_reported(set), 0);
    auto dropped = db_->execute_direct_query("DROP TABLE c1_s4_probe");
    ASSERT_FALSE(dropped.is_error()) << dropped.error()->what();
    EXPECT_EQ(rows_reported(dropped), 0);
    auto gone = db_->execute_direct_query("SELECT 1 FROM c1_s4_probe");
    EXPECT_TRUE(gone.is_error()) << "the DROP must have committed";
}

TEST_F(RunMetadataDbFixture, ADayTMinusOneUpdateOnAMissingDateReportsZeroRows) {
    // The Day T-1 finalization's shape (live_portfolio_conservative.cpp, "UPDATE
    // trading.live_results ... WHERE strategy_id AND portfolio_id AND DATE(date) = T-1") on a
    // date with no row: T-5 C-LAST's Monday 2026-05-04 run on a Mon-Fri cron, whose T-1 was
    // the Sunday 2026-05-03 that a cron never writes.
    const std::string update =
        "WITH day_before AS (SELECT COALESCE(current_portfolio_value, 500000.0) AS portfolio "
        "  FROM trading.live_results WHERE strategy_id = '" + std::string(kProbeStrategy) +
        "' AND portfolio_id = '" + std::string(kProbePortfolio) +
        "' AND DATE(date) < '2026-05-03' ORDER BY date DESC LIMIT 1) "
        "UPDATE trading.live_results SET current_portfolio_value = "
        "COALESCE((SELECT portfolio FROM day_before), 500000.0) "
        "WHERE strategy_id = '" + std::string(kProbeStrategy) + "' AND portfolio_id = '" +
        std::string(kProbePortfolio) + "' AND DATE(date) = '2026-05-03'";
    auto result = db_->execute_direct_query(update);
    ASSERT_FALSE(result.is_error()) << result.error()->what();
    EXPECT_EQ(rows_reported(result), 0)
        << "a Day T-1 UPDATE that finalized nothing must be told apart from one that did; "
           "the runners warn on 0 (S-4)";
}
