// The risk loop's log, pinned line for line.
//
// The risk-module refactor (T-6a commits 4-6: RiskModule, CarverRiskModule, the
// PM applying decisions by action) must leave every log line the loop prints
// exactly as it was, including the [Component] tag in front of it. The tag is
// thread-local state that Logger::register_component overwrites (logger.hpp), so
// one extra register call anywhere in the loop re-tags every later line, and the
// gate's log comparison sees the whole rest of the run as changed.
//
// This test was written and pinned on the commit-3 tree (9d65e3fd), before any
// refactor line existed. It drives one PortfolioManager with the Carver risk
// manager through the three shapes a gate lap takes -- an empty book, a book the
// risk manager cannot measure, and a book that breaches the leverage cap and runs
// all five laps -- and compares the complete captured stdout with that pin.
// Any later commit that changes it must declare the change.

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
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

// Returns script[k] from the k-th get_target_positions() call after the k-th on_data.
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

    std::unordered_map<std::string, Position> get_target_positions() const override {
        if (calls_ == 0 || calls_ > script_.size()) return {};
        return script_[calls_ - 1];
    }

private:
    std::vector<Book> script_;
    size_t calls_{0};
};

Timestamp day(int d) {
    return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d));  // 2026-01-01 + d
}

Bar bar(const std::string& symbol, int d, double close) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = day(d);
    b.open = b.high = b.low = b.close = Decimal(close);
    b.volume = 1000.0;
    return b;
}

Position pos(const std::string& symbol, double qty, double price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(price);
    p.last_update = day(0);
    return p;
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

}  // namespace

class RiskLoopLogGolden : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }
    void TearDown() override {
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }
    std::shared_ptr<MockPostgresDatabase> db_;
};

TEST_F(RiskLoopLogGolden, CarverLapsMatchTheCommit3Sequence) {
    // Call 1: an empty book. Call 2: one bar at the same instant as call 1's, so the
    // window has no return and the risk manager cannot measure. Call 3: three dated
    // bars and a book at 0.5x gross against a 0.35x cap: scale 0.7, 5 -> 3.5
    // contracts, fractional on every lap, forced to 4 after the fifth.
    StrategyConfig sc;
    sc.capital_allocation = 1000.0;
    sc.max_leverage = 10.0;
    sc.asset_classes = {AssetClass::FUTURES};
    sc.frequencies = {DataFrequency::DAILY};
    auto strategy = std::make_shared<ScriptedStrategy>(
        "GOLDEN_S", sc, db_,
        std::vector<Book>{{}, {{"ZZA", pos("ZZA", 3.0, 100.0)}}, {{"ZZA", pos("ZZA", 5.0, 100.0)}}});
    ASSERT_TRUE(strategy->initialize().is_ok());
    ASSERT_TRUE(strategy->start().is_ok());

    PortfolioConfig pc{1000.0, 0.0, 1.0, 0.0, /*optimization=*/false};
    pc.allow_fractional_positions = false;
    pc.risk_config.capital = 1000.0;
    pc.risk_config.var_limit = 1e6;
    pc.risk_config.jump_risk_limit = 1e6;
    pc.risk_config.max_correlation = 1.0;
    pc.risk_config.max_gross_leverage = 0.35;
    pc.risk_config.max_net_leverage = 0.35;
    pc.risk_modules = {test_carver_module(pc.risk_config)};

    LoggerConfig lc;
    lc.destination = LogDestination::CONSOLE;
    lc.min_level = LogLevel::INFO;
    lc.include_timestamp = false;
    Logger::instance().initialize(lc);

    ::testing::internal::CaptureStdout();
    {
        PortfolioManager pm(pc, "PM_GOLDEN");
        ASSERT_TRUE(pm.add_strategy(strategy, 1.0, false).is_ok());
        ASSERT_TRUE(pm.process_market_data({bar("ZZA", 0, 100.0)}).is_ok());
        ASSERT_TRUE(pm.process_market_data({bar("ZZA", 0, 100.0)}).is_ok());
        ASSERT_TRUE(pm.process_market_data(
                          {bar("ZZA", 1, 102.0), bar("ZZA", 2, 99.0), bar("ZZA", 3, 101.0)})
                        .is_ok());
    }
    std::vector<std::string> got = lines_of(::testing::internal::GetCapturedStdout());
    // The registry's symbol list depends on what else ran in this process; the line itself is pinned.
    for (auto& l : got) {
        const auto at = l.find("Available symbols: ");
        if (at != std::string::npos) l = l.substr(0, at) + "Available symbols: <registry>";
    }

    // Pinned on 9d65e3fd (C3b) from this test's own capture.
    const std::vector<std::string> expected = {
        "[INFO] [RiskManager] Risk manager initialized successfully with capital=1000",
        "[INFO] [PortfolioManager] Added subscription for PORTFOLIO_MANAGER with 2 event types and 0 symbols",
        "[INFO] [PortfolioManager] Added strategy GOLDEN_S with allocation 1.000000",
        "[INFO] [Scripted Strategy] Total historical data: 0 returns across 0 symbols",
        "[INFO] [Scripted Strategy] Iteration 1 of dynamic optimization + risk loop",
        "[INFO] [RiskManager] Using risk manager",
        "[INFO] [RiskManager] No positions to apply risk management to",
        "[INFO] [RiskManager] Portfolio risk management applied successfully in iteration 1",
        "[INFO] [RiskManager] No partial contracts after iteration 1. Converged!",
        "[INFO] [RiskManager] Final positions fully integer after 1 iterations.",
        "[INFO] [RiskManager] Generating executions for strategy GOLDEN_S, target_positions size: 0, existing executions: 0",
        "[INFO] [RiskManager] Filled-position ledger for strategy GOLDEN_S size: 0",
        "[INFO] [RiskManager] Total executions generated for strategy GOLDEN_S: 0",
        "[INFO] [Scripted Strategy] Total historical data: 0 returns across 0 symbols",
        "[INFO] [Scripted Strategy] Iteration 1 of dynamic optimization + risk loop",
        "[INFO] [RiskManager] Using risk manager",
        "[WARNING] [RiskManager] RiskManager: Market data not available, returning default result",
        "[INFO] [RiskManager] Risk management result: risk_exceeded=0, scale=1.000000, portfolio_mult=1.000000, jump_mult=1.000000, correlation_mult=1.000000, leverage_mult=1.000000",
        "[INFO] [RiskManager] Risk limits not exceeded, no scaling needed",
        "[INFO] [RiskManager] Risk management applied successfully",
        "[INFO] [RiskManager] Portfolio risk management applied successfully in iteration 1",
        "[INFO] [RiskManager] No partial contracts after iteration 1. Converged!",
        "[INFO] [RiskManager] Final positions fully integer after 1 iterations.",
        "[INFO] [RiskManager] Generating executions for strategy GOLDEN_S, target_positions size: 1, existing executions: 0",
        "[INFO] [RiskManager] Filled-position ledger for strategy GOLDEN_S size: 0",
        "[INFO] [RiskManager] Generated execution for strategy GOLDEN_S: ZZA BUY qty=3",
        "[INFO] [RiskManager] Total executions generated for strategy GOLDEN_S: 1",
        "[INFO] [Scripted Strategy] Total historical data: 0 returns across 0 symbols",
        "[INFO] [Scripted Strategy] Iteration 1 of dynamic optimization + risk loop",
        "[INFO] [RiskManager] Using risk manager",
        "[ERROR] [RiskManager] Instrument not found: ZZA. Available symbols: <registry>",
        "[INFO] [RiskManager] Risk management result: risk_exceeded=1, scale=0.700000, portfolio_mult=1.000000, jump_mult=1.000000, correlation_mult=1.000000, leverage_mult=0.700000",
        "[WARNING] [RiskManager] Risk limits exceeded, scaling positions by 0.700000",
        "[INFO] [RiskManager] Risk management applied successfully",
        "[INFO] [RiskManager] Portfolio risk management applied successfully in iteration 1",
        "[INFO] [RiskManager] Fractional contract detected in iteration 1: ZZA, quantity=3.5",
        "[INFO] [RiskManager] Iteration 2 of dynamic optimization + risk loop",
        "[INFO] [RiskManager] Using risk manager",
        "[ERROR] [RiskManager] Instrument not found: ZZA. Available symbols: <registry>",
        "[INFO] [RiskManager] Risk management result: risk_exceeded=0, scale=1.000000, portfolio_mult=1.000000, jump_mult=1.000000, correlation_mult=1.000000, leverage_mult=1.000000",
        "[INFO] [RiskManager] Risk limits not exceeded, no scaling needed",
        "[INFO] [RiskManager] Risk management applied successfully",
        "[INFO] [RiskManager] Portfolio risk management applied successfully in iteration 2",
        "[INFO] [RiskManager] Fractional contract detected in iteration 2: ZZA, quantity=3.5",
        "[INFO] [RiskManager] Iteration 3 of dynamic optimization + risk loop",
        "[INFO] [RiskManager] Using risk manager",
        "[ERROR] [RiskManager] Instrument not found: ZZA. Available symbols: <registry>",
        "[INFO] [RiskManager] Risk management result: risk_exceeded=0, scale=1.000000, portfolio_mult=1.000000, jump_mult=1.000000, correlation_mult=1.000000, leverage_mult=1.000000",
        "[INFO] [RiskManager] Risk limits not exceeded, no scaling needed",
        "[INFO] [RiskManager] Risk management applied successfully",
        "[INFO] [RiskManager] Portfolio risk management applied successfully in iteration 3",
        "[INFO] [RiskManager] Fractional contract detected in iteration 3: ZZA, quantity=3.5",
        "[INFO] [RiskManager] Iteration 4 of dynamic optimization + risk loop",
        "[INFO] [RiskManager] Using risk manager",
        "[ERROR] [RiskManager] Instrument not found: ZZA. Available symbols: <registry>",
        "[INFO] [RiskManager] Risk management result: risk_exceeded=0, scale=1.000000, portfolio_mult=1.000000, jump_mult=1.000000, correlation_mult=1.000000, leverage_mult=1.000000",
        "[INFO] [RiskManager] Risk limits not exceeded, no scaling needed",
        "[INFO] [RiskManager] Risk management applied successfully",
        "[INFO] [RiskManager] Portfolio risk management applied successfully in iteration 4",
        "[INFO] [RiskManager] Fractional contract detected in iteration 4: ZZA, quantity=3.5",
        "[INFO] [RiskManager] Iteration 5 of dynamic optimization + risk loop",
        "[INFO] [RiskManager] Using risk manager",
        "[ERROR] [RiskManager] Instrument not found: ZZA. Available symbols: <registry>",
        "[INFO] [RiskManager] Risk management result: risk_exceeded=0, scale=1.000000, portfolio_mult=1.000000, jump_mult=1.000000, correlation_mult=1.000000, leverage_mult=1.000000",
        "[INFO] [RiskManager] Risk limits not exceeded, no scaling needed",
        "[INFO] [RiskManager] Risk management applied successfully",
        "[INFO] [RiskManager] Portfolio risk management applied successfully in iteration 5",
        "[INFO] [RiskManager] Fractional contract detected in iteration 5: ZZA, quantity=3.5",
        "[WARNING] [RiskManager] Max iterations reached (5). Forcing final rounding to remove any partial contracts.",
        "[INFO] [RiskManager] Final forced rounding for ZZA: 3.500000 -> 4",
        "[INFO] [RiskManager] Final rounding completed. No partial contracts remain.",
        "[INFO] [RiskManager] Generating executions for strategy GOLDEN_S, target_positions size: 1, existing executions: 1",
        "[INFO] [RiskManager] Filled-position ledger for strategy GOLDEN_S size: 1",
        "[INFO] [RiskManager] Generated execution for strategy GOLDEN_S: ZZA BUY qty=1",
        "[INFO] [RiskManager] Total executions generated for strategy GOLDEN_S: 2",
    };

    if (got != expected) {
        std::string dump;
        for (const auto& l : got) {
            std::string esc;
            for (char c : l) {
                if (c == '"' || c == '\\') esc += '\\';
                esc += c;
            }
            dump += "        \"" + esc + "\",\n";
        }
        ADD_FAILURE() << "captured " << got.size() << " lines, pinned " << expected.size()
                      << "; captured:\n" << dump;
    }
    EXPECT_EQ(got, expected);
}
