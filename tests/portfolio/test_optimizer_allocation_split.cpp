// tests/portfolio/test_optimizer_allocation_split.cpp
//
// T-7a INSERT S2, the allocation rounded twice (ledger OPT-N2/N3). PortfolioManager::
// optimize_positions aggregates the sleeves in weight space, contribution = q x w x allocation, so
// the optimizer's answer (optimized / w) counts allocation-weighted contracts. It then hands the
// answer back to the sleeves. Before S2:
//
//     stored_s = round( round(optimized / w) x share_s / allocation_s )
//
// The optimizer's answer was rounded first and every sleeve's part rounded again. At allocation
// 1.0 the second rounding is the identity. At 0.7 / 0.3 it is not, and the sleeve integers need
// not sum to anything the optimizer produced. After S2 the allocation is undone on the WEIGHT:
//
//     quota_s = (optimized / w) x share_s / allocation_s        (the sleeve's contracts, unrounded)
//     book    = round(sum of quota_s)                           (rounded ONCE)
//     stored  = largest-remainder split of book over the quotas (sums to book exactly)
//
// Tie rule: the larger remainder first; on equal remainders the smaller strategy_id. A book whose
// quotas sum below zero is split as the mirror of the long book.
//
// Ledger N2 (the follow-up, test_optimizer_allocation_once.cpp) then removed the allocation from
// both ends: contribution = q x w, quota_s = (optimized / w) x share_s, book = round(optimized / w).
// Every case below stores the same integers under both rules; the arithmetic after each "after:"
// is S2's, and the line "N2:" gives the fixed base's.
//
// These tests drive a real PortfolioManager and observe only get_strategy_positions(), which
// exists before and after the fix, so the same file is RED on the parent source and GREEN on
// the fix. The sleeves are fixed-book strategies: every symbol is priced at the PM's default
// 0.01 weight per contract and at zero cost. With the optimizer's deadband ON (use_buffering)
// the held book is kept exactly (the optimizer returns the current weights), which is how a
// non-whole aggregate such as 0.7 x 2 + 0.3 x 1 = 1.7 reaches the distribution.

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
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

class SplitFixedBookStrategy : public BaseStrategy {
public:
    SplitFixedBookStrategy(std::string id, StrategyConfig config,
                           std::shared_ptr<DatabaseInterface> db, Book book)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          book_(std::move(book)) {
        metadata_.name = "Split Fixed Book Strategy";
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

Timestamp split_day(int d) {
    return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d));
}

std::vector<double> split_closes(const std::string& symbol, int days) {
    uint64_t state = 1469598103934665603ULL;
    for (char c : symbol) state = (state ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
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

constexpr int kSplitDays = 40;

std::vector<Bar> split_bars(const std::vector<std::string>& symbols) {
    std::vector<Bar> bars;
    std::unordered_map<std::string, std::vector<double>> closes;
    for (const auto& s : symbols) closes[s] = split_closes(s, kSplitDays);
    for (int d = 0; d < kSplitDays; ++d) {
        for (const auto& s : symbols) {
            Bar b;
            b.symbol = s;
            b.timestamp = split_day(d);
            b.open = b.high = b.low = b.close = Decimal(closes[s][d]);
            b.volume = 1000.0;
            bars.push_back(b);
        }
    }
    return bars;
}

Position split_pos(const std::string& symbol, double qty) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(100.0);
    p.last_update = split_day(0);
    return p;
}

Book split_book(const std::vector<std::pair<std::string, double>>& rows) {
    Book b;
    for (const auto& [symbol, qty] : rows) b[symbol] = split_pos(symbol, qty);
    return b;
}

PortfolioConfig split_config(bool use_buffering) {
    PortfolioConfig pc{1'000'000.0, 1.0, 0.0, /*use_optimization=*/true};
    pc.allow_fractional_positions = false;
    pc.opt_config.tau = 1.0;
    pc.opt_config.capital = 1'000'000.0;
    pc.opt_config.cost_penalty_scalar = 50.0;
    pc.opt_config.max_iterations = 100;
    pc.opt_config.convergence_threshold = 1e-6;
    pc.opt_config.use_buffering = use_buffering;
    pc.opt_config.buffer_size_factor = 0.05;
    pc.risk_config.capital = 1'000'000.0;
    pc.risk_modules = {test_none_module()};
    return pc;
}

class AllocationSplit : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        mock_db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(mock_db_->connect().is_ok());
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::WARNING;
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

    void make_pm(bool use_buffering) {
        static int n = 0;
        strategies_.clear();
        pm_ = std::make_unique<PortfolioManager>(split_config(use_buffering),
                                                 "PM_SPLIT_" + std::to_string(++n));
    }

    void add(const std::string& id, Book target, double allocation) {
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto s = std::make_shared<SplitFixedBookStrategy>(id, sc, mock_db_, std::move(target));
        ASSERT_TRUE(s->initialize().is_ok());
        ASSERT_TRUE(s->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(s, allocation, /*use_optimization=*/true).is_ok());
        strategies_.push_back(s);
    }

    void hold(const std::string& id, const std::string& symbol, double qty) {
        ASSERT_TRUE(pm_->update_strategy_position(id, symbol, split_pos(symbol, qty)).is_ok());
    }

    void rebalance(const std::vector<std::string>& symbols) {
        ASSERT_TRUE(
            pm_->process_market_data(split_bars(symbols), false, split_day(kSplitDays)).is_ok());
    }

    double stored(const std::string& sid, const std::string& symbol) const {
        const auto all = pm_->get_strategy_positions();
        auto s = all.find(sid);
        if (s == all.end()) return 0.0;
        auto p = s->second.find(symbol);
        return p == s->second.end() ? 0.0 : static_cast<double>(p->second.quantity);
    }

    std::shared_ptr<MockPostgresDatabase> mock_db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::vector<std::shared_ptr<StrategyInterface>> strategies_;
};

}  // namespace

// THE RED CASE (BASE's allocations). Held TF 2 / FAST 1 (aggregate 0.7 x 2 + 0.3 x 1 = 1.7
// weighted contracts); targets TF 3 / FAST 1 (contributions 2.1 and 0.3, shares 0.875 / 0.125).
// The deadband keeps the held book, so the optimizer answers 1.7.
//   before: round(1.7) = 2; TF round(2 x 0.875 / 0.7) = round(2.5) = 3,
//           FAST round(2 x 0.125 / 0.3) = round(0.833) = 1; sleeves sum 4, a trade the
//           optimizer did not make (its answer in the sleeves' contracts is 2.833).
//   after:  quotas 1.7 x 0.875 / 0.7 = 2.125 and 1.7 x 0.125 / 0.3 = 0.708; book
//           round(2.833) = 3; floors 2 / 0, one extra to the larger remainder (FAST 0.708):
//           TF 2 / FAST 1, the held book, sum 3.
//   N2:     aggregate 3 (the held book) kept; quotas 3 x 0.75 = 2.25 and 3 x 0.25 = 0.75; book 3;
//           TF 2 / FAST 1.
TEST_F(AllocationSplit, TwoSleevesAtSevenTenthsAndThreeTenthsSumToTheOptimizersInteger) {
    make_pm(/*use_buffering=*/true);
    add("TREND_FOLLOWING", split_book({{"ZNX", 3.0}}), 0.7);
    add("TREND_FOLLOWING_FAST", split_book({{"ZNX", 1.0}}), 0.3);
    hold("TREND_FOLLOWING", "ZNX", 2.0);
    hold("TREND_FOLLOWING_FAST", "ZNX", 1.0);
    rebalance({"ZNX"});

    const double tf = stored("TREND_FOLLOWING", "ZNX");
    const double fast = stored("TREND_FOLLOWING_FAST", "ZNX");
    EXPECT_EQ(tf + fast, 3.0) << "the sleeve integers must sum to round(2.125 + 0.708) = 3";
    EXPECT_EQ(tf, 2.0);
    EXPECT_EQ(fast, 1.0);
}

// The one-lot of a lone 0.3 sleeve. Held FAST 1 on a symbol TF does not want (TF target 0):
// aggregate 0.3, and the deadband keeps it.
//   before: round(0.3) = 0, so FAST round(0 x 1 / 0.3) = 0: the held lot is sold although the
//           optimizer kept it.
//   after:  quota 0.3 x 1 / 0.3 = 1; book 1; FAST 1.
//   N2:     aggregate 1 kept; quota 1; FAST 1.
TEST_F(AllocationSplit, ALoneThreeTenthsSleeveKeepsTheLotTheOptimizerHeld) {
    make_pm(/*use_buffering=*/true);
    add("TREND_FOLLOWING", split_book({{"ZNX", 0.0}}), 0.7);
    add("TREND_FOLLOWING_FAST", split_book({{"ZNX", 1.0}}), 0.3);
    hold("TREND_FOLLOWING_FAST", "ZNX", 1.0);
    rebalance({"ZNX"});

    EXPECT_EQ(stored("TREND_FOLLOWING", "ZNX"), 0.0);
    EXPECT_EQ(stored("TREND_FOLLOWING_FAST", "ZNX"), 1.0);
}

// Ties, at 0.5 / 0.5. Held A 1 / B 0 (aggregate 0.5); targets 1 / 1 (shares 0.5 / 0.5); the
// deadband keeps 0.5.
//   before: round(0.5) = 1; each sleeve round(1 x 0.5 / 0.5) = 1; sum 2 for an answer of 0.5.
//   after:  quotas 0.5 / 0.5; book round(1.0) = 1; floors 0 / 0; the one extra contract goes to
//           the smaller strategy_id on the tied remainder, whatever the registration order.
//   N2:     aggregate 1 kept (A's held lot); quotas 0.5 / 0.5; the same split.
TEST_F(AllocationSplit, ATieGoesToTheSmallerStrategyId) {
    make_pm(/*use_buffering=*/true);
    add("SLEEVE_Z", split_book({{"ZNX", 1.0}}), 0.5);  // registered first
    add("SLEEVE_A", split_book({{"ZNX", 1.0}}), 0.5);
    hold("SLEEVE_A", "ZNX", 1.0);
    rebalance({"ZNX"});

    EXPECT_EQ(stored("SLEEVE_A", "ZNX") + stored("SLEEVE_Z", "ZNX"), 1.0)
        << "the sleeves sum to the book's integer, round(0.5 + 0.5)";
    EXPECT_EQ(stored("SLEEVE_A", "ZNX"), 1.0) << "tie: the smaller strategy_id";
    EXPECT_EQ(stored("SLEEVE_Z", "ZNX"), 0.0);
}

// Control: one sleeve at allocation 1.0 (CONSERVATIVE's and the futures backtest's shape). The
// quota IS the optimizer's answer and the split is round(answer): unchanged by S2, held
// (deadband) and traded (greedy).
TEST_F(AllocationSplit, OneSleeveAtAllocationOneIsUnchanged) {
    make_pm(/*use_buffering=*/false);
    add("TREND_FOLLOWING", split_book({{"ESX", 3.0}}), 1.0);
    rebalance({"CLX", "ESX"});
    EXPECT_EQ(stored("TREND_FOLLOWING", "ESX"), 3.0);

    make_pm(/*use_buffering=*/true);
    add("TREND_FOLLOWING", split_book({{"ESX", 3.0}}), 1.0);
    hold("TREND_FOLLOWING", "ESX", 2.0);
    rebalance({"CLX", "ESX"});
    EXPECT_EQ(stored("TREND_FOLLOWING", "ESX"), 2.0) << "the deadband keeps the held 2";
}

// Control: two sleeves whose quotas are whole (the greedy answer equals the aggregate target):
// 0.7 / 0.3, both want 1 (aggregate 1.0, answer 1): quotas 1 / 1, book 2; the same before and
// after. Under S2 the optimizer's 1 counted allocation-weighted contracts and the split read it
// as the sleeves' 2; under N2 the aggregate is 2 contracts, the answer 2, the quotas 1 / 1.
TEST_F(AllocationSplit, WholeQuotasAreStoredAsTheyAre) {
    make_pm(/*use_buffering=*/false);
    add("TREND_FOLLOWING", split_book({{"ZNX", 1.0}}), 0.7);
    add("TREND_FOLLOWING_FAST", split_book({{"ZNX", 1.0}}), 0.3);
    rebalance({"ZNX"});
    EXPECT_EQ(stored("TREND_FOLLOWING", "ZNX"), 1.0);
    EXPECT_EQ(stored("TREND_FOLLOWING_FAST", "ZNX"), 1.0);
}

// T-7b-2 8b commit 2 through a real PortfolioManager (the optimizer's aggregate is in WEIGHT, the
// opposed branch reads the sleeves' targets in CONTRACTS, so this pins the call site's plumbing).
// Held TF +1 / FAST -1 (the account is flat), targets TF +1 / FAST -1 (the net target is 0), the
// deadband keeps the flat account: the optimizer answers 0.
//   parent: the contributions sum to 0, the guard gives both a share of 0: TF 0, FAST 0, a pair of
//           crossing fills that flattens both sleeves although neither sleeve asked to trade.
//   fixed:  no deviation to split: TF +1, FAST -1, the account still flat.
TEST_F(AllocationSplit, OpposedSleevesThatCancelKeepTheirOwnBooks) {
    make_pm(/*use_buffering=*/true);
    add("TREND_FOLLOWING", split_book({{"ZNX", 1.0}}), 0.5);
    add("TREND_FOLLOWING_FAST", split_book({{"ZNX", -1.0}}), 0.5);
    hold("TREND_FOLLOWING", "ZNX", 1.0);
    hold("TREND_FOLLOWING_FAST", "ZNX", -1.0);
    rebalance({"ZNX"});

    EXPECT_EQ(stored("TREND_FOLLOWING", "ZNX"), 1.0);
    EXPECT_EQ(stored("TREND_FOLLOWING_FAST", "ZNX"), -1.0);
}
