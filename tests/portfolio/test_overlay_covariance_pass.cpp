// risk.json's carver module overlay_covariance, through the engine: the switch goes from the book's
// config to the one pass, and with "carver" the pass reads each symbol's sizing standard deviation
// as the trend sleeve publishes it (TrendFollowingStrategy::overlay_series, the estimate's sigma).
//
// What runs. PortfolioManager::process_market_data on a book whose overlay sleeve is a real
// TrendFollowingStrategy, so build_one_pass_inputs, one_pass::rebalance and the sleeve's own
// overlay_series are the engine's. Only the sleeve's on_data is replaced: the test writes each
// symbol's estimate, overlay window, optimiser closes and N* itself, as on_data would leave them.
//
// The proof. The same bars and the same N* are run four times: "sample" and "carver", each with
// the sleeve's sizing sigmas s and s / 2. The book holds N* and no limit binds, so the stored book
// is N* in all four. On "carver" the covariance is sigma_i sigma_j rho_ij and each shock sigma is
// sigma_i, so halving every sigma halves the stored book's R and R_shock exactly; on "sample"
// neither reads the sizing sigmas, so both are unchanged.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

// As test_worked_days.cpp: every standard header first, then the two headers with their members
// opened (the test writes the trend sleeve's instrument_data_ and the manager's cost model).
#define private public
#define protected public
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#undef protected
#undef private

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "one_pass_test_fixture.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/risk/risk_detail.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

const std::vector<std::string> kSymbols = {"ZT", "MNQ", "MES"};
const std::vector<double> kPrice = {104.13, 24772.0, 6706.0};
const std::vector<double> kMultiplier = {2000.0, 2.0, 5.0};
const std::vector<double> kSigma = {0.030, 0.240, 0.160};       // s: the sleeve's sizing sigmas
const std::vector<double> kJumpDaily = {0.004, 0.030, 0.020};  // not the sizing sigma
const std::vector<double> kNStar = {3.0, 1.0, 1.0};
constexpr double kCapital = 500'000.0;
constexpr int kDates = 200;
constexpr double kLastDay = 20600.0;
const char* const kSleeve = "TREND";

// The trend sleeve with on_data replaced: what the test wrote into instrument_data_ stays.
class FixedTrend : public TrendFollowingStrategy {
public:
    using TrendFollowingStrategy::TrendFollowingStrategy;
    Result<void> on_data(const std::vector<Bar>& data) override {
        (void)data;
        return Result<void>();
    }
};

}  // namespace

class OverlayCovariancePass : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://overlay_covariance_pass");
        ASSERT_TRUE(db_->connect().is_ok());
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::ERR;
        Logger::instance().initialize(lc);
    }

    void TearDown() override {
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    // One rebalance of a fresh book holding N*: `rule` on the carver module, the sleeve's sizing
    // sigmas `sigma_scale` x s. Every other input is the same on every call.
    OnePassDay run(const std::string& rule, double sigma_scale) {
        static int n = 0;
        PortfolioConfig pc = one_pass_config(kSleeve, kCapital);
        std::get<CarverModuleConfig>(pc.risk_modules[0].params).overlay_covariance = rule;
        pc.overlay_carver = rule == "carver";  // as apply_loop_config sets it from the module
        PortfolioManager pm(pc, "PM_OVCOV_" + std::to_string(++n));
        pm.set_backtest_mode(false);

        StrategyConfig sc;
        sc.capital_allocation = kCapital;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        TrendFollowingConfig tc;
        tc.risk_target = 0.20;
        tc.idm = 2.5;
        tc.ema_windows = {{2, 8}};
        auto sleeve = std::make_shared<FixedTrend>(kSleeve, sc, tc, db_);
        EXPECT_TRUE(sleeve->initialize().is_ok());
        EXPECT_TRUE(sleeve->start().is_ok());
        const double pi = std::acos(-1.0);
        for (size_t i = 0; i < kSymbols.size(); ++i) {
            InstrumentData& d = sleeve->instrument_data_[kSymbols[i]];
            d.contract_size = kMultiplier[i];
            d.weight = 1.0 / kSymbols.size();
            d.estimate.valid = true;
            d.estimate.sigma = sigma_scale * kSigma[i];
            d.estimate.jump_sigma_daily = kJumpDaily[i];
            d.optimal_position = kNStar[i];
            d.final_position = kNStar[i];
            d.current_forecast = 10.0;
            // 200 consecutive dates of returns, a different mix of two waves per symbol (so the
            // pairs are correlated but not perfectly), and the closes that have those returns.
            std::vector<double> closes(kDates + 1);
            closes[kDates] = kPrice[i];
            for (int t = 0; t < kDates; ++t) {
                const double r = 0.004 * std::cos(pi * (t + 1) * 3.0 / kDates) +
                                 0.002 * (i + 1) * std::sin(pi * (t + 1) * (5.0 + i) / kDates);
                d.overlay_days.push_back(kLastDay - (kDates - 1 - t));
                d.overlay_returns.push_back(r);
            }
            for (int t = kDates; t > 0; --t) closes[t - 1] = closes[t] / (1.0 + d.overlay_returns[t - 1]);
            for (int t = 0; t <= kDates; ++t) {
                d.opt_days.push_back(kLastDay - (kDates - t));
                d.opt_closes.push_back(closes[t]);
                d.opt_levels.push_back(closes[t]);
                d.price_history.push_back(closes[t]);
            }
        }
        EXPECT_TRUE(pm.add_strategy(sleeve, 1.0, true).is_ok());
        for (size_t i = 0; i < kSymbols.size(); ++i) {  // the held book, seeded: N* itself
            Position p;
            p.symbol = kSymbols[i];
            p.quantity = Decimal(kNStar[i]);
            p.average_price = Decimal(kPrice[i]);
            EXPECT_TRUE(pm.update_strategy_position(kSleeve, kSymbols[i], p).is_ok());
        }
        for (const auto& symbol : kSymbols) {
            transaction_cost::AssetCostConfig cost;
            cost.symbol = symbol;
            cost.commission_per_unit = 1.50;
            cost.spread_cost_multiplier = 0.0;
            cost.max_impact_bps = 0.0;
            pm.cost_manager_.register_asset_config(cost);
        }
        EXPECT_TRUE(pm.set_sizing_capital(kCapital).is_ok());
        std::vector<Bar> bars;
        for (size_t i = 0; i < kSymbols.size(); ++i) {
            bars.push_back(one_pass_bar(kSymbols[i], 1, kPrice[i]));
            pm.update_cost_manager_market_data(kSymbols[i], 100000.0, kPrice[i], kPrice[i]);
        }
        ::testing::internal::CaptureStdout();
        const auto processed = pm.process_market_data(bars);
        ::testing::internal::GetCapturedStdout();
        EXPECT_TRUE(processed.is_ok()) << (processed.is_error() ? processed.error()->what() : "");
        return pm.last_one_pass();
    }

    std::shared_ptr<MockPostgresDatabase> db_;
};

// The switch reaches the pass and "carver" reads the sleeve's sizing sigmas; "sample" does not.
TEST_F(OverlayCovariancePass, CarverReadsTheSleevesSizingSigmasAndSampleDoesNot) {
    const OnePassDay sample = run("sample", 1.0);
    const OnePassDay sample_half = run("sample", 0.5);
    const OnePassDay carver = run("carver", 1.0);
    const OnePassDay carver_half = run("carver", 0.5);
    double gross = 0.0;
    for (size_t i = 0; i < kSymbols.size(); ++i) gross += std::abs(kNStar[i] * kMultiplier[i] * kPrice[i]);
    for (const OnePassDay* d : {&sample, &sample_half, &carver, &carver_half}) {
        ASSERT_TRUE(d->ran && d->sized && !d->refused);
        ASSERT_TRUE(d->covariance_measured) << "a complete window, not blind";
        EXPECT_EQ(d->risk_requested, 1.0) << "no cut";
        EXPECT_EQ(d->binding_term, "none");
        EXPECT_DOUBLE_EQ(d->trimmed_contracts, 0.0);
        // the stored book is the held book, N*, in all four: its notional at the signal closes
        EXPECT_NEAR(d->stored_gross, gross, 1e-9 * gross);
    }

    // "sample": the sizing sigmas are not read.
    EXPECT_EQ(sample_half.stored_r, sample.stored_r);
    EXPECT_EQ(sample_half.stored_r_shock, sample.stored_r_shock);

    // "carver": the pass got the switch (its readings are not the sample covariance's) and the
    // sleeve's sizing sigmas (halving them halves R and R_shock).
    ASSERT_GT(carver.stored_r, 0.0);
    EXPECT_GT(std::abs(carver.stored_r - sample.stored_r), 1e-6);
    EXPECT_NEAR(carver_half.stored_r, 0.5 * carver.stored_r, 1e-12 * carver.stored_r);
    EXPECT_NEAR(carver_half.stored_r_shock, 0.5 * carver.stored_r_shock, 1e-12 * carver.stored_r_shock);

    // R_shock on "carver" is sum |x_i| sigma_i with x_i = N_i M_i P_i / E: the sigma is the
    // estimate's, not the jump sigma or any other.
    double shock = 0.0;
    for (size_t i = 0; i < kSymbols.size(); ++i) {
        shock += std::abs(kNStar[i] * kMultiplier[i] * kPrice[i] / kCapital) * kSigma[i];
    }
    EXPECT_NEAR(carver.stored_r_shock, shock, 1e-12 * shock);
}
