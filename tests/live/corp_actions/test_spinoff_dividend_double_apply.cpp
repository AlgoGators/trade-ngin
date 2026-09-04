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
// WHAT IS ACTUALLY WRONG, AND WHAT IS NOT (corrected in B-5d). The class-1 dividend divided
// the basis by 1 + d/c against the same ex-date close `SpinoffBarColumns` uses, and on a
// dividend-encoded bar that is EXACTLY the factor the spinoff path would have divided by. So
// the PARENT'S BASIS IS ALREADY RIGHT. The first version of this WARN said the book "needs a
// manual restatement", which pointed a reader at the one number that is correct.
//
// What is missing is the CHILD: never received, never priced, never sold. The value that left
// the parent's basis -- the pool `B(1 - 1/F)` -- went nowhere instead of into the child's.
//
// THE FIX IS STILL A REFUSAL, NOT A CORRECTION, but for a narrower reason: delivering the
// child now would need the share count held on the ex-date and the child's first close at the
// time, and the dividend row recorded neither. Inventing either would be a guess written into
// `average_price` and stamped in `trading.corp_action_applied`, where nothing reconsiders it.
// (A bar that also folds a split_factor above 1 into the distribution is the one case where
// the parent IS additionally short; the WARN names that factor when it is there.)

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_audit_log.hpp"
#include "trade_ngin/live/corporate_actions_lifecycle.hpp"

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

// The arithmetic the corrected WARN rests on, not just its wording: on a dividend-encoded
// spinoff bar the factor the class-1 applier used IS the factor the spinoff path would have
// used, so the parent's basis is right and nothing about it needs repairing.
TEST(SpinoffDividendDoubleApply, TheClass1DividendFactorIsTheSpinoffFactorOnADividendBar) {
    // MMM 2024-04-01: div_cash 17.3875 against a raw ex-date close of 94.02, no split column.
    SpinoffBarColumns mmm;
    mmm.has_dividend = true;
    mmm.dividend_cash = 17.3875;
    mmm.close_at_ex_date = 94.02;

    EXPECT_DOUBLE_EQ(mmm.dividend_factor(), mmm.spinoff_factor())
        << "the class-1 dividend and the spinoff divide the parent's basis by the SAME "
           "number, which is why an already-applied dividend leaves the basis correct";
    EXPECT_NEAR(mmm.spinoff_factor(), 1.1849340565837057, 1e-12);
    EXPECT_FALSE(mmm.has_reverse_split());

    // A reverse-split bar is the same story for the distribution half: the reverse split is
    // routed to class 1 on its own and answers to its own dedup row.
    SpinoffBarColumns hlt;
    hlt.has_split = true;
    hlt.split_factor = 0.3333333333;
    hlt.has_dividend = true;
    hlt.dividend_cash = 24.48;
    hlt.close_at_ex_date = 58.00;
    EXPECT_NEAR(hlt.spinoff_factor(), hlt.dividend_factor(), 1e-9);

    // The ONE case where the parent really is left short: a split_factor above 1 folded into
    // the distribution. The WARN names that residual factor when it is there.
    SpinoffBarColumns abt;
    abt.has_split = true;
    abt.split_factor = 1.0688328345;
    abt.has_dividend = true;
    abt.dividend_cash = 2.9154459753;
    abt.close_at_ex_date = 42.32;
    EXPECT_NE(abt.spinoff_factor(), abt.dividend_factor());
    EXPECT_NEAR(abt.spinoff_factor() / abt.dividend_factor(), 1.0688328345, 1e-9)
        << "the residual is exactly the folded split factor";
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

    // And it says so, loudly, and says the RIGHT thing about what is broken.
    EXPECT_NE(src.find("SPINOFF NOT ROUTED -- THIS BAR WAS ALREADY APPLIED AS A "), std::string::npos);
    EXPECT_NE(src.find("'s cost basis is ALREADY CORRECT and must not be restated "),
              std::string::npos)
        << "the parent basis is right -- the class-1 dividend divided by the same factor the "
           "spinoff would have -- and a WARN that sends someone to restate it points them at "
           "the one number that is correct";
    EXPECT_NE(src.find("What is missing is the DISTRIBUTION"), std::string::npos)
        << "the WARN must name what IS missing: the child, never received, never priced, "
           "never sold";
    EXPECT_EQ(src.find("needs a manual restatement"), std::string::npos)
        << "the superseded wording is back";
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

// ---------------------------------------------------------------------------
// B-viii: what is ACTUALLY still wrong when an earlier run applied part of the bar.
//
// The BA-26 branch computed `residual = spinoff_factor / dividend_factor` and, whenever
// that was not 1, told the operator the parent was "additionally short by a factor of
// <residual>". That is right only when the earlier run applied the DIVIDEND row alone.
//
// If it also applied the bar's split_factor row as an ordinary class-1 split -- which is
// what a pre-spinoff-path run did, and is exactly E2-F31 -- then the basis was divided by
// dividend_factor AND by split_factor, i.e. by total_factor, which is the whole step the
// spinoff path would have applied. The basis is therefore ALREADY CORRECT, and what is
// wrong is the SHARE COUNT: a split_factor above 1 on a spinoff bar is part of the
// distribution and the holder's share count never moved, so the class-1 split minted
// phantom shares.
//
// Telling the operator the basis is short when the shares are inflated points the repair at
// the wrong column. The routing never asked is_applied(SPLIT), so it could not tell the two
// cases apart.
// ---------------------------------------------------------------------------

TEST(SpinoffDividendDoubleApply, DividendOnlyApplied_TheBasisIsShortByTheSplitStep) {
    // RTX 2020-04-03: div_cash and split_factor 2.0116 on one bar.
    SpinoffBarColumns col;
    col.has_dividend = true;
    col.dividend_cash = 2.31;
    col.close_at_ex_date = 51.0;
    col.has_split = true;
    col.split_factor = 2.0116;

    const auto gap = col.already_applied_gap(/*dividend_applied=*/true, /*split_applied=*/false);

    EXPECT_FALSE(gap.basis_correct);
    EXPECT_NEAR(gap.basis_short_by, col.split_step(), 1e-12)
        << "only the dividend factor was taken out; the split step is still missing";
    EXPECT_DOUBLE_EQ(gap.shares_inflated_by, 1.0) << "no class-1 split was applied";
}

TEST(SpinoffDividendDoubleApply, BothApplied_TheBasisIsRightAndTheSHARESAreInflated) {
    SpinoffBarColumns col;
    col.has_dividend = true;
    col.dividend_cash = 2.31;
    col.close_at_ex_date = 51.0;
    col.has_split = true;
    col.split_factor = 2.0116;

    const auto gap = col.already_applied_gap(/*dividend_applied=*/true, /*split_applied=*/true);

    EXPECT_TRUE(gap.basis_correct)
        << "dividend_factor * split_factor == total_factor, which is the whole step";
    EXPECT_NEAR(gap.basis_short_by, 1.0, 1e-12);
    EXPECT_NEAR(gap.shares_inflated_by, 2.0116, 1e-12)
        << "a split_factor above 1 on a spinoff bar is distribution, not a share-count "
           "change -- applying it as class-1 minted phantom shares (E2-F31)";
}

TEST(SpinoffDividendDoubleApply, ARealReverseSplitAppliedAsClass1DoesNotInflateShares) {
    // HLT 2017-01-04: split_factor 0.3333 IS a genuine 1-for-3 reverse split, and applying
    // it as a class-1 split is correct -- the share count really did change.
    SpinoffBarColumns col;
    col.has_dividend = true;
    col.dividend_cash = 1.0;
    col.close_at_ex_date = 50.0;
    col.has_split = true;
    col.split_factor = 0.3333;

    const auto gap = col.already_applied_gap(/*dividend_applied=*/true, /*split_applied=*/true);

    EXPECT_TRUE(gap.basis_correct);
    EXPECT_DOUBLE_EQ(gap.shares_inflated_by, 1.0)
        << "a reverse split below 1 is a real share-count change, not phantom shares";
}

TEST(SpinoffDividendDoubleApply, ADividendOnlyBarLeavesNothingShort) {
    // No split column at all: the dividend factor IS the whole step, so applying the
    // dividend restated the basis exactly as the spinoff path would have.
    SpinoffBarColumns col;
    col.has_dividend = true;
    col.dividend_cash = 2.31;
    col.close_at_ex_date = 51.0;

    const auto gap = col.already_applied_gap(true, false);
    EXPECT_TRUE(gap.basis_correct);
    EXPECT_NEAR(gap.basis_short_by, 1.0, 1e-12);
    EXPECT_DOUBLE_EQ(gap.shares_inflated_by, 1.0);
}

// ---------------------------------------------------------------------------
// B-viii, second shape: a coincident REVERSE split must not be reported as a basis shortfall.
//
// With only the DIVIDEND dedup present on a bar that also carries a reverse split,
// total_factor()/dividend_factor() is the reverse-split step -- a number BELOW 1 -- and the
// WARN called it "the parent is additionally short by a factor of 0.333333". It is not: a
// reverse split is a real share-count change and the class-1 path applies it itself
// (E2-F48 / b11bdea8). The operator was being pointed at a restatement that was about to
// happen anyway.
// ---------------------------------------------------------------------------

TEST(SpinoffDividendDoubleApply, AReverseSplitIsNotABasisShortfall) {
    // HLT 2017-01-04 shape: 1-for-3 reverse split on the spinoff ex-date, plus a dividend row.
    SpinoffBarColumns col;
    col.has_dividend = true;
    col.dividend_cash = 1.0;
    col.close_at_ex_date = 50.0;
    col.has_split = true;
    col.split_factor = 0.3333;

    const auto gap = col.already_applied_gap(/*dividend_applied=*/true, /*split_applied=*/false);

    EXPECT_TRUE(gap.basis_correct)
        << "the reverse split is class-1's job, so nothing about the basis is outstanding";
    EXPECT_NEAR(gap.basis_short_by, 1.0, 1e-9)
        << "reporting 0.3333 here told the operator to divide by a factor class-1 applies";
    EXPECT_DOUBLE_EQ(gap.shares_inflated_by, 1.0);
}

TEST(SpinoffDividendDoubleApply, AForwardSplitStillIsABasisShortfall) {
    // The other shape must be unchanged: a split_factor ABOVE 1 on a spinoff bar is part of
    // the distribution, so if the earlier run did not apply it the basis really is short by it.
    SpinoffBarColumns col;
    col.has_dividend = true;
    col.dividend_cash = 2.31;
    col.close_at_ex_date = 51.0;
    col.has_split = true;
    col.split_factor = 2.0116;

    const auto gap = col.already_applied_gap(/*dividend_applied=*/true, /*split_applied=*/false);
    EXPECT_FALSE(gap.basis_correct);
    EXPECT_NEAR(gap.basis_short_by, 2.0116, 1e-9);
}

TEST(SpinoffDividendDoubleApply, AReverseSplitAlreadyAppliedIsAlsoNotAShortfall) {
    SpinoffBarColumns col;
    col.has_dividend = true;
    col.dividend_cash = 1.0;
    col.close_at_ex_date = 50.0;
    col.has_split = true;
    col.split_factor = 0.3333;

    const auto gap = col.already_applied_gap(true, true);
    EXPECT_TRUE(gap.basis_correct);
    EXPECT_DOUBLE_EQ(gap.shares_inflated_by, 1.0);
}
