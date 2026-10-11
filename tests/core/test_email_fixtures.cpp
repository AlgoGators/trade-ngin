// tests/core/test_email_fixtures.cpp
//
// One fixture per book: the daily report body of a futures book of one sleeve (CONSERVATIVE), of
// a futures book of two sleeves (BASE) and of the equity book, rendered by CALLING the report
// builder with fixed inputs and no database, and compared byte for byte with the stored body under
// tests/core/fixtures/email/. A change to the report shows as a diff of the stored body in the
// commit that makes it.
//
// The inputs are what each runner hands the builder (the key names of its metric maps included),
// on figures shaped like a stored day (the held symbols are the fixture's own roots, Q-prefixed,
// because the instrument registry is one per process and other tests assert real roots absent): a
// held row whose basis is not its close, a price below one cent, a symbol with no bar on the
// settled day, a roll's two legs, a netted day on the book of two sleeves, fractional shares on the
// equity book. With no database the charts, the Symbols Reference and the rollover notice are not
// built; those are executed by their own tests.
//
// To rewrite the stored bodies after an intended change:
//   TRADE_NGIN_UPDATE_EMAIL_FIXTURES=1 ./trade_ngin_tests --gtest_filter='EmailFixture*'

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/report_over_limit.hpp"
#include "trade_ngin/core/report_risk_lines.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"

using namespace trade_ngin;

namespace {

namespace fs = std::filesystem;

// tests/core/fixtures/email, found from the test's working directory (the build tree sits inside
// the source tree).
fs::path fixture_dir() {
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        const fs::path candidate = dir / "tests" / "core" / "fixtures" / "email";
        if (fs::exists(candidate))
            return candidate;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

// "Today's Positions" walks an unordered map, so the order of its rows is the standard library's
// (the known row-order item of the email pass). The comparison therefore reads each run of
// multi-line table rows ("<tr>" alone on a line to "</tr>" alone on a line) in sorted order; every
// other line is compared where it stands.
std::string with_sorted_table_rows(const std::string& body) {
    const std::vector<std::string> lines = lines_of(body);
    std::string out;
    std::vector<std::string> run;
    const auto flush = [&]() {
        std::sort(run.begin(), run.end());
        for (const auto& block : run)
            out += block;
        run.clear();
    };
    for (size_t i = 0; i < lines.size();) {
        if (lines[i] == "<tr>") {
            std::string block;
            size_t j = i;
            for (; j < lines.size(); ++j) {
                block += lines[j] + "\n";
                if (lines[j] == "</tr>")
                    break;
            }
            run.push_back(block);
            i = j + 1;
            continue;
        }
        flush();
        out += lines[i] + "\n";
        ++i;
    }
    flush();
    return out;
}

// The body equals the stored one; on a difference the first differing line of each is shown.
void expect_body_is_fixture(const std::string& name, const std::string& rendered) {
    const std::string body = with_sorted_table_rows(rendered);
    const fs::path dir = fixture_dir();
    ASSERT_FALSE(dir.empty()) << "tests/core/fixtures/email not found from "
                              << fs::current_path().string();
    const fs::path path = dir / name;
    if (std::getenv("TRADE_NGIN_UPDATE_EMAIL_FIXTURES") != nullptr) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << body;
        ASSERT_TRUE(out.good()) << "could not write " << path.string();
        return;
    }
    ASSERT_TRUE(fs::exists(path)) << path.string() << " is missing";
    const std::string stored = read_file(path);
    if (body == stored)
        return;
    const auto got = lines_of(body);
    const auto want = lines_of(stored);
    size_t at = 0;
    while (at < got.size() && at < want.size() && got[at] == want[at])
        ++at;
    ADD_FAILURE() << name << ": the body differs from the stored body at line " << (at + 1) << " ("
                  << got.size() << " lines rendered, " << want.size() << " stored)\n"
                  << "  stored:   " << (at < want.size() ? want[at] : "<end of body>") << "\n"
                  << "  rendered: " << (at < got.size() ? got[at] : "<end of body>");
}

FuturesSpec futures_spec(const std::string& root, double multiplier, double initial_margin) {
    FuturesSpec s;
    s.root_symbol = root;
    s.exchange = "CME";
    s.currency = "USD";
    s.multiplier = multiplier;
    s.tick_size = 0.25;
    s.commission_per_contract = 2.0;
    s.initial_margin = initial_margin;
    s.maintenance_margin = initial_margin * 0.9;
    s.weight = 1.0;
    s.trading_hours = "17:00-16:00";
    return s;
}

EquitySpec cash_equity_spec() {
    EquitySpec s;
    s.exchange = "NASDAQ";
    s.currency = "USD";
    s.lot_size = 1.0;
    s.tick_size = 0.01;
    s.commission_per_share = 0.005;
    s.is_etf = false;
    s.is_marginable = true;
    s.account_mode = EquityAccountMode::CASH;
    s.sector = "Technology";
    s.industry = "Software";
    s.trading_hours = "09:30-16:00";
    return s;
}

Position row(const std::string& symbol, double quantity, double average_price,
             double realized_pnl = 0.0) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(quantity);
    p.average_price = Decimal(average_price);
    p.unrealized_pnl = Decimal(0.0);
    p.realized_pnl = Decimal(realized_pnl);
    return p;
}

ExecutionReport fill(const std::string& symbol, Side side, double quantity, double price,
                     double cost, double netting_adjustment = 0.0) {
    ExecutionReport e;
    e.symbol = symbol;
    e.side = side;
    e.filled_quantity = Quantity(quantity);
    e.fill_price = Price(price);
    e.total_transaction_costs = Decimal(cost);
    e.netting_adjustment = Decimal(netting_adjustment);
    return e;
}

ExecutionReport roll_leg(const std::string& symbol, bool closing, Side side, double price,
                         double cost, const std::string& contract) {
    ExecutionReport e = fill(symbol, side, 1.0, price, cost);
    e.execution_type = ExecutionType::ROLL;
    e.exec_id = std::string("ROLL_") + symbol + (closing ? "_RC" : "_RO");
    e.instrument_id = contract;
    return e;
}

// The overlay limits both futures books run with (L_max 8, L_net_max 6).
std::vector<RiskModuleConfig> overlay_modules() {
    CarverModuleConfig carver;
    carver.r_max = 2.25;
    carver.r_jump_max = 4.5;
    carver.r_shock_max = 4.0;
    carver.max_gross_leverage = 8.0;
    carver.max_net_leverage = 6.0;
    RiskModuleConfig module;
    module.id = "carver";
    module.type = "carver";
    module.params = carver;
    return {module};
}

OverLimitReport over_limit_of(double sizing_capital, double gross, double net) {
    OnePassDay day;
    day.ran = true;
    day.sized = true;
    day.sizing_capital = sizing_capital;
    OverlayColumns columns;
    columns.overlay_gross_leverage = gross;
    columns.overlay_net_leverage = net;
    return over_limit_report(day, columns, overlay_modules(), 2.0);
}

class EmailFixture : public ::testing::Test {
protected:
    void SetUp() override {
        auto& registry = InstrumentRegistry::instance();
        const auto future = [&](const std::string& root, double multiplier, double margin) {
            registry.register_instrument(root, std::make_shared<FuturesInstrument>(
                                                   root, futures_spec(root, multiplier, margin)));
        };
        future("QZT", 2000.0, 1100.0);
        future("QZN", 1000.0, 2200.0);
        future("Q6J", 12500000.0, 3300.0);
        future("Q6L", 100000.0, 1500.0);
        future("QKE", 50.0, 2400.0);
        future("QMBT", 0.1, 2000.0);
        future("QMES", 5.0, 2300.0);
        future("QM2K", 5.0, 1000.0);
        for (const char* symbol : {"QGOOGL", "QTMUS"}) {
            registry.register_instrument(
                symbol, std::make_shared<EquityInstrument>(symbol, cash_equity_spec()));
        }
    }

    // What both futures runners put in the report's metric map (the key names are the runners').
    static std::map<std::string, double> futures_metrics() {
        std::map<std::string, double> m = {
            {"Daily Return", -0.000407},
            {"Daily Unrealized PnL", 0.0},
            {"Daily Realized PnL", 0.0},
            {"Daily Total PnL", -5.99},
            {"Total Cumulative Return", 0.70},
            {"Total Annualized Return", 1.29},
            {"Total Unrealized PnL", 0.0},
            {"Total Realized PnL", 4553.59},
            {"Total PnL", 3505.07},
            {"Sharpe Ratio", 0.2013},
            {"Sortino Ratio", 0.2704},
            {"Max Drawdown", 6.2271},
            {"Volatility", 7.7612},
            {"Win Rate", 50.8772},
            {"Average Win", 0.3121},
            {"Average Loss", -0.3011},
            {"Profit Factor", 1.0733},
            {"Best Day", 1.2341},
            {"Worst Day", -1.4127},
            {"Downside Deviation", 5.7748},
            {"Gross Profit", 27.1527},
            {"Gross Loss", -25.2924},
            {"Winning Days", 87.0},
            {"Losing Days", 84.0},
            {"Flat Days", 0.0},
            {"Total Days", 171.0},
            {"Total Transaction Costs", 1048.52},
            {"Total Roll Costs", 310.44},
            {"Daily Roll Costs", 4.90},
            {"Current Portfolio Value", 503505.07},
            {"Gross Leverage", 0.8581},
            {"Net Leverage", 0.2417},
            {"Equity-to-Margin Ratio", 37.5749},
            {"Margin Cushion", 97.6049},
            {"Margin Posted", 13400.0},
            {"Cash Available", 490105.07},
        };
        for (const auto& [name, value] : report_risk_metrics(0.0838, 0.20, 4555.42, 7, 7)) {
            m[name] = value;
        }
        return m;
    }

    // CONSERVATIVE: one sleeve, Tuesday 2026-04-28, the settled day Monday 2026-04-27.
    std::string conservative_body() {
        const std::unordered_map<std::string, Position> sleeve = {
            {"QZT.v.0", row("QZT.v.0", -1.0, 103.73828125)},
            {"Q6J.v.0", row("Q6J.v.0", -1.0, 0.0062995)},
            {"QKE.v.0", row("QKE.v.0", 1.0, 677.0)},
            // held at its stored price by the book gate: the basis is not the close
            {"Q6L.v.0", row("Q6L.v.0", 2.0, 0.2004)},
            {"QMBT.v.0", row("QMBT.v.0", 1.0, 77740.0)},
            {"QMES.v.0", row("QMES.v.0", 1.0, 7217.25)},
            {"QM2K.v.0", row("QM2K.v.0", 2.0, 2806.7)},
        };
        const StrategyPositionsMap book = {{"TREND_FOLLOWING", sleeve}};
        const std::unordered_map<std::string, double> marks = {
            {"QZT.v.0", 103.73828125}, {"Q6J.v.0", 0.0062995}, {"QKE.v.0", 677.0},
            {"Q6L.v.0", 0.1998},       {"QMBT.v.0", 77740.0},  {"QMES.v.0", 7217.25},
            {"QM2K.v.0", 2806.7}};
        const StrategyExecutionsMap executions = {
            {"TREND_FOLLOWING",
             {fill("QM2K.v.0", Side::BUY, 1.0, 2806.7, 1.09),
              roll_leg("QMBT.v.0", true, Side::SELL, 77960.0, 2.45, "42185193"),
              roll_leg("QMBT.v.0", false, Side::BUY, 79070.0, 2.45, "42013708")}}};
        // The settled day's rows as stored after the finalize. HE printed no bar on either day;
        // MBT's row is the confirming bar of its roll (no P&L booked on it).
        const StrategyPositionsMap settled = {
            {"TREND_FOLLOWING",
             {{"QZT.v.0", row("QZT.v.0", -1.0, 103.73828125, -46.88)},
              {"Q6J.v.0", row("Q6J.v.0", -1.0, 0.0062995, -168.75)},
              {"QKE.v.0", row("QKE.v.0", 1.0, 677.0, 425.0)},
              {"Q6L.v.0", row("Q6L.v.0", 2.0, 0.2004, 80.0)},
              {"QMBT.v.0", row("QMBT.v.0", 1.0, 77740.0, 0.0)},
              {"QMES.v.0", row("QMES.v.0", 1.0, 7217.25, 318.75)},
              {"HE.v.0", row("HE.v.0", 1.0, 91.3, 0.0)}}}};
        const std::unordered_map<std::string, double> t1_close = {
            {"QZT.v.0", 103.73828125}, {"Q6J.v.0", 0.0062995}, {"QKE.v.0", 677.0},
            {"Q6L.v.0", 0.1998},       {"QMBT.v.0", 77740.0},  {"QMES.v.0", 7217.25}};
        const std::unordered_map<std::string, double> t2_close = {
            {"QZT.v.0", 103.71484375}, {"Q6J.v.0", 0.006313}, {"QKE.v.0", 668.5},
            {"Q6L.v.0", 0.2},          {"QMBT.v.0", 79070.0}, {"QMES.v.0", 7153.5}};
        const std::map<std::string, double> settled_metrics = {
            {"Daily Return", 0.1201},          {"Daily Unrealized PnL", 0.0},
            {"Daily Realized PnL", 608.12},    {"Daily Total PnL", 604.19},
            {"Daily Transaction Costs", 3.93}, {"Daily Roll Costs", 2.84}};
        std::unordered_map<std::string, Position> combined = sleeve;
        return sender_.generate_trading_report_body(
            book, combined, std::nullopt, futures_metrics(), executions, "2026-04-28",
            "CONSERVATIVE_PORTFOLIO", true, marks, nullptr, settled, t1_close, t2_close,
            settled_metrics, "LIVE_TREND_FOLLOWING",
            over_limit_of(501250.34, 0.8619713, 0.2427915));
    }

    // BASE: two sleeves, Monday 2026-04-27, the settled day Sunday 2026-04-26. The sleeves hold
    // ZN on opposite sides (the account's notional and margin net them) and both traded MES (a
    // netted day).
    std::string base_body() {
        const std::unordered_map<std::string, Position> trend = {
            {"QMES.v.0", row("QMES.v.0", 2.0, 7153.5)},
            {"QZN.v.0", row("QZN.v.0", 2.0, 110.890625)},
            {"QKE.v.0", row("QKE.v.0", 1.0, 668.5)},
        };
        const std::unordered_map<std::string, Position> fast = {
            {"QMES.v.0", row("QMES.v.0", 1.0, 7153.5)},
            {"QZN.v.0", row("QZN.v.0", -1.0, 110.890625)},
        };
        const StrategyPositionsMap book = {{"TREND_FOLLOWING", trend},
                                           {"TREND_FOLLOWING_FAST", fast}};
        const std::unordered_map<std::string, Position> combined = {
            {"QMES.v.0", row("QMES.v.0", 3.0, 7153.5)},
            {"QZN.v.0", row("QZN.v.0", 1.0, 110.890625)},
            {"QKE.v.0", row("QKE.v.0", 1.0, 668.5)},
        };
        const std::unordered_map<std::string, double> marks = {
            {"QMES.v.0", 7153.5}, {"QZN.v.0", 110.890625}, {"QKE.v.0", 668.5}};
        // TREND bought 2 MES and FAST sold 1: the account traded 1, so each sleeve's row carries
        // a share of the cost the account did not pay.
        const StrategyExecutionsMap executions = {
            {"TREND_FOLLOWING", {fill("QMES.v.0", Side::BUY, 2.0, 7153.5, 2.18, 0.7267)}},
            {"TREND_FOLLOWING_FAST", {fill("QMES.v.0", Side::SELL, 1.0, 7153.5, 1.09, 0.3633)}}};
        const StrategyPositionsMap settled = {
            {"TREND_FOLLOWING",
             {{"QZN.v.0", row("QZN.v.0", 2.0, 110.890625, 62.5)},
              // an agricultural root, no longer held, with no P&L on a Sunday
              {"KE.v.0", row("KE.v.0", 1.0, 668.5, 0.0)}}},
            {"TREND_FOLLOWING_FAST", {{"QZN.v.0", row("QZN.v.0", -1.0, 110.890625, -31.25)}}}};
        const std::unordered_map<std::string, double> t1_close = {{"QZN.v.0", 110.890625},
                                                                  {"KE.v.0", 668.5}};
        const std::unordered_map<std::string, double> t2_close = {{"QZN.v.0", 110.859375},
                                                                  {"KE.v.0", 668.5}};
        const std::map<std::string, double> settled_metrics = {
            {"Daily Return", 0.0062},         {"Daily Unrealized PnL", 0.0},
            {"Daily Realized PnL", 31.25},    {"Daily Total PnL", 31.25},
            {"Daily Transaction Costs", 0.0}, {"Daily Roll Costs", 0.0}};
        std::map<std::string, double> metrics = futures_metrics();
        metrics["Margin Posted"] = 11500.0;  // the account's: MES 3, ZN 1 net, KE 1
        metrics["Cash Available"] = 492005.07;
        return sender_.generate_trading_report_body(
            book, combined, std::nullopt, metrics, executions, "2026-04-27", "BASE_PORTFOLIO", true,
            marks, nullptr, settled, t1_close, t2_close, settled_metrics,
            "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST",
            over_limit_of(499357.14, 0.5035118, 0.5035118));
    }

    // The equity book: Monday 2026-06-22; its settled day is the previous session, Thursday
    // 2026-06-18 (Friday 06-19 was a market holiday). The metric maps carry the keys the equity
    // runner sets.
    std::string equity_body() {
        const std::unordered_map<std::string, Position> positions = {
            {"QTMUS", row("QTMUS", 25.791441, 173.97)},
            {"QGOOGL", row("QGOOGL", 11.093403, 337.39)},
        };
        const std::unordered_map<std::string, double> closes = {{"QTMUS", 173.97},
                                                                {"QGOOGL", 353.65}};
        const std::vector<ExecutionReport> executions = {
            fill("QTMUS", Side::BUY, 25.791441, 173.97, 1.34),
            fill("QGOOGL", Side::SELL, 2.370356, 353.65, 1.02)};
        const std::unordered_map<std::string, Position> settled = {
            {"QGOOGL", row("QGOOGL", 13.463759, 337.39, 0.0)}};
        const std::unordered_map<std::string, double> t1_close = {{"QGOOGL", 353.65}};
        const std::unordered_map<std::string, double> t2_close = {{"QGOOGL", 337.39}};
        const std::map<std::string, double> settled_metrics = {
            {"Daily Return", 0.22},          {"Daily Unrealized PnL", 218.92},
            {"Daily Realized PnL", 0.0},     {"Daily Total PnL", 217.83},
            {"Daily Commissions", 1.090112}, {"Dividend Income (cumulative)", 6.565561}};
        const std::map<std::string, double> metrics = {
            {"Daily Return", -0.0024},
            {"Daily Unrealized PnL", 0.0},
            {"Daily Realized PnL", 0.0},
            {"Daily Total PnL", -2.36},
            {"Total Cumulative Return", -0.45},
            {"Total Annualized Return", -1.86},
            {"Total Unrealized PnL", 180.38},
            {"Total Realized PnL", -559.17},
            {"Total PnL", -448.21},
            {"Volatility", 19.24},
            {"Total Commissions", 69.42},
            {"Current Portfolio Value", 99551.79},
            {"Dividend Income (cumulative, informational)", 6.565561},
            {"Gross Leverage", 0.0845},
            {"Net Leverage", 0.0845},
            {"Portfolio Leverage", 0.0845},
            {"Equity-to-Margin Ratio", 12.0966},
            {"Margin Cushion", 100.0},
            {"Margin Posted", 8229.74},
            {"Cash Available", 91322.05},
        };
        return sender_.generate_trading_report_body(
            positions, std::nullopt, metrics, executions, "2026-06-22", true, closes, nullptr,
            settled, t1_close, t2_close, settled_metrics, "LIVE_EQUITY_MEAN_REVERSION",
            "EQUITY_MR_PORTFOLIO");
    }

    EmailSender sender_{EmailSenderConfig{}};
};

}  // namespace

TEST_F(EmailFixture, TheConservativeBodyIsTheStoredBody) {
    expect_body_is_fixture("conservative_2026-04-28.html", conservative_body());
}

TEST_F(EmailFixture, TheBaseBodyIsTheStoredBody) {
    expect_body_is_fixture("base_2026-04-27.html", base_body());
}

TEST_F(EmailFixture, TheEquityBodyIsTheStoredBody) {
    expect_body_is_fixture("equity_2026-06-22.html", equity_body());
}

// T-8D D1, T-8D-2 R62 and the lead's "raw closes beside settled P&L": the labels, on the bodies.
TEST_F(EmailFixture, TheSleeveNamesItsKindAndTheSettledTableNamesItsMarks) {
    const std::string futures = conservative_body();
    EXPECT_NE(futures.find(">Trend Following (futures)</h3>\n<table>\n<tr><th>Symbol</th>"
                           "<th>Quantity</th><th>Market Price</th>"),
              std::string::npos);
    const std::string base = base_body();
    EXPECT_NE(base.find(">Trend Following Fast (futures)</h3>"), std::string::npos);
    const std::string marks =
        "<th>Prior mark (T-2 close)</th><th>Mark (T-1 close)</th>"
        "<th>Realized PnL</th>";
    const std::string note =
        "The two marks are raw closes. Realized PnL is the settled day's "
        "stored figure; it is not recomputed from the two marks";
    for (const std::string& body : {futures, base, equity_body()}) {
        EXPECT_NE(body.find(marks), std::string::npos);
        EXPECT_EQ(body.find("Entry Price"), std::string::npos);
        EXPECT_EQ(body.find("Exit Price"), std::string::npos);
        const auto at = body.find(note);
        ASSERT_NE(at, std::string::npos);
        EXPECT_EQ(body.find(note, at + 1), std::string::npos) << "the note is printed once";
    }
    // The executions footer of a futures book: today's, and the netting lines above it stay.
    EXPECT_NE(base.find("<strong>Netting Adjustment:</strong> -$1.09</div>\n"
                        "<div class=\"metric\"><strong>Today's Transaction Costs:</strong> $2.18"),
              std::string::npos);
    EXPECT_EQ(futures.find("Netting Adjustment"), std::string::npos)
        << "a one-sleeve body carries no netting line";
}

// The same inputs render the same bytes twice: the body carries no clock.
TEST_F(EmailFixture, EveryBodyRendersTheSameBytesTwice) {
    EXPECT_EQ(conservative_body(), conservative_body());
    EXPECT_EQ(base_body(), base_body());
    EXPECT_EQ(equity_body(), equity_body());
}
