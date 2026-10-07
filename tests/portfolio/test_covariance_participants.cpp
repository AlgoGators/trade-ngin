// tests/portfolio/test_covariance_participants.cpp
//
// T-7b-1 7d, the S3 follow-up (T-7a_S1_S3_CODE_REVIEW S3-1, S3-2 and §3.6; ledger
// PM-covariance-intersection-followups). S3 builds the optimizer's covariance on the INTERSECTION of
// the matrix symbols' dates. Two edges of that rule are fixed here:
//
//  * S3-1, a stale participant: one symbol whose feed stopped ends the shared window for every
//    symbol. A participant whose last usable date is more than k dates of the UNION of the
//    participants' dates behind the newest union date (k = portfolio.json
//    "covariance_stale_dates", absent means 5) is left out of the intersection and gets the
//    guarded column (0.01 variance, zero covariances: the C-20 path), with a WARN naming it.
//  * S3-2, the 20-return floor on the intersection: when the intersection gives fewer than 20
//    returns, the participant with the fewest usable dates is left out (ties: the later first
//    usable date, then the smaller symbol name), again and again until it gives 20 or one
//    participant is left, instead of the WHOLE matrix falling to the 0.01 diagonal.
//
// And the optimizer's and the risk gate's max |rho| are logged side by side once per rebalance
// (COVARIANCE_MAX_RHO, lap 1).
//
// Every test here compiles against the parent source (4c349ad5): the helper is read through
// `#define private public` as test_portfolio_manager_internals.cpp does, the stale window is set
// through PortfolioConfig::from_json, and the loader is read through AppConfig::to_json. The ones
// that describe a change are RED there; the controls (exactly k stays, a calendar gap is not a
// stale date, the KNOWN E-7 case, no line without the optimizer) pass on both.

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"

#define private public
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#undef private

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

using Book = std::unordered_map<std::string, Position>;
using Matrix = std::vector<std::vector<double>>;
using DatedCloses = std::unordered_map<std::string, std::map<int64_t, double>>;

// ---- the helper's input: date-keyed closes -------------------------------------------------

// A close for (series, day) that does not depend on which other days a symbol has.
double close_of(const std::string& series, int64_t day) {
    uint64_t h = 1469598103934665603ULL;
    for (char c : series) h = (h ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
    h ^= static_cast<uint64_t>(day) * 0x9E3779B97F4A7C15ULL;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 31;
    const double u = static_cast<double>(h >> 11) / 9007199254740992.0;
    return 100.0 + 0.5 * static_cast<double>(day % 97) + 4.0 * (u - 0.5);
}

// Days first..last inclusive, less `skip`.
std::map<int64_t, double> days(const std::string& series, int64_t first, int64_t last,
                               const std::set<int64_t>& skip = {}) {
    std::map<int64_t, double> m;
    for (int64_t d = first; d <= last; ++d) {
        if (!skip.count(d)) m[d] = close_of(series, d);
    }
    return m;
}

std::map<int64_t, double> join(std::map<int64_t, double> a, const std::map<int64_t, double>& b) {
    for (const auto& [d, c] : b) a[d] = c;
    return a;
}

// The covariance's guarded column i: 0.01 variance, zero covariances with everyone.
void expect_guarded(const Matrix& cov, size_t i, const std::string& what) {
    ASSERT_LT(i, cov.size());
    EXPECT_EQ(cov[i][i], 0.01) << what << ": the guarded column's variance";
    for (size_t j = 0; j < cov.size(); ++j) {
        if (j == i) continue;
        EXPECT_EQ(cov[i][j], 0.0) << what << ": row " << i << " col " << j;
        EXPECT_EQ(cov[j][i], 0.0) << what << ": row " << j << " col " << i;
    }
}

// cov's top-left n x n block is `ref` bit for bit.
void expect_block(const Matrix& cov, const Matrix& ref, const std::string& what) {
    ASSERT_LE(ref.size(), cov.size());
    for (size_t i = 0; i < ref.size(); ++i) {
        for (size_t j = 0; j < ref.size(); ++j) {
            EXPECT_EQ(cov[i][j], ref[i][j]) << what << ": cell (" << i << "," << j << ")";
        }
    }
}

// ---- the optimizer path: bars through process_market_data ----------------------------------

// Calendar day o counted from 2026-01-01 (a Thursday), 00:00 UTC, as futures bars are stamped.
Timestamp cal_day(int o) { return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * o)); }
int weekday_of(int o) { return (3 + o) % 7; }  // Monday 0 .. Sunday 6

std::vector<int> weekdays(int n) {
    std::vector<int> out;
    for (int o = 4; static_cast<int>(out.size()) < n; ++o) {
        if (weekday_of(o) < 5) out.push_back(o);
    }
    return out;
}

std::vector<int> last_n(const std::vector<int>& v, size_t n) {
    return std::vector<int>(v.end() - static_cast<std::ptrdiff_t>(n), v.end());
}
std::vector<int> first_n(const std::vector<int>& v, size_t n) {
    return std::vector<int>(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n));
}

struct Feed {
    std::string symbol;
    std::vector<int> days;
    double quantity{1.0};
};

std::vector<Bar> feed_bars(const std::vector<Feed>& feeds) {
    std::vector<Bar> bars;
    for (const auto& f : feeds) {
        for (int o : f.days) {
            Bar b;
            b.symbol = f.symbol;
            b.timestamp = cal_day(o);
            b.open = b.high = b.low = b.close = Decimal(close_of(f.symbol, o));
            b.volume = 1000.0;
            bars.push_back(b);
        }
    }
    return bars;
}

class ParticipantsFixedBookStrategy : public BaseStrategy {
public:
    ParticipantsFixedBookStrategy(std::string id, StrategyConfig config,
                                  std::shared_ptr<DatabaseInterface> db, Book book)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          book_(std::move(book)) {
        metadata_.name = "Covariance Participants Fixed Book Strategy";
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

// A sleeve with a trend sleeve's warm-up rule (it signals a symbol once its own history holds as
// many prices as the longest EMA window of the TREND or the FAST configuration) that holds a fixed
// book and a hand-set price count per symbol: on_data only counts the call. T-OPT E-7.
// It is NOT a TrendFollowingStrategy: a book that holds one and names no overlay sleeve is refused
// (PortfolioManager::process_market_data), and these cases are the generic step's participant
// rule, which reads a sleeve through is_signalling() alone.
template <class TrendConfig>
class ParticipantsWarmupStrategy : public BaseStrategy {
public:
    ParticipantsWarmupStrategy(std::string id, StrategyConfig config,
                               std::shared_ptr<DatabaseInterface> db, Book book)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          book_(std::move(book)) {
        metadata_.name = "Covariance Participants Warm-up Strategy";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        (void)data;
        ++calls_;
        return Result<void>();
    }
    std::unordered_map<std::string, Position> get_target_positions() const override {
        return calls_ == 0 ? Book{} : book_;
    }
    // `prices` closes in the sleeve's own history of `symbol`.
    void set_prices(const std::string& symbol, size_t prices) { prices_[symbol] = prices; }
    // The longest EMA window: the prices a trend sleeve waits for before it signals.
    size_t warmup_prices() const {
        int max_window = 0;
        for (const auto& w : trend_config_.ema_windows) max_window = std::max(max_window, w.second);
        return static_cast<size_t>(max_window);
    }
    // TrendFollowingStrategy::is_signalling's rule on the hand-set count.
    bool is_signalling(const std::string& symbol) const override {
        const auto it = prices_.find(symbol);
        return it != prices_.end() && !(it->second < warmup_prices());
    }

private:
    Book book_;
    size_t calls_{0};
    TrendConfig trend_config_{};
    std::unordered_map<std::string, size_t> prices_;
};

using TrendWarmup = ParticipantsWarmupStrategy<TrendFollowingConfig>;
// The FAST sleeve's configuration: fast_trend_following_config().
struct FastTrendConfig : TrendFollowingConfig {
    FastTrendConfig() : TrendFollowingConfig(fast_trend_following_config()) {}
};
using FastWarmup = ParticipantsWarmupStrategy<FastTrendConfig>;

PortfolioConfig participants_config(bool optimization = true, bool carver = false) {
    PortfolioConfig pc{1'000'000.0, 1.0, 0.0, optimization};
    pc.allow_fractional_positions = false;
    pc.opt_config.tau = 1.0;
    pc.opt_config.capital = 1'000'000.0;
    pc.opt_config.cost_penalty_scalar = 50.0;
    pc.opt_config.max_iterations = 100;
    pc.opt_config.convergence_threshold = 1e-6;
    pc.opt_config.use_buffering = false;
    pc.risk_config.capital = 1'000'000.0;
    pc.risk_config.var_limit = 1e6;
    pc.risk_config.jump_risk_limit = 1e6;
    pc.risk_config.max_correlation = 1.0;
    pc.risk_config.max_gross_leverage = 1e6;
    pc.risk_config.max_net_leverage = 1e6;
    pc.risk_config.confidence_level = 0.99;
    pc.risk_config.lookback_period = 252;
    pc.risk_modules = {carver ? test_carver_module(pc.risk_config) : test_none_module()};
    return pc;
}

std::vector<std::string> lines_with(const std::string& text, const std::string& tag) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(tag) != std::string::npos) out.push_back(line);
    }
    return out;
}

// "key=value" from a log line; "" when absent.
std::string field(const std::string& line, const std::string& key) {
    const std::string k = " " + key + "=";
    const auto p = line.find(k);
    if (p == std::string::npos) return "";
    const auto s = p + k.size();
    const auto e = line.find_first_of(" :", s);
    return line.substr(s, e == std::string::npos ? std::string::npos : e - s);
}

class CovarianceParticipants : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        mock_db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(mock_db_->connect().is_ok());
        set_log_level(LogLevel::WARNING);
        helper_ = std::make_unique<PortfolioManager>(participants_config(), "PM_PARTICIPANTS_H");
    }
    void TearDown() override {
        helper_.reset();
        pms_.clear();
        strategies_.clear();
        mock_db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    static void set_log_level(LogLevel level) {
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = level;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
    }

    // One PortfolioManager, one sleeve holding `quantity` of every fed symbol, every bar in one
    // process_market_data call. Returns the PM (kept alive by the fixture).
    PortfolioManager& run(const std::vector<Feed>& feeds, const PortfolioConfig& pc) {
        static int n = 0;
        ++n;
        auto pm = std::make_unique<PortfolioManager>(pc, "PM_PARTICIPANTS_" + std::to_string(n));
        Book book;
        for (const auto& f : feeds) {
            Position p;
            p.symbol = f.symbol;
            p.quantity = Decimal(f.quantity);
            p.average_price = Decimal(100.0);
            p.last_update = cal_day(0);
            book[f.symbol] = p;
        }
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto s = std::make_shared<ParticipantsFixedBookStrategy>(
            "TREND_FOLLOWING_P" + std::to_string(n), sc, mock_db_, book);
        EXPECT_TRUE(s->initialize().is_ok());
        EXPECT_TRUE(s->start().is_ok());
        EXPECT_TRUE(pm->add_strategy(s, 1.0, pc.use_optimization).is_ok());
        int last = 0;
        for (const auto& f : feeds) last = std::max(last, f.days.back());
        EXPECT_TRUE(pm->process_market_data(feed_bars(feeds), false, cal_day(last + 1)).is_ok());
        strategies_.push_back(s);
        pms_.push_back(std::move(pm));
        return *pms_.back();
    }

    // One sleeve spec for run_sleeves: a trend strategy of type S (TrendWarmup or
    // FastWarmup) at `allocation`, and each symbol's price count in ITS OWN history (a symbol not
    // listed gets the sleeve's full warm-up). The book is every fed symbol at its Feed quantity.
    struct Sleeve {
        std::string kind;  // "trend" or "fast"
        double allocation{1.0};
        std::map<std::string, long> prices_vs_warmup;  // symbol -> offset from the warm-up count
    };

    // One PortfolioManager, one or more trend sleeves (see Sleeve), every bar in one
    // process_market_data call. The PM's own history is the bars; each sleeve's is hand-set.
    PortfolioManager& run_sleeves(const std::vector<Feed>& feeds, const std::vector<Sleeve>& sleeves,
                                  const PortfolioConfig& pc) {
        static int n = 0;
        ++n;
        auto pm = std::make_unique<PortfolioManager>(pc, "PM_WARMUP_" + std::to_string(n));
        Book book;
        for (const auto& f : feeds) {
            Position p;
            p.symbol = f.symbol;
            p.quantity = Decimal(f.quantity);
            p.average_price = Decimal(100.0);
            p.last_update = cal_day(0);
            book[f.symbol] = p;
        }
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        size_t k = 0;
        for (const auto& sl : sleeves) {
            const std::string id = "TREND_WARMUP_" + std::to_string(n) + "_" + std::to_string(k++);
            std::shared_ptr<StrategyInterface> s;
            auto fill = [&](auto& strat) {
                for (const auto& f : feeds) {
                    auto it = sl.prices_vs_warmup.find(f.symbol);
                    const long offset = it == sl.prices_vs_warmup.end() ? 0 : it->second;
                    strat->set_prices(f.symbol, static_cast<size_t>(
                                                    static_cast<long>(strat->warmup_prices()) + offset));
                }
            };
            if (sl.kind == "fast") {
                auto t = std::make_shared<FastWarmup>(id, sc, mock_db_, book);
                fill(t);
                s = t;
            } else {
                auto t = std::make_shared<TrendWarmup>(id, sc, mock_db_, book);
                fill(t);
                s = t;
            }
            EXPECT_TRUE(s->initialize().is_ok());
            EXPECT_TRUE(s->start().is_ok());
            EXPECT_TRUE(pm->add_strategy(s, sl.allocation, pc.use_optimization).is_ok());
            strategies_.push_back(s);
        }
        int last = 0;
        for (const auto& f : feeds) last = std::max(last, f.days.back());
        EXPECT_TRUE(pm->process_market_data(feed_bars(feeds), false, cal_day(last + 1)).is_ok());
        pms_.push_back(std::move(pm));
        return *pms_.back();
    }

    Matrix optimizer_covariance(const std::vector<Feed>& feeds) {
        auto& pm = run(feeds, participants_config());
        EXPECT_TRUE(pm.covariance_cache_valid_) << "the optimizer did not build a covariance";
        return pm.cached_covariance_;
    }

    std::shared_ptr<MockPostgresDatabase> mock_db_;
    std::unique_ptr<PortfolioManager> helper_;
    std::vector<std::unique_ptr<PortfolioManager>> pms_;
    std::vector<std::shared_ptr<StrategyInterface>> strategies_;
};

}  // namespace

// ===== (1) the stale participant =====
//
// The stale count is in dates of the UNION of the participants' usable dates: a participant's lag
// is the number of union dates strictly after its own last usable date. More than k (5) is stale.

// A and B print days 1000..1059; C stops at 1053, six union dates (1054..1059) behind. C is left
// out of the intersection, so A's and B's series run over all 60 dates, exactly what they are
// without C; C's series is empty and its column is guarded.
//   parent: the intersection ends at C's 1053 for everyone: A and B get 53 returns, not 59.
TEST_F(CovarianceParticipants, AParticipantMoreThanKUnionDatesStaleIsExcludedAndGuarded) {
    const DatedCloses closes{{"A", days("A", 1000, 1059)},
                             {"B", days("B", 1000, 1059)},
                             {"C", days("C", 1000, 1053)}};
    const DatedCloses without_c{{"A", closes.at("A")}, {"B", closes.at("B")}};
    const auto r = helper_->date_aligned_returns(closes);
    const auto ref = helper_->date_aligned_returns(without_c);
    ASSERT_EQ(ref.at("A").size(), 59u);
    EXPECT_TRUE(r.at("C").empty()) << "C (6 union dates stale) still has " << r.at("C").size()
                                   << " returns";
    EXPECT_EQ(r.at("A"), ref.at("A"));
    EXPECT_EQ(r.at("B"), ref.at("B"));

    const Matrix cov = helper_->calculate_covariance_matrix(r);
    const Matrix ref_cov = helper_->calculate_covariance_matrix(ref);
    ASSERT_EQ(cov.size(), 3u);
    expect_guarded(cov, 2, "C");
    expect_block(cov, ref_cov, "A/B without C");
}

// At exactly k (C stops at 1054, five union dates behind) C stays, and the intersection ends at
// its last date for everyone, as under S3. A control: the parent does the same.
TEST_F(CovarianceParticipants, AParticipantExactlyKUnionDatesStaleStays) {
    const DatedCloses closes{{"A", days("A", 1000, 1059)},
                             {"B", days("B", 1000, 1059)},
                             {"C", days("C", 1000, 1054)}};
    const auto r = helper_->date_aligned_returns(closes);
    EXPECT_EQ(r.at("C").size(), 54u);
    EXPECT_EQ(r.at("A").size(), 54u);
    EXPECT_EQ(r.at("B").size(), 54u);
}

// A calendar gap nobody printed is not a stale date: A and B print 1000..1049 and 1057..1059,
// C stops at 1049. C is ten calendar days behind but only three union dates, so it stays. A
// control: the parent does the same.
TEST_F(CovarianceParticipants, StalenessCountsUnionDatesNotCalendarDays) {
    const auto gap = std::set<int64_t>{1050, 1051, 1052, 1053, 1054, 1055, 1056};
    const DatedCloses closes{{"A", days("A", 1000, 1059, gap)},
                             {"B", days("B", 1000, 1059, gap)},
                             {"C", days("C", 1000, 1049)}};
    const auto r = helper_->date_aligned_returns(closes);
    EXPECT_EQ(r.at("C").size(), 49u);
    EXPECT_EQ(r.at("A").size(), 49u);
}

// A union date counts whoever printed it: A alone prints 1055..1060, so B (stopped at 1054) is six
// union dates behind and left out; A's series is its own 60 returns.
//   parent: the intersection ends at 1054; A gets 54 returns and B is not empty.
TEST_F(CovarianceParticipants, ADatePrintedByOneParticipantIsAUnionDate) {
    const DatedCloses closes{{"A", days("A", 1000, 1060)}, {"B", days("B", 1000, 1054)}};
    const auto r = helper_->date_aligned_returns(closes);
    EXPECT_TRUE(r.at("B").empty());
    EXPECT_EQ(r.at("A").size(), 60u);
}

// k comes from portfolio.json "covariance_stale_dates" (PortfolioConfig): at 2, a participant three
// union dates behind is stale.
//   parent: the key is not read; C stays and everyone's window ends at 1056.
TEST_F(CovarianceParticipants, TheStaleWindowIsConfigurable) {
    PortfolioConfig pc = participants_config();
    pc.from_json(nlohmann::json{{"covariance_stale_dates", 2}});
    PortfolioManager pm(pc, "PM_PARTICIPANTS_K2");
    const DatedCloses closes{{"A", days("A", 1000, 1059)},
                             {"B", days("B", 1000, 1059)},
                             {"C", days("C", 1000, 1056)}};
    const auto r = pm.date_aligned_returns(closes);
    EXPECT_TRUE(r.at("C").empty()) << "C (3 union dates stale, k = 2) still has "
                                   << r.at("C").size() << " returns";
    EXPECT_EQ(r.at("A").size(), 59u);
    const auto r_default = helper_->date_aligned_returns(closes);
    EXPECT_EQ(r_default.at("C").size(), 56u) << "at the default k = 5 C stays";
}

// ===== (2) the 20-return floor on the intersection =====

// S3-2 by hand. A and B print 1000..1059 without 1045; N is new: 1039..1059, 21 closes (20 own
// returns, so the optimizer admits it) with 1045, which A and B lack. The four-way intersection is
// 20 dates, 19 returns.
//   parent: every series has 19 returns and calculate_covariance_matrix returns the 0.01 diagonal
//           for the WHOLE matrix (A/B covariance 0).
//   fix:    N is left out; A and B keep their 58 returns and their real covariance.
TEST_F(CovarianceParticipants, AShortIntersectionDropsTheShortestParticipantNotTheMatrix) {
    const std::set<int64_t> hole{1045};
    const DatedCloses closes{{"A", days("A", 1000, 1059, hole)},
                             {"B", days("B", 1000, 1059, hole)},
                             {"N", days("N", 1039, 1059)}};
    const DatedCloses without_n{{"A", closes.at("A")}, {"B", closes.at("B")}};
    const auto r = helper_->date_aligned_returns(closes);
    const auto ref = helper_->date_aligned_returns(without_n);
    ASSERT_EQ(ref.at("A").size(), 58u);
    EXPECT_TRUE(r.at("N").empty()) << "N still has " << r.at("N").size() << " returns";
    EXPECT_EQ(r.at("A"), ref.at("A"));
    EXPECT_EQ(r.at("B"), ref.at("B"));

    const Matrix cov = helper_->calculate_covariance_matrix(r);
    const Matrix ref_cov = helper_->calculate_covariance_matrix(ref);
    ASSERT_EQ(cov.size(), 3u);
    EXPECT_NE(cov[0][1], 0.0) << "the whole-matrix 0.01 diagonal fired";
    EXPECT_NE(cov[0][0], 0.01) << "the whole-matrix 0.01 diagonal fired";
    expect_guarded(cov, 2, "N");
    expect_block(cov, ref_cov, "A/B without N");
}

// Tie-break, first key: the FEWEST usable dates goes first. N1 has 23 dates (1036..1059 less
// 1050), N2 21 (1037..1059 less 1040, 1041); together 20 dates, 19 returns. N2 is left out and
// N1's 23 dates give 22 returns. (Leaving N1 out would have given N2 20.)
//   parent: nobody is left out; 19 returns each.
TEST_F(CovarianceParticipants, TheFloorLeavesOutTheFewestDatesFirst) {
    const DatedCloses closes{{"A", days("A", 1000, 1059)},
                             {"B", days("B", 1000, 1059)},
                             {"N1", days("N1", 1036, 1059, {1050})},
                             {"N2", days("N2", 1037, 1059, {1040, 1041})}};
    const auto r = helper_->date_aligned_returns(closes);
    EXPECT_TRUE(r.at("N2").empty());
    EXPECT_EQ(r.at("N1").size(), 22u);
    EXPECT_EQ(r.at("A").size(), 22u);
}

// Tie-break, second key: equal counts, the LATER first date goes first. N1 21 dates from 1036
// (1036..1059 less 1050..1052), N2 21 dates from 1039 (1039..1059); together 18 dates. N2 is left
// out; N1 keeps 20 returns.
TEST_F(CovarianceParticipants, TheFloorBreaksACountTieByTheLaterFirstDate) {
    const DatedCloses closes{{"A", days("A", 1000, 1059)},
                             {"B", days("B", 1000, 1059)},
                             {"N1", days("N1", 1036, 1059, {1050, 1051, 1052})},
                             {"N2", days("N2", 1039, 1059)}};
    const auto r = helper_->date_aligned_returns(closes);
    EXPECT_TRUE(r.at("N2").empty());
    EXPECT_EQ(r.at("N1").size(), 20u);
}

// Tie-break, last key: equal counts and equal first dates, the SMALLER symbol name goes first.
// N1 1039..1059 and N2 1039..1049 plus 1051..1060, 21 dates each from 1039; together 20 dates.
// N1 is left out; A, B (to 1060) and N2 share N2's 21 dates, 20 returns.
TEST_F(CovarianceParticipants, TheFloorBreaksAFullTieByTheSmallerSymbol) {
    const DatedCloses closes{{"A", days("A", 1000, 1060)},
                             {"B", days("B", 1000, 1060)},
                             {"N1", days("N1", 1039, 1059)},
                             {"N2", join(days("N2", 1039, 1049), days("N2", 1051, 1060))}};
    const auto r = helper_->date_aligned_returns(closes);
    EXPECT_TRUE(r.at("N1").empty());
    EXPECT_EQ(r.at("N2").size(), 20u);
}

// ===== the optimizer path =====

// S3-1 end to end: CCC has a zero target (it is in the matrix because the sleeve names it) and its
// feed stops six weekdays before the others'. The optimizer's AAA/BBB block is the two-symbol
// matrix over every date; CCC's column is guarded.
//   parent: everyone's window ends at CCC's last date.
TEST_F(CovarianceParticipants, AStaleZeroTargetSymbolNoLongerTruncatesTheOptimizersWindow) {
    const auto w = weekdays(60);
    const Matrix cov = optimizer_covariance(
        {{"AAA", w, 1.0}, {"BBB", w, 1.0}, {"CCC", first_n(w, 54), 0.0}});
    const Matrix pair = optimizer_covariance({{"AAA", w, 1.0}, {"BBB", w, 1.0}});
    ASSERT_EQ(cov.size(), 3u);
    ASSERT_EQ(pair.size(), 2u);
    expect_guarded(cov, 2, "CCC");
    expect_block(cov, pair, "AAA/BBB without CCC");
}

// S3-2 end to end (the edge of ledger OPT-new-symbol-collapses-min-periods, E-7, that the floor
// closes): NEW prints the last 20 weekdays plus one Sunday among them, 21 closes and 20 own
// returns, so the optimizer admits it; the intersection is 20 dates, 19 returns.
//   parent: the whole matrix is the 0.01 diagonal.
//   fix:    NEW is left out; AAA/BBB keep the two-symbol matrix over every date.
TEST_F(CovarianceParticipants, ANewSymbolBelowTheFloorNoLongerCollapsesTheMatrix) {
    const auto w = weekdays(60);
    auto fresh = last_n(w, 20);
    int sunday = fresh[9] + 1;
    while (weekday_of(sunday) != 6) ++sunday;
    ASSERT_LT(sunday, fresh[10]);
    fresh.push_back(sunday);
    std::sort(fresh.begin(), fresh.end());

    auto& pm = run({{"AAA", w, 1.0}, {"BBB", w, 1.0}, {"NEW", fresh, 1.0}}, participants_config());
    ASSERT_TRUE(pm.covariance_cache_valid_);
    ASSERT_EQ(pm.cached_symbols_, (std::vector<std::string>{"AAA", "BBB", "NEW"}))
        << "NEW must be admitted on its own 20 returns for the case to exist";
    const Matrix cov = pm.cached_covariance_;
    const Matrix pair = optimizer_covariance({{"AAA", w, 1.0}, {"BBB", w, 1.0}});
    EXPECT_NE(cov[0][1], 0.0) << "the whole-matrix 0.01 diagonal fired";
    expect_guarded(cov, 2, "NEW");
    expect_block(cov, pair, "AAA/BBB without NEW");
}

// ===== E-7, a symbol no strategy signals (T-OPT, ledger OPT-new-symbol-collapses-min-periods) =====
//
// NEW is a contract the trend sleeve does not signal yet: its own history holds one price fewer
// than the longest EMA window, so on_data skips it ("Waiting for enough data") and it has no
// target of its own (quantity 0 here). The PM's own history of it is 30 weekdays (29 returns),
// above the optimizer's 20-return admission and above 7d's floor, so on the parent it enters the
// optimizer and the intersection over every participant is its 30 dates: AAA's and BBB's window
// falls from 60 dates (59 returns) to 30 (29). This was the pinned KNOWN case
// (KnownANewSymbolAboveTheFloorStillSetsEveryonesWindow); it flips here.
//   parent: the matrix is AAA/BBB/NEW and the AAA/BBB block is the one over NEW's 30 dates.
//   fix:    NEW is left out of the optimizer; AAA/BBB is the two-symbol matrix over all 60 dates,
//           the logged window is 59 returns, and NEW keeps its own (zero) target.
TEST_F(CovarianceParticipants, ANewSymbolNoStrategySignalsNoLongerSetsEveryonesWindow) {
    const auto w = weekdays(60);
    const auto fresh = last_n(w, 30);
    set_log_level(LogLevel::INFO);
    ::testing::internal::CaptureStdout();
    auto& pm = run_sleeves({{"AAA", w, 1.0}, {"BBB", w, 1.0}, {"NEW", fresh, 0.0}},
                           {{"trend", 1.0, {{"NEW", -1}}}}, participants_config());
    const std::string out = ::testing::internal::GetCapturedStdout();
    set_log_level(LogLevel::WARNING);
    const Matrix pair_long = optimizer_covariance({{"AAA", w, 1.0}, {"BBB", w, 1.0}});

    ASSERT_TRUE(pm.covariance_cache_valid_) << "the optimizer did not build a covariance";
    EXPECT_EQ(pm.cached_symbols_, (std::vector<std::string>{"AAA", "BBB"}))
        << "a symbol the sleeve does not signal is still in the optimizer's matrix";
    ASSERT_EQ(pm.cached_covariance_.size(), pair_long.size());
    expect_block(pm.cached_covariance_, pair_long, "AAA/BBB over all 60 dates");

    const auto cov_lines = lines_with(out, "COVARIANCE_DATE_ALIGNED ");
    ASSERT_EQ(cov_lines.size(), 1u);
    EXPECT_EQ(field(cov_lines[0], "symbols"), "2/2") << cov_lines[0];
    EXPECT_EQ(field(cov_lines[0], "returns"), "59") << "everyone's window: " << cov_lines[0];

    const auto excluded = lines_with(out, "OPTIMIZER_NOT_SIGNALLING ");
    ASSERT_EQ(excluded.size(), 1u) << "one line per optimizer call naming the symbols left out";
    EXPECT_EQ(field(excluded[0], "count"), "1") << excluded[0];
    EXPECT_EQ(field(excluded[0], "symbols"), "NEW") << excluded[0];
    EXPECT_EQ(field(excluded[0], "nonzero_targets"), "0") << excluded[0];

    const auto positions = pm.get_strategy_positions();
    ASSERT_EQ(positions.size(), 1u);
    const auto& sleeve = positions.begin()->second;
    ASSERT_TRUE(sleeve.count("NEW"));
    EXPECT_EQ(static_cast<double>(sleeve.at("NEW").quantity), 0.0)
        << "NEW is not the optimizer's to size: it keeps the sleeve's own target";
}

// The boundary, and the residue that stays KNOWN: at EXACTLY the longest window's prices the
// sleeve signals NEW (on_data's test is `size < max_window`), so NEW is admitted and a new symbol
// the strategies DO signal still sets everyone's window to its 30 dates. Excluding a signalled
// symbol would drop a real target from the optimizer; shortening everyone's window to a newly
// listed, signalled contract's history is today's rule on both sides (T-OPT E-7 names only the
// non-signalling case). A control: the parent does the same.
TEST_F(CovarianceParticipants, KnownANewSymbolTheStrategiesSignalStillSetsEveryonesWindow) {
    const auto w = weekdays(60);
    const auto fresh = last_n(w, 30);
    auto& pm = run_sleeves({{"AAA", w, 1.0}, {"BBB", w, 1.0}, {"NEW", fresh, 1.0}},
                           {{"trend", 1.0, {{"NEW", 0}}}}, participants_config());
    const Matrix pair_short = optimizer_covariance({{"AAA", fresh, 1.0}, {"BBB", fresh, 1.0}});
    const Matrix pair_long = optimizer_covariance({{"AAA", w, 1.0}, {"BBB", w, 1.0}});
    ASSERT_TRUE(pm.covariance_cache_valid_);
    ASSERT_EQ(pm.cached_symbols_, (std::vector<std::string>{"AAA", "BBB", "NEW"}));
    expect_block(pm.cached_covariance_, pair_short, "AAA/BBB over NEW's 30 dates");
    EXPECT_NE(pm.cached_covariance_[0][0], pair_long[0][0]) << "the window is not NEW's";
}

// The FAST sleeve (BASE's 0.3 sleeve, longest EMA window 64) has the same warm-up and the same
// answer: one price short of it, NEW is left out.
//   parent: NEW is in the matrix.
TEST_F(CovarianceParticipants, ANewSymbolTheFastSleeveDoesNotSignalIsLeftOut) {
    const auto w = weekdays(60);
    const auto fresh = last_n(w, 30);
    auto& pm = run_sleeves({{"AAA", w, 1.0}, {"BBB", w, 1.0}, {"NEW", fresh, 0.0}},
                           {{"fast", 1.0, {{"NEW", -1}}}}, participants_config());
    ASSERT_TRUE(pm.covariance_cache_valid_);
    EXPECT_EQ(pm.cached_symbols_, (std::vector<std::string>{"AAA", "BBB"}));
}

// Two optimizing sleeves (BASE's shape, 0.7 trend + 0.3 fast): a symbol enters when ANY of them
// signals it. The trend sleeve is one price short on NEW, the fast sleeve has its full 64; NEW
// has the fast sleeve's target, so it stays in the optimizer. A control: the parent does the same.
TEST_F(CovarianceParticipants, ASymbolAnyOptimizingSleeveSignalsStays) {
    const auto w = weekdays(60);
    const auto fresh = last_n(w, 30);
    auto& pm = run_sleeves({{"AAA", w, 1.0}, {"BBB", w, 1.0}, {"NEW", fresh, 1.0}},
                           {{"trend", 0.7, {{"NEW", -1}}}, {"fast", 0.3, {{"NEW", 0}}}},
                           participants_config());
    ASSERT_TRUE(pm.covariance_cache_valid_);
    EXPECT_EQ(pm.cached_symbols_, (std::vector<std::string>{"AAA", "BBB", "NEW"}));
}

// ... and it is left out only when NO optimizing sleeve signals it: both one price short.
//   parent: NEW is in the matrix.
TEST_F(CovarianceParticipants, ASymbolNoOptimizingSleeveSignalsIsLeftOut) {
    const auto w = weekdays(60);
    const auto fresh = last_n(w, 30);
    auto& pm = run_sleeves({{"AAA", w, 1.0}, {"BBB", w, 1.0}, {"NEW", fresh, 0.0}},
                           {{"trend", 0.7, {{"NEW", -1}}}, {"fast", 0.3, {{"NEW", -1}}}},
                           participants_config());
    ASSERT_TRUE(pm.covariance_cache_valid_);
    EXPECT_EQ(pm.cached_symbols_, (std::vector<std::string>{"AAA", "BBB"}));
}

// While NO symbol is signalled (the backtest's warm-up) the optimizer builds no matrix and every
// sleeve keeps its own (zero) target.
//   parent: the optimizer builds the three-symbol matrix from the PM's history.
TEST_F(CovarianceParticipants, NoMatrixWhileNoSymbolIsSignalled) {
    const auto w = weekdays(60);
    auto& pm = run_sleeves({{"AAA", w, 0.0}, {"BBB", w, 0.0}, {"CCC", w, 0.0}},
                           {{"trend", 1.0, {{"AAA", -1}, {"BBB", -1}, {"CCC", -1}}}},
                           participants_config());
    EXPECT_FALSE(pm.covariance_cache_valid_) << "a matrix over symbols nobody signals";
    for (const auto& [sid, book] : pm.get_strategy_positions()) {
        for (const auto& [symbol, p] : book) {
            EXPECT_EQ(static_cast<double>(p.quantity), 0.0) << sid << " " << symbol;
        }
    }
}

// ===== (3) max |rho|, optimizer and gate, one line per rebalance =====

// With the optimizer and the Carver gate on, each rebalance prints exactly one COVARIANCE_MAX_RHO
// line. optimizer= is max |rho| of the optimizer's matrix over the pairs of lap 1's book (both
// legs held), gate= is the Carver gate's own max |rho| on that book (RiskResult.correlation_risk).
//   parent: no such line.
TEST_F(CovarianceParticipants, OneMaxRhoLinePerRebalanceCarriesTheOptimizersAndTheGates) {
    const auto w = weekdays(60);
    set_log_level(LogLevel::INFO);
    ::testing::internal::CaptureStdout();
    auto& pm = run({{"AAA", w, 1.0}, {"BBB", w, 1.0}, {"CCC", w, 1.0}},
                   participants_config(/*optimization=*/true, /*carver=*/true));
    const std::string out = ::testing::internal::GetCapturedStdout();
    set_log_level(LogLevel::WARNING);

    const auto lines = lines_with(out, "COVARIANCE_MAX_RHO ");
    ASSERT_EQ(lines.size(), 1u) << "one line per rebalance";
    const std::string& line = lines[0];

    // The expected optimizer value: the held pairs of the book the gate measured. The gate
    // cut nothing (every limit is out of reach), so that book is the stored one.
    ASSERT_TRUE(pm.covariance_cache_valid_);
    const auto positions = pm.get_strategy_positions();
    std::set<std::string> held;
    for (const auto& [sid, book] : positions) {
        for (const auto& [symbol, p] : book) {
            if (std::abs(static_cast<double>(p.quantity)) > 1e-12) held.insert(symbol);
        }
    }
    ASSERT_GE(held.size(), 2u) << "the case needs at least one held pair";
    const auto& syms = pm.cached_symbols_;
    const auto& cov = pm.cached_covariance_;
    double expected = 0.0;
    for (size_t i = 0; i < syms.size(); ++i) {
        for (size_t j = i + 1; j < syms.size(); ++j) {
            if (!held.count(syms[i]) || !held.count(syms[j])) continue;
            expected = std::max(expected,
                                std::abs(cov[i][j] / std::sqrt(cov[i][i] * cov[j][j])));
        }
    }
    const std::string opt = field(line, "optimizer");
    ASSERT_FALSE(opt.empty()) << line;
    EXPECT_NEAR(std::stod(opt), expected, 1e-6) << line;
    EXPECT_EQ(field(line, "held"), std::to_string(held.size())) << line;

    double gate_expected = -1.0;
    for (const auto& rec : pm.last_risk_decisions()) {
        if (rec.lap == 1 && rec.requested.metrics.has_value()) {
            gate_expected = rec.requested.metrics->correlation_risk;
            break;
        }
    }
    ASSERT_GE(gate_expected, 0.0) << "no lap-1 gate reading recorded";
    const std::string gate = field(line, "gate");
    ASSERT_FALSE(gate.empty()) << line;
    EXPECT_NEAR(std::stod(gate), gate_expected, 1e-6) << line;
    EXPECT_EQ(field(line, "gate_module"), "carver") << line;
}

// Without the optimizer there is no optimizer matrix, and no line (the equity books). A control.
TEST_F(CovarianceParticipants, NoMaxRhoLineWithoutTheOptimizer) {
    const auto w = weekdays(60);
    set_log_level(LogLevel::INFO);
    ::testing::internal::CaptureStdout();
    run({{"AAA", w, 1.0}, {"BBB", w, 1.0}},
        participants_config(/*optimization=*/false, /*carver=*/true));
    const std::string out = ::testing::internal::GetCapturedStdout();
    set_log_level(LogLevel::WARNING);
    EXPECT_TRUE(lines_with(out, "COVARIANCE_MAX_RHO").empty());
}

// ===== (4) the Carver gate's window: the E-7 mechanism (T-7b-2 CGW) =====
//
// The gate's window keeps the last lookback_period dates of every bar it was fed, and F5 keeps only
// the dates on which EVERY symbol of that window printed. Before CGW the intersection ran over every
// symbol with a bar, so a symbol no strategy signals, targets or holds (a contract still warming up)
// cut every other symbol's window to its own dates, exactly as it did the optimizer's before E-7.
// Since CGW the intersection (and the gate's MarketData) runs over the gate's participants: a symbol
// some strategy lists and signals, or one any strategy targets or holds non-zero. 150 weekdays keep
// F5 engaged (floor 120).

// NEW prints only the last 130 of 150 weekdays; the trend sleeve is one price short of signalling it
// and its target is 0. It is left out of the gate's window: the gate reads AAA and BBB over all 150
// dates, and one GATE_NOT_SIGNALLING line names NEW.
//   parent: the gate reads 3 symbols over NEW's 130 dates (20 dropped) and prints no such line.
TEST_F(CovarianceParticipants, AGateWindowLeavesOutASymbolNoStrategySignalsTargetsOrHolds) {
    const auto w = weekdays(150);
    const auto fresh = last_n(w, 130);
    set_log_level(LogLevel::INFO);
    ::testing::internal::CaptureStdout();
    run_sleeves({{"AAA", w, 1.0}, {"BBB", w, 1.0}, {"NEW", fresh, 0.0}},
                {{"trend", 1.0, {{"NEW", -1}}}},
                participants_config(/*optimization=*/true, /*carver=*/true));
    const std::string out = ::testing::internal::GetCapturedStdout();
    set_log_level(LogLevel::WARNING);

    const auto win = lines_with(out, "T4_RISK_WINDOW ");
    ASSERT_FALSE(win.empty()) << out;
    for (const auto& line : win) {
        EXPECT_EQ(field(line, "symbols"), "2") << line;
        EXPECT_EQ(field(line, "dates"), "150") << "every date, NEW's missing 20 kept: " << line;
        EXPECT_EQ(field(line, "dates_dropped"), "0") << line;
        EXPECT_EQ(field(line, "f5_engaged"), "1") << line;
    }
    const auto left = lines_with(out, "GATE_NOT_SIGNALLING ");
    ASSERT_EQ(left.size(), 1u) << "one line per rebalance naming the symbols left out:\n" << out;
    EXPECT_EQ(field(left[0], "count"), "1") << left[0];
    EXPECT_EQ(field(left[0], "symbols"), "NEW") << left[0];
}

// A symbol no strategy signals but a strategy TARGETS (the sleeve's target stands while it warms up)
// stays in the gate's window: the book can hold it, so the gate must measure it. A control: the
// parent does the same (every symbol is in).
TEST_F(CovarianceParticipants, AGateWindowKeepsATargetedSymbolNoStrategySignals) {
    const auto w = weekdays(150);
    const auto fresh = last_n(w, 130);
    set_log_level(LogLevel::INFO);
    ::testing::internal::CaptureStdout();
    run_sleeves({{"AAA", w, 1.0}, {"BBB", w, 1.0}, {"NEW", fresh, 1.0}},
                {{"trend", 1.0, {{"NEW", -1}}}},
                participants_config(/*optimization=*/true, /*carver=*/true));
    const std::string out = ::testing::internal::GetCapturedStdout();
    set_log_level(LogLevel::WARNING);
    const auto win = lines_with(out, "T4_RISK_WINDOW ");
    ASSERT_FALSE(win.empty()) << out;
    for (const auto& line : win) {
        EXPECT_EQ(field(line, "symbols"), "3") << line;
        EXPECT_EQ(field(line, "dates"), "130") << line;
    }
    EXPECT_TRUE(lines_with(out, "GATE_NOT_SIGNALLING ").empty()) << out;
    EXPECT_TRUE(lines_with(out, "POSGUARD_MISS").empty()) << "every held symbol maps: " << out;
}

// The residue that stays KNOWN, as for the optimizer (E-7): a newly listed contract the strategies DO
// signal still sets everyone's window to its own dates, because the book can hold it. A control: the
// parent does the same.
TEST_F(CovarianceParticipants, KnownANewSymbolTheStrategiesSignalStillSetsTheGatesWindow) {
    const auto w = weekdays(150);
    const auto fresh = last_n(w, 130);
    set_log_level(LogLevel::INFO);
    ::testing::internal::CaptureStdout();
    run_sleeves({{"AAA", w, 1.0}, {"BBB", w, 1.0}, {"NEW", fresh, 0.0}},
                {{"trend", 1.0, {{"NEW", 0}}}},
                participants_config(/*optimization=*/true, /*carver=*/true));
    const std::string out = ::testing::internal::GetCapturedStdout();
    set_log_level(LogLevel::WARNING);
    const auto win = lines_with(out, "T4_RISK_WINDOW ");
    ASSERT_FALSE(win.empty()) << out;
    for (const auto& line : win) {
        EXPECT_EQ(field(line, "symbols"), "3") << line;
        EXPECT_EQ(field(line, "dates"), "130") << "NEW's dates for everyone: " << line;
        EXPECT_EQ(field(line, "dates_dropped"), "20") << line;
    }
    EXPECT_TRUE(lines_with(out, "GATE_NOT_SIGNALLING ").empty()) << out;
}

// Two sleeves (BASE's shape): NEW stays in the gate's window when ANY sleeve signals it, and is left
// out only when none does. Here the fast sleeve signals it.
//   A control for the first half: the parent keeps it too.
TEST_F(CovarianceParticipants, AGateWindowKeepsASymbolAnySleeveSignals) {
    const auto w = weekdays(150);
    const auto fresh = last_n(w, 130);
    set_log_level(LogLevel::INFO);
    ::testing::internal::CaptureStdout();
    run_sleeves({{"AAA", w, 1.0}, {"BBB", w, 1.0}, {"NEW", fresh, 0.0}},
                {{"trend", 0.7, {{"NEW", -1}}}, {"fast", 0.3, {{"NEW", 0}}}},
                participants_config(/*optimization=*/true, /*carver=*/true));
    const std::string out = ::testing::internal::GetCapturedStdout();
    set_log_level(LogLevel::WARNING);
    const auto win = lines_with(out, "T4_RISK_WINDOW ");
    ASSERT_FALSE(win.empty()) << out;
    for (const auto& line : win) EXPECT_EQ(field(line, "symbols"), "3") << line;
}
