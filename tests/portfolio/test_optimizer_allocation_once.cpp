// tests/portfolio/test_optimizer_allocation_once.cpp
//
// Ledger N2, the S2 follow-up: the allocation applied twice in PortfolioManager::
// optimize_positions. A sleeve sizes its contracts on its own capital slice (capital x
// allocation), so one of its contracts is one contract of the account's book, and the account
// holds the SUM of the sleeves' contracts (apply_risk_management says so and gates that sum).
// The weight of one contract in the account is w = notional / total capital. Before this fix the
// aggregation weighted every sleeve contract by w x allocation, so the optimizer answered in
// allocation-weighted contracts, and the distribution divided by the allocation again:
//
//     quota_s = (optimized / w) x share_s / allocation_s,   share_s = q_s a_s / sum(q a)
//             = (optimized / w) x q_s / sum(q a)
//
// One optimizer step (one w) therefore became 1 / (sum(q a) / sum(q)) contracts of book: 1.64 on
// BASE's 6C of 2026-04-24 (TF 1 at 0.7, FAST 0.293 at 0.3), 3.33 on a symbol only the 0.3 sleeve
// holds. After the fix the aggregate carries the allocation once (inside q):
//
//     contribution_s = q_s x w,   quota_s = (optimized / w) x q_s / sum(q),
//     book = round(optimized / w), stored = largest-remainder split of book (S2's split)
//
// These tests drive a real PortfolioManager and observe only get_strategy_positions(), which
// exists before and after the fix, so the same file is RED on the parent source and GREEN on
// the fix. Fixed-book sleeves: every symbol is priced at the PM's default 0.01 weight per
// contract and at zero cost; the optimizer's deadband is OFF, so the greedy's answer (whole
// steps of w from zero) reaches the distribution as it is.

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

using OnceBook = std::unordered_map<std::string, Position>;

class OnceFixedBookStrategy : public BaseStrategy {
public:
    OnceFixedBookStrategy(std::string id, StrategyConfig config,
                          std::shared_ptr<DatabaseInterface> db, OnceBook book)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          book_(std::move(book)) {
        metadata_.name = "Allocation Once Fixed Book Strategy";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        (void)data;
        ++calls_;
        return Result<void>();
    }
    std::unordered_map<std::string, Position> get_target_positions() const override {
        return calls_ == 0 ? OnceBook{} : book_;
    }

private:
    OnceBook book_;
    size_t calls_{0};
};

Timestamp once_day(int d) {
    return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d));
}

// A deterministic random walk seeded by `seed`; two symbols given the same seed print the same
// closes, so their returns (and every covariance entry between them) are identical.
std::vector<double> once_closes(const std::string& seed, int days) {
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

constexpr int kOnceDays = 40;

// symbol -> the seed of its closes
std::vector<Bar> once_bars(const std::vector<std::pair<std::string, std::string>>& symbol_seed) {
    std::vector<Bar> bars;
    std::unordered_map<std::string, std::vector<double>> closes;
    for (const auto& [s, seed] : symbol_seed) closes[s] = once_closes(seed, kOnceDays);
    for (int d = 0; d < kOnceDays; ++d) {
        for (const auto& [s, seed] : symbol_seed) {
            (void)seed;
            Bar b;
            b.symbol = s;
            b.timestamp = once_day(d);
            b.open = b.high = b.low = b.close = Decimal(closes[s][d]);
            b.volume = 1000.0;
            bars.push_back(b);
        }
    }
    return bars;
}

Position once_pos(const std::string& symbol, double qty) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(100.0);
    p.last_update = once_day(0);
    return p;
}

OnceBook once_book(const std::vector<std::pair<std::string, double>>& rows) {
    OnceBook b;
    for (const auto& [symbol, qty] : rows) b[symbol] = once_pos(symbol, qty);
    return b;
}

PortfolioConfig once_config() {
    PortfolioConfig pc{1'000'000.0, 1.0, 0.0, /*use_optimization=*/true};
    pc.allow_fractional_positions = false;
    pc.opt_config.tau = 1.0;
    pc.opt_config.capital = 1'000'000.0;
    pc.opt_config.cost_penalty_scalar = 50.0;
    pc.opt_config.max_iterations = 100;
    pc.opt_config.convergence_threshold = 1e-6;
    pc.opt_config.use_buffering = false;
    pc.opt_config.buffer_size_factor = 0.05;
    pc.risk_config.capital = 1'000'000.0;
    pc.risk_modules = {test_none_module()};
    return pc;
}

class AllocationOnce : public TestBase {
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

    void make_pm() {
        static int n = 0;
        strategies_.clear();
        pm_ = std::make_unique<PortfolioManager>(once_config(), "PM_ONCE_" + std::to_string(++n));
    }

    void add(const std::string& id, OnceBook target, double allocation) {
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0 * allocation;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto s = std::make_shared<OnceFixedBookStrategy>(id, sc, mock_db_, std::move(target));
        ASSERT_TRUE(s->initialize().is_ok());
        ASSERT_TRUE(s->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(s, allocation, /*use_optimization=*/true).is_ok());
        strategies_.push_back(s);
    }

    void rebalance(const std::vector<std::pair<std::string, std::string>>& symbol_seed) {
        ASSERT_TRUE(
            pm_->process_market_data(once_bars(symbol_seed), false, once_day(kOnceDays)).is_ok());
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

// BASE's 6C on 2026-04-24 (T-7a-REBUILD_AUDIT §2.2): TF (0.7) asks 1, FAST (0.3) asks 0.292997,
// the book asks 1.292997 contracts; nothing held.
//   before: aggregate 0.7 x 1 + 0.3 x 0.292997 = 0.787899 weighted contracts; the greedy steps
//           once (|0.787899 - 1| < 0.787899), answer 1; quotas TF 1 x 0.7 / 0.787899 / 0.7 =
//           1.269199, FAST 1 x 0.087899 / 0.787899 / 0.3 = 0.371871; book round(1.641070) = 2;
//           TF 1 / FAST 1: the book is rounded UP through the split, 2 for an ask of 1.29.
//   after:  aggregate 1.292997 contracts; the greedy steps once (a second step would leave
//           0.707003 > 0.292997), answer 1; quotas TF 1 / 1.292997 = 0.773397, FAST 0.226603;
//           book round(1.0) = 1; the one contract to the larger remainder: TF 1 / FAST 0.
TEST_F(AllocationOnce, TheSixCBookOfTwentyFourAprilStoresOneNotTwo) {
    make_pm();
    add("TREND_FOLLOWING", once_book({{"6CX", 1.0}}), 0.7);
    add("TREND_FOLLOWING_FAST", once_book({{"6CX", 0.292997}}), 0.3);
    rebalance({{"6CX", "6CX"}});

    const double tf = stored("TREND_FOLLOWING", "6CX");
    const double fast = stored("TREND_FOLLOWING_FAST", "6CX");
    EXPECT_EQ(tf + fast, 1.0) << "the book is the optimizer's integer, round(1.0) = 1, not 2";
    EXPECT_EQ(tf, 1.0);
    EXPECT_EQ(fast, 0.0);
}

// A symbol only the 0.3 sleeve asks for, 2 contracts.
//   before: aggregate 0.3 x 2 = 0.6; the greedy steps once, answer 1; quota 1 / 0.3 = 3.33;
//           stored 3, one optimizer step being 3.33 contracts of book.
//   after:  aggregate 2; the greedy steps twice, answer 2; stored 2.
TEST_F(AllocationOnce, AFastOnlyAskOfTwoStoresTwoNotThree) {
    make_pm();
    add("TREND_FOLLOWING", once_book({{"6AX", 0.0}}), 0.7);
    add("TREND_FOLLOWING_FAST", once_book({{"6AX", 2.0}}), 0.3);
    rebalance({{"6AX", "6AX"}});

    EXPECT_EQ(stored("TREND_FOLLOWING_FAST", "6AX"), 2.0);
    EXPECT_EQ(stored("TREND_FOLLOWING", "6AX"), 0.0);
}

// The same sleeve asks 1 contract.
//   before: aggregate 0.3; a step would leave 0.7 > 0.3, answer 0; stored 0 (the quantum of
//           3.33 contracts rounds a lone FAST lot away as readily as it rounds 2 up to 3).
//   after:  aggregate 1; answer 1; stored 1.
TEST_F(AllocationOnce, AFastOnlyAskOfOneStoresOne) {
    make_pm();
    add("TREND_FOLLOWING", once_book({{"6AX", 0.0}}), 0.7);
    add("TREND_FOLLOWING_FAST", once_book({{"6AX", 1.0}}), 0.3);
    rebalance({{"6AX", "6AX"}});

    EXPECT_EQ(stored("TREND_FOLLOWING_FAST", "6AX"), 1.0);
}

// The 6A shape of 2026-04-28: the optimizer answers one contract on a symbol only FAST holds,
// while FAST itself asked 0.441733 of it, because the greedy substitutes it for a correlated
// symbol. Here the substitute is exact: A6X and B6Y print the same closes (one covariance
// entry for all four cells), TF (0.7) asks B6Y 1, FAST (0.3) asks A6X 0.441733. Both steps then
// leave the same tracking error to the last bit and the greedy keeps the first index, A6X.
//   before: aggregates A6X 0.132520, B6Y 0.7 (sum 0.832520); first step to A6X leaves |0.832520
//           - 1| = 0.167480, no second step; answer A6X 1, B6Y 0; FAST quota 1 / 0.3 = 3.33,
//           stored FAST A6X 3: one contract of answer is three of book.
//   after:  aggregates 0.441733 and 1 (sum 1.441733); first step to A6X leaves 0.441733, a
//           second step 0.558267; answer A6X 1, B6Y 0; stored FAST A6X 1.
TEST_F(AllocationOnce, ASubstitutedFastOnlySymbolStoresTheOptimizersOneContract) {
    make_pm();
    add("TREND_FOLLOWING", once_book({{"A6X", 0.0}, {"B6Y", 1.0}}), 0.7);
    add("TREND_FOLLOWING_FAST", once_book({{"A6X", 0.441733}, {"B6Y", 0.0}}), 0.3);
    rebalance({{"A6X", "SAME"}, {"B6Y", "SAME"}});

    const double fast_a = stored("TREND_FOLLOWING_FAST", "A6X");
    const double tf_b = stored("TREND_FOLLOWING", "B6Y");
    EXPECT_EQ(fast_a, 1.0) << "the optimizer's one contract, not 1 / 0.3 = 3.33 rounded to 3";
    EXPECT_EQ(fast_a + stored("TREND_FOLLOWING", "A6X") + tf_b +
                  stored("TREND_FOLLOWING_FAST", "B6Y"),
              1.0)
        << "the account holds the one contract the optimizer answered";
}

// Control, CONSERVATIVE's shape: one sleeve at allocation 1.0. The weight and the quota are
// the same numbers before and after (x 1.0 and / 1.0 are exact), so the stored book is too.
TEST_F(AllocationOnce, OneSleeveAtAllocationOneStoresTheGreedyAnswer) {
    make_pm();
    add("TREND_FOLLOWING", once_book({{"6CX", 1.292997}}), 1.0);
    rebalance({{"6CX", "6CX"}});
    EXPECT_EQ(stored("TREND_FOLLOWING", "6CX"), 1.0);

    make_pm();
    add("TREND_FOLLOWING", once_book({{"6AX", 2.0}}), 1.0);
    rebalance({{"6AX", "6AX"}});
    EXPECT_EQ(stored("TREND_FOLLOWING", "6AX"), 2.0);
}
