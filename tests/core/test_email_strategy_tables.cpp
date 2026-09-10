// tests/core/test_email_strategy_tables.cpp
//
// FUT-email-zero-basis (C-5 B5): one unpriceable row must not suppress the whole
// futures daily report.
//
// 9348920d fixed this for the SINGLE-table overload, format_positions_table,
// which is what the equity runner's simple path uses. It did not touch the
// PER-STRATEGY overload, format_strategy_positions_tables ->
// format_single_strategy_table, which is what generate_trading_report_body calls
// for every runner that has more than one strategy -- the futures runners.
//
// There, margin was computed from Position::average_price alone and a
// non-positive result threw std::runtime_error, and the catch re-threw it. The
// throw escapes format_single_strategy_table, escapes
// format_strategy_positions_tables and escapes generate_trading_report_body, so
// the whole email is lost. The portfolio-total loop lower down prices margin the
// same way inside a catch(...) that swallows it, so the "Total Margin Posted"
// line understated with nothing said.
//
// Two different rows reach the non-positive margin, and both are covered:
//
//   * an EQUITY row with no basis and no close. EquityInstrument's margin is a
//     fraction of notional, so price 0 gives margin 0. This is the equity-shaped
//     defect on the multi-strategy path 9348920d missed.
//   * a FUTURES row whose instrument carries initial_margin 0.
//     FuturesInstrument::get_margin_requirement(price, qty) ignores price and
//     returns |qty| * initial_margin, so the basis cannot make it zero but a
//     contract whose metadata never got a margin can. That is the futures-shaped
//     reachability, and it is a metadata fault that must be reported rather than
//     suppress the report.

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>

// format_strategy_positions_tables is private; reach it the way
// test_email_positions_table.cpp already reaches this class. Every std header the
// target transitively pulls must be loaded BEFORE the macro flip, or libc++'s own
// private members are redeclared public and its internals stop compiling.
#include <algorithm>
#include <chrono>
#include <functional>
#include <iomanip>
#include <ios>
#include <iterator>
#include <numeric>
#include <optional>
#include <ostream>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

#define private public
#include "trade_ngin/core/email_sender.hpp"
#undef private

#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"

using namespace trade_ngin;

namespace {

EquitySpec equity_spec() {
    EquitySpec s;
    s.exchange = "NASDAQ";
    s.currency = "USD";
    s.lot_size = 1.0;
    s.tick_size = 0.01;
    s.commission_per_share = 0.005;
    s.is_etf = false;
    s.is_marginable = true;
    s.account_mode = EquityAccountMode::REG_T;
    s.sector = "Technology";
    s.industry = "Software";
    s.trading_hours = "09:30-16:00";
    return s;
}

FuturesSpec futures_spec(double initial_margin) {
    FuturesSpec s;
    s.root_symbol = "ZZ";
    s.exchange = "CME";
    s.currency = "USD";
    s.multiplier = 50.0;
    s.tick_size = 0.25;
    s.commission_per_contract = 2.0;
    s.initial_margin = initial_margin;
    s.maintenance_margin = initial_margin * 0.9;
    s.weight = 1.0;
    s.trading_hours = "17:00-16:00";
    return s;
}

Position held(const std::string& symbol, double qty, double average_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(average_price);
    p.unrealized_pnl = Decimal(0.0);
    p.realized_pnl = Decimal(0.0);
    p.last_update = std::chrono::system_clock::now();
    return p;
}

class EmailStrategyTablesTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& registry = InstrumentRegistry::instance();
        // The runners strip ".v.N" before the registry lookup, so register the roots.
        registry.register_instrument("ZEBASIS",
                                     std::make_shared<EquityInstrument>("ZEBASIS", equity_spec()));
        registry.register_instrument("ZEPRICED",
                                     std::make_shared<EquityInstrument>("ZEPRICED", equity_spec()));
        registry.register_instrument(
            "ZFNOMARGIN",
            std::make_shared<FuturesInstrument>("ZFNOMARGIN", futures_spec(0.0)));
        registry.register_instrument(
            "ZFGOOD", std::make_shared<FuturesInstrument>("ZFGOOD", futures_spec(12000.0)));
    }

    EmailSender sender_{EmailSenderConfig{}};
};

}  // namespace

// The defect, directly: an equity row with neither basis nor close must not
// abort the per-strategy tables.
TEST_F(EmailStrategyTablesTest, AZeroBasisRowDoesNotAbortTheStrategyTables) {
    const StrategyPositionsMap book{
        {"MEAN_REVERSION", {{"ZEBASIS", held("ZEBASIS", 100.0, 0.0)}}}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_strategy_positions_tables(book, {}, {}); })
        << "a single unresolved basis used to abort the entire daily report";
    EXPECT_NE(html.find("ZEBASIS"), std::string::npos)
        << "the row must still appear; excluding it silently would hide the position";
    EXPECT_NE(html.find("Total Margin Posted"), std::string::npos)
        << "the portfolio summary must still be rendered";
}

// A futures contract whose metadata carries no margin is the futures-reachable
// shape: the price is irrelevant to FuturesInstrument's margin, so only the spec
// can make it zero.
TEST_F(EmailStrategyTablesTest, AFuturesContractWithNoMarginInItsSpecDoesNotAbortTheTables) {
    const StrategyPositionsMap book{
        {"TREND_FOLLOWING", {{"ZFNOMARGIN.v.0", held("ZFNOMARGIN.v.0", 3.0, 4200.0)}}}};
    const std::unordered_map<std::string, double> prices{{"ZFNOMARGIN.v.0", 4250.0}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_strategy_positions_tables(book, prices, {}); })
        << "an instrument with initial_margin 0 used to abort the entire daily report";
    EXPECT_NE(html.find("ZFNOMARGIN.v.0"), std::string::npos);
}

// The row that cannot be priced costs the report nothing: every other row, and
// the strategy the healthy row belongs to, is still there in full.
TEST_F(EmailStrategyTablesTest, OneBadRowDoesNotSuppressTheOtherStrategies) {
    const StrategyPositionsMap book{
        {"MEAN_REVERSION", {{"ZEBASIS", held("ZEBASIS", 100.0, 0.0)}}},
        {"TREND_FOLLOWING", {{"ZFGOOD.v.0", held("ZFGOOD.v.0", 2.0, 4200.0)}}},
    };
    const std::unordered_map<std::string, double> prices{{"ZFGOOD.v.0", 4250.0}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_strategy_positions_tables(book, prices, {}); });
    EXPECT_NE(html.find("ZEBASIS"), std::string::npos);
    EXPECT_NE(html.find("ZFGOOD.v.0"), std::string::npos);
    EXPECT_NE(html.find("MEAN_REVERSION"), std::string::npos);
    EXPECT_NE(html.find("TREND_FOLLOWING"), std::string::npos);
    // 2 contracts at 12,000 initial margin, and the unpriceable row adds nothing.
    EXPECT_NE(html.find("24,000.00"), std::string::npos)
        << "the healthy row's margin must still reach the portfolio total";
}

// Margin is priced from a MARK: a current close makes a zero-basis equity row
// fully computable, so it must be used rather than falling straight to the
// warning and dropping the row out of the margin total.
TEST_F(EmailStrategyTablesTest, ACurrentCloseCoversAZeroBasis) {
    const StrategyPositionsMap book{
        {"MEAN_REVERSION", {{"ZEBASIS", held("ZEBASIS", 100.0, 0.0)}}}};
    const std::unordered_map<std::string, double> prices{{"ZEBASIS", 150.0}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_strategy_positions_tables(book, prices, {}); });
    EXPECT_NE(html.find("ZEBASIS"), std::string::npos);
    // REG_T is 50 percent of notional: 100 x 150 x 0.5 = 7,500.
    EXPECT_NE(html.find("7,500.00"), std::string::npos)
        << "with a close available the row must be INSIDE the margin total, not warned past";
}

// The healthy path is unchanged. This is what makes the change a reported-column
// one rather than a behavioural one.
TEST_F(EmailStrategyTablesTest, ANormalFuturesBookIsUnaffected) {
    const StrategyPositionsMap book{
        {"TREND_FOLLOWING",
         {{"ZFGOOD.v.0", held("ZFGOOD.v.0", 2.0, 4200.0)},
          {"ZFNOMARGIN.v.0", held("ZFNOMARGIN.v.0", 0.0, 0.0)}}}};
    const std::unordered_map<std::string, double> prices{{"ZFGOOD.v.0", 4250.0}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_strategy_positions_tables(book, prices, {}); });
    EXPECT_NE(html.find("ZFGOOD.v.0"), std::string::npos);
    EXPECT_EQ(html.find("ZFNOMARGIN.v.0"), std::string::npos)
        << "a zero-quantity row is not an open position";
    EXPECT_NE(html.find("24,000.00"), std::string::npos);
}

// An empty book is still the empty answer, not a crash.
TEST_F(EmailStrategyTablesTest, AnEmptyBookRendersTheEmptyAnswer) {
    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_strategy_positions_tables({}, {}, {}); });
    EXPECT_NE(html.find("No positions"), std::string::npos);
}
