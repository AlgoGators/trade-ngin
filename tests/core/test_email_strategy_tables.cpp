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

// A futures row with no basis keeps its margin: FuturesInstrument's margin is
// |qty| x initial_margin and does not depend on the price at all, so "no basis"
// is not "no margin" for a future. Dropping such a row out of the total would be
// a regression the throw never caused, and it is what a price-first guard does if
// it decides unpriceability from the price instead of from the instrument.
TEST_F(EmailStrategyTablesTest, AFuturesRowWithNoBasisStillContributesItsMargin) {
    const StrategyPositionsMap book{
        {"TREND_FOLLOWING", {{"ZFGOOD.v.0", held("ZFGOOD.v.0", 2.0, 0.0)}}}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_strategy_positions_tables(book, {}, {}); });
    EXPECT_NE(html.find("ZFGOOD.v.0"), std::string::npos);
    // 2 contracts at 12,000, exactly as with a basis: the price is not an input.
    EXPECT_NE(html.find("24,000.00"), std::string::npos)
        << "a futures row without a basis was dropped from the margin total; its margin "
           "never depended on the basis";
}

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
    // The sub-headers are title-cased by format_strategy_display_name, so both
    // strategies' sections are still rendered.
    EXPECT_NE(html.find("Mean Reversion"), std::string::npos);
    EXPECT_NE(html.find("Trend Following"), std::string::npos);
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
// An equity row that HAS a basis keeps the margin the old code gave it, even
// when a current close is also known: the per-strategy overload prices from the
// basis first, exactly as before. (Basis 50 x 100 shares x 50% = 2,500; pricing
// from the close of 52 would have given 2,600.)
TEST_F(EmailStrategyTablesTest, AnEquityRowWithABasisKeepsItsBasisPricedMargin) {
    const StrategyPositionsMap book{
        {"MEAN_REVERSION", {{"ZEPRICED", held("ZEPRICED", 100.0, 50.0)}}}};
    const std::unordered_map<std::string, double> prices{{"ZEPRICED", 52.0}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_strategy_positions_tables(book, prices, {}); });
    // Pin the margin cell itself, not any occurrence of the number: other
    // columns legitimately show close-priced figures.
    const auto margin_at = html.find("Margin:</strong> $2,500.00");
    EXPECT_NE(margin_at, std::string::npos)
        << "the equity row's margin was re-priced from the close instead of its basis; "
           "the per-strategy overload must price from the basis first, as it always did. "
           "HTML was:\n" << html;
    EXPECT_EQ(html.find("Margin:</strong> $2,600.00"), std::string::npos);
    EXPECT_NE(html.find("Total Margin Posted:</strong> $2,500.00"), std::string::npos)
        << "the portfolio total must be priced the same way as the per-strategy cell, "
           "from the basis, so the page agrees with itself. HTML was:\n" << html;
    EXPECT_EQ(html.find("Total Margin Posted:</strong> $2,600.00"), std::string::npos);
}

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

// CM1 d: an execution whose contract the instrument registry does not hold shows no guessed
// notional. The deleted fallback tables priced ZR at 2,000 and ZN at 100,000 (the metadata says
// 1,000); now the cell reads n/a and the row is left out of the notional total, both tables.
namespace {
ExecutionReport fill(const std::string& symbol, double qty, double price) {
    ExecutionReport e;
    e.symbol = symbol;
    e.side = Side::BUY;
    e.filled_quantity = Quantity(qty);
    e.fill_price = Price(price);
    e.total_transaction_costs = Decimal(3.0);
    return e;
}
}  // namespace

TEST_F(EmailStrategyTablesTest, AnExecutionWhoseContractIsNotInTheRegistryShowsNoGuessedNotional) {
    ASSERT_FALSE(InstrumentRegistry::instance().has_instrument("ZN"));
    const std::vector<ExecutionReport> execs = {fill("ZN.v.0", 1.0, 110.5)};

    const std::string single = sender_.format_executions_table(execs);
    EXPECT_EQ(single.find("$11050000.00"), std::string::npos) << single;
    EXPECT_NE(single.find("<td>n/a</td>"), std::string::npos) << single;

    const std::string per_strategy =
        sender_.format_single_strategy_executions_table("TREND_FOLLOWING", execs);
    EXPECT_EQ(per_strategy.find("11,050,000.00"), std::string::npos) << per_strategy;
    EXPECT_NE(per_strategy.find("<td>n/a</td>"), std::string::npos) << per_strategy;
    EXPECT_NE(per_strategy.find("<strong>Notional:</strong> $0.00"), std::string::npos)
        << per_strategy;
}

// A registered contract keeps its notional from the registry's multiplier.
TEST_F(EmailStrategyTablesTest, AnExecutionOfARegisteredContractKeepsItsNotional) {
    const std::vector<ExecutionReport> execs = {fill("ZFGOOD.v.0", 2.0, 100.0)};
    const std::string per_strategy =
        sender_.format_single_strategy_executions_table("TREND_FOLLOWING", execs);
    EXPECT_NE(per_strategy.find("$10,000.00"), std::string::npos) << per_strategy;  // 2 x 100 x 50
}

// T-ROLLX (LOOP_SPEC v6.1 section 6.5): the email's trade counts and traded notional are STRATEGY
// rows only; the ROLL legs are their own block with the two contract ids; every cost total keeps
// both. (ZFGOOD is a registered future with multiplier 50.)
namespace {
ExecutionReport typed(const std::string& symbol, Side side, double qty, double price, double cost,
                      ExecutionType type, const std::string& exec_id, const std::string& id) {
    ExecutionReport e;
    e.symbol = symbol;
    e.side = side;
    e.filled_quantity = Quantity(qty);
    e.fill_price = Price(price);
    e.total_transaction_costs = Decimal(cost);
    e.execution_type = type;
    e.exec_id = exec_id;
    e.instrument_id = id;
    return e;
}
}  // namespace

TEST_F(EmailStrategyTablesTest, RollLegsAreTheirOwnBlockNotTradesAndTheirCostIsInTheTotal) {
    const std::vector<ExecutionReport> execs = {
        typed("ZFGOOD.v.0", Side::SELL, 2.0, 100.0, 4.0, ExecutionType::ROLL, "EXEC_ZFGOOD.v.0_20251028_RC", "864"),
        typed("ZFGOOD.v.0", Side::BUY, 2.0, 103.0, 4.0, ExecutionType::ROLL, "EXEC_ZFGOOD.v.0_20251028_RO", "863"),
        typed("ZFGOOD.v.0", Side::BUY, 1.0, 103.0, 3.0, ExecutionType::STRATEGY, "EXEC_ZFGOOD.v.0_20251028", ""),
    };
    const std::string single = sender_.format_executions_table(execs);
    EXPECT_NE(single.find("<strong>Trades:</strong> 1<br>"), std::string::npos) << single;
    EXPECT_NE(single.find("<strong>Roll Fills:</strong> 2"), std::string::npos) << single;
    EXPECT_NE(single.find("Roll Costs (upper bound):</strong> $8.00"), std::string::npos) << single;
    EXPECT_NE(single.find("<strong>Transaction Costs:</strong> $11.00"), std::string::npos) << single;
    EXPECT_NE(single.find("<strong>Notional Traded:</strong> $5,150.00"), std::string::npos)
        << "1 x 103 x 50, the STRATEGY fill alone: " << single;
    EXPECT_NE(single.find("<td>864</td>"), std::string::npos) << single;
    EXPECT_NE(single.find("<td>863</td>"), std::string::npos) << single;

    const std::string per = sender_.format_single_strategy_executions_table("TREND_FOLLOWING", execs);
    EXPECT_NE(per.find("<strong>Trades:</strong> 1 |"), std::string::npos) << per;
    EXPECT_NE(per.find("<strong>Roll Fills:</strong> 2"), std::string::npos) << per;
    EXPECT_NE(per.find("<strong>Transaction Costs:</strong> $11.00"), std::string::npos) << per;

    std::unordered_map<std::string, std::vector<ExecutionReport>> by_sleeve{{"TREND_FOLLOWING", execs}};
    const std::string all = sender_.format_strategy_executions_tables(by_sleeve);
    EXPECT_NE(all.find("<strong>Total Trades:</strong> 1<"), std::string::npos) << all;
    EXPECT_NE(all.find("<strong>Total Notional Traded:</strong> $5,150.00"), std::string::npos) << all;
    EXPECT_NE(all.find("<strong>Total Transaction Costs:</strong> $11.00"), std::string::npos) << all;
    EXPECT_NE(all.find("<strong>Roll Fills:</strong> 2 | <strong>Roll Costs (upper bound):</strong> $8.00"), std::string::npos) << all;
}

// The cost after netting (HD 2026-10-09): every cost TOTAL of the email is what the account was
// charged, each row's own cost minus its signed adjustment; a row's own cost stays in its cell.
//   FAST  SELL 1 own 4.50 adjustment -0.915 (same side: charged 5.415)
//   TREND SELL 1 own 4.50 adjustment -0.915
//   TREND BUY 1 / FAST SELL 1 of another symbol, own 1.62 each, a full cross: charged 0
// RED on the own costs (12.24), and on a dropped or clamped negative adjustment (9.00).
TEST_F(EmailStrategyTablesTest, TheCostTotalsAreTheCostAfterNetting) {
    auto row = [](const std::string& symbol, Side side, double cost, double adjustment) {
        auto e = typed(symbol, side, 1.0, 103.0, cost, ExecutionType::STRATEGY, "EXEC_" + symbol, "");
        e.netting_adjustment = Decimal(adjustment);
        return e;
    };
    const std::vector<ExecutionReport> fast = {row("ZFGOOD.v.0", Side::SELL, 4.50, -0.915),
                                               row("ZFGOOD.v.0", Side::SELL, 1.62, 1.62)};
    const std::vector<ExecutionReport> trend = {row("ZFGOOD.v.0", Side::SELL, 4.50, -0.915),
                                                row("ZFGOOD.v.0", Side::BUY, 1.62, 1.62)};
    const std::string single = sender_.format_executions_table(fast);
    EXPECT_NE(single.find("<strong>Transaction Costs:</strong> $5.42"), std::string::npos) << single;
    const std::string per = sender_.format_single_strategy_executions_table("TREND_FOLLOWING", trend);
    EXPECT_NE(per.find("<strong>Transaction Costs:</strong> $5.42"), std::string::npos) << per;
    std::unordered_map<std::string, std::vector<ExecutionReport>> by_sleeve{
        {"TREND_FOLLOWING_FAST", fast}, {"TREND_FOLLOWING", trend}};
    const std::string all = sender_.format_strategy_executions_tables(by_sleeve);
    EXPECT_NE(all.find("<strong>Total Transaction Costs:</strong> $10.83"), std::string::npos) << all;
}

TEST_F(EmailStrategyTablesTest, WithoutRollLegsTheTablesAreAsBefore) {
    const std::vector<ExecutionReport> execs = {
        typed("ZFGOOD.v.0", Side::BUY, 1.0, 103.0, 3.0, ExecutionType::STRATEGY, "EXEC_ZFGOOD.v.0_20251028", ""),
    };
    const std::string single = sender_.format_executions_table(execs);
    EXPECT_NE(single.find("<strong>Trades:</strong> 1<br>"), std::string::npos);
    EXPECT_EQ(single.find("Roll Fills"), std::string::npos) << "no roll block without legs";
}
