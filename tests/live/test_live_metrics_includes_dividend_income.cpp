#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_audit_log.hpp"
#include "trade_ngin/live/dividend_counter.hpp"

using namespace trade_ngin;

// Phase 4.5 test TDI.2 — wire-up contract.
//
// The live equity app builds its metrics map at daily finalization with a
// "total_dividend_income" key whose value comes from
// CorporateActionsAuditLog::total_cumulative_dividend_income(). This test
// reproduces that exact call pattern against a state file with a known
// cumulative total, and asserts the metric lands at the expected key with
// the expected value. Pins the contract the live app depends on.

namespace {

class LiveMetricsIncludesDividendIncomeTest : public ::testing::Test {
protected:
    void SetUp() override {
        state_dir_ = (std::filesystem::temp_directory_path() /
                      ("ca_metric_wireup_" + std::to_string(std::rand())))
                         .string();
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(state_dir_, ec);
    }
    std::string state_dir_;
};

PositionAdjustment dividend_adj(const std::string& symbol,
                                const std::string& date,
                                double qty_after,
                                double per_share) {
    PositionAdjustment adj;
    adj.symbol = symbol;
    adj.event_date = date;
    adj.type = CorpActionType::DIVIDEND;
    adj.quantity_before = qty_after;
    adj.quantity_after = qty_after;
    adj.avg_price_before = 100.0;
    adj.avg_price_after = 99.75;
    adj.event_value = per_share;
    adj.ratio_change = 1.0 + per_share / 100.0;
    return adj;
}

}  // namespace

// Set up a state file with $75 in cumulative dividend income, then build a
// metrics map the same way the live app does and assert the key/value.
TEST_F(LiveMetricsIncludesDividendIncomeTest, MetricsMapPicksUpCumulativeFromAuditLog) {
    // Arrange: write a state file with two dividends totaling $75.
    {
        CorporateActionsAuditLog log(state_dir_);
        log.load();
        // 100 shares * $0.25 = $25
        log.record(dividend_adj("AAPL", "2024-08-12", 100.0, 0.25));
        // 100 shares * $0.50 = $50
        log.record(dividend_adj("MSFT", "2024-08-15", 100.0, 0.50));
        ASSERT_TRUE(log.save());
    }

    // Act: mirror the live app's wire-up at the metrics-build site.
    double total_dividend_income = 0.0;
    {
        CorporateActionsAuditLog div_log(state_dir_);
        div_log.load();
        total_dividend_income = div_log.total_cumulative_dividend_income();
    }
    std::unordered_map<std::string, double> double_metrics = {
        {"total_pnl", 1234.5},               // placeholder for other metrics
        {"total_dividend_income", total_dividend_income},
    };

    // Assert: the metric is exactly the cumulative sum, under the right key.
    ASSERT_TRUE(double_metrics.count("total_dividend_income"));
    EXPECT_DOUBLE_EQ(double_metrics["total_dividend_income"], 75.0);

    // Sanity: total_pnl is independent (we did NOT add dividend income to it,
    // per the audit's locked decision #8).
    EXPECT_DOUBLE_EQ(double_metrics["total_pnl"], 1234.5);
}

// First-run / missing-state-file: metric is 0 (file absent → load returns
// false → in-memory state empty → cumulative is 0).
TEST_F(LiveMetricsIncludesDividendIncomeTest, MissingStateFileYieldsZeroMetric) {
    CorporateActionsAuditLog div_log(state_dir_);
    div_log.load();  // returns false; we ignore the return per the live app
    EXPECT_DOUBLE_EQ(div_log.total_cumulative_dividend_income(), 0.0);
}

// T-8D-2 R44 (b): the counter the live equity app stores. It is dated by ex-date and counted on
// the shares of the position row dated the ex-date, which include the fill the run of the
// ex-date made at the cum-dividend close; the recorded qty_held is the row of the day before.
namespace {

// The position rows of the two ex-dates in the equity chain (trading.positions, 2026-06-08 and
// 2026-06-15), as a lookup.
CorporateActionsAuditLog::SharesOnExDateRow chain_rows() {
    return [](const std::string& symbol, const std::string& ex_date) -> std::optional<double> {
        if (symbol == "GOOGL" && ex_date == "2026-06-08") return 13.041359;
        if (symbol == "META" && ex_date == "2026-06-15") return 7.147202;
        return std::nullopt;
    };
}

}  // namespace

TEST_F(LiveMetricsIncludesDividendIncomeTest, TheCounterIsDatedByExDateOnTheSharesOfTheExDateRow) {
    CorporateActionsAuditLog log(state_dir_);
    log.load();
    // Recorded as the dedup rows hold them: the quantity of the day before each ex-date.
    log.record(dividend_adj("META", "2026-06-15", 7.127855, 0.525));
    log.record(dividend_adj("GOOGL", "2026-06-08", 12.833803, 0.22));

    // The undated sum on the recorded quantities: what every row from the run date carried.
    EXPECT_NEAR(log.total_cumulative_dividend_income(), 6.565561, 5e-7);

    const auto rows = chain_rows();
    // A row before the first ex-date carries nothing; the ex-date row itself carries its event.
    EXPECT_DOUBLE_EQ(log.dividend_income_through("2026-06-07", rows), 0.0);
    EXPECT_DOUBLE_EQ(log.dividend_income_through("2026-06-08", rows), 13.041359 * 0.22);
    EXPECT_DOUBLE_EQ(log.dividend_income_through("2026-06-14", rows), 13.041359 * 0.22);
    // META joins on its ex-date row, on 7.147202 shares, not on the recorded 7.127855.
    const double both = 13.041359 * 0.22 + 7.147202 * 0.525;
    EXPECT_DOUBLE_EQ(log.dividend_income_through("2026-06-15", rows), both);
    EXPECT_DOUBLE_EQ(log.dividend_income_through("2026-07-09", rows), both);
    EXPECT_NEAR(both, 6.621380, 5e-7);
}

TEST_F(LiveMetricsIncludesDividendIncomeTest, TheCounterSurvivesAReloadAndDoesNotDependOnRowOrder) {
    {
        CorporateActionsAuditLog log(state_dir_);
        log.load();
        log.record(dividend_adj("META", "2026-06-15", 7.127855, 0.525));
        log.record(dividend_adj("GOOGL", "2026-06-08", 12.833803, 0.22));
        ASSERT_TRUE(log.save());
    }
    CorporateActionsAuditLog reloaded(state_dir_);
    reloaded.load();
    CorporateActionsAuditLog other_order(state_dir_ + "_b");
    other_order.record(dividend_adj("GOOGL", "2026-06-08", 12.833803, 0.22));
    other_order.record(dividend_adj("META", "2026-06-15", 7.127855, 0.525));

    const auto rows = chain_rows();
    EXPECT_DOUBLE_EQ(reloaded.dividend_income_through("2026-06-16", rows),
                     13.041359 * 0.22 + 7.147202 * 0.525);
    EXPECT_EQ(reloaded.dividend_income_through("2026-06-16", rows),
              other_order.dividend_income_through("2026-06-16", rows));
}

TEST_F(LiveMetricsIncludesDividendIncomeTest, AnEventWithNoExDateRowIsCountedOnTheRecordedQuantityAndNamed) {
    CorporateActionsAuditLog log(state_dir_);
    log.load();
    log.record(dividend_adj("GOOGL", "2026-06-08", 12.833803, 0.22));
    log.record(dividend_adj("AAPL", "2026-06-10", 100.0, 0.25));  // no position row on its ex-date

    std::vector<std::string> named;
    EXPECT_DOUBLE_EQ(log.dividend_income_through("2026-06-30", chain_rows(), &named),
                     13.041359 * 0.22 + 100.0 * 0.25);
    ASSERT_EQ(named.size(), 1u);
    EXPECT_EQ(named[0], "AAPL 2026-06-10");

    // A row that exists and holds nothing is a value, not a missing row: the event counts 0.
    named.clear();
    const CorporateActionsAuditLog::SharesOnExDateRow flat =
        [](const std::string&, const std::string&) -> std::optional<double> { return 0.0; };
    EXPECT_DOUBLE_EQ(log.dividend_income_through("2026-06-30", flat, &named), 0.0);
    EXPECT_TRUE(named.empty());

    // No lookup at all: every event on its recorded quantity, the undated sum when all are in.
    EXPECT_DOUBLE_EQ(log.dividend_income_through("2026-06-30", nullptr),
                     log.total_cumulative_dividend_income());
}

// A read that fails is not "the symbol has no row": the lookup remembers the ex-date, and the
// run then writes no figure of its own (live/dividend_counter.hpp).
namespace {

ExDateRowShares::Rows rows_of(const std::string& symbol, double quantity) {
    Position row;
    row.symbol = symbol;
    row.quantity = Quantity(quantity);
    return ExDateRowShares::Rows{{symbol, row}};
}

}  // namespace

TEST_F(LiveMetricsIncludesDividendIncomeTest, TheExDateRowsAreReadOncePerExDate) {
    int reads = 0;
    ExDateRowShares shares([&](const std::string& ex_date) -> Result<ExDateRowShares::Rows> {
        ++reads;
        if (ex_date == "2026-06-08") return rows_of("GOOGL", 13.041359);
        return rows_of("META", 7.147202);
    });
    ASSERT_TRUE(shares("GOOGL", "2026-06-08").has_value());
    EXPECT_DOUBLE_EQ(*shares("GOOGL", "2026-06-08"), 13.041359);
    EXPECT_DOUBLE_EQ(*shares("META", "2026-06-15"), 7.147202);
    // A symbol with no row on a date that was read is an answer, not a failure.
    EXPECT_FALSE(shares("AAPL", "2026-06-15").has_value());
    EXPECT_EQ(reads, 2);
    EXPECT_TRUE(shares.failed_reads().empty());
}

TEST_F(LiveMetricsIncludesDividendIncomeTest, AFailedReadOfAnExDateRowIsRememberedNotCountedAsNoRow) {
    int reads = 0;
    ExDateRowShares shares([&](const std::string& ex_date) -> Result<ExDateRowShares::Rows> {
        ++reads;
        if (ex_date == "2026-06-15") {
            return make_error<ExDateRowShares::Rows>(ErrorCode::DATABASE_ERROR, "connection lost",
                                                     "test");
        }
        return rows_of("GOOGL", 13.041359);
    });
    CorporateActionsAuditLog log(state_dir_);
    log.load();
    log.record(dividend_adj("GOOGL", "2026-06-08", 12.833803, 0.22));
    log.record(dividend_adj("META", "2026-06-15", 7.127855, 0.525));

    std::vector<std::string> named;
    const double through_t1 = log.dividend_income_through("2026-06-15", std::ref(shares), &named);
    const double through_today = log.dividend_income_through("2026-06-16", std::ref(shares), &named);
    // The sum fell back to the recorded quantity for META: a figure the run must not store.
    EXPECT_DOUBLE_EQ(through_t1, 13.041359 * 0.22 + 7.127855 * 0.525);
    ASSERT_EQ(shares.failed_reads().size(), 1u) << "one entry per ex-date, however often asked";
    EXPECT_EQ(shares.failed_reads()[0], "2026-06-15 (connection lost)");
    EXPECT_EQ(reads, 2) << "a failed ex-date is not read again";

    // The Day T-1 row is left as it is; the run's own row carries the Day T-1 row's stored figure.
    const auto cells = dividend_counter_cells(!shares.failed_reads().empty(), through_t1,
                                              through_today, 2.869099);
    EXPECT_FALSE(cells.day_t1.has_value());
    ASSERT_TRUE(cells.today.has_value());
    EXPECT_DOUBLE_EQ(*cells.today, 2.869099);
    // With no stored figure to carry, the run names none: never a zero of its own.
    const auto bare = dividend_counter_cells(true, through_t1, through_today, std::nullopt);
    EXPECT_FALSE(bare.day_t1.has_value());
    EXPECT_FALSE(bare.today.has_value());
}

TEST_F(LiveMetricsIncludesDividendIncomeTest, ARunWhoseReadsAllAnsweredWritesBothFigures) {
    const auto cells = dividend_counter_cells(false, 2.869099, 6.621380, 2.823437);
    ASSERT_TRUE(cells.day_t1.has_value());
    ASSERT_TRUE(cells.today.has_value());
    EXPECT_DOUBLE_EQ(*cells.day_t1, 2.869099);
    EXPECT_DOUBLE_EQ(*cells.today, 6.621380);
}
