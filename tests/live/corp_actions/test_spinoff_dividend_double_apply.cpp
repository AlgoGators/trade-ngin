// tests/live/corp_actions/test_spinoff_dividend_double_apply.cpp
//
// BA-26 -- the terms feed lags the price feed, and the routing never noticed.
//
// THE SEQUENCE.
//   run N    `equities_data.ohlcv_1d` carries MMM's div_cash 17.3875 on 2024-04-01.
//            `equities_data.corporate_action` does NOT yet carry the matching `spinoff` row
//            (that feed lags; it stopped entirely on 2025-08-29). With no terms row the bar
//            is indistinguishable from a dividend, so the class-1 applier applies it:
//            basis /= 1 + 17.3875/94.02, and a DIVIDEND dedup row is written.
//   later    the terms row arrives.
//   run N+k  the routing sees a spinoff bar and checks the ledger -- for is_applied(SPINOFF),
//            and since E2-F50 for is_applied(SPLIT|ADR_SPLIT). It never checked
//            is_applied(DIVIDEND). So it routes the SAME bar to the spinoff handler, which
//            divides the basis by 1 + d/c a SECOND time and delivers a child against it.
//
// The double restatement is the whole distribution applied twice: on MMM's real numbers a
// basis of 106.07 goes to 89.5155 (correct) and then to 75.5486 (wrong by 15.6 %), and the
// child's allocated basis is struck off the wrong pool.
//
// THE FIX IS A REFUSAL, NOT A CORRECTION. By the time the second run notices, the book is
// already wrong and the ledger cannot put it right: the share count held on the ex-date and
// the child's first close at the time were never recorded against the dividend row. Inventing
// either would be a guess written into `average_price` and stamped in
// `trading.corp_action_applied`, where nothing reconsiders it. The run refuses to route,
// names the double-apply, and leaves it to a human.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_audit_log.hpp"

using namespace trade_ngin;

namespace {

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_runner() {
    auto path = find_repo_file("apps/strategies/live_equity_mean_reversion.cpp");
    if (path.empty()) return {};
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A scratch state directory the audit log can own without touching the real one.
class TempStateDir {
public:
    TempStateDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("tn_ba26_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TempStateDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    std::string str() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST(SpinoffDividendDoubleApply, TheLedgerRemembersTheDividendTheEarlierRunApplied) {
    // Run N: the bar was applied as an ordinary dividend and recorded.
    TempStateDir dir;
    CorporateActionsAuditLog log(dir.str());

    PositionAdjustment applied;
    applied.symbol = "MMM";
    applied.event_date = "2024-04-01";
    applied.type = CorpActionType::DIVIDEND;
    applied.quantity_before = 100.0;
    applied.quantity_after = 100.0;
    applied.avg_price_before = 106.07;
    applied.avg_price_after = 106.07 / (1.0 + 17.3875 / 94.02);
    applied.event_value = 17.3875;
    applied.ratio_change = 1.0 + 17.3875 / 94.02;
    log.record(applied);

    // Run N+k asks the three questions the routing asks. Only the third was ever asked
    // before BA-26, and it is the one that is true.
    EXPECT_FALSE(log.is_applied("MMM", "2024-04-01", CorpActionType::SPINOFF));
    EXPECT_FALSE(log.is_applied("MMM", "2024-04-01", CorpActionType::SPLIT));
    EXPECT_TRUE(log.is_applied("MMM", "2024-04-01", CorpActionType::DIVIDEND))
        << "the ledger has always carried this fact; the routing simply never asked for it";

    // The dedup key is per (symbol, ex-date, type): a dividend on another date says nothing.
    EXPECT_FALSE(log.is_applied("MMM", "2024-04-02", CorpActionType::DIVIDEND));
    EXPECT_FALSE(log.is_applied("SOLV", "2024-04-01", CorpActionType::DIVIDEND));
}

TEST(SpinoffDividendDoubleApply, RoutingItAgainWouldRestateTheBasisTwiceOverMMMsRealNumbers) {
    // The stake, in the arithmetic the two paths share. Both divide by the SAME factor, so
    // applying both is the distribution counted twice.
    const double basis = 106.07;                       // MMM's real 2024-03-28 close
    const double factor = 1.0 + 17.3875 / 94.02;       // its real bar: div_cash over raw close
    const double after_dividend = basis / factor;      // what run N left in the book
    const double after_spinoff_too = after_dividend / factor;  // what run N+k would leave

    EXPECT_NEAR(after_dividend, 89.515530, 1e-6) << "B-4's hand-checked figure";
    EXPECT_NEAR(after_spinoff_too, 75.544736, 1e-6);
    EXPECT_GT((after_dividend - after_spinoff_too) / after_dividend, 0.15)
        << "the second restatement moves the basis by more than 15 %, and it is persisted "
           "into average_price and stamped in corp_action_applied where nothing revisits it";

    // And the child's basis would be allocated out of the wrong pool: B(1 - 1/F) off an
    // already-restated basis is smaller than off the true one by exactly the same factor.
    const double pool_correct = basis * (1.0 - 1.0 / factor);
    const double pool_wrong = after_dividend * (1.0 - 1.0 / factor);
    EXPECT_NEAR(pool_wrong, pool_correct / factor, 1e-9);
}

TEST(SpinoffDividendDoubleApply, TheRoutingAsksTheLedgerAndRefusesRatherThanCorrecting) {
    const std::string src = read_runner();
    if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

    // It asks.
    EXPECT_NE(src.find("const bool dividend_already_applied ="), std::string::npos)
        << "the routing does not consult is_applied(DIVIDEND); a bar an earlier run applied "
           "as a dividend would be routed as a spinoff and restated a second time (BA-26)";

    // The answer keeps the bar in class 1 rather than routing it.
    EXPECT_NE(src.find("dividend_already_applied ||"), std::string::npos)
        << "the answer must feed row_stays_class1, or asking changes nothing";

    // And it says so, loudly, naming the double-apply and refusing to invent a repair.
    EXPECT_NE(src.find("SPINOFF REFUSED -- ALREADY APPLIED AS A DIVIDEND"), std::string::npos);
    EXPECT_NE(src.find("needs a manual restatement (BA-26)"), std::string::npos)
        << "the WARN must say the book cannot be repaired from the ledger -- the share count "
           "and the child's first close at the time were never recorded -- rather than "
           "implying the run has fixed it";
}

TEST(SpinoffDividendDoubleApply, AnUnrecordedDividendDoesNotBlockANormalSpinoff) {
    // The converse: with nothing in the ledger the routing must proceed exactly as before.
    // This is the ordinary case -- B-4's MMM replay and B-5a's RTX replay both ran it -- and
    // a check that refused every spinoff would be worse than the defect.
    TempStateDir dir;
    CorporateActionsAuditLog log(dir.str());
    EXPECT_FALSE(log.is_applied("RTX", "2020-04-03", CorpActionType::DIVIDEND));
    EXPECT_FALSE(log.is_applied("RTX", "2020-04-03", CorpActionType::SPINOFF));

    // A recorded SPINOFF is the pre-existing dedup and is a different question again.
    PositionAdjustment spun;
    spun.symbol = "RTX";
    spun.event_date = "2020-04-03";
    spun.type = CorpActionType::SPINOFF;
    spun.ratio_change = 2.011630082114961;
    log.record(spun);
    EXPECT_TRUE(log.is_applied("RTX", "2020-04-03", CorpActionType::SPINOFF));
    EXPECT_FALSE(log.is_applied("RTX", "2020-04-03", CorpActionType::DIVIDEND))
        << "recording the spinoff must not make the dividend look applied, or every "
           "successfully routed bar would refuse itself on the next run";
}
