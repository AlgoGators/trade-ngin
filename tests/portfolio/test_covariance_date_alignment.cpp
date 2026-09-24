// tests/portfolio/test_covariance_date_alignment.cpp
//
// T-7a INSERT S3, the optimizer's covariance aligned by DATE (ledger PM-covariance-count-aligned).
// Before S3, PortfolioManager::calculate_covariance_matrix took the last min_periods returns of each
// symbol by COUNT (min_periods = the shortest symbol's return count), so a symbol whose date set
// differs from the others' (a feed gap, a Sunday-stamped bar, MBT's weekend bars) had its k-th-last
// return paired with a different day of every other symbol. Measured on the clone for 2026-05-01:
// 475 of the 480 MBT/MES rows paired different dates.
//
// After S3 the series are built on the INTERSECTION of the dates the symbols in the matrix have: a
// date one symbol lacks is dropped for all, and the return on the next shared date runs from the
// previous SHARED date for every symbol, so each return spans the same interval for every symbol.
//
// These tests drive a real PortfolioManager with the optimizer on and read the covariance the
// optimizer was handed (cached_covariance_, cached_symbols_), which exist before and after the
// fix, so the same file is RED on the parent source and GREEN on the fix. Every bar of a run is
// fed in one process_market_data call; a symbol's close on a calendar day is the same whichever
// other days it has (one random walk per series over every calendar day).

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
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

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

using Book = std::unordered_map<std::string, Position>;
using Matrix = std::vector<std::vector<double>>;

// Calendar day o counted from 2026-01-01 (a Thursday), 00:00 UTC, as futures bars are stamped.
Timestamp cal_day(int o) { return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * o)); }
int weekday_of(int o) { return (3 + o) % 7; }  // Monday 0 .. Sunday 6

// The first n weekdays from Monday 2026-01-05 (o = 4).
std::vector<int> weekdays(int n) {
    std::vector<int> out;
    for (int o = 4; static_cast<int>(out.size()) < n; ++o) {
        if (weekday_of(o) < 5) out.push_back(o);
    }
    return out;
}

// The close of a series on every calendar day 0..199: a seeded random walk, so a close depends
// only on (series, day), never on which other days a symbol is given.
double walk_close(const std::string& series, int o) {
    static std::map<std::string, std::vector<double>> cache;
    auto it = cache.find(series);
    if (it == cache.end()) {
        uint64_t state = 1469598103934665603ULL;
        for (char c : series) state = (state ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
        std::vector<double> closes;
        double price = 100.0;
        for (int d = 0; d < 200; ++d) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            const double u = static_cast<double>(state >> 11) / 9007199254740992.0;
            price *= 1.0 + 0.04 * (u - 0.5);
            closes.push_back(price);
        }
        it = cache.emplace(series, std::move(closes)).first;
    }
    return it->second.at(static_cast<size_t>(o));
}

// One symbol's bars: its calendar days and the series its closes are read from.
struct Feed {
    std::string symbol;
    std::vector<int> days;
    std::string series;  // defaults to the symbol
};

std::vector<Bar> feed_bars(const std::vector<Feed>& feeds) {
    std::vector<Bar> bars;
    for (const auto& f : feeds) {
        const std::string& series = f.series.empty() ? f.symbol : f.series;
        for (int o : f.days) {
            const double c = walk_close(series, o);
            Bar b;
            b.symbol = f.symbol;
            b.timestamp = cal_day(o);
            b.open = b.high = b.low = b.close = Decimal(c);
            b.volume = 1000.0;
            bars.push_back(b);
        }
    }
    return bars;
}

std::vector<int> without(std::vector<int> days, const std::set<int>& drop) {
    days.erase(std::remove_if(days.begin(), days.end(), [&](int o) { return drop.count(o) > 0; }),
               days.end());
    return days;
}

std::vector<int> with(std::vector<int> days, const std::set<int>& add) {
    for (int o : add) days.push_back(o);
    std::sort(days.begin(), days.end());
    return days;
}

class DateAlignFixedBookStrategy : public BaseStrategy {
public:
    DateAlignFixedBookStrategy(std::string id, StrategyConfig config,
                               std::shared_ptr<DatabaseInterface> db, Book book)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          book_(std::move(book)) {
        metadata_.name = "Date Align Fixed Book Strategy";
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

PortfolioConfig align_config() {
    PortfolioConfig pc{1'000'000.0, 1.0, 0.0, /*use_optimization=*/true};
    pc.allow_fractional_positions = false;
    pc.opt_config.tau = 1.0;
    pc.opt_config.capital = 1'000'000.0;
    pc.opt_config.cost_penalty_scalar = 50.0;
    pc.opt_config.max_iterations = 100;
    pc.opt_config.convergence_threshold = 1e-6;
    pc.opt_config.use_buffering = false;
    pc.risk_config.capital = 1'000'000.0;
    pc.risk_modules = {test_none_module()};
    return pc;
}

double correlation(const Matrix& cov, size_t i, size_t j) {
    return cov[i][j] / std::sqrt(cov[i][i] * cov[j][j]);
}

class CovarianceDateAlignment : public TestBase {
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
        pms_.clear();
        strategies_.clear();
        mock_db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    // One PortfolioManager, one sleeve wanting one contract of every symbol, every bar in one
    // call: returns the covariance the optimizer was handed, over the symbols in sorted order.
    Matrix optimizer_covariance(const std::vector<Feed>& feeds) {
        static int n = 0;
        ++n;
        auto pm = std::make_unique<PortfolioManager>(align_config(),
                                                     "PM_COVDATE_" + std::to_string(n));
        Book book;
        std::vector<std::string> symbols;
        for (const auto& f : feeds) {
            Position p;
            p.symbol = f.symbol;
            p.quantity = Decimal(1.0);
            p.average_price = Decimal(100.0);
            p.last_update = cal_day(0);
            book[f.symbol] = p;
            symbols.push_back(f.symbol);
        }
        std::sort(symbols.begin(), symbols.end());
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto s = std::make_shared<DateAlignFixedBookStrategy>("TREND_FOLLOWING_" + std::to_string(n),
                                                              sc, mock_db_, book);
        EXPECT_TRUE(s->initialize().is_ok());
        EXPECT_TRUE(s->start().is_ok());
        EXPECT_TRUE(pm->add_strategy(s, 1.0, /*use_optimization=*/true).is_ok());
        int last = 0;
        for (const auto& f : feeds) last = std::max(last, f.days.back());
        EXPECT_TRUE(pm->process_market_data(feed_bars(feeds), false, cal_day(last + 1)).is_ok());
        EXPECT_TRUE(pm->covariance_cache_valid_) << "the optimizer did not build a covariance";
        EXPECT_EQ(pm->cached_symbols_, symbols);
        Matrix cov = pm->cached_covariance_;
        strategies_.push_back(s);
        pms_.push_back(std::move(pm));
        return cov;
    }

    std::shared_ptr<MockPostgresDatabase> mock_db_;
    std::vector<std::unique_ptr<PortfolioManager>> pms_;
    std::vector<std::shared_ptr<StrategyInterface>> strategies_;
};

}  // namespace

// An extra weekend date on ONE symbol changes nothing. AAA and BBB on 45 weekdays; then the same,
// with AAA also printing on the Sunday after its 20th weekday.
//   before: AAA has 45 returns (Fri->Sun and Sun->Mon among them), BBB 44; min_periods 44, and
//           AAA's last 44 returns pair Sun->Mon with BBB's Fri->Mon and every older AAA return one
//           row later than BBB's: a different matrix.
//   after:  the Sunday is not in BBB's dates, so it is dropped; AAA's return on that Monday runs
//           from the Friday, exactly as without the Sunday: the same matrix, bit for bit.
TEST_F(CovarianceDateAlignment, AnExtraWeekendDateOnOneSymbolGivesTheSameCorrelation) {
    const auto w = weekdays(45);
    int sunday = w[19] + 1;
    while (weekday_of(sunday) != 6) ++sunday;
    ASSERT_LT(sunday, w[20]);

    const Matrix plain = optimizer_covariance({{"AAA", w, ""}, {"BBB", w, ""}});
    const Matrix weekend = optimizer_covariance({{"AAA", with(w, {sunday}), ""}, {"BBB", w, ""}});

    ASSERT_EQ(plain.size(), 2u);
    ASSERT_EQ(weekend.size(), 2u);
    EXPECT_EQ(correlation(weekend, 0, 1), correlation(plain, 0, 1))
        << "AAA/BBB correlation with AAA's extra Sunday bar " << correlation(weekend, 0, 1)
        << " vs without it " << correlation(plain, 0, 1);
    EXPECT_EQ(weekend, plain) << "the whole matrix must be the one without the weekend bar";
}

// A symbol missing a weekday shifts nothing for the others beyond dropping that day. AAA, BBB, CCC
// on 45 weekdays, CCC without the 26th. The reference is the same three series with that day
// removed from ALL of them; the AAA/BBB block is also the two-symbol matrix without that day.
//   before: CCC has 43 returns, AAA and BBB 44; AAA and BBB lose their FIRST return instead of the
//           gap day and keep two returns (into and out of the gap day) where CCC has one, so from
//           the gap back every CCC return is paired with the other two's previous day.
//   after:  the gap day is dropped for all; every other day is paired with itself.
TEST_F(CovarianceDateAlignment, AMissingWeekdayOnOneSymbolOnlyDropsThatDayForTheOthers) {
    const auto w = weekdays(45);
    const std::set<int> gap{w[25]};

    const Matrix with_gap =
        optimizer_covariance({{"AAA", w, ""}, {"BBB", w, ""}, {"CCC", without(w, gap), ""}});
    const Matrix reference = optimizer_covariance(
        {{"AAA", without(w, gap), ""}, {"BBB", without(w, gap), ""}, {"CCC", without(w, gap), ""}});
    const Matrix pair_only =
        optimizer_covariance({{"AAA", without(w, gap), ""}, {"BBB", without(w, gap), ""}});

    ASSERT_EQ(with_gap.size(), 3u);
    EXPECT_EQ(with_gap, reference)
        << "CCC's missing day must drop that day for all and change nothing else";
    for (size_t i = 0; i < 2; ++i) {
        for (size_t j = 0; j < 2; ++j) {
            EXPECT_EQ(with_gap[i][j], pair_only[i][j])
                << "AAA/BBB cell (" << i << "," << j << ") moved beyond dropping the gap day";
        }
    }
}

// The pairing is by date. BBB is AAA's own price series (the same close on every date both
// have), but BBB lacks a whole week (the MES 2026-04-13..17 shape) and AAA has one Sunday bar
// (the Sunday-stamped bar each side had in the clone's window).
//   before: pairing by count, AAA's returns are paired with BBB's from five and six rows away:
//           the correlation of a series with itself comes out far from 1.
//   after:  on the dates both have, the two series are identical, so every return pair is
//           equal and the correlation is exactly 1: cov(AAA,BBB) == var(AAA) == var(BBB).
TEST_F(CovarianceDateAlignment, ReturnsArePairedByDate) {
    const auto w = weekdays(45);
    const std::set<int> gap_week{w[15], w[16], w[17], w[18], w[19]};
    int sunday = w[34] + 1;
    while (weekday_of(sunday) != 6) ++sunday;
    ASSERT_LT(sunday, w[35]);

    const Matrix cov = optimizer_covariance(
        {{"AAA", with(w, {sunday}), "SAME"}, {"BBB", without(w, gap_week), "SAME"}});

    ASSERT_EQ(cov.size(), 2u);
    EXPECT_EQ(cov[0][1], cov[0][0]) << "correlation " << correlation(cov, 0, 1)
                                    << ": BBB's returns were not paired with AAA's same dates";
    EXPECT_EQ(cov[1][1], cov[0][0]);
    EXPECT_EQ(cov[1][0], cov[0][1]);
}
