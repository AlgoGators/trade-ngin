// tests/live/test_sleeve_seeding.cpp
//
// T-7a commit 3a (T-BASE, HD 2026-09-18 §28c item 3): every sleeve of a live book is seeded from
// its OWN stored rows of the previous day, into its own strategy object (positions_, the Carver
// buffer anchor) and into its own PortfolioManager slot (current_positions, the optimizer's
// baseline and the book a REFUSE ships).
//
// Before this commit both futures runners seeded strategy_names[0] only. On BASE (two sleeves,
// TREND_FOLLOWING and TREND_FOLLOWING_FAST) the second sleeve was never seeded: its PM slot held
// whatever the bar replay left there (or nothing, once the bus is off), so the optimizer compared a
// `current` summed over one sleeve against a `target` summed over two (T-BASE §6.2, the 6M.v.0
// sale), and a portfolio REFUSE shipped the unseeded sleeve FLAT, which the runner's diff against
// trading.positions turns into a liquidation of that sleeve's held book.
//
// The runners are `main()`s and cannot be linked here, so the seed moved into one header both twins
// call (include/trade_ngin/live/sleeve_seeding.hpp) and is run here against a real
// PortfolioManager; where the runners call it is checked in their source, as
// tests/live/test_live_run_refusal_arms.cpp does.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
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
#include "trade_ngin/live/sleeve_seeding.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/basic_risk_modules.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

using Book = std::unordered_map<std::string, Position>;

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
constexpr auto npos = std::string::npos;

// A sleeve whose target is fixed once it has been fed (as a trend sleeve's is after on_data).
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

Timestamp day(int d) { return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d)); }

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

double qty_in(const Book& book, const std::string& symbol) {
    auto it = book.find(symbol);
    return it == book.end() ? 0.0 : static_cast<double>(it->second.quantity);
}

class SleeveSeedingFixture : public TestBase {
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

    void make_pm(const std::string& id) { pm_ = std::make_unique<PortfolioManager>(small_config(), id); }

    // Registered in the order given; `names_` mirrors the runners' strategy_names and `strategies_`
    // their `strategies` vector (strategies[i] was created from strategy_names[i]).
    void add(const std::string& id, Book target, double allocation) {
        StrategyConfig sc;
        sc.capital_allocation = 1000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto s = std::make_shared<FixedBookStrategy>(id, sc, mock_db_, std::move(target));
        EXPECT_TRUE(s->initialize().is_ok());
        EXPECT_TRUE(s->start().is_ok());
        EXPECT_TRUE(pm_->add_strategy(s, allocation, false).is_ok());
        strategies_.push_back(s);
        names_.push_back(id);
    }

    // The stored rows of the previous day, per sleeve, as load_positions_by_date returns them.
    SleeveBookLoader loader(std::unordered_map<std::string, Book> stored) {
        return [this, stored](const std::string& name) {
            loads_.push_back(name);
            auto it = stored.find(name);
            return Result<Book>(it == stored.end() ? Book{} : it->second);
        };
    }

    std::vector<SleeveSeed> seed(std::unordered_map<std::string, Book> stored) {
        ::testing::internal::CaptureStdout();
        auto out = seed_every_sleeve(strategies_, names_, *pm_, loader(std::move(stored)));
        log_ = ::testing::internal::GetCapturedStdout();
        return out;
    }

    std::shared_ptr<MockPostgresDatabase> mock_db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::vector<std::shared_ptr<StrategyInterface>> strategies_;
    std::vector<std::string> names_;
    std::vector<std::string> loads_;
    std::string log_;
};

}  // namespace

// =============================================================================================
// The helper: every sleeve, its own rows, both places.
// =============================================================================================

TEST_F(SleeveSeedingFixture, TheSecondSleeveIsSeededFromItsOwnRowsIntoItsStrategyAndItsPmSlot) {
    make_pm("PM_C3A_TWO");
    add("TREND_FOLLOWING", {}, 0.7);
    add("TREND_FOLLOWING_FAST", {}, 0.3);
    const Book tf_rows = {{"6M.v.0", make_pos("6M.v.0", 1.0, 0.0575)},
                          {"MES.v.0", make_pos("MES.v.0", 1.0, 7153.5)}};
    const Book fast_rows = {{"6M.v.0", make_pos("6M.v.0", 1.0, 0.0575)},
                            {"6C.v.0", make_pos("6C.v.0", 1.0, 0.73325)},
                            {"MBT.v.0", make_pos("MBT.v.0", 2.0, 77960.0)}};

    const auto seeds = seed({{"TREND_FOLLOWING", tf_rows}, {"TREND_FOLLOWING_FAST", fast_rows}});

    // Each sleeve's stored book was read, under its own strategy_name.
    ASSERT_EQ(loads_.size(), 2u);
    EXPECT_EQ(loads_[0], "TREND_FOLLOWING");
    EXPECT_EQ(loads_[1], "TREND_FOLLOWING_FAST")
        << "only strategy_names[0] was read: the second sleeve's stored rows are ignored";
    ASSERT_EQ(seeds.size(), 2u);
    EXPECT_EQ(seeds[1].rows, 3u);
    EXPECT_EQ(seeds[1].pm_seeded, 3);

    // Fix #1: the FAST strategy object holds FAST's rows (not TF's).
    const auto& fast_held = strategies_[1]->get_positions();
    EXPECT_EQ(fast_held.size(), 3u) << "the FAST strategy object was never seeded";
    EXPECT_DOUBLE_EQ(qty_in(fast_held, "MBT.v.0"), 2.0);
    EXPECT_DOUBLE_EQ(qty_in(fast_held, "6C.v.0"), 1.0);
    EXPECT_DOUBLE_EQ(qty_in(fast_held, "MES.v.0"), 0.0) << "FAST was seeded with TF's rows";
    const auto& tf_held = strategies_[0]->get_positions();
    EXPECT_EQ(tf_held.size(), 2u);
    EXPECT_DOUBLE_EQ(qty_in(tf_held, "MES.v.0"), 1.0);
    EXPECT_DOUBLE_EQ(qty_in(tf_held, "MBT.v.0"), 0.0) << "TF was seeded with FAST's rows";

    // Fix #7: the PM slot of each sleeve holds that sleeve's rows (the optimizer's baseline).
    const auto pm_books = pm_->get_strategy_positions();
    ASSERT_TRUE(pm_books.count("TREND_FOLLOWING_FAST"));
    EXPECT_DOUBLE_EQ(qty_in(pm_books.at("TREND_FOLLOWING_FAST"), "MBT.v.0"), 2.0)
        << "the FAST PM slot is not seeded: the optimizer anchors FAST's held book at zero";
    EXPECT_DOUBLE_EQ(qty_in(pm_books.at("TREND_FOLLOWING_FAST"), "6C.v.0"), 1.0);
    EXPECT_DOUBLE_EQ(qty_in(pm_books.at("TREND_FOLLOWING_FAST"), "6M.v.0"), 1.0);
    EXPECT_DOUBLE_EQ(qty_in(pm_books.at("TREND_FOLLOWING"), "MES.v.0"), 1.0);
    EXPECT_DOUBLE_EQ(qty_in(pm_books.at("TREND_FOLLOWING"), "MBT.v.0"), 0.0);

    // One line per sleeve, the text the one-sleeve seed printed.
    EXPECT_NE(log_.find("Seeded 2 positions into PortfolioManager.current_positions for "
                        "optimizer-baseline correctness (strategy_name=TREND_FOLLOWING)"),
              npos);
    EXPECT_NE(log_.find("Seeded 3 positions into PortfolioManager.current_positions for "
                        "optimizer-baseline correctness (strategy_name=TREND_FOLLOWING_FAST)"),
              npos);
}

TEST_F(SleeveSeedingFixture, ARefuseDayShipsEachSleevesHeldBookNotAFlatSecondSleeve) {
    // A portfolio REFUSE means "ship yesterday's book". Yesterday's book is only in the PM if the
    // runner put it there: with the second sleeve unseeded the REFUSE ships that sleeve FLAT and
    // the runner's per-sleeve diff against trading.positions liquidates what it holds.
    make_pm("PM_C3A_REFUSE");
    add("TREND_FOLLOWING", {{"ZZA", make_pos("ZZA", 5.0, 100.0)}}, 0.7);
    add("TREND_FOLLOWING_FAST", {{"ZZA", make_pos("ZZA", 4.0, 100.0)},
                                 {"ZZB", make_pos("ZZB", 6.0, 50.0)}}, 0.3);
    seed({{"TREND_FOLLOWING", {{"ZZA", make_pos("ZZA", 1.0, 100.0)}}},
          {"TREND_FOLLOWING_FAST", {{"ZZA", make_pos("ZZA", 1.0, 100.0)},
                                    {"ZZB", make_pos("ZZB", 2.0, 50.0)}}}});
    ASSERT_TRUE(pm_->set_risk_modules({std::make_shared<RefuseOnConditionRiskModule>(
                                          "stop", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0},
                                          "book over the stop")})
                    .is_ok());
    ::testing::internal::CaptureStdout();
    const auto result = pm_->process_market_data(
        {make_bar("ZZA", 1, 100.0), make_bar("ZZB", 1, 50.0), make_bar("ZZA", 2, 101.0),
         make_bar("ZZB", 2, 51.0), make_bar("ZZA", 3, 99.0), make_bar("ZZB", 3, 49.0)});
    ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(result.is_ok());

    const auto books = pm_->get_strategy_positions();
    EXPECT_DOUBLE_EQ(qty_in(books.at("TREND_FOLLOWING"), "ZZA"), 1.0);
    EXPECT_DOUBLE_EQ(qty_in(books.at("TREND_FOLLOWING_FAST"), "ZZB"), 2.0)
        << "the REFUSE shipped TREND_FOLLOWING_FAST flat: its held ZZB (2) would be sold";
    EXPECT_DOUBLE_EQ(qty_in(books.at("TREND_FOLLOWING_FAST"), "ZZA"), 1.0);
}

TEST_F(SleeveSeedingFixture, ASleeveWithNoStoredRowsIsLeftAloneAndSaysSo) {
    make_pm("PM_C3A_EMPTY");
    add("TREND_FOLLOWING", {}, 0.7);
    add("TREND_FOLLOWING_FAST", {}, 0.3);
    const auto seeds = seed({{"TREND_FOLLOWING", {{"ZZA", make_pos("ZZA", 1.0, 100.0)}}}});
    ASSERT_EQ(seeds.size(), 2u);
    EXPECT_EQ(seeds[1].rows, 0u);
    EXPECT_EQ(seeds[1].pm_seeded, 0);
    EXPECT_TRUE(strategies_[1]->get_positions().empty());
    EXPECT_NE(log_.find("No yesterday positions to seed for strategy TREND_FOLLOWING_FAST "
                        "(first run or no data)"),
              npos);
    // The first sleeve is still seeded.
    EXPECT_DOUBLE_EQ(qty_in(strategies_[0]->get_positions(), "ZZA"), 1.0);
}

TEST_F(SleeveSeedingFixture, AOneSleeveBookIsSeededExactlyAsTheOneSleeveSeedDid) {
    // CONSERVATIVE: one sleeve. One read, one strategy seed, one PM seed, one line.
    make_pm("PM_C3A_ONE");
    add("TREND_FOLLOWING", {}, 1.0);
    const auto seeds = seed({{"TREND_FOLLOWING", {{"ZZA", make_pos("ZZA", 2.0, 100.0)},
                                                  {"ZZB", make_pos("ZZB", -1.0, 50.0)}}}});
    ASSERT_EQ(loads_.size(), 1u);
    ASSERT_EQ(seeds.size(), 1u);
    EXPECT_EQ(seeds[0].pm_seeded, 2);
    EXPECT_DOUBLE_EQ(qty_in(strategies_[0]->get_positions(), "ZZB"), -1.0);
    EXPECT_DOUBLE_EQ(qty_in(pm_->get_strategy_positions().at("TREND_FOLLOWING"), "ZZA"), 2.0);
    size_t n = 0;
    for (size_t at = log_.find("Seeded "); at != npos; at = log_.find("Seeded ", at + 1)) ++n;
    EXPECT_EQ(n, 2u) << "one 'into strategy' and one 'into PortfolioManager' line, as before";
}

TEST_F(SleeveSeedingFixture, OppositeSleevesKeepTheirOwnSignsTheNetIsNotSeeded) {
    // Recorded behaviour for ledger BASE-opposite-sleeve-anchor (T-7b designs the netting): the
    // stored ZT.v.0 2025-11-08 shape, TREND_FOLLOWING +1 and TREND_FOLLOWING_FAST -1. Each slot keeps
    // its own sign; nothing here nets them.
    make_pm("PM_C3A_OPPOSITE");
    add("TREND_FOLLOWING", {}, 0.7);
    add("TREND_FOLLOWING_FAST", {}, 0.3);
    seed({{"TREND_FOLLOWING", {{"ZT.v.0", make_pos("ZT.v.0", 1.0, 104.0)}}},
          {"TREND_FOLLOWING_FAST", {{"ZT.v.0", make_pos("ZT.v.0", -1.0, 104.0)}}}});
    const auto books = pm_->get_strategy_positions();
    EXPECT_DOUBLE_EQ(qty_in(books.at("TREND_FOLLOWING"), "ZT.v.0"), 1.0);
    EXPECT_DOUBLE_EQ(qty_in(books.at("TREND_FOLLOWING_FAST"), "ZT.v.0"), -1.0);
    EXPECT_DOUBLE_EQ(qty_in(strategies_[1]->get_positions(), "ZT.v.0"), -1.0);
}

// =============================================================================================
// The runners: both twins seed through the helper, with the same block.
// =============================================================================================

TEST(SleeveSeedingSource, BothTwinsSeedEverySleeveThroughTheHelperBeforeTheRebalance) {
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(src.find("strategy_names.empty() ? std::string() : strategy_names[0]"), npos)
            << "the runner still seeds strategy_names[0] only";
        EXPECT_EQ(src.find("tf_strategy->seed_positions("), npos)
            << "the runner seeds the first strategy object only";
        const auto call = src.find("seed_every_sleeve(strategies, strategy_names, *portfolio,");
        const auto process = src.find("portfolio->process_market_data(all_bars);");
        const auto gate = src.find("if (!skip_strategy_processing) {");
        ASSERT_NE(process, npos);
        ASSERT_NE(gate, npos);
        EXPECT_NE(call, npos) << "the runner does not seed through seed_every_sleeve";
        if (call == npos) continue;
        EXPECT_LT(gate, call);
        EXPECT_LT(call, process) << "every sleeve must be seeded BEFORE the day's rebalance";
        const auto loader = src.find("combined_strategy_id, seed_strategy_name,", call);
        EXPECT_NE(loader, npos) << "each sleeve must be read under its own strategy_name";
        EXPECT_NE(src.find("BASE-opposite-sleeve-anchor", gate), npos)
            << "the opposite-sleeve NOTE belongs at the seed site";
    }
}

TEST(SleeveSeedingSource, TheTwinsCarryTheSameSeedBlock) {
    std::vector<std::string> blocks;
    for (const char* runner : kFuturesRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const auto from = src.find("if (!skip_strategy_processing) {");
        const auto to = src.find("portfolio->process_market_data(all_bars);");
        ASSERT_NE(from, npos);
        ASSERT_NE(to, npos);
        ASSERT_LT(from, to);
        const auto seed_from = src.find("            {\n                auto seed_previous_date", from);
        const auto seed_to = src.find("// Process data through portfolio pipeline", from);
        ASSERT_NE(seed_from, npos);
        ASSERT_NE(seed_to, npos);
        blocks.push_back(src.substr(from, seed_to - from));
    }
    EXPECT_EQ(blocks[0], blocks[1])
        << "live_portfolio_conservative.cpp and live_portfolio.cpp are twins: the seed block must "
           "be byte-identical in both";
}
