// R4b: the report's roll view read from the database (contract_rolls::report_roll_view), and the bar
// source's log floor (R4a open question 4), on the stage-3 copy (TRADE_NGIN_TEST_DSN, contract bars with
// their expiration). Read only.
//   * The worked example's silver roll: December (42218) to March (42044525) on 2025-11-25 is "Silver,
//     December to March", the price difference to three decimals; on 2025-11-21 SIZ5 is held inside its
//     window (first notice Fri 11-28, last actionable bar Wed 11-26, its run 11-27), the target SIH6; on
//     11-19 the window is not open. A micro stays on the vendor's series: its contract, no window.
//   * per_contract_bars logs the rule's fallbacks from the source's floor on, once per run: a load whose
//     window ends before the floor marks no message logged; with no floor the same load logs it, and a second
//     load does not.
#include <gtest/gtest.h>

#include <pqxx/pqxx>
#include <memory>
#include <string>

#include "contract_rolls_test_util.hpp"
#include "trade_ngin/data/per_contract_source.hpp"
#include "trade_ngin/data/report_roll_view.hpp"

using namespace trade_ngin;
using namespace trade_ngin::contract_rolls;
using contract_rolls_test::d;

class ReportRollViewDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string c = contract_rolls_test::dsn();
        if (c.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        try {
            conn_ = std::make_unique<pqxx::connection>(c);
            pqxx::read_transaction t(*conn_);
            const auto r = t.exec(
                "SELECT count(*) FILTER (WHERE expiration IS NULL), count(*) FROM "
                "futures_data.contract_ohlcv_1d WHERE root = 'SI' AND date BETWEEN '2025-11-01' AND '2025-11-30'");
            if (r[0][1].as<int64_t>() == 0 || r[0][0].as<int64_t>() > 0) {
                GTEST_SKIP() << "no silver contract bars with an expiration";
            }
        } catch (const std::exception& e) {
            GTEST_SKIP() << "database unreachable: " << e.what();
        }
    }
    void TearDown() override { PerContractSource::instance().disable(); }
    std::unique_ptr<pqxx::connection> conn_;
};

TEST_F(ReportRollViewDbTest, TheSilverRollAndOutlookAreTheWorkedExamples) {
    pqxx::read_transaction t(*conn_);
    ReportRollQuery q;
    q.date = "2025-11-21";
    q.held_symbols = {"SI.v.0", "MES.v.0"};
    q.rolls = {{"EXEC_SI.v.0_20251125_RC", "SI.v.0", "42218", "42044525", "2025-11-25"}};
    const auto view = report_roll_view(t, q, contract_rolls_test::shipped_calendars(),
                                       [](const std::string&) { return 0.005; });
    EXPECT_TRUE(view.per_contract_rolls);
    ASSERT_EQ(view.names.count("EXEC_SI.v.0_20251125_RC"), 1u);
    const auto& n = view.names.at("EXEC_SI.v.0_20251125_RC");
    EXPECT_EQ(n.name, "Silver");
    EXPECT_EQ(n.from_month, "December");
    EXPECT_EQ(n.to_month, "March");
    EXPECT_EQ(n.gap_places, 3);
    ASSERT_EQ(view.outlook.count("SI"), 1u);
    const auto& si = view.outlook.at("SI");
    EXPECT_EQ(si.held, "SIZ5");
    EXPECT_TRUE(si.in_window) << "the window opens 11-20";
    EXPECT_EQ(si.deadline, "2025-11-28");
    EXPECT_TRUE(si.first_notice);
    EXPECT_EQ(si.roll_by, "2025-11-27");
    EXPECT_EQ(si.target, "SIH6");
    EXPECT_EQ(si.cycle, "HKNUZ");
    ASSERT_EQ(view.outlook.count("MES"), 1u);
    // R6: MES is the rule's too; before the 2026-02-22 change it holds ESZ5, printed as the micro's month.
    const auto& mes = view.outlook.at("MES");
    if (mes.held_symbol == "ESZ5") {
        EXPECT_EQ(mes.held, "MESZ5");
        EXPECT_EQ(mes.target, "MESH6");
        EXPECT_EQ(mes.deadline, "2025-12-19");
        EXPECT_EQ(mes.roll_by, "2025-12-18");
        EXPECT_FALSE(mes.in_window) << "the window opens 12-12";
    } else {
        ADD_FAILURE() << "MES holds " << mes.held_symbol << " on 2025-11-21 (ES contract bars not loaded?)";
    }
    ReportRollQuery early = q;
    early.date = "2025-11-19";
    early.held_symbols = {"SI.v.0"};
    early.rolls.clear();
    const auto before = report_roll_view(t, early, contract_rolls_test::shipped_calendars(), {});
    ASSERT_EQ(before.outlook.count("SI"), 1u);
    EXPECT_FALSE(before.outlook.at("SI").in_window);
    EXPECT_TRUE(before.names.empty());
}

TEST_F(ReportRollViewDbTest, TheBarSourceLogsTheRulesFallbacksFromItsFloorOnce) {
    const std::string line = "ZW.v.0 2014-08-31 fallback vendor_bar: ZWZ4 has no contract row and the vendor "
                             "names it: the bar is the vendor's own print (close 573.75)";
    auto& source = PerContractSource::instance();
    pqxx::read_transaction t(*conn_);
    // The floor after the window: the load logs nothing, so the message is still new.
    source.enable(contract_rolls_test::shipped_calendars());
    source.set_log_floor(d("2026-09-14"));
    auto bars = per_contract_bars(t, "ZW", "2014-08-25 00:00:00+00", "2014-09-05 00:00:00+00",
                                  contract_rolls_test::shipped_calendars());
    ASSERT_TRUE(bars.is_ok()) << bars.error()->what();
    ASSERT_FALSE(bars.value().empty());
    EXPECT_TRUE(source.first_log(line)) << "a fallback before the floor was logged";
    // No floor: the load logs it (the message is taken), and a second load does not log it again.
    source.enable(contract_rolls_test::shipped_calendars());
    ASSERT_TRUE(per_contract_bars(t, "ZW", "2014-08-25 00:00:00+00", "2014-09-05 00:00:00+00",
                                  contract_rolls_test::shipped_calendars())
                    .is_ok());
    EXPECT_FALSE(source.first_log(line)) << "the load did not log the fallback of its window";
}
