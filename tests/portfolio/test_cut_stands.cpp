// T-7b-3 D-1, D-1b and rulings 7 and 8 (HD 2026-09-27): the cut a lap delivers is the stored book,
// and the cut rule skips a symbol the BOOK_GATE will hold.
//
// D-1. Before, the lap right after a lap that delivered a cut (T-7b-2 9e, cut_delivery.hpp)
// re-optimised the cut book from the HELD anchor, so the optimizer's deadband, measured against
// the held book, could put the cut contract back (btfut 2025-04-08: ZN cut to 1, stored 2). Ruling
// 8: every cut lap is delivered once and the loop ends there (RISK_CUT_ONCE), so no lap re-reads the
// cut book: neither the deadband nor a re-read gate asking for the residual (btfut 2025-09-30: 0.983,
// 0.993, 0.992, 0.999, one more contract each) can move it.
//
// D-1b. A symbol the BOOK_GATE holds at its held quantity after the loop (futures backtest: its
// signal-group bar is not a SESSION; live: its T-1 verdict is not SESSION) used to be a removal
// candidate like any other; the cut removed its held contract and the hold then put it back (btfut
// 2026-01-12: 6L, delivered 0.8856 against 0.8699). Now it is fixed at its held quantity and never
// cut, and RISK_CUT_BOOK_GATE names it.

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <chrono>
#include <cstdint>
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
#include "trade_ngin/live/run_metadata_marks.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/basic_risk_modules.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

using StandsBook = std::unordered_map<std::string, Position>;

// Returns the same target book on every call after its first on_data.
class StandsFixedBookStrategy : public BaseStrategy {
public:
    StandsFixedBookStrategy(std::string id, StrategyConfig config,
                            std::shared_ptr<DatabaseInterface> db, StandsBook book)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          book_(std::move(book)) {
        metadata_.name = "Cut Stands Fixed Book Strategy";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        (void)data;
        ++calls_;
        return Result<void>();
    }
    StandsBook get_target_positions() const override { return calls_ == 0 ? StandsBook{} : book_; }

private:
    StandsBook book_;
    size_t calls_{0};
};

Timestamp stands_day(int d) {
    return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d));
}

// A deterministic random walk per seed (test_optimizer_allocation_once.cpp's walk).
std::vector<double> stands_closes(const std::string& seed, int days) {
    uint64_t state = 1469598103934665603ULL;
    for (char c : seed) state = (state ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
    std::vector<double> closes;
    double price = 100.0;
    for (int d = 0; d < days; ++d) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const double u = static_cast<double>(state >> 11) / 9007199254740992.0;
        price *= 1.0 + 0.04 * (u - 0.5);
        closes.push_back(price);
    }
    return closes;
}

constexpr int kStandsDays = 40;

// Days [from, to) of each symbol's walk, one bar per symbol per day.
std::vector<Bar> stands_bars(const std::vector<std::string>& symbols, int from, int to) {
    std::vector<Bar> bars;
    for (int d = from; d < to; ++d) {
        for (const auto& s : symbols) {
            const auto closes = stands_closes(s, to);
            Bar b;
            b.symbol = s;
            b.timestamp = stands_day(d);
            b.open = b.high = b.low = b.close = Decimal(closes[d]);
            b.volume = 100000.0;
            bars.push_back(b);
        }
    }
    return bars;
}

Position stands_pos(const std::string& symbol, double qty) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(100.0);
    p.last_update = stands_day(0);
    return p;
}

StandsBook stands_book(const std::vector<std::pair<std::string, double>>& rows) {
    StandsBook b;
    for (const auto& [symbol, qty] : rows) b[symbol] = stands_pos(symbol, qty);
    return b;
}

// Whole contracts, the optimizer on, every symbol at the PM's default 0.01 weight per contract
// (10,000 of notional on 1,000,000). `deadband`: the optimizer's buffering on (0.05 x tau).
PortfolioConfig stands_config(bool deadband) {
    PortfolioConfig pc{1'000'000.0, 1.0, 0.0, /*use_optimization=*/true};
    pc.allow_fractional_positions = false;
    pc.opt_config.tau = 1.0;
    pc.opt_config.capital = 1'000'000.0;
    pc.opt_config.cost_penalty_scalar = 50.0;
    pc.opt_config.max_iterations = 100;
    pc.opt_config.convergence_threshold = 1e-6;
    pc.opt_config.use_buffering = deadband;
    pc.opt_config.buffer_size_factor = 0.05;
    pc.risk_config.capital = 1'000'000.0;
    pc.risk_modules = {test_none_module()};
    return pc;
}

std::vector<std::string> stands_lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

// The loop's lines of one captured call (the laps, the optimizer's answers, the gate, the cut).
std::string loop_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) {
        for (const char* key : {"Iteration ", "Symbol ", "RISK_APPLIED", "RISK_CUT_", "Converged",
                                "Optimization metrics"}) {
            if (l.find(key) != std::string::npos) {
                out += "    " + l + "\n";
                break;
            }
        }
    }
    return out;
}

size_t count_with(const std::vector<std::string>& lines, const std::string& key) {
    size_t n = 0;
    for (const auto& l : lines)
        if (l.find(key) != std::string::npos) ++n;
    return n;
}

std::filesystem::path stands_find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string stands_read_source(const std::string& relative) {
    auto path = stands_find_repo_file(relative);
    if (path.empty()) return {};
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

class CutStands : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://cutstands");
        ASSERT_TRUE(db_->connect().is_ok());
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::INFO;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
    }
    void TearDown() override {
        pm_.reset();
        strategy_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    void make_pm(bool deadband, StandsBook target) {
        static int n = 0;
        pm_ = std::make_unique<PortfolioManager>(stands_config(deadband),
                                                 "PM_STANDS_" + std::to_string(++n));
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        strategy_ = std::make_shared<StandsFixedBookStrategy>("STANDS_S", sc, db_, std::move(target));
        ASSERT_TRUE(strategy_->initialize().is_ok());
        ASSERT_TRUE(strategy_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(strategy_, 1.0, /*use_optimization=*/true).is_ok());
    }

    void cut_by(double scale, bool every_lap = false) {
        ASSERT_TRUE(pm_->set_risk_modules(
                           {std::make_shared<ConstantScaleRiskModule>("cut", scale, every_lap)})
                        .is_ok());
    }

    // One process_market_data call with stdout captured; returns its lines.
    std::vector<std::string> run(const std::vector<Bar>& bars, int as_of,
                                 const std::unordered_set<std::string>* sessions = nullptr) {
        ::testing::internal::CaptureStdout();
        const auto r = pm_->process_market_data(bars, false, stands_day(as_of), sessions);
        const std::string out = ::testing::internal::GetCapturedStdout();
        EXPECT_TRUE(r.is_ok());
        return stands_lines(out);
    }

    double stored(const std::string& symbol) const {
        const auto all = pm_->get_strategy_positions();
        auto s = all.find("STANDS_S");
        if (s == all.end()) return 0.0;
        auto p = s->second.find(symbol);
        return p == s->second.end() ? 0.0 : static_cast<double>(p->second.quantity);
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::shared_ptr<StandsFixedBookStrategy> strategy_;
};

// ---- D-1 and ruling 8: the cut book is the stored book ----------------------------------------

// The deadband itself, on the optimizer alone: from a held 2 contracts (weight 0.02) toward a
// target of 1 (0.01), the greedy answers 1, but one contract's tracking error (0.01 x sigma) is
// inside the buffer (0.05 x tau), so buffering answers the held 2. Without buffering it answers 1.
// This is what the lap after a cut did at f2932054: it re-optimised the cut book (1) anchored on
// the held book (2) and the deadband put the cut contract back.
TEST(CutStandsDeadband, OneContractBelowTheHeldBookIsInsideTheDeadband) {
    DynamicOptConfig c;
    c.tau = 1.0;
    c.capital = 1'000'000.0;
    c.cost_penalty_scalar = 50.0;
    c.max_iterations = 100;
    c.convergence_threshold = 1e-6;
    c.buffer_size_factor = 0.05;
    const std::vector<std::vector<double>> cov{{0.04}};  // sigma 0.2 a year
    c.use_buffering = true;
    auto with = DynamicOptimizer(c).optimize({0.02}, {0.01}, {0.0}, {0.01}, cov);
    ASSERT_TRUE(with.is_ok());
    EXPECT_NEAR(with.value().positions[0], 0.02, 1e-12) << "the deadband keeps the held 2";
    c.use_buffering = false;
    auto without = DynamicOptimizer(c).optimize({0.02}, {0.01}, {0.0}, {0.01}, cov);
    ASSERT_TRUE(without.is_ok());
    EXPECT_NEAR(without.value().positions[0], 0.01, 1e-12) << "the greedy alone answers 1";
}

// The brief's RED test. Held 2 contracts of ZZA, the strategy asks 2, the gate SCALEs by 0.5 on
// lap 1 only. Lap 1: the optimizer answers 2 (the held book, inside the deadband), the gate asks
// 0.5, the cut delivers 1 (RISK_CUT_BOOK ... ZZA held=2 lap=2 cut=1). At f2932054 lap 2
// re-optimised that 1 from the held 2, the deadband answered 2, the gate answered NONE and the
// stored book read 2. Now the loop ends on lap 1 and the stored book reads 1.
TEST_F(CutStands, TheCutBookStandsThroughTheNextLapInsideTheHeldBooksDeadband) {
    make_pm(/*deadband=*/true, stands_book({{"ZZA", 2.0}}));
    cut_by(0.5);
    ASSERT_TRUE(pm_->update_strategy_position("STANDS_S", "ZZA", stands_pos("ZZA", 2.0)).is_ok());

    const auto lines = run(stands_bars({"ZZA"}, 0, kStandsDays), kStandsDays);
    const std::string trace = "the loop's lines:\n" + loop_lines(lines);

    ASSERT_EQ(count_with(lines, "RISK_CUT_BOOK lap=1 "), 1u) << trace;
    EXPECT_EQ(count_with(lines, " ZZA held=2 lap=2 cut=1"), 1u)
        << "lap 1's cut delivers 1 of the held 2\n" << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_ONCE lap=1: the cut is delivered once; the loop ends"), 1u)
        << trace;
    EXPECT_EQ(count_with(lines, "Iteration 2 "), 0u) << "no lap after the cut (ruling 8)\n" << trace;
    EXPECT_EQ(stored("ZZA"), 1.0)
        << "the stored book must be the cut book, not the held book the deadband restores\n"
        << trace;
}

// Ruling 8, replacing (a)'s RISK_CUT_STANDS test (that lap no longer exists). No hold, held 4 of
// ZZA, the strategy asks 4 and the gate SCALEs by 0.95 on EVERY lap. Lap 1 cuts 40,000 to 38,000:
// one contract, 3 (30,000). At ef7feaaf lap 2 re-read that book, the gate asked 0.95 again and the
// whole-contract cut took one more contract each lap, 3, 2, 1, 0, until forced rounding (the btfut
// 2025-09-30 shape). Now the cut is delivered once and the loop ends: the stored book is 3.
TEST_F(CutStands, ANoHoldCutIsDeliveredOnceWhenTheGateWouldCutAgain) {
    make_pm(/*deadband=*/true, stands_book({{"ZZA", 4.0}}));
    cut_by(0.95, /*every_lap=*/true);
    ASSERT_TRUE(pm_->update_strategy_position("STANDS_S", "ZZA", stands_pos("ZZA", 4.0)).is_ok());

    const auto lines = run(stands_bars({"ZZA"}, 0, kStandsDays), kStandsDays);
    const std::string trace = "the loop's lines:\n" + loop_lines(lines);

    EXPECT_EQ(count_with(lines, "RISK_APPLIED lap="), 1u) << "one gate lap\n" << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_BOOK lap="), 1u) << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_ONCE lap=1: the cut is delivered once; the loop ends"), 1u)
        << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_STANDS"), 0u) << trace;
    EXPECT_EQ(count_with(lines, "Optimization metrics"), 1u) << "lap 1 only\n" << trace;
    EXPECT_EQ(count_with(lines, "Max iterations reached"), 0u) << trace;
    EXPECT_EQ(count_with(lines, "RISK_OVER_LIMIT_BY_HOLD"), 0u) << "no hold, no mark\n" << trace;
    EXPECT_FALSE(pm_->last_over_limit_by_hold().over_limit_by_hold);
    EXPECT_EQ(stored("ZZA"), 3.0) << "one contract cut, not one per lap\n" << trace;
}

// ---- D-1b through process_market_data's session set (the futures backtest) -------------------

// Call 1 (the none gate) fills ZZA 1 and ZZB 2. Call 2: ZZB's signal-group bar is not a SESSION,
// the gate SCALEs by 0.75 (30,000 -> 22,500 of notional, one contract to cut, both held). The
// removal nearest the target in tracking error is ZZB's. At f2932054 the cut removed ZZB, the
// BOOK_GATE then held ZZB at its filled 2 and the stored book read ZZA 1, ZZB 2 (delivered 1.0
// against 0.75). Now ZZB is fixed at 2 and ZZA is cut: ZZA 0, ZZB 2. The deadband is off so the
// next lap cannot hide either rule.
TEST_F(CutStands, ANonSessionSymbolsHeldContractIsNotCutInTheBacktest) {
    make_pm(/*deadband=*/false, stands_book({{"ZZA", 1.0}, {"ZZB", 2.0}}));
    cut_by(1.0);
    const std::unordered_set<std::string> both{"ZZA", "ZZB"};
    run(stands_bars({"ZZA", "ZZB"}, 0, kStandsDays), kStandsDays, &both);
    ASSERT_EQ(stored("ZZA"), 1.0);
    ASSERT_EQ(stored("ZZB"), 2.0);

    cut_by(0.75);
    const std::unordered_set<std::string> zza_only{"ZZA"};
    const auto lines =
        run(stands_bars({"ZZA", "ZZB"}, kStandsDays, kStandsDays + 1), kStandsDays + 1, &zza_only);
    const std::string trace = "the loop's lines:\n" + loop_lines(lines);

    EXPECT_EQ(stored("ZZB"), 2.0) << trace;
    EXPECT_EQ(stored("ZZA"), 0.0) << "the cut must come from the symbol that can trade\n" << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_BOOK_GATE lap=1 held_by_book_gate=1: ZZB held=2 lap=2"),
              1u)
        << trace;
    EXPECT_EQ(count_with(lines, "BOOK_GATE backtest ZZB"), 0u)
        << "ZZB's target is its filled quantity, so the hold has nothing to hold\n" << trace;
}

// A warm-up cycle generates no executions and the BOOK_GATE does not run on it, so nothing is
// held: the same call as above with skip_execution_generation cuts ZZB as the rule ranks it.
TEST_F(CutStands, AWarmUpCycleHoldsNothing) {
    make_pm(/*deadband=*/false, stands_book({{"ZZA", 1.0}, {"ZZB", 2.0}}));
    cut_by(1.0);
    const std::unordered_set<std::string> both{"ZZA", "ZZB"};
    run(stands_bars({"ZZA", "ZZB"}, 0, kStandsDays), kStandsDays, &both);

    cut_by(0.75);
    const std::unordered_set<std::string> zza_only{"ZZA"};
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(stands_bars({"ZZA", "ZZB"}, kStandsDays, kStandsDays + 1),
                                         /*skip_execution_generation=*/true,
                                         stands_day(kStandsDays + 1), &zza_only)
                    .is_ok());
    const auto lines = stands_lines(::testing::internal::GetCapturedStdout());
    const std::string trace = "the loop's lines:\n" + loop_lines(lines);
    EXPECT_EQ(count_with(lines, "RISK_CUT_BOOK_GATE"), 0u) << trace;
    EXPECT_EQ(stored("ZZA"), 1.0) << trace;
    EXPECT_EQ(stored("ZZB"), 1.0) << trace;
}

// ---- D-1b through set_book_gate_holds (the live futures runners) -----------------------------

// The same book with no session set (a live caller): set_book_gate_holds({ZZB}) fixes ZZB at its
// held 2 and the cut takes ZZA; the next call, without the setter, holds nothing (the set was
// that call's only).
TEST_F(CutStands, TheLiveSetterHoldsForTheNextCallOnly) {
    make_pm(/*deadband=*/false, stands_book({{"ZZA", 1.0}, {"ZZB", 2.0}}));
    cut_by(1.0);
    run(stands_bars({"ZZA", "ZZB"}, 0, kStandsDays), kStandsDays);

    cut_by(0.75);
    pm_->set_book_gate_holds({"ZZB"});
    auto lines = run(stands_bars({"ZZA", "ZZB"}, kStandsDays, kStandsDays + 1), kStandsDays + 1);
    std::string trace = "the loop's lines:\n" + loop_lines(lines);
    EXPECT_EQ(stored("ZZA"), 0.0) << trace;
    EXPECT_EQ(stored("ZZB"), 2.0) << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_BOOK_GATE lap=1 held_by_book_gate=1: ZZB held=2 lap=2"),
              1u)
        << trace;

    lines = run(stands_bars({"ZZA", "ZZB"}, kStandsDays + 1, kStandsDays + 2), kStandsDays + 2);
    trace = "the loop's lines:\n" + loop_lines(lines);
    EXPECT_EQ(count_with(lines, "RISK_CUT_BOOK lap=1 "), 1u) << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_BOOK_GATE"), 0u) << "the setter's set was used up\n"
                                                          << trace;
}

// Both futures runners hand the PM every symbol whose T-1 verdict is not SESSION right before the
// rebalance, with the same block; the equity runner does not.
TEST(CutStandsRunnerSource, BothFuturesRunnersPassTheNonSessionSymbolsBeforeTheRebalance) {
    const std::string block =
        "                std::unordered_set<std::string> book_gate_holds;\n"
        "                for (const auto& symbol : symbols) {\n"
        "                    if (!t1_classification.is_session(symbol)) "
        "book_gate_holds.insert(symbol);\n"
        "                }\n"
        "                portfolio->set_book_gate_holds(std::move(book_gate_holds));\n"
        "            }\n";
    // T-7b-3 (c) R-3 puts the rebalance behind the sizing hold (sizing_hold ? Result<void>() :
    // portfolio->process_market_data(...)); the holds are still handed over right before it: the
    // first process_market_data call after the block is the rebalance, with no other call between.
    const std::string rebalance = "portfolio->process_market_data(strategy_feed_bars)";
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        SCOPED_TRACE(runner);
        const std::string src = stands_read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const size_t at = src.find(block);
        ASSERT_NE(at, std::string::npos)
            << "the runner must set the BOOK_GATE holds right before process_market_data";
        const size_t next = src.find(rebalance, at + block.size());
        ASSERT_NE(next, std::string::npos) << "no rebalance after the holds are set";
        const std::string between = src.substr(at + block.size(), next - (at + block.size()));
        EXPECT_EQ(between.find(';'), std::string::npos)
            << "a statement sits between the holds and the rebalance: " << between;
    }
    const std::string eq = stands_read_source("apps/strategies/live_equity_mean_reversion.cpp");
    if (!eq.empty()) {
        EXPECT_EQ(eq.find("set_book_gate_holds"), std::string::npos);
    }
}

// ---- T-7b-3 ruling 7 (HD 2026-09-27): a cut with BOOK_GATE holds is delivered once -------------
//
// btfut 2025-09-29 on 740af282: the gate cut on five laps. Each lap re-read the cut book, the held
// symbols it may not cut kept the reading high, and each further cut fell on the rest. Now a cut lap
// whose rule fixed a held symbol is the gate's level applied once to the cuttable symbols and the
// loop ends there (RISK_CUT_ONCE). A book the held contracts alone keep above the level is stored,
// warned (RISK_OVER_LIMIT_BY_HOLD) and recorded for the live runners' metadata mark.

// Call 1 fills ZZA 1, ZZB 2. Call 2: ZZB is not a SESSION and the gate SCALEs by 0.9 on EVERY lap.
// Lap 1: 30,000 -> 27,000, ZZB fixed at 2, ZZA cut: 20,000 (met). At 740af282 laps 2 to 5 re-read
// the same book (0, 2), cut again to 18,000, 16,200, ... with nothing left to cut, and the loop ran
// out of laps. Now the loop ends on lap 1 with the same book.
TEST_F(CutStands, ACutWithBookGateHoldsIsDeliveredOnceAndEndsTheLoop) {
    make_pm(/*deadband=*/false, stands_book({{"ZZA", 1.0}, {"ZZB", 2.0}}));
    cut_by(1.0);
    const std::unordered_set<std::string> both{"ZZA", "ZZB"};
    run(stands_bars({"ZZA", "ZZB"}, 0, kStandsDays), kStandsDays, &both);

    cut_by(0.9, /*every_lap=*/true);
    const std::unordered_set<std::string> zza_only{"ZZA"};
    const auto lines =
        run(stands_bars({"ZZA", "ZZB"}, kStandsDays, kStandsDays + 1), kStandsDays + 1, &zza_only);
    const std::string trace = "the loop's lines:\n" + loop_lines(lines);

    EXPECT_EQ(count_with(lines, "RISK_APPLIED lap="), 1u) << "one gate lap\n" << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_BOOK lap="), 1u) << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_BOOK_GATE lap=1 held_by_book_gate=1: ZZB held=2 lap=2"), 1u)
        << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_ONCE lap=1: the cut is delivered once; the loop ends"), 1u)
        << trace;
    EXPECT_EQ(count_with(lines, "Iteration 2 "), 0u) << trace;
    EXPECT_EQ(count_with(lines, "Max iterations reached"), 0u) << trace;
    EXPECT_EQ(count_with(lines, "RISK_OVER_LIMIT_BY_HOLD"), 0u) << "the level was met\n" << trace;
    EXPECT_EQ(stored("ZZA"), 0.0) << trace;
    EXPECT_EQ(stored("ZZB"), 2.0) << trace;
}

// Ruling 8, replacing ruling 7's "a cut without holds keeps the SKIP loop": a cut lap with no hold
// also ends the loop. At ef7feaaf lap 2 re-read the cut book (RISK_CUT_STANDS) and the gate answered
// NONE: two gate laps.
TEST_F(CutStands, ACutWithoutHoldsAlsoEndsTheLoop) {
    make_pm(/*deadband=*/false, stands_book({{"ZZA", 1.0}, {"ZZB", 2.0}}));
    cut_by(1.0);
    const std::unordered_set<std::string> both{"ZZA", "ZZB"};
    run(stands_bars({"ZZA", "ZZB"}, 0, kStandsDays), kStandsDays, &both);

    cut_by(0.75);
    const auto lines =
        run(stands_bars({"ZZA", "ZZB"}, kStandsDays, kStandsDays + 1), kStandsDays + 1, &both);
    const std::string trace = "the loop's lines:\n" + loop_lines(lines);
    EXPECT_EQ(count_with(lines, "RISK_CUT_ONCE lap=1: the cut is delivered once; the loop ends"), 1u)
        << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_STANDS"), 0u) << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_BOOK_GATE"), 0u) << trace;
    EXPECT_EQ(count_with(lines, "RISK_APPLIED lap="), 1u) << trace;
    EXPECT_EQ(count_with(lines, "Final positions fully integer after 1 iterations."), 1u) << trace;
}

// Three held contracts, ZZB and ZZC not SESSIONs. The gate asks 0.5: 30,000 -> 15,000. The held ZZB
// and ZZC alone are 20,000, ZZA is cut and the level is still unmet: the book is stored at 20,000
// with one WARN naming the held symbols in order.
TEST_F(CutStands, AnUnmetLevelIsWarnedOverTheLimitByHold) {
    make_pm(/*deadband=*/false, stands_book({{"ZZA", 1.0}, {"ZZB", 1.0}, {"ZZC", 1.0}}));
    cut_by(1.0);
    const std::unordered_set<std::string> all{"ZZA", "ZZB", "ZZC"};
    run(stands_bars({"ZZA", "ZZB", "ZZC"}, 0, kStandsDays), kStandsDays, &all);

    cut_by(0.5);
    const std::unordered_set<std::string> zza_only{"ZZA"};
    const auto lines = run(stands_bars({"ZZA", "ZZB", "ZZC"}, kStandsDays, kStandsDays + 1),
                           kStandsDays + 1, &zza_only);
    const std::string trace = "the loop's lines:\n" + loop_lines(lines);
    EXPECT_EQ(count_with(lines, "[WARNING] [RiskManager] RISK_OVER_LIMIT_BY_HOLD lap=1 target=15000.00 "
                                "cut_book=20000.00 held=ZZB,ZZC: the cuttable symbols are exhausted "
                                "and the BOOK_GATE holds keep the book above the gate's level; the "
                                "book is stored over the limit"),
              1u)
        << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_ONCE lap=1"), 1u) << trace;
    EXPECT_EQ(count_with(lines, "RISK_APPLIED lap="), 1u) << trace;
    EXPECT_EQ(stored("ZZA"), 0.0) << trace;
    EXPECT_EQ(stored("ZZB"), 1.0) << trace;
    EXPECT_EQ(stored("ZZC"), 1.0) << trace;
}

// The source pin: both futures runners write the over_limit_by_hold mark with the same block, after
// the PM call and before its error check, through mark_over_limit_by_hold.
TEST(CutStandsRunnerSource, BothFuturesRunnersMarkTheRowOverTheLimitByHoldTheSameWay) {
    std::vector<std::string> blocks;
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        SCOPED_TRACE(runner);
        const std::string src = stands_read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const auto process = src.find("portfolio->process_market_data(strategy_feed_bars);");
        const auto from = src.find("            // T-7b-3 ruling 7 (HD 2026-09-27)", process);
        const auto check = src.find("if (port_process_result.is_error())", process);
        ASSERT_NE(process, std::string::npos);
        ASSERT_NE(check, std::string::npos);
        ASSERT_NE(from, std::string::npos) << "the runner does not mark the row over the limit by hold";
        ASSERT_LT(from, check);
        const std::string block = src.substr(from, check - from);
        EXPECT_NE(block.find("portfolio->last_over_limit_by_hold()"), std::string::npos);
        EXPECT_NE(block.find("portfolio_config_json = mark_over_limit_by_hold("), std::string::npos);
        EXPECT_NE(block.find("db->store_live_run_metadata("), std::string::npos);
        blocks.push_back(block);
    }
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0], blocks[1]) << "the twins differ";
}

// ---- ruling 7: the record and the mark (new API) ---------------------------------------------

// The record on the PM: set on the unmet day with the sorted held symbols, the level and the cut
// book; not set on a met day; reset by the next call.
TEST_F(CutStands, TheOverLimitByHoldRecordIsSetOnlyWhenTheLevelIsUnmet) {
    make_pm(/*deadband=*/false, stands_book({{"ZZA", 1.0}, {"ZZB", 1.0}, {"ZZC", 1.0}}));
    cut_by(1.0);
    const std::unordered_set<std::string> all{"ZZA", "ZZB", "ZZC"};
    run(stands_bars({"ZZA", "ZZB", "ZZC"}, 0, kStandsDays), kStandsDays, &all);
    EXPECT_FALSE(pm_->last_over_limit_by_hold().over_limit_by_hold);

    cut_by(0.5);
    const std::unordered_set<std::string> zza_only{"ZZA"};
    run(stands_bars({"ZZA", "ZZB", "ZZC"}, kStandsDays, kStandsDays + 1), kStandsDays + 1,
        &zza_only);
    const OverLimitByHold r = pm_->last_over_limit_by_hold();
    EXPECT_TRUE(r.over_limit_by_hold);
    EXPECT_EQ(r.symbols, (std::vector<std::string>{"ZZB", "ZZC"}));
    EXPECT_DOUBLE_EQ(r.target, 15000.0);
    EXPECT_DOUBLE_EQ(r.cut_book, 20000.0);
    EXPECT_EQ(r.lap, 1);

    cut_by(1.0);
    run(stands_bars({"ZZA", "ZZB", "ZZC"}, kStandsDays + 1, kStandsDays + 2), kStandsDays + 2, &all);
    EXPECT_FALSE(pm_->last_over_limit_by_hold().over_limit_by_hold) << "reset by the next call";
}

// A met level on a cut with holds: the loop ends, nothing is warned, the record stays clear.
TEST_F(CutStands, AMetLevelWithHoldsIsNotOverTheLimit) {
    make_pm(/*deadband=*/false, stands_book({{"ZZA", 1.0}, {"ZZB", 1.0}, {"ZZC", 1.0}}));
    cut_by(1.0);
    const std::unordered_set<std::string> all{"ZZA", "ZZB", "ZZC"};
    run(stands_bars({"ZZA", "ZZB", "ZZC"}, 0, kStandsDays), kStandsDays, &all);

    cut_by(0.8);  // 30,000 -> 24,000: ZZA cut, 20,000 is within the level
    const std::unordered_set<std::string> zza_only{"ZZA"};
    const auto lines = run(stands_bars({"ZZA", "ZZB", "ZZC"}, kStandsDays, kStandsDays + 1),
                           kStandsDays + 1, &zza_only);
    const std::string trace = "the loop's lines:\n" + loop_lines(lines);
    EXPECT_EQ(count_with(lines, "RISK_OVER_LIMIT_BY_HOLD"), 0u) << trace;
    EXPECT_EQ(count_with(lines, "RISK_CUT_ONCE lap=1"), 1u) << trace;
    EXPECT_FALSE(pm_->last_over_limit_by_hold().over_limit_by_hold);
    EXPECT_EQ(stored("ZZA"), 0.0) << trace;
}

// The mark: every other key of the first upsert kept, the held symbols, the level and the cut book.
TEST(CutStandsMark, TheOverLimitByHoldMarkKeepsTheRowAndNamesTheHolds) {
    nlohmann::json row{{"use_optimization", true}, {"risk_refusal", {{"module", "carver"}}}};
    const auto marked = mark_over_limit_by_hold(row, {"6L.v.0", "MYM.v.0"}, 401355.47, 405524.75, 1);
    EXPECT_EQ(marked.at("use_optimization"), true);
    EXPECT_EQ(marked.at("risk_refusal").at("module"), "carver");
    const auto& m = marked.at("over_limit_by_hold");
    EXPECT_EQ(m.at("symbols"), (std::vector<std::string>{"6L.v.0", "MYM.v.0"}));
    EXPECT_DOUBLE_EQ(m.at("target").get<double>(), 401355.47);
    EXPECT_DOUBLE_EQ(m.at("cut_book").get<double>(), 405524.75);
    EXPECT_EQ(m.at("lap").get<int>(), 1);
    EXPECT_FALSE(m.at("reason").get<std::string>().empty());
}
