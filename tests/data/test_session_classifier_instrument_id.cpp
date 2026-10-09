// The session classifier's instrument-id continuity limb (T-7b-2 C10a; HD 2026-09-24 ruling 16:
// "a bar whose instrument id differs from the previous and the next session's is JUNK").
//
// The verdict of the bar dated D reads the bars dated <= D only. Live classifies T-1 before T's
// bar exists; the backtest holds the cycle's group before it classifies the signal group, and must
// not use it. So on its own day an id change is UNCONFIRMED and held (JUNK, the default
// id_change_hold_fraction, ruled 0.10: a change printing below 0.10 of the symbol's norm), and
// the next session's bar confirms it a roll or a one-day flip
// (id_note). The norm's SESSION flags read each earlier bar with that hindsight.
//
// Three layers: the limb on constructed series; the query builder and the feed helper; and a
// database test (TRADE_NGIN_TEST_DSN only, read-only) on the clone's real bars and ids: 6E, 6J, ZN
// on 2025-11-05 and 2025-09-03, and the 2026 table against the Python reference
// (T-7b-2_evidence/tooling/c10_idflip.py, analysis/c10/C10a_2026_counts.txt).

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/market_data_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/session_classifier.hpp"

using namespace trade_ngin;
using Day = SessionClassifier::Day;

namespace {

constexpr auto npos = std::string::npos;

Day ymd_day(const std::string& s) {
    const int y = std::stoi(s.substr(0, 4));
    const unsigned m = static_cast<unsigned>(std::stoi(s.substr(5, 2)));
    const unsigned d = static_cast<unsigned>(std::stoi(s.substr(8, 2)));
    return std::chrono::sys_days{std::chrono::year{y} / std::chrono::month{m} / std::chrono::day{d}};
}

Bar bar_on(const std::string& symbol, const std::string& date, double v) {
    return Bar(Timestamp(ymd_day(date)) + std::chrono::hours(5), 100.0, 101.0, 99.0, 100.5, v,
               symbol);
}

const HolidayLookup kNoHolidays = [](const std::string&) -> std::optional<HolidayInfo> {
    return std::nullopt;
};

/// One bar and its id.
void add(SessionClassifier& c, const std::string& symbol, const std::string& date, double v,
         const std::string& id) {
    c.add_bar(bar_on(symbol, date, v));
    if (!id.empty()) c.add_instrument_id(symbol, ymd_day(date), id);
}

/// `n` weekday bars of volume `v` and id `id` ending the weekday before `last_exclusive`.
void add_history(SessionClassifier& c, const std::string& symbol, const std::string& last_exclusive,
                 int n, double v, const std::string& id) {
    Day d = ymd_day(last_exclusive);
    int added = 0;
    while (added < n) {
        d -= std::chrono::days{1};
        const std::chrono::weekday wd{d};
        if (wd == std::chrono::Saturday || wd == std::chrono::Sunday) continue;
        add(c, symbol, SessionClassifier::ymd(d), v, id);
        ++added;
    }
}

SymbolDayVerdict at(const SessionClassifier& c, const std::string& symbol,
                    const std::string& date) {
    return c.classify_symbol_day(symbol, ymd_day(date), kNoHolidays);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The limb (2026-03-02 is a Monday)
// ---------------------------------------------------------------------------------------------

// A thin flip: 5,000 lots against a norm of 100,000 is above both volume cuts (1 % and the
// 1,000-lot ceiling), so the volume limbs call it a session, but below the ruled 0.10.
TEST(InstrumentIdLimb, AOneDayFlipIsHeldOnItsDayAndConfirmedByTheNextSession) {
    SessionClassifier c;
    add_history(c, "F", "2026-03-04", 20, 100000, "A");
    add(c, "F", "2026-03-04", 5000, "X");

    // Live's view of T-1 = 03-04: no later bar exists.
    const auto day_of = at(c, "F", "2026-03-04");
    EXPECT_EQ(day_of.verdict, SessionVerdict::JUNK) << day_of.reason;
    EXPECT_NE(day_of.reason.find("instrument id change (A -> X, established A) not confirmed "
                                 "until the next session"),
              npos)
        << day_of.reason;
    EXPECT_EQ(day_of.instrument_id, "X");
    EXPECT_EQ(day_of.previous_instrument_id, "A");
    EXPECT_TRUE(day_of.id_note.empty());

    // The backtest's view: the next group (03-05, back on A) is already in the classifier. The
    // verdict of 03-04 does not read it.
    add(c, "F", "2026-03-05", 100000, "A");
    const auto with_next = at(c, "F", "2026-03-04");
    EXPECT_EQ(with_next.verdict, SessionVerdict::JUNK);
    EXPECT_EQ(with_next.reason, day_of.reason);

    // 03-05 is back on the established id: not a change, a session, and it confirms the flip.
    const auto next = at(c, "F", "2026-03-05");
    EXPECT_EQ(next.verdict, SessionVerdict::SESSION) << next.reason;
    EXPECT_NE(next.id_note.find("the previous session's instrument id change on 2026-03-04 "
                                "(A -> X) is confirmed a ONE-DAY FLIP: this session is back on A"),
              npos)
        << next.id_note;
}

TEST(InstrumentIdLimb, AFullVolumeRollTradesOnItsDayAndIsConfirmedARoll) {
    // HD 2026-09-25: a change at full volume (a roll's usual first day, as the volume leader) is
    // not held; the next session still confirms it.
    SessionClassifier c;
    add_history(c, "F", "2026-03-04", 20, 100000, "A");
    add(c, "F", "2026-03-04", 100000, "B");
    add(c, "F", "2026-03-05", 100000, "B");
    add(c, "F", "2026-03-06", 100000, "B");
    const auto first = at(c, "F", "2026-03-04");
    EXPECT_EQ(first.verdict, SessionVerdict::SESSION) << first.reason;
    EXPECT_EQ(first.instrument_id, "B");
    const auto second = at(c, "F", "2026-03-05");
    EXPECT_EQ(second.verdict, SessionVerdict::SESSION) << second.reason;
    EXPECT_NE(second.id_note.find("is confirmed a ROLL: this session keeps B"), npos)
        << second.id_note;
    const auto third = at(c, "F", "2026-03-06");
    EXPECT_EQ(third.verdict, SessionVerdict::SESSION);
    EXPECT_TRUE(third.id_note.empty()) << third.id_note;
}

TEST(InstrumentIdLimb, AThinRollIsHeldOnItsFirstDayOnly) {
    // A roll whose first day prints thin (a Sunday session on the new contract) is held on it:
    // on the day it cannot be told from a flip.
    SessionClassifier c;
    add_history(c, "F", "2026-03-04", 20, 100000, "A");
    add(c, "F", "2026-03-04", 5000, "B");
    add(c, "F", "2026-03-05", 100000, "B");
    EXPECT_EQ(at(c, "F", "2026-03-04").verdict, SessionVerdict::JUNK);
    const auto second = at(c, "F", "2026-03-05");
    EXPECT_EQ(second.verdict, SessionVerdict::SESSION) << second.reason;
    EXPECT_NE(second.id_note.find("is confirmed a ROLL: this session keeps B"), npos)
        << second.id_note;
}

TEST(InstrumentIdLimb, ATwoDayExcursionIsTwoChangesNotAFlip) {
    // HD's rule is the one-day flip: X on two sessions differs from its neighbour on one side
    // only, so each end of the excursion is an id change (each held on its day when thin).
    SessionClassifier c;
    add_history(c, "F", "2026-03-04", 20, 100000, "A");
    add(c, "F", "2026-03-04", 5000, "X");
    add(c, "F", "2026-03-05", 5000, "X");
    add(c, "F", "2026-03-06", 5000, "A");
    add(c, "F", "2026-03-09", 100000, "A");
    EXPECT_EQ(at(c, "F", "2026-03-04").verdict, SessionVerdict::JUNK);
    EXPECT_EQ(at(c, "F", "2026-03-05").verdict, SessionVerdict::SESSION);
    EXPECT_EQ(at(c, "F", "2026-03-06").verdict, SessionVerdict::JUNK);
    const auto back = at(c, "F", "2026-03-09");
    EXPECT_EQ(back.verdict, SessionVerdict::SESSION);
    EXPECT_NE(back.id_note.find("is confirmed a ROLL: this session keeps A"), npos)
        << back.id_note;
}

TEST(InstrumentIdLimb, AThirdIdAfterAFlipIsAChangeAgainstTheEstablishedId) {
    SessionClassifier c;
    add_history(c, "F", "2026-03-04", 20, 100000, "A");
    add(c, "F", "2026-03-04", 5000, "X");
    add(c, "F", "2026-03-05", 5000, "Y");
    const auto v = at(c, "F", "2026-03-05");
    EXPECT_EQ(v.verdict, SessionVerdict::JUNK) << v.reason;
    EXPECT_NE(v.reason.find("(X -> Y, established A)"), npos) << v.reason;
    EXPECT_NE(v.id_note.find("(A -> X) is confirmed a ONE-DAY FLIP, and this session prints a "
                             "third id Y"),
              npos)
        << v.id_note;
}

TEST(InstrumentIdLimb, AConfirmedFlipDoesNotTeachTheNorm) {
    // norm over the 2 trailing weekday SESSION bars. The flip on 03-04 prints 100,000 lots
    // against a symbol that trades 100.
    SessionClassifierConfig cfg;
    cfg.norm_window_bars = 2;
    auto build = [&](bool with_ids) {
        SessionClassifier c(cfg);
        add(c, "F", "2026-03-02", 100, with_ids ? "A" : "");
        add(c, "F", "2026-03-03", 100, with_ids ? "A" : "");
        add(c, "F", "2026-03-04", 100000, with_ids ? "X" : "");
        add(c, "F", "2026-03-05", 100, with_ids ? "A" : "");
        add(c, "F", "2026-03-06", 90, with_ids ? "A" : "");
        return c;
    };
    // Without ids (the parent's classifier) the flip is a session and teaches the norm:
    // median(100, 100000) = 50,050, so 90 lots on 03-06 is a corrupt print.
    const auto no_ids = build(false);
    const auto a = at(no_ids, "F", "2026-03-06");
    ASSERT_TRUE(a.norm.has_value());
    EXPECT_DOUBLE_EQ(*a.norm, 50050.0);
    EXPECT_EQ(a.verdict, SessionVerdict::JUNK);
    // With ids, the flip is confirmed by 03-05 and never counts: median(100 on 03-05, 100 on
    // 03-03) = 100, and 90 lots is a session.
    const auto ids = build(true);
    const auto b = at(ids, "F", "2026-03-06");
    ASSERT_TRUE(b.norm.has_value());
    EXPECT_DOUBLE_EQ(*b.norm, 100.0);
    EXPECT_EQ(b.verdict, SessionVerdict::SESSION) << b.reason;
}

TEST(InstrumentIdLimb, IdsAddedAfterAClassificationInvalidateTheCachedFlags) {
    SessionClassifierConfig cfg;
    cfg.norm_window_bars = 2;
    SessionClassifier c(cfg);
    for (const auto& [d, v] : std::vector<std::pair<std::string, double>>{
             {"2026-03-02", 100}, {"2026-03-03", 100}, {"2026-03-04", 100000},
             {"2026-03-05", 100}, {"2026-03-06", 90}}) {
        c.add_bar(bar_on("F", d, v));
    }
    EXPECT_EQ(at(c, "F", "2026-03-06").verdict, SessionVerdict::JUNK);  // flags cached, no ids
    c.add_instrument_ids({{"F", "2026-03-02", "A"}, {"F", "2026-03-03", "A"},
                          {"F", "2026-03-04", "X"}, {"F", "2026-03-05", "A"},
                          {"F", "2026-03-06", "A"}});
    EXPECT_EQ(c.instrument_id_count(), 5u);
    EXPECT_EQ(at(c, "F", "2026-03-06").verdict, SessionVerdict::SESSION)
        << "the cached flag of the flip was not recomputed";
}

TEST(InstrumentIdLimb, TheHoldFractionDecidesWhichUnconfirmedChangesAreHeldOnTheDay) {
    auto build = [](double fraction) {
        SessionClassifierConfig cfg;
        cfg.id_change_hold_fraction = fraction;
        SessionClassifier c(cfg);
        add_history(c, "ROLL", "2026-03-04", 20, 100000, "A");
        add(c, "ROLL", "2026-03-04", 100000, "B");  // a roll: full volume
        add_history(c, "FLIP", "2026-03-04", 20, 139313, "A");
        add(c, "FLIP", "2026-03-04", 3506, "X");  // 6E's 2025-11-05 shape: 2.5 % of the norm
        return c;
    };
    const auto inf = build(std::numeric_limits<double>::infinity());  // hold every change
    EXPECT_EQ(at(inf, "ROLL", "2026-03-04").verdict, SessionVerdict::JUNK);
    EXPECT_EQ(at(inf, "FLIP", "2026-03-04").verdict, SessionVerdict::JUNK);
    const auto tenth = build(0.10);  // the ruled value
    EXPECT_EQ(at(tenth, "ROLL", "2026-03-04").verdict, SessionVerdict::SESSION);
    EXPECT_EQ(at(tenth, "FLIP", "2026-03-04").verdict, SessionVerdict::JUNK);
    const auto none = build(0.0);
    EXPECT_EQ(at(none, "ROLL", "2026-03-04").verdict, SessionVerdict::SESSION);
    EXPECT_EQ(at(none, "FLIP", "2026-03-04").verdict, SessionVerdict::SESSION);
    EXPECT_EQ(kIdChangeHoldFraction, 0.10) << "HD 2026-09-25";
    EXPECT_EQ(SessionClassifierConfig{}.id_change_hold_fraction, kIdChangeHoldFraction)
        << "the ruled value is the default";
    // At the boundary: exactly 0.10 of the norm is not below it.
    SessionClassifier edge;
    add_history(edge, "E", "2026-03-04", 20, 100000, "A");
    add(edge, "E", "2026-03-04", 10000, "X");
    EXPECT_EQ(at(edge, "E", "2026-03-04").verdict, SessionVerdict::SESSION);
    SessionClassifier below;
    add_history(below, "E", "2026-03-04", 20, 100000, "A");
    add(below, "E", "2026-03-04", 9999, "X");
    EXPECT_EQ(at(below, "E", "2026-03-04").verdict, SessionVerdict::JUNK);
}

TEST(InstrumentIdLimb, WithoutIdsTheLimbJudgesNothing) {
    SessionClassifier c;
    add_history(c, "F", "2026-03-04", 20, 100000, "");
    add(c, "F", "2026-03-04", 100000, "X");  // an id, but the previous bar has none
    add(c, "F", "2026-03-05", 100000, "");
    EXPECT_EQ(at(c, "F", "2026-03-04").verdict, SessionVerdict::SESSION);
    EXPECT_EQ(at(c, "F", "2026-03-05").verdict, SessionVerdict::SESSION);
    EXPECT_EQ(at(c, "F", "2026-03-04").previous_instrument_id, "");
}

TEST(InstrumentIdLimb, AVolumeJunkBarKeepsItsVolumeReason) {
    // 6J 2025-11-05's shape: 77 lots on a flipped id is a corrupt print first.
    SessionClassifier c;
    add_history(c, "F", "2026-03-04", 20, 134020, "A");
    add(c, "F", "2026-03-04", 77, "X");
    const auto v = at(c, "F", "2026-03-04");
    EXPECT_EQ(v.verdict, SessionVerdict::JUNK);
    EXPECT_NE(v.reason.find("corrupt print"), npos) << v.reason;
}

TEST(InstrumentIdLimb, TheBacktestsGroupViewEqualsLivesT1View) {
    // The backtest adds group t, then classifies group t-1. Live has bars up to T-1 only.
    SessionClassifier live;
    SessionClassifier backtest;
    for (auto* c : {&live, &backtest}) {
        add_history(*c, "F", "2026-03-04", 20, 100000, "A");
        add_history(*c, "G", "2026-03-04", 20, 100000, "G1");
        add(*c, "F", "2026-03-04", 5000, "X");
        add(*c, "G", "2026-03-04", 100000, "G1");
    }
    add(backtest, "F", "2026-03-05", 100000, "A");
    add(backtest, "G", "2026-03-05", 100000, "G2");
    const std::vector<Bar> group = {bar_on("F", "2026-03-04", 5000),
                                    bar_on("G", "2026-03-04", 100000)};
    const auto l = classify_bar_group(live, group);
    const auto b = classify_bar_group(backtest, group);
    ASSERT_EQ(l.size(), 2u);
    ASSERT_EQ(b.size(), 2u);
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(l[i].verdict, b[i].verdict) << l[i].symbol;
        EXPECT_EQ(l[i].reason, b[i].reason) << l[i].symbol;
    }
    EXPECT_EQ(b[0].verdict, SessionVerdict::JUNK);
    EXPECT_EQ(b[1].verdict, SessionVerdict::SESSION);
}

// ---------------------------------------------------------------------------------------------
// The query and the feed helper
// ---------------------------------------------------------------------------------------------

TEST(InstrumentIdQuery, TheQueryKeepsTheLoadersCopyAndJoinsTheSamePrintOnly) {
    const std::string q =
        market_data_utils::build_futures_instrument_id_query("futures_data.ohlcv_1d", true);
    EXPECT_NE(q.find("SELECT DISTINCT ON (symbol, time) symbol, time, open, high, low, close, "
                     "volume FROM futures_data.ohlcv_1d WHERE time BETWEEN $1 AND $2 AND symbol "
                     "= ANY($3) ORDER BY symbol, time, volume DESC, close, open, high, low"),
              npos)
        << q;
    EXPECT_NE(q.find("JOIN futures_data.ohlcv_1d_raw AS r ON r.symbol = k.symbol AND r.ts_event "
                     "= k.time AND r.volume = k.volume AND r.open = k.open AND r.high = k.high "
                     "AND r.low = k.low AND r.close = k.close"),
              npos)
        << q;
    const std::string all =
        market_data_utils::build_futures_instrument_id_query("futures_data.ohlcv_1d", false);
    EXPECT_EQ(all.find("$3"), npos) << all;
}

TEST(InstrumentIdQuery, TheFeedHelperTakesTheRowsOrSaysWhyNot) {
    SessionClassifier c;
    const auto fed = feed_instrument_ids(
        c, Result<std::vector<market_data_utils::FuturesInstrumentId>>(
               std::vector<market_data_utils::FuturesInstrumentId>{
                   {"6E.v.0", "2025-11-05", "42040878"}, {"6E.v.0", "not-a-date", "1"}}));
    EXPECT_TRUE(fed.fed);
    EXPECT_EQ(fed.ids, 1u);
    EXPECT_EQ(fed.line.rfind("INSTRUMENT_ID_FEED 1 kept bars carry a vendor instrument id", 0),
              0u)
        << fed.line;
    SessionClassifier d;
    const auto failed = feed_instrument_ids(
        d, make_error<std::vector<market_data_utils::FuturesInstrumentId>>(
               ErrorCode::DATABASE_ERROR, "no such table", "test"));
    EXPECT_FALSE(failed.fed);
    EXPECT_EQ(d.instrument_id_count(), 0u);
    EXPECT_NE(failed.line.find("INSTRUMENT_ID_FEED no instrument ids (no such table): the "
                               "continuity limb judges no bar on this run"),
              npos)
        << failed.line;
}

// ---------------------------------------------------------------------------------------------
// Both engines feed the ids before they classify (the runners are main()s: their placement is
// read from the source, as test_risk_module_error_path.cpp does)
// ---------------------------------------------------------------------------------------------

namespace {

std::string read_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) {
            std::ifstream in(dir / relative);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
        dir = dir.parent_path();
    }
    return {};
}

}  // namespace

TEST(InstrumentIdWiring, BothTwinsFeedTheIdsAfterTheBarsAndBeforeTheT1Classification) {
    std::string blocks[2];
    int i = 0;
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        SCOPED_TRACE(runner);
        const std::string src = read_repo_file(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const auto bars = src.find("session_classifier.add_bars(all_bars);");
        // T-ROLLX-FIX commit 4 (N-2): the ids are fed from the classifier's history start, the
        // prefix before the window included.
        const auto feed = src.find(
            "db->get_futures_instrument_ids(symbols, k01_history_start, end_date));", bars);
        const auto t1 = src.find("classify_t1(session_classifier, symbols,", bars);
        ASSERT_NE(bars, npos);
        ASSERT_NE(feed, npos) << "the runner never feeds the instrument ids";
        ASSERT_NE(t1, npos);
        EXPECT_LT(feed, t1);
        blocks[i++] = src.substr(bars, t1 - bars);
    }
    EXPECT_EQ(blocks[0], blocks[1]) << "the twins' C10a hunks differ";
}

TEST(InstrumentIdWiring, TheBacktestFeedsTheIdsOnceTheBarsAreLoaded) {
    const std::string src = read_repo_file("src/backtest/backtest_coordinator.cpp");
    if (src.empty()) GTEST_SKIP() << "coordinator source not found";
    const auto feed = src.find("pg->get_futures_instrument_ids(symbols, start_date, end_date)");
    ASSERT_NE(feed, npos) << "the backtest never feeds the instrument ids";
    const auto grouped = src.rfind("group_bars_by_timestamp(all_bars);", feed);
    const auto portfolio = src.rfind("BacktestCoordinator::run_portfolio(", feed);
    const auto loop = src.find("for (const auto& [timestamp, bars] : grouped_bars)", feed);
    ASSERT_NE(grouped, npos);
    ASSERT_NE(portfolio, npos);
    ASSERT_NE(loop, npos);
    EXPECT_LT(portfolio, grouped) << "fed in run_portfolio, after its bars are loaded";
    EXPECT_NE(src.find("process_portfolio_day(timestamp, bars", loop), npos);
}

// ---------------------------------------------------------------------------------------------
// The clone's real bars and ids (TRADE_NGIN_TEST_DSN only; read-only)
// ---------------------------------------------------------------------------------------------

namespace {

std::string holidays_path() {
    for (const auto* p : {"include/trade_ngin/core/holidays.json",
                          "../include/trade_ngin/core/holidays.json",
                          "../../include/trade_ngin/core/holidays.json",
                          "../../../include/trade_ngin/core/holidays.json"}) {
        if (std::ifstream(p).good()) return p;
    }
    return {};
}

}  // namespace

class InstrumentIdDb : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();
        const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
        if (!dsn || !*dsn) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set";
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        }
        conn_ = dsn;
        try {
            pqxx::connection probe(conn_);
        } catch (const std::exception& e) {
            if (require_db) FAIL() << "database unreachable: " << e.what();
            GTEST_SKIP() << "database unreachable";
        }
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::WARNING;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
        db_ = std::make_shared<PostgresDatabase>(conn_);
        ASSERT_TRUE(db_->connect().is_ok());
    }
    void TearDown() override {
        if (db_) db_->disconnect();
    }

    /// The loader's bars and the ids for `symbols` over [from, to], in one classifier.
    SessionClassifier load(const std::vector<std::string>& symbols, const std::string& from,
                           const std::string& to) {
        SessionClassifier c;
        const Timestamp a(ymd_day(from));
        const Timestamp b(ymd_day(to));
        auto res = db_->get_market_data(symbols, a, b, AssetClass::FUTURES, DataFrequency::DAILY,
                                        "ohlcv");
        EXPECT_TRUE(res.is_ok());
        if (res.is_error()) return c;
        auto bars = DataConversionUtils::arrow_table_to_bars(res.value());
        EXPECT_TRUE(bars.is_ok());
        if (bars.is_error()) return c;
        c.add_bars(bars.value());
        const auto feed = feed_instrument_ids(c, db_->get_futures_instrument_ids(symbols, a, b));
        EXPECT_TRUE(feed.fed) << feed.line;
        return c;
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(InstrumentIdDb, TheThreeBarIdSequencesOnTheClone) {
    auto rows = db_->get_futures_instrument_ids({"6E.v.0", "6J.v.0", "ZN.v.0", "6A.v.0"},
                                                Timestamp(ymd_day("2025-08-25")),
                                                Timestamp(ymd_day("2025-11-10")));
    ASSERT_TRUE(rows.is_ok()) << rows.error()->what();
    std::map<std::pair<std::string, std::string>, std::string> id;
    for (const auto& r : rows.value()) id[{r.symbol, r.date}] = r.instrument_id;
    auto id_at = [&id](const std::string& s, const std::string& d) { return id[{s, d}]; };
    // symbol, (previous, day, next) ids around 2025-09-03 and 2025-11-05.
    const std::vector<std::tuple<std::string, std::string, std::string, std::string, std::string>>
        expected = {
            {"6E.v.0", "2025-09-03", "3624", "3624", "3624"},
            {"6J.v.0", "2025-09-03", "6796", "6796", "6796"},
            {"ZN.v.0", "2025-09-03", "42002111", "42066706", "42002111"},
            {"6E.v.0", "2025-11-05", "4274", "42040878", "4274"},
            {"6J.v.0", "2025-11-05", "4197", "42039890", "4197"},
            {"ZN.v.0", "2025-11-05", "42002111", "42002111", "42002111"},
        };
    for (const auto& [s, d, prev, day, next] : expected) {
        SCOPED_TRACE(s + " " + d);
        const Day dd = ymd_day(d);
        EXPECT_EQ(id_at(s, SessionClassifier::ymd(dd - std::chrono::days{1})), prev);
        EXPECT_EQ(id_at(s, d), day);
        EXPECT_EQ(id_at(s, SessionClassifier::ymd(dd + std::chrono::days{1})), next);
    }
    // 6A 2025-11-05: the raw table holds the 196-lot copy, the loader keeps the 76,895-lot one:
    // not the same print, so no id (its neighbours have one).
    EXPECT_EQ(id.count(std::make_pair(std::string("6A.v.0"), std::string("2025-11-05"))), 0u);
    EXPECT_EQ(id.count(std::make_pair(std::string("6A.v.0"), std::string("2025-11-04"))), 1u);
    EXPECT_EQ(id.count(std::make_pair(std::string("6A.v.0"), std::string("2025-11-06"))), 1u);
}

TEST_F(InstrumentIdDb, TheNamedCellsOnTheRealBarsLiveAndBacktestViews) {
    const std::vector<std::string> syms = {"6E.v.0", "6J.v.0", "ZN.v.0"};
    // The backtest's view (every later bar present) and live's T-1 view of each date (the
    // window ends on the classified date).
    const auto full = load(syms, "2025-01-01", "2025-11-10");
    struct Cell {
        const char* symbol;
        const char* date;
        SessionVerdict verdict;
        const char* reason_has;
    };
    const std::vector<Cell> cells = {
        {"6E.v.0", "2025-11-05", SessionVerdict::JUNK, "instrument id change (4274 -> 42040878"},
        {"6J.v.0", "2025-11-05", SessionVerdict::JUNK, "corrupt print"},
        {"ZN.v.0", "2025-11-05", SessionVerdict::SESSION, ""},
        {"6E.v.0", "2025-09-03", SessionVerdict::SESSION, ""},
        {"6J.v.0", "2025-09-03", SessionVerdict::SESSION, ""},
        {"ZN.v.0", "2025-09-03", SessionVerdict::JUNK,
         "instrument id change (42002111 -> 42066706"},
    };
    for (const auto& cell : cells) {
        SCOPED_TRACE(std::string(cell.symbol) + " " + cell.date);
        const auto v = at(full, cell.symbol, cell.date);
        EXPECT_EQ(v.verdict, cell.verdict) << v.reason;
        EXPECT_NE(v.reason.find(cell.reason_has), npos) << v.reason;
        const auto live = load(syms, "2025-01-01", cell.date);
        const auto lv = at(live, cell.symbol, cell.date);
        EXPECT_EQ(lv.verdict, v.verdict) << "live's T-1 view differs: " << lv.reason;
        EXPECT_EQ(lv.reason, v.reason);
    }
    // The next session confirms each flip.
    for (const auto& [s, d] : std::vector<std::pair<std::string, std::string>>{
             {"6E.v.0", "2025-11-06"}, {"6J.v.0", "2025-11-06"}, {"ZN.v.0", "2025-09-04"}}) {
        const auto v = at(full, s, d);
        EXPECT_EQ(v.verdict, SessionVerdict::SESSION) << s << " " << d << ": " << v.reason;
        EXPECT_NE(v.id_note.find("is confirmed a ONE-DAY FLIP"), npos) << s << " " << v.id_note;
    }
}

TEST_F(InstrumentIdDb, The2026TableMatchesThePythonReference) {
    const std::string cal_path = holidays_path();
    if (cal_path.empty()) GTEST_SKIP() << "shipped holidays.json not reachable from cwd";
    HolidayChecker checker(cal_path);
    ASSERT_TRUE(checker.loaded());
    long stored = 0;
    std::string last;
    std::vector<std::string> universe;
    {
        pqxx::connection pc(conn_);
        pqxx::work w(pc);
        const auto r =
            w.exec("SELECT count(*), max(time)::date::text FROM futures_data.ohlcv_1d");
        stored = r[0][0].as<long>();
        last = r[0][1].as<std::string>();
        for (const auto& row :
             w.exec("SELECT DISTINCT symbol FROM futures_data.ohlcv_1d ORDER BY symbol")) {
            universe.push_back(row[0].as<std::string>());
        }
    }
    if (stored != 165037 || last != "2026-08-06") {
        GTEST_SKIP() << "the table is not the clone the reference was counted on (" << stored
                     << " rows, last " << last << ")";
    }
    const auto c = load(universe, "2024-01-01", "2026-08-06");
    EXPECT_GT(c.instrument_id_count(), 0u);
    const auto cal = holiday_lookup(checker);
    std::map<SessionVerdict, int> counts;
    for (Day d = ymd_day("2026-01-01"); d <= ymd_day("2026-08-06"); d += std::chrono::days{1}) {
        const auto t1 = classify_t1(c, universe, d, cal);
        counts[SessionVerdict::SESSION] += static_cast<int>(t1.session);
        counts[SessionVerdict::JUNK] += static_cast<int>(t1.junk);
        counts[SessionVerdict::NO_BAR_CLOSURE] += static_cast<int>(t1.closure);
        counts[SessionVerdict::NO_BAR_FEED_HOLE] += static_cast<int>(t1.feed_hole);
    }
    // c10_idflip.py report <dir> 0.10 on the same clone: OLD 5,950 / 51 / 1,496 / 351; at the
    // ruled 0.10, 22 SESSION bars become JUNK (every one an unconfirmed id change printing below
    // 0.10 of its norm; no-bar verdicts do not move). Holding every change gave 5,813 / 188.
    EXPECT_EQ(counts[SessionVerdict::SESSION], 5928);
    EXPECT_EQ(counts[SessionVerdict::JUNK], 73);
    EXPECT_EQ(counts[SessionVerdict::NO_BAR_CLOSURE], 1496);
    EXPECT_EQ(counts[SessionVerdict::NO_BAR_FEED_HOLE], 351);
}
