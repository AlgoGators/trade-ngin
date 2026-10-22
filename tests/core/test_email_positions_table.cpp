// tests/core/test_email_positions_table.cpp
//
// D9 / BA-16: one unpriceable row must not suppress the whole daily email.
//
// format_positions_table computed margin from Position::average_price alone.
// On the equity path that column is a COST BASIS, and 0 is its documented "no
// basis known" value (docs/AVERAGE_PRICE_LIFECYCLE.md rule 5) -- reachable for a
// held position whose basis could not be resolved, a state the runner already
// reports as an ERROR and carries inert. get_margin_requirement(0, qty) then
// returned 0, the function threw, and the bare `throw;` in its catch propagated
// out: one such row and nobody received a report at all.
//
// Margin asks what a position is WORTH, so the current close is the right input
// and the basis is the fallback -- the same preference the notional block in the
// same loop already applies.

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>

// format_positions_table is private; reach it the way this suite already
// reaches ConfigLoader's internals. Every std header the target transitively
// pulls must be loaded BEFORE the macro flip, or libc++'s own private members
// are redeclared public and its internals stop compiling.
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

class EmailPositionsTableTest : public ::testing::Test {
protected:
    void SetUp() override {
        EmailSender::reset_footnotes();
        auto& registry = InstrumentRegistry::instance();
        registry.register_instrument(
            "ZBASIS", std::make_shared<EquityInstrument>("ZBASIS", equity_spec()));
        registry.register_instrument(
            "ZPRICED", std::make_shared<EquityInstrument>("ZPRICED", equity_spec()));
    }

    EmailSender sender_{EmailSenderConfig{}};
};

}  // namespace

// The defect, directly: a held row with no basis and no close must not throw.
TEST_F(EmailPositionsTableTest, AZeroBasisHeldRowDoesNotAbortTheEmail) {
    const std::unordered_map<std::string, Position> positions{
        {"ZBASIS", held("ZBASIS", 100.0, 0.0)}};

    std::string html;
    ASSERT_NO_THROW({
        html = sender_.format_positions_table(positions, true, {}, {});
    }) << "a single unresolved basis used to abort the entire daily report";

    EXPECT_NE(html.find("ZBASIS"), std::string::npos)
        << "the row must still appear; excluding it silently would hide the position";
    EXPECT_NE(html.find("<table>"), std::string::npos) << "a real table, not an empty string";
}

// The same row alongside a normal one: the healthy position's figures survive.
TEST_F(EmailPositionsTableTest, AZeroBasisRowDoesNotSuppressTheOtherPositions) {
    const std::unordered_map<std::string, Position> positions{
        {"ZBASIS", held("ZBASIS", 100.0, 0.0)},
        {"ZPRICED", held("ZPRICED", 50.0, 200.0)},
    };
    const std::unordered_map<std::string, double> prices{{"ZPRICED", 210.0}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_positions_table(positions, true, prices, {}); });

    EXPECT_NE(html.find("ZPRICED"), std::string::npos);
    EXPECT_NE(html.find("ZBASIS"), std::string::npos)
        << "both rows are reported; one bad row costs the report nothing";
}

// Margin is priced from a MARK. A current close makes a zero-basis row fully
// computable, so it must be used rather than falling straight to the warning.
TEST_F(EmailPositionsTableTest, ACurrentCloseCoversAZeroBasis) {
    const std::unordered_map<std::string, Position> positions{
        {"ZBASIS", held("ZBASIS", 100.0, 0.0)}};
    const std::unordered_map<std::string, double> prices{{"ZBASIS", 150.0}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_positions_table(positions, true, prices, {}); });
    EXPECT_NE(html.find("ZBASIS"), std::string::npos);
    // The close, not the basis, is what the row reports as its market price.
    EXPECT_NE(html.find("150"), std::string::npos)
        << "the current close must reach the row; it is also what margin is priced from";
}

// A normal book is unchanged -- the guard must not alter the healthy path.
TEST_F(EmailPositionsTableTest, ANormalBookIsUnaffected) {
    const std::unordered_map<std::string, Position> positions{
        {"ZPRICED", held("ZPRICED", 50.0, 200.0)}};
    const std::unordered_map<std::string, double> prices{{"ZPRICED", 210.0}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_positions_table(positions, true, prices, {}); });
    EXPECT_NE(html.find("ZPRICED"), std::string::npos);
    EXPECT_NE(html.find("210"), std::string::npos);
}

// A closed row carries no basis by design (rule 5) and is skipped by the
// quantity filter, so it can never reach the margin path at all. Pinned so a
// future change to that filter surfaces here.
TEST_F(EmailPositionsTableTest, AClosedRowIsNotReportedAsAPosition) {
    const std::unordered_map<std::string, Position> positions{
        {"ZBASIS", held("ZBASIS", 0.0, 0.0)}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_positions_table(positions, true, {}, {}); });
    EXPECT_EQ(html.find("ZBASIS"), std::string::npos)
        << "a zero-quantity row is not an open position";
}

// T-8D D3 (a): the margin line is priced at the cost basis, the price MarginManager stores
// margin_posted at, so the email and the database agree. 50 shares bought at 200, closing at
// 210, Reg T long: 0.5 x 50 x 200 = 5,000 (at the close it read 5,250). The notional is the
// other way round (T-8D-2 R60): at the close.
TEST_F(EmailPositionsTableTest, TheMarginLineIsPricedAtTheCostBasis) {
    const std::unordered_map<std::string, Position> positions{
        {"ZPRICED", held("ZPRICED", 50.0, 200.0)}};
    const std::unordered_map<std::string, double> prices{{"ZPRICED", 210.0}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_positions_table(positions, true, prices, {}); });
    EXPECT_NE(html.find("<strong>Total Margin Posted:</strong> $5,000"), std::string::npos)
        << html.substr(html.find("Total Notional") == std::string::npos
                           ? 0
                           : html.find("Total Notional"));
    EXPECT_EQ(html.find("<strong>Total Margin Posted:</strong> $5,250"), std::string::npos);
    // T-8D-2 R60 (T-8D D9): the notional is at the Day T-1 close, 50 x 210 = 10,500 (at the basis
    // it read 10,000), and so is the row: its notional is its quantity at the price it prints.
    EXPECT_NE(html.find("<strong>Total Notional:</strong> $10,500.00"), std::string::npos) << html;
    EXPECT_EQ(html.find("$10,000"), std::string::npos) << html;
    EXPECT_NE(html.find("<td>$210.0000</td>\n<td>$10,500.00</td>\n<td>100.00%</td>"),
              std::string::npos)
        << html;
}

// A held row with no close in the map is priced at its basis, in the price cell and the notional.
TEST_F(EmailPositionsTableTest, ARowWithNoCloseIsPricedAtItsBasisInBothCells) {
    const std::unordered_map<std::string, Position> positions{
        {"ZPRICED", held("ZPRICED", 50.0, 200.0)}};
    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_positions_table(positions, true, {}, {}); });
    EXPECT_NE(html.find("<td>$200.0000<sup style=\"font-size: 9px; color: #000;\">1</sup></td>\n<td>$10,000.00</td>"), std::string::npos) << html;
    EXPECT_NE(html.find("<strong>Total Notional:</strong> $10,000.00"), std::string::npos) << html;
}

// With no basis the close is the fallback: 0.5 x 100 x 150 = 7,500.
TEST_F(EmailPositionsTableTest, TheMarginLineFallsBackToTheCloseWithoutABasis) {
    const std::unordered_map<std::string, Position> positions{
        {"ZBASIS", held("ZBASIS", 100.0, 0.0)}};
    const std::unordered_map<std::string, double> prices{{"ZBASIS", 150.0}};

    std::string html;
    ASSERT_NO_THROW({ html = sender_.format_positions_table(positions, true, prices, {}); });
    EXPECT_NE(html.find("<strong>Total Margin Posted:</strong> $7,500"), std::string::npos);
}

// HD 2026-10-10: the summary under the single positions table prints the overlay's expected risk
// beside the risk target and the one-day 95 percent VaR in dollars, in place of "Portfolio VaR".
// HD 10-13: neither the expected risk, nor the VaR, nor a portfolio VaR is in the summary any more
// (the first two are in the account detail file).
TEST_F(EmailPositionsTableTest, TheSummaryCarriesNoExpectedRiskAndNoVar) {
    const std::unordered_map<std::string, Position> positions{
        {"ZPRICED", held("ZPRICED", 50.0, 200.0)}};
    const std::unordered_map<std::string, double> prices{{"ZPRICED", 210.0}};
    const std::map<std::string, double> metrics{{"Portfolio VaR", 7.12}, {"Expected Risk", 12.3449},
                                                {"Risk Target", 20.0}, {"VaR 95 1-Day", 1234567.891}};
    const std::string html = sender_.format_positions_table(positions, true, prices, metrics);
    for (const char* absent : {"Portfolio VaR", "Expected Risk", "95% VaR"})
        EXPECT_EQ(html.find(absent), std::string::npos) << absent;
}

// T-EMAIL P2-3: the rows are in symbol order, whatever order the map hands them out in. The same
// twelve positions are put in a map built two ways (different bucket counts, reversed insertion),
// which iterate differently, and the table must be the same text with the symbols ascending.
TEST_F(EmailPositionsTableTest, RowsAreInSymbolOrderWhateverTheMapIterationOrderIs) {
    std::vector<std::string> symbols;
    for (int i = 1; i <= 12; ++i) {
        std::string s = "ZORD" + std::string(i < 10 ? "0" : "") + std::to_string(i);
        InstrumentRegistry::instance().register_instrument(
            s, std::make_shared<EquityInstrument>(s, equity_spec()));
        symbols.push_back(s);
    }
    std::unordered_map<std::string, Position> forward;
    forward.reserve(8);
    for (const auto& s : symbols)
        forward.emplace(s, held(s, 10.0, 100.0));
    std::unordered_map<std::string, Position> backward;
    backward.reserve(1000);
    for (auto it = symbols.rbegin(); it != symbols.rend(); ++it) {
        backward.emplace(*it, held(*it, 10.0, 100.0));
    }

    const std::string a = sender_.format_positions_table(forward, true, {}, {});
    const std::string b = sender_.format_positions_table(backward, true, {}, {});
    EXPECT_EQ(a, b);
    std::size_t at = 0;
    for (const auto& s : symbols) {
        const std::size_t found = a.find(s, at);
        ASSERT_NE(found, std::string::npos) << s << " is missing or out of order";
        at = found;
    }
}
