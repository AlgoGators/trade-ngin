// The backtest predicate (T-7a commit 4; T-4c J1 re-keyed on the session classifier).
//
// In the portfolio backtest the PM sizes orders as (target - filled ledger) and prices them from
// the SIGNAL group (the previous day's bars). A symbol with no bar there used to be skipped for
// the FILL only: `info.current_positions = info.target_positions` had already moved the BOOK, so
// the equity curve earned P&L on contracts never bought (T-4c: 6L 2025-06-30 -85.00, MYM
// 2026-04-24 -154.00). With a session set (the coordinator passes it for FUTURES), a symbol whose
// signal-group bar is not a SESSION -- no bar, or a JUNK bar -- gets no fill and no book change:
// its book is held at the filled ledger. Without a set (every live caller, the equity backtest)
// the old skip is unchanged.

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

using Book = std::unordered_map<std::string, Position>;

Timestamp day(int d) { return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d)); }

Bar make_bar(const std::string& symbol, int d, double close, double volume = 100000.0) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = day(d);
    b.open = Decimal(close);
    b.high = Decimal(close * 1.01);
    b.low = Decimal(close * 0.99);
    b.close = Decimal(close);
    b.volume = volume;
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

class ScriptedStrategy : public BaseStrategy {
public:
    ScriptedStrategy(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db,
                     std::vector<Book> script)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          script_(std::move(script)) {
        metadata_.name = "Scripted Strategy";
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

}  // namespace

class PmSessionHoldTest : public TestBase {
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

    void make_pm(std::vector<Book> script) {
        static int n = 0;
        pm_ = std::make_unique<PortfolioManager>(base_config(), "PM_SH_" + std::to_string(++n));
        StrategyConfig sc;
        sc.capital_allocation = 1000000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        strategy_ = std::make_shared<ScriptedStrategy>("SH_S", sc, db_, std::move(script));
        ASSERT_TRUE(strategy_->initialize().is_ok());
        ASSERT_TRUE(strategy_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(strategy_, 1.0, false).is_ok());
    }

    /// Executions of `symbol` the manager holds right now (they accumulate across calls).
    size_t executions_of(const std::string& symbol) {
        size_t n = 0;
        for (const auto& [sid, reports] : pm_->get_strategy_executions()) {
            (void)sid;
            for (const auto& r : reports) n += (r.symbol == symbol);
        }
        return n;
    }

    double quantity(const std::string& symbol) {
        return static_cast<double>(pm_->get_strategy_positions().at("SH_S").at(symbol).quantity);
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::shared_ptr<ScriptedStrategy> strategy_;
};

// A JUNK bar: the symbol HAS a bar (and a price) in the signal group, but it is not a session.
TEST_F(PmSessionHoldTest, AJunkSignalBarGivesNoFillAndNoBookChange) {
    make_pm({{{"ZZA", make_pos("ZZA", 2, 100)}, {"ZZB", make_pos("ZZB", 3, 50)}},
             {{"ZZA", make_pos("ZZA", 4, 100)}, {"ZZB", make_pos("ZZB", 5, 50)}}});
    const std::unordered_set<std::string> both{"ZZA", "ZZB"};
    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 1, 100), make_bar("ZZB", 1, 50)}, false,
                                         day(2), &both)
                    .is_ok());
    ASSERT_EQ(executions_of("ZZB"), 1u);
    ASSERT_DOUBLE_EQ(quantity("ZZB"), 3.0);

    const std::unordered_set<std::string> zza_only{"ZZA"};  // ZZB's day-2 bar is JUNK
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 2, 101), make_bar("ZZB", 2, 51, 3)},
                                         false, day(3), &zza_only)
                    .is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(executions_of("ZZB"), 1u) << "no fill at the junk print";
    EXPECT_DOUBLE_EQ(quantity("ZZB"), 3.0) << "and no book change: held at the filled ledger";
    EXPECT_EQ(executions_of("ZZA"), 2u) << "every other symbol trades normally";
    EXPECT_DOUBLE_EQ(quantity("ZZA"), 4.0);
    EXPECT_NE(out.find("BOOK_GATE backtest ZZB (SH_S): no SESSION bar in the signal group -- book "
                       "held at filled qty=3.000000 instead of target 5.000000"),
              std::string::npos)
        << out;
}

TEST_F(PmSessionHoldTest, ASymbolWithNoSignalBarIsHeldWhenTheSetIsGiven) {
    make_pm({{{"ZZA", make_pos("ZZA", 2, 100)}, {"ZZB", make_pos("ZZB", 3, 50)}},
             {{"ZZA", make_pos("ZZA", 2, 100)}, {"ZZB", make_pos("ZZB", 4, 50)}},
             {{"ZZA", make_pos("ZZA", 2, 100)}, {"ZZB", make_pos("ZZB", 4, 50)}}});
    const std::unordered_set<std::string> both{"ZZA", "ZZB"};
    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 1, 100), make_bar("ZZB", 1, 50)}, false,
                                         day(2), &both)
                    .is_ok());
    const std::unordered_set<std::string> zza_only{"ZZA"};
    ASSERT_TRUE(
        pm_->process_market_data({make_bar("ZZA", 2, 101)}, false, day(3), &zza_only).is_ok());
    EXPECT_DOUBLE_EQ(quantity("ZZB"), 3.0) << "the book does not run ahead of its fill";
    EXPECT_EQ(executions_of("ZZB"), 1u);
    // The next cycle whose signal group carries a ZZB session trades the gap at that bar.
    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 3, 102), make_bar("ZZB", 3, 52)}, false,
                                         day(4), &both)
                    .is_ok());
    EXPECT_DOUBLE_EQ(quantity("ZZB"), 4.0);
    EXPECT_EQ(executions_of("ZZB"), 2u);
}

// The parent's behaviour, kept for every caller that passes no set (live runners, equities).
TEST_F(PmSessionHoldTest, WithoutASetTheOldSkipStillMovesTheBookWithoutAFill) {
    make_pm({{{"ZZA", make_pos("ZZA", 2, 100)}, {"ZZB", make_pos("ZZB", 3, 50)}},
             {{"ZZA", make_pos("ZZA", 2, 100)}, {"ZZB", make_pos("ZZB", 4, 50)}}});
    ASSERT_TRUE(
        pm_->process_market_data({make_bar("ZZA", 1, 100), make_bar("ZZB", 1, 50)}, false, day(2))
            .is_ok());
    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 2, 101)}, false, day(3)).is_ok());
    EXPECT_DOUBLE_EQ(quantity("ZZB"), 4.0) << "no set: the book moves to the target";
    EXPECT_EQ(executions_of("ZZB"), 1u) << "and no fill";
}

TEST(PmSessionHoldSource, TheCoordinatorPassesTheSetForFuturesOnly) {
    const std::string src = read_source("src/backtest/backtest_coordinator.cpp");
    if (src.empty()) GTEST_SKIP() << "coordinator source not found";
    EXPECT_NE(src.find("session_hold_enabled_ = (asset_class == AssetClass::FUTURES);"),
              std::string::npos);
    EXPECT_NE(src.find("classify_bar_group(session_classifier_, bars_for_signals)"),
              std::string::npos)
        << "the verdict is taken on the SIGNAL group's bars (the previous group)";
    // T-7b-1 7a: the PM is fed the signal group with its JUNK bars delayed one cycle (as live);
    // the session set still goes with it.
    EXPECT_NE(src.find("portfolio->process_market_data(*signal_feed, is_warmup, timestamp,\n"
                       "                                                              session_symbols);"),
              std::string::npos);
    const auto add = src.find("if (session_hold_enabled_) session_classifier_.add_bars(bars);");
    const auto first_return = src.find("return Result<void>();  // Early return, don't process day 1");
    ASSERT_NE(add, std::string::npos);
    ASSERT_NE(first_return, std::string::npos);
    EXPECT_LT(add, first_return) << "the first group must enter the classifier too";
}
