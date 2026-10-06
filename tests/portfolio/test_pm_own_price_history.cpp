// The PortfolioManager keeps its own price history (T-6c commit B).
//
// The optimiser's covariance used to be built from price histories BORROWED from the
// strategies (each strategy's get_price_history()), and when two strategies offered different
// series for one symbol an interim rule kept the first-registered one (PM_HISTORY_MERGE). The
// series therefore depended on which sleeve was registered first and on what each sleeve chose
// to keep: a mean-reversion sleeve trims to 40 prices, a trend sleeve keeps up to 756, a fast
// trend sleeve that never clears splices a replayed window onto itself (T-BASE_ADVERSARIAL
// finding 4, T-BASE_DECISION section 3.3).
//
// Now the PM records one close per symbol per date from the bars process_market_data is fed,
// capped at PortfolioConfig::covariance_history_prices (756 unless configured), oldest date
// dropped first, and builds the returns from that. These tests observe the PM only through
// historical_returns_ and calculate_covariance_matrix, which exist before and after the change,
// so the same file shows RED on the parent source and GREEN on the fix.

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"

#define private public
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#undef private

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

// Day d at 00:00 UTC, counted from 2026-01-01.
Timestamp utc_day(int d) { return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d)); }

double close_of(const std::string& symbol, int d) {
    const double phase = symbol == "AAPL" ? 0.0 : (symbol == "MSFT" ? 1.1 : 2.3);
    return 100.0 * (1.0 + 0.02 * std::sin(0.37 * d + phase)) + 0.01 * d;
}

Bar day_bar(const std::string& symbol, int d, double close) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = utc_day(d);
    b.open = Decimal(close);
    b.high = Decimal(close * 1.01);
    b.low = Decimal(close * 0.99);
    b.close = Decimal(close);
    b.volume = 1'000'000.0;
    return b;
}

Bar day_bar(const std::string& symbol, int d) { return day_bar(symbol, d, close_of(symbol, d)); }

// Optimisation and risk OFF: these tests are about which prices the PM keeps, not about what
// the optimiser does with them.
PortfolioConfig plain_config(const nlohmann::json& extra = nlohmann::json::object()) {
    PortfolioConfig c{1'000'000.0, 100'000.0, 1.0, 0.0, false};
    c.opt_config.capital = 1'000'000.0;
    c.risk_config.capital = 1'000'000.0;
    c.risk_modules = {test_none_module()};
    // Through from_json, so this file compiles against the parent source too, where the key
    // has no reader (and the tests that need it fail there, which is their RED).
    if (!extra.empty()) c.from_json(extra);
    return c;
}

StrategyConfig sleeve_config(const std::vector<std::string>& symbols) {
    StrategyConfig sc;
    sc.capital_allocation = 1'000'000.0;
    sc.max_leverage = 2.0;
    sc.asset_classes = {AssetClass::EQUITIES};
    sc.frequencies = {DataFrequency::DAILY};
    for (const auto& s : symbols) {
        sc.trading_params[s] = 1.0;
        sc.position_limits[s] = 1000.0;
    }
    return sc;
}

// A sleeve with the trend sleeve's history contract (trend_following.cpp on_data): every close
// it is fed is appended in feed order, a call carrying more than 100 bars for a symbol clears
// that symbol first, and at most `cap` prices are kept (0 = unbounded).
class TrendHistorySleeve : public BaseStrategy {
public:
    TrendHistorySleeve(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db,
                       size_t cap = 756)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          cap_(cap) {
        metadata_.name = "Trend History Sleeve";
    }
    Result<void> on_data(const std::vector<Bar>& data,
                         StrategyConsumptionTrace* trace = nullptr) override {
        auto base = BaseStrategy::on_data(data, trace);
        if (base.is_error()) return base;
        std::unordered_map<std::string, size_t> per_symbol;
        for (const auto& bar : data) ++per_symbol[bar.symbol];
        for (const auto& [symbol, n] : per_symbol) {
            if (n > 100) history_[symbol].clear();
        }
        for (const auto& bar : data) {
            auto& h = history_[bar.symbol];
            h.push_back(bar.close.as_double());
            if (cap_ > 0 && h.size() > cap_) h.pop_front();
        }
        return Result<void>();
    }
    std::unordered_map<std::string, std::vector<double>> get_price_history() const override {
        std::unordered_map<std::string, std::vector<double>> out;
        for (const auto& [symbol, h] : history_) out[symbol].assign(h.begin(), h.end());
        return out;
    }

private:
    size_t cap_;
    std::unordered_map<std::string, std::deque<double>> history_;
};

// A sleeve whose history is fabricated: every close it has been fed, twice over (the shape of
// the fast trend sleeve's uncleared replay, T-BASE_DECISION section 3.2).
class DoubledHistorySleeve : public TrendHistorySleeve {
public:
    using TrendHistorySleeve::TrendHistorySleeve;
    std::unordered_map<std::string, std::vector<double>> get_price_history() const override {
        auto h = TrendHistorySleeve::get_price_history();
        for (auto& [symbol, prices] : h) {
            const std::vector<double> once = prices;
            prices.insert(prices.end(), once.begin(), once.end());
        }
        return h;
    }
};

// A sleeve that refuses the call when told to (and then records nothing).
class SwitchableSleeve : public TrendHistorySleeve {
public:
    using TrendHistorySleeve::TrendHistorySleeve;
    bool fail_next{false};
    Result<void> on_data(const std::vector<Bar>& data,
                         StrategyConsumptionTrace* trace = nullptr) override {
        if (fail_next) {
            fail_next = false;
            return make_error<void>(ErrorCode::MARKET_DATA_ERROR, "simulated ingest failure",
                                    "SwitchableSleeve");
        }
        return TrendHistorySleeve::on_data(data, trace);
    }
};

std::vector<double> expected_returns(const std::vector<double>& closes) {
    std::vector<double> r;
    for (size_t i = 1; i < closes.size(); ++i) r.push_back((closes[i] - closes[i - 1]) / closes[i - 1]);
    return r;
}

std::vector<double> closes_for(const std::string& symbol, int first_day, int last_day) {
    std::vector<double> c;
    // Through Decimal, as the bar carries it: the PM records static_cast<double>(bar.close).
    for (int d = first_day; d <= last_day; ++d) c.push_back(static_cast<double>(Decimal(close_of(symbol, d))));
    return c;
}

}  // namespace

class PmOwnPriceHistoryTest : public TestBase {
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

    std::string uid(const std::string& prefix) {
        static int n = 0;
        return prefix + "_" + std::to_string(++n);
    }

    std::unique_ptr<PortfolioManager> make_pm(const nlohmann::json& extra = nlohmann::json::object()) {
        return std::make_unique<PortfolioManager>(plain_config(extra), uid("PM_OWN"));
    }

    std::shared_ptr<MeanReversionStrategy> make_mean_reversion(const std::vector<std::string>& symbols) {
        MeanReversionConfig mr;
        mr.lookback_period = 20;
        mr.vol_lookback = 20;  // keeps max(20, 20) * 2 = 40 prices per symbol
        auto s = std::make_shared<MeanReversionStrategy>(uid("MEAN_REVERSION"), sleeve_config(symbols),
                                                         mr, db_);
        EXPECT_TRUE(s->initialize().is_ok());
        RiskLimits limits;
        limits.max_position_size = 1000.0;
        limits.max_notional_value = 1e9;
        limits.max_drawdown = 0.5;
        limits.max_leverage = 2.0;
        EXPECT_TRUE(s->update_risk_limits(limits).is_ok());
        EXPECT_TRUE(s->start().is_ok());
        return s;
    }

    template <typename Sleeve>
    std::shared_ptr<Sleeve> make_sleeve(const std::string& prefix, const std::vector<std::string>& symbols,
                                        size_t cap = 756) {
        auto s = std::make_shared<Sleeve>(uid(prefix), sleeve_config(symbols), db_, cap);
        EXPECT_TRUE(s->initialize().is_ok());
        EXPECT_TRUE(s->start().is_ok());
        return s;
    }

    // One process_market_data call per day, days [0, n), every symbol each day.
    void feed_days(PortfolioManager& pm, const std::vector<std::string>& symbols, int first, int last) {
        for (int d = first; d <= last; ++d) {
            std::vector<Bar> bars;
            for (const auto& s : symbols) bars.push_back(day_bar(s, d));
            ASSERT_TRUE(pm.process_market_data(bars).is_ok()) << "day " << d;
        }
    }

    std::shared_ptr<MockPostgresDatabase> db_;
};

// Mean reversion (trims to 40 prices) beside a trend sleeve (keeps up to 756) on the same two
// symbols: the PM's history length and covariance are the same whichever sleeve is registered
// first, and the same as either sleeve alone would give, because no sleeve's history is read.
// At the parent source the first-registered sleeve's series is kept: 39 returns with mean
// reversion first, 59 with trend first.
TEST_F(PmOwnPriceHistoryTest, MeanReversionBesideTrendGivesOneHistoryWhicheverIsRegisteredFirst) {
    const std::vector<std::string> symbols{"AAPL", "MSFT"};
    std::vector<std::vector<std::vector<double>>> covariances;
    std::vector<std::string> labels;
    for (int arm = 0; arm < 4; ++arm) {
        auto pm = make_pm();
        auto mr = make_mean_reversion(symbols);
        auto tr = make_sleeve<TrendHistorySleeve>("TREND_FOLLOWING", symbols);
        switch (arm) {
            case 0:  // mean reversion registered first
                ASSERT_TRUE(pm->add_strategy(mr, 0.5).is_ok());
                ASSERT_TRUE(pm->add_strategy(tr, 0.5).is_ok());
                labels.push_back("mean reversion first");
                break;
            case 1:  // trend registered first
                ASSERT_TRUE(pm->add_strategy(tr, 0.5).is_ok());
                ASSERT_TRUE(pm->add_strategy(mr, 0.5).is_ok());
                labels.push_back("trend first");
                break;
            case 2:
                ASSERT_TRUE(pm->add_strategy(mr, 1.0).is_ok());
                labels.push_back("mean reversion alone");
                break;
            default:
                ASSERT_TRUE(pm->add_strategy(tr, 1.0).is_ok());
                labels.push_back("trend alone");
                break;
        }
        feed_days(*pm, symbols, 0, 59);
        SCOPED_TRACE(labels.back());
        for (const auto& s : symbols) {
            ASSERT_EQ(pm->historical_returns_.count(s), 1u) << s;
            EXPECT_EQ(pm->historical_returns_.at(s).size(), 59u)
                << s << ": the PM's history is not the 60 dates it was fed (" << labels.back() << ")";
            EXPECT_EQ(pm->historical_returns_.at(s), expected_returns(closes_for(s, 0, 59))) << s;
        }
        covariances.push_back(pm->calculate_covariance_matrix(pm->historical_returns_));
    }
    for (size_t k = 1; k < covariances.size(); ++k) {
        EXPECT_EQ(covariances[k], covariances[0])
            << "the covariance with " << labels[k] << " differs from " << labels[0];
    }
}

// A sleeve that reports a fabricated doubled history, registered FIRST, changes nothing in the
// PM: its returns are the same bytes as a PM fed the same bars with a clean sleeve only.
TEST_F(PmOwnPriceHistoryTest, ASleeveWithAFabricatedDoubledHistoryChangesNothingInThePm) {
    const std::vector<std::string> symbols{"AAPL", "MSFT"};
    auto clean_pm = make_pm();
    ASSERT_TRUE(clean_pm->add_strategy(make_sleeve<TrendHistorySleeve>("CLEAN", symbols), 1.0).is_ok());
    feed_days(*clean_pm, symbols, 0, 44);

    auto dirty_pm = make_pm();
    ASSERT_TRUE(dirty_pm->add_strategy(make_sleeve<DoubledHistorySleeve>("DOUBLED", symbols), 0.5).is_ok());
    ASSERT_TRUE(dirty_pm->add_strategy(make_sleeve<TrendHistorySleeve>("CLEAN", symbols), 0.5).is_ok());
    feed_days(*dirty_pm, symbols, 0, 44);

    for (const auto& s : symbols) {
        ASSERT_EQ(dirty_pm->historical_returns_.count(s), 1u) << s;
        EXPECT_EQ(dirty_pm->historical_returns_.at(s).size(), 44u)
            << s << ": the doubled sleeve's series reached the PM";
        EXPECT_EQ(dirty_pm->historical_returns_.at(s), clean_pm->historical_returns_.at(s)) << s;
    }
    EXPECT_EQ(dirty_pm->calculate_covariance_matrix(dirty_pm->historical_returns_),
              clean_pm->calculate_covariance_matrix(clean_pm->historical_returns_));
}

// A repeated date does not lengthen the series: a duplicate bar inside one call, and the same
// date fed again in a later call, each overwrite that date's close. The trend sleeve beside it
// appends both, so at the parent source the PM's series is two prices longer.
TEST_F(PmOwnPriceHistoryTest, ARepeatedDateDoesNotLengthenTheSeries) {
    auto pm = make_pm();
    ASSERT_TRUE(pm->add_strategy(make_sleeve<TrendHistorySleeve>("TREND_FOLLOWING", {"AAPL"}), 1.0).is_ok());

    // Days 0..9 in one call, with day 5 twice (the second copy with a different close).
    std::vector<Bar> first;
    for (int d = 0; d <= 9; ++d) {
        first.push_back(day_bar("AAPL", d));
        if (d == 5) first.push_back(day_bar("AAPL", 5, 123.0));
    }
    ASSERT_TRUE(pm->process_market_data(first).is_ok());
    // Day 9 again in a later call, then day 10.
    ASSERT_TRUE(pm->process_market_data({day_bar("AAPL", 9, 111.0)}).is_ok());
    ASSERT_TRUE(pm->process_market_data({day_bar("AAPL", 10)}).is_ok());

    std::vector<double> closes = closes_for("AAPL", 0, 10);
    closes[5] = 123.0;  // the later copy of day 5 overwrote the earlier
    closes[9] = 111.0;  // the later call's day 9 overwrote the earlier
    ASSERT_EQ(pm->historical_returns_.count("AAPL"), 1u);
    EXPECT_EQ(pm->historical_returns_.at("AAPL").size(), 10u)
        << "11 dates were fed; a repeated date lengthened the series";
    EXPECT_EQ(pm->historical_returns_.at("AAPL"), expected_returns(closes));
}

// The overwrite is the second line of defence behind the loader's de-duplication (T-6c commit
// B0), so it is logged: the same date twice in one call, or a later call bringing a different
// close for a stored date, prints PM_HISTORY_REPEATED_DATE; a later call re-feeding a stored date
// with the same close (the bar replay followed by the final feed) is silent.
TEST_F(PmOwnPriceHistoryTest, AnOverwrittenDateIsLoggedAndASameCloseRefeedIsNot) {
    LoggerConfig lc;
    lc.destination = LogDestination::CONSOLE;
    lc.min_level = LogLevel::INFO;
    lc.include_timestamp = false;
    Logger::instance().initialize(lc);
    auto pm = make_pm();
    ASSERT_TRUE(pm->add_strategy(make_sleeve<TrendHistorySleeve>("TREND_FOLLOWING", {"AAPL"}), 1.0).is_ok());

    auto count = [](const std::string& out) {
        size_t n = 0;
        for (size_t at = out.find("PM_HISTORY_REPEATED_DATE"); at != std::string::npos;
             at = out.find("PM_HISTORY_REPEATED_DATE", at + 1))
            ++n;
        return n;
    };

    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm->process_market_data({day_bar("AAPL", 0, 100.0), day_bar("AAPL", 1, 101.0)}).is_ok());
    ASSERT_TRUE(pm->process_market_data({day_bar("AAPL", 0, 100.0), day_bar("AAPL", 1, 101.0),
                                         day_bar("AAPL", 2, 102.0)})
                    .is_ok());
    std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(count(out), 0u) << "a same-close re-feed of stored dates must be silent:\n" << out;

    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm->process_market_data({day_bar("AAPL", 3, 103.0), day_bar("AAPL", 3, 103.0)}).is_ok());
    ASSERT_TRUE(pm->process_market_data({day_bar("AAPL", 2, 99.5)}).is_ok());
    out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(count(out), 2u) << out;
    EXPECT_NE(out.find("PM_HISTORY_REPEATED_DATE symbol=AAPL date=2026-01-04 old_close=103 "
                       "new_close=103 in_call=1"),
              std::string::npos)
        << out;
    EXPECT_NE(out.find("PM_HISTORY_REPEATED_DATE symbol=AAPL date=2026-01-03 old_close=102 "
                       "new_close=99.5 in_call=0"),
              std::string::npos)
        << out;
    ASSERT_EQ(pm->historical_returns_.count("AAPL"), 1u);
    EXPECT_EQ(pm->historical_returns_.at("AAPL").size(), 3u) << "four dates, three returns";
}

// The cap: 756 prices by default, the oldest date dropped first at the 757th. The sleeve beside
// it keeps everything (cap 0), so at the parent source the PM keeps 757 prices.
TEST_F(PmOwnPriceHistoryTest, TheDefaultCapDropsTheOldestPriceAt757) {
    auto pm = make_pm();
    ASSERT_TRUE(pm->add_strategy(make_sleeve<TrendHistorySleeve>("KEEPS_ALL", {"AAPL"}, 0), 1.0).is_ok());
    feed_days(*pm, {"AAPL"}, 0, 755);  // 756 dates
    ASSERT_EQ(pm->historical_returns_.count("AAPL"), 1u);
    EXPECT_EQ(pm->historical_returns_.at("AAPL").size(), 755u) << "756 dates: nothing is dropped";
    EXPECT_EQ(pm->historical_returns_.at("AAPL").front(),
              expected_returns(closes_for("AAPL", 0, 1)).front());

    feed_days(*pm, {"AAPL"}, 756, 756);  // the 757th date
    EXPECT_EQ(pm->historical_returns_.at("AAPL").size(), 755u)
        << "757 dates: the oldest price must be dropped, keeping 756";
    EXPECT_EQ(pm->historical_returns_.at("AAPL"), expected_returns(closes_for("AAPL", 1, 756)))
        << "the price dropped was not the oldest";
}

// A configured cap is honoured the same way (portfolio.json "covariance_history_prices").
TEST_F(PmOwnPriceHistoryTest, AConfiguredCapKeepsThatManyPrices) {
    auto pm = make_pm({{"covariance_history_prices", 25}});
    ASSERT_TRUE(pm->add_strategy(make_sleeve<TrendHistorySleeve>("KEEPS_ALL", {"AAPL"}, 0), 1.0).is_ok());
    feed_days(*pm, {"AAPL"}, 0, 39);
    ASSERT_EQ(pm->historical_returns_.count("AAPL"), 1u);
    EXPECT_EQ(pm->historical_returns_.at("AAPL"), expected_returns(closes_for("AAPL", 15, 39)))
        << "covariance_history_prices = 25 must keep the newest 25 dates";
}

// A value below 2 cannot give a return: a PortfolioConfig built in code is refused too.
TEST_F(PmOwnPriceHistoryTest, ACapBelowTwoIsRefusedByTheConstructor) {
    EXPECT_THROW(make_pm({{"covariance_history_prices", 1}}), std::invalid_argument);
    EXPECT_THROW(make_pm({{"covariance_history_prices", 0}}), std::invalid_argument);
    EXPECT_NO_THROW(make_pm({{"covariance_history_prices", 2}}));
}

// A symbol that leaves the feed keeps its series, which stops growing; the covariance still
// truncates to the shortest symbol by count, from the tail, exactly as before.
TEST_F(PmOwnPriceHistoryTest, ASymbolThatLeavesTheFeedKeepsItsSeriesAndStopsGrowing) {
    auto pm = make_pm();
    ASSERT_TRUE(pm->add_strategy(make_sleeve<TrendHistorySleeve>("TREND_FOLLOWING", {"AAPL", "MSFT"}), 1.0)
                    .is_ok());
    feed_days(*pm, {"AAPL", "MSFT"}, 0, 29);
    feed_days(*pm, {"AAPL"}, 30, 39);  // MSFT leaves the feed

    ASSERT_EQ(pm->historical_returns_.count("MSFT"), 1u) << "MSFT's series was dropped";
    EXPECT_EQ(pm->historical_returns_.at("MSFT"), expected_returns(closes_for("MSFT", 0, 29)))
        << "MSFT's series must be kept as it was when it left, and stop growing";
    EXPECT_EQ(pm->historical_returns_.at("AAPL"), expected_returns(closes_for("AAPL", 0, 39)));

    // The shortest-symbol truncation: both series cut to MSFT's 29 returns, from the tail.
    std::unordered_map<std::string, std::vector<double>> tails{
        {"AAPL", expected_returns(closes_for("AAPL", 10, 39))},
        {"MSFT", expected_returns(closes_for("MSFT", 0, 29))}};
    EXPECT_EQ(pm->calculate_covariance_matrix(pm->historical_returns_),
              pm->calculate_covariance_matrix(tails));
}

// The E3 timing: the history is updated after the strategy loop, from this call's bars. A
// cycle a strategy refused (process_market_data returns an error before the update) adds no
// price, so the next return spans the refused day.
TEST_F(PmOwnPriceHistoryTest, ARefusedCycleAddsNoPriceToTheHistory) {
    auto pm = make_pm();
    auto sleeve = make_sleeve<SwitchableSleeve>("SWITCH", {"AAPL"});
    ASSERT_TRUE(pm->add_strategy(sleeve, 1.0).is_ok());
    ASSERT_TRUE(pm->process_market_data({day_bar("AAPL", 0, 100.0)}).is_ok());
    sleeve->fail_next = true;
    ASSERT_TRUE(pm->process_market_data({day_bar("AAPL", 1, 200.0)}).is_error());
    ASSERT_TRUE(pm->process_market_data({day_bar("AAPL", 2, 110.0)}).is_ok());
    ASSERT_EQ(pm->historical_returns_.count("AAPL"), 1u);
    EXPECT_EQ(pm->historical_returns_.at("AAPL"), (std::vector<double>{0.1}))
        << "the refused day's bar entered the PM's history";
}
