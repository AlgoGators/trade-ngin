// The optimizer's covariance reads the ADJUSTED returns (LOOP_SPEC v6.1 section 2.3; T-ROLLX
// commit 1): a bar whose instrument_id differs from the previous stored bar's is a contract
// switch, its step is removed from the level, and the return across it (per symbol, and across a
// dropped intersection date) is the adjusted change over the raw previous close.
#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"

#define private public
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

PortfolioConfig config() {
    PortfolioConfig c{1'000'000.0, 0.6, 0.05, false};
    c.opt_config.tau = 1.0;
    c.opt_config.capital = 1'000'000.0;
    c.risk_config.capital = 1'000'000.0;
    c.risk_config.lookback_period = 252;
    c.risk_modules = {test_none_module()};
    return c;
}

Bar bar(const std::string& symbol, int day, double close, const std::string& id) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = std::chrono::system_clock::time_point(std::chrono::hours(24 * (20000 + day)));
    b.open = b.high = b.low = b.close = Decimal(close);
    b.volume = 1000.0;
    b.instrument_id = id;
    return b;
}

double ret(double a, double b) { return (b - a) / a; }

}  // namespace

class CovarianceAdjustedReturnsTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        static int n = 0;
        manager_ = std::make_unique<PortfolioManager>(config(), "PM_ADJ_" + std::to_string(++n));
    }
    void TearDown() override {
        manager_.reset();
        StateManager::reset_instance();
        TestBase::TearDown();
    }
    std::unique_ptr<PortfolioManager> manager_;
};

// NG rolls on day 3 (864 -> 863, the close jumping 3.376 -> 3.965): the stored per-symbol return
// on day 3 is 0, the other returns are the raw simple returns; ES, with one id, is untouched.
TEST_F(CovarianceAdjustedReturnsTest, AContractSwitchIsAZeroReturnInTheStoredHistory) {
    manager_->update_historical_returns({bar("NG", 1, 3.337, "864"), bar("ES", 1, 5000.0, "1"),
                                         bar("NG", 2, 3.376, "864"), bar("ES", 2, 5050.0, "1"),
                                         bar("NG", 3, 3.965, "863"), bar("ES", 3, 5025.0, "1"),
                                         bar("NG", 4, 3.822, "863"), bar("ES", 4, 5075.0, "1")});
    const auto& ng = manager_->historical_returns_.at("NG");
    ASSERT_EQ(ng.size(), 3u);
    EXPECT_DOUBLE_EQ(ng[0], ret(3.337, 3.376));
    EXPECT_DOUBLE_EQ(ng[1], 0.0) << "the switch's step is not a return";
    EXPECT_DOUBLE_EQ(ng[2], ret(3.965, 3.822));
    const auto& es = manager_->historical_returns_.at("ES");
    EXPECT_EQ(es, (std::vector<double>{ret(5000.0, 5050.0), ret(5050.0, 5025.0), ret(5025.0, 5075.0)}));
    ASSERT_EQ(manager_->ids_by_date_.at("NG").size(), 4u);
}

// The date-aligned series the covariance is built from: the adjusted level removes the step, so
// across the intersection (day 3 dropped because ES did not print) NG's day-2 -> day-4 return is
// the real move 3.376 -> 3.822 - 0.589 = 3.233 over the raw 3.376, not the splice's 3.376 -> 3.822.
TEST_F(CovarianceAdjustedReturnsTest, TheDateAlignedReturnsReadTheAdjustedLevelOverTheRawClose) {
    manager_->update_historical_returns({bar("NG", 1, 3.337, "864"), bar("ES", 1, 5000.0, "1"),
                                         bar("NG", 2, 3.376, "864"), bar("ES", 2, 5050.0, "1"),
                                         bar("NG", 3, 3.965, "863"),
                                         bar("NG", 4, 3.822, "863"), bar("ES", 4, 5075.0, "1"),
                                         bar("NG", 5, 3.803, "863"), bar("ES", 5, 5100.0, "1")});
    std::unordered_map<std::string, std::map<int64_t, double>> closes;
    for (const auto& s : {"NG", "ES"}) closes[s] = manager_->closes_by_date_.at(s);
    const auto adjusted = manager_->adjusted_closes_by_symbol(closes);
    ASSERT_EQ(adjusted.at("NG").size(), 5u);
    const auto& a = adjusted.at("NG");
    const int64_t d1 = closes.at("NG").begin()->first;
    // The adjusted level is the raw close PLUS the later steps: the segment before the switch is
    // lifted by the step 3.965 - 3.376 = 0.589 (contango), the latest segment is the raw close.
    EXPECT_NEAR(a.at(d1), 3.337 + 0.589, 1e-12);
    EXPECT_NEAR(a.at(d1 + 1), 3.376 + 0.589, 1e-12);
    EXPECT_NEAR(a.at(d1 + 2), 3.965, 1e-12);
    EXPECT_NEAR(a.at(d1 + 3), 3.822, 1e-12);
    EXPECT_NEAR(a.at(d1 + 4), 3.803, 1e-12);
    EXPECT_EQ(adjusted.at("ES"), closes.at("ES")) << "one id: the raw series";

    const auto r = manager_->date_aligned_returns(closes, adjusted);
    ASSERT_EQ(r.at("NG").size(), 3u) << "intersection {1, 2, 4, 5}";
    EXPECT_NEAR(r.at("NG")[0], ret(3.337, 3.376), 1e-12);
    EXPECT_NEAR(r.at("NG")[1], (3.822 - (3.376 + 0.589)) / 3.376, 1e-12)
        << "day 2 -> day 4 without the step";
    EXPECT_NEAR(r.at("NG")[2], ret(3.822, 3.803), 1e-12);
    EXPECT_EQ(r.at("ES"), (std::vector<double>{ret(5000.0, 5050.0), ret(5050.0, 5075.0), ret(5075.0, 5100.0)}));

    // The raw one-argument form is the old series, with the step in it (the control).
    const auto raw = manager_->date_aligned_returns(closes);
    EXPECT_NEAR(raw.at("NG")[1], ret(3.376, 3.822), 1e-12);
    EXPECT_EQ(raw.at("ES"), r.at("ES"));
}

// With no switch anywhere the adjusted path is the raw path, bit for bit.
TEST_F(CovarianceAdjustedReturnsTest, WithoutASwitchTheAdjustedSeriesIsTheRawSeries) {
    std::vector<Bar> bars;
    for (int d = 1; d <= 30; ++d) {
        bars.push_back(bar("A", d, 100.0 + std::sin(d * 0.3) * 3.0, "7"));
        bars.push_back(bar("B", d, 50.0 + std::cos(d * 0.2) * 2.0, ""));
    }
    manager_->update_historical_returns(bars);
    std::unordered_map<std::string, std::map<int64_t, double>> closes;
    for (const auto& s : {"A", "B"}) closes[s] = manager_->closes_by_date_.at(s);
    const auto adjusted = manager_->adjusted_closes_by_symbol(closes);
    EXPECT_EQ(adjusted.at("A"), closes.at("A"));
    EXPECT_EQ(adjusted.at("B"), closes.at("B"));
    EXPECT_EQ(manager_->date_aligned_returns(closes, adjusted), manager_->date_aligned_returns(closes));
}
