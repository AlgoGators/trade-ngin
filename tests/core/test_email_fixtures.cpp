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
#include <regex>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trade_ngin/core/config_loader.hpp"
#define private public  // format_rollover_warning is called directly below
#include "trade_ngin/core/email_sender.hpp"
#undef private
#include <arrow/api.h>
#include "trade_ngin/core/report_over_limit.hpp"
#include "trade_ngin/core/report_price_text.hpp"
#include "trade_ngin/core/report_risk_lines.hpp"
#include "trade_ngin/core/report_settled_marks.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/futures.hpp"

#include "../data/test_db_utils.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/portfolio/sizing_capital.hpp"

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

// What a runner tells the report about the day: its settled day, and the n and K of its statistics.
EmailReportContext context_of(const std::string& settled_date, int sessions, double per_year) {
    EmailReportContext context;
    context.settled_date = settled_date;
    context.statistics_sessions = sessions;
    context.sessions_per_year = per_year;
    return context;
}

// The book's start date (its metadata row) and, for the equity book, its asset class.
EmailReportContext with_book(EmailReportContext context, const std::string& live_start_date,
                             bool equities = false) {
    context.live_start_date = live_start_date;
    context.equities = equities;
    return context;
}

// The two capital keys of the day's risk_detail: sizing_capital and account_value.
// The symbols whose settled bar booked no move, by kind (T-EMAIL).
EmailReportContext with_zero_kinds(EmailReportContext context, const std::string& roll,
                                   const std::string& withheld = "") {
    if (!roll.empty()) context.settled_zero_kinds.roll.insert(roll);
    if (!withheld.empty()) context.settled_zero_kinds.withheld.insert(withheld);
    return context;
}

EmailReportContext with_capital(EmailReportContext context, double sizing_capital,
                                double account_value) {
    context.has_sizing_capital = true;
    context.sizing_capital = sizing_capital;
    context.account_value_at_sizing = account_value;
    return context;
}

// The capital state a futures fixture is fed: the book's settled daily net P&L in date order and
// the three figures the sizing rule (portfolio/sizing_capital.hpp, half_compounded_capital) gives
// for it. A test below CALLS the rule on each: a fixture is never fed a state the rule cannot
// produce (a sizing capital above the starting capital, as the first CONSERVATIVE body was).
struct FixtureCapital {
    double starting_capital;
    std::vector<double> settled_nets;
    double account_value;   // the starting capital plus the cumulative settled net P&L
    double sizing_capital;  // the starting capital less the fall from the high-water mark
    double peak;            // the high-water mark of the cumulative settled net P&L, floored at 0
};
// CONSERVATIVE: 5,759.80 up at its high, 2,254.73 given back since.
const FixtureCapital kConservativeCapital{
    500000.0, {5759.80, -2254.73}, 503505.07, 497745.27, 5759.80};
// BASE: never above its starting capital, 642.86 under it.
const FixtureCapital kBaseCapital{500000.0, {-642.86}, 499357.14, 499357.14, 0.0};

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
            {"Average Loss", 0.3011},
            {"Profit Factor", 1.0735},
            {"Best Day", 1.2341},
            {"Worst Day", -1.4127},
            {"Downside Deviation", 5.7748},
            {"Gross Profit", 27152.70},
            {"Gross Loss", 25292.40},
            {"Winning Days", 87.0},
            {"Losing Days", 84.0},
            {"Flat Days", 0.0},
            {"Total Days", 171.0},
            {"Total Transaction Costs", 1048.52},
            {"Total Roll Costs", 310.44},
            {"Daily Roll Costs", 4.90},
            {"Current Portfolio Value", 503505.07},
            {kReportGrossNotional, 431957.5625},
            {"Gross Leverage", 0.8581},
            {"Net Leverage", 0.2417},
            {"Equity-to-Margin Ratio", 31.2736},
            {"Margin Cushion", 97.1222},
            {"Margin Posted", 16100.0},
            {"Cash Available", 487405.07},
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
        // The settled day's rows as stored after the finalize, with the two prices the settle
        // booked each on: quantity x (to - from) x contract size is the row's P&L (QZT -1 x
        // 0.0234375 x 2,000 = -46.88; Q6J -1 x 0.0000135 x 12,500,000 = -168.75; QKE 1 x 8.50 x
        // 50 = 425.00; Q6L 2 x 0.0004 x 100,000 = 80.00; QMES 1 x 63.75 x 5 = 318.75). MBT's row
        // is the confirming bar of its roll: the settle books 0 on it by rule. HE printed no bar
        // on the settled day: the settle booked 0, its "from" entry (92.10, the bar before its
        // newest bar) was never used, and its last known close is 91.30.
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
            {"QZT.v.0", 103.71484375}, {"Q6J.v.0", 0.006286}, {"QKE.v.0", 668.5},
            {"Q6L.v.0", 0.1994},       {"QMBT.v.0", 79070.0}, {"QMES.v.0", 7153.5},
            {"HE.v.0", 92.1}};
        const SettledTablePrices settled_prices =
            settled_table_prices(t2_close, t1_close, {{"HE.v.0", 91.3}});
        const std::map<std::string, double> settled_metrics = {
            {"Daily Return", 0.1201},          {"Daily Unrealized PnL", 0.0},
            {"Daily Realized PnL", 608.12},    {"Daily Total PnL", 604.19},
            {"Daily Transaction Costs", 3.93}, {"Daily Roll Costs", 2.84}};
        std::unordered_map<std::string, Position> combined = sleeve;
        return sender_.generate_trading_report_body(
            book, combined, std::nullopt, futures_metrics(), executions, "2026-04-28",
            "CONSERVATIVE_PORTFOLIO", true, marks, nullptr, settled, settled_prices.latest,
            settled_prices.previous, settled_metrics, "LIVE_TREND_FOLLOWING",
            // gross 431,957.5625 and net 121,699.32 on the sizing capital 497,745.27
            over_limit_of(kConservativeCapital.sizing_capital, 0.8678286, 0.2445012),
            with_zero_kinds(with_book(with_capital(context_of("2026-04-27", 171, 311.0574),
                                                   kConservativeCapital.sizing_capital,
                                                   kConservativeCapital.account_value),
                                      "2025-10-05"),
                            "QMBT.v.0"));
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
        // The settle's own inputs. KE has no Sunday session: no price to settle at, 0 booked;
        // its "from" entry is Thursday's 679.50 (never used) and its last known close Friday's
        // 668.50. QZN: 2 x 0.03125 x 1,000 = 62.50 and -1 x 0.03125 x 1,000 = -31.25.
        const std::unordered_map<std::string, double> t1_close = {{"QZN.v.0", 110.890625}};
        const std::unordered_map<std::string, double> t2_close = {{"QZN.v.0", 110.859375},
                                                                  {"KE.v.0", 679.5}};
        const SettledTablePrices settled_prices =
            settled_table_prices(t2_close, t1_close, {{"KE.v.0", 668.5}});
        const std::map<std::string, double> settled_metrics = {
            {"Daily Return", 0.0062},         {"Daily Unrealized PnL", 0.0},
            {"Daily Realized PnL", 31.25},    {"Daily Total PnL", 31.25},
            {"Daily Transaction Costs", 0.0}, {"Daily Roll Costs", 0.0}};
        std::map<std::string, double> metrics = futures_metrics();
        metrics["Margin Posted"] = 11500.0;  // the account's: MES 3, ZN 1 net, KE 1
        metrics[kReportGrossNotional] = 251618.125;
        // a book with more than one year of sessions behind it (K = 311.0574)
        metrics["Winning Days"] = 160.0;
        metrics["Losing Days"] = 158.0;
        metrics["Flat Days"] = 2.0;
        metrics["Total Days"] = 320.0;
        metrics["Cash Available"] = 492005.07;
        metrics["Equity-to-Margin Ratio"] = 43.7831;
        metrics["Margin Cushion"] = 97.9444;
        return sender_.generate_trading_report_body(
            book, combined, std::nullopt, metrics, executions, "2026-04-27", "BASE_PORTFOLIO", true,
            marks, nullptr, settled, settled_prices.latest, settled_prices.previous,
            settled_metrics, "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST",
            over_limit_of(kBaseCapital.sizing_capital, 0.5035118, 0.5035118),
            with_book(with_capital(context_of("2026-04-26", 320, 311.0574),
                                   kBaseCapital.sizing_capital, kBaseCapital.account_value),
                      "2025-11-11"));
    }

    // The equity book: Wednesday 2026-06-24; its settled day is the previous session, Tuesday
    // 2026-06-23 (a weekday after a weekday: no day banner). The metric maps carry the keys the equity
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
            {"Daily Return", 0.22},
            {"Daily Unrealized PnL", 218.92},
            {"Daily Realized PnL", 0.0},
            {"Daily Total PnL", 217.83},
            {"Daily Transaction Costs", 1.090112}};
        // The statistics and the two return lines, from the day's stored row.
        LiveResultsRow stored;
        stored.total_cumulative_return = -0.45;
        stored.total_annualized_return = -1.86;
        stored.sharpe_ratio = -0.4213;
        stored.sortino_ratio = -0.5512;
        stored.max_drawdown = 1.1207;
        stored.volatility = 1.4335;
        stored.win_rate = 40.0;
        stored.avg_win = 0.0713;
        stored.avg_loss = 0.0621;
        stored.profit_factor = 0.7654;
        stored.best_day = 0.2199;
        stored.worst_day = -0.3105;
        stored.downside_deviation = 1.0961;
        stored.gross_profit = 1424.61;
        stored.gross_loss = 1861.27;
        stored.winning_days = 20;
        stored.losing_days = 30;
        stored.total_days = 55;
        std::map<std::string, double> metrics = statistics_report_metrics(stored);
        const std::map<std::string, double> book = {
            {"Total Unrealized PnL", 180.38},
            {"Total Realized PnL", -559.17},
            {"Total PnL", -448.21},
            {"Total Transaction Costs", 69.42},
            {"Current Portfolio Value", 99551.79},
            {kReportDividendIncome, 6.565561},
            {"Gross Leverage", 0.0845},
            {"Net Leverage", 0.0845},
            {"Equity-to-Margin Ratio", 12.0966},
            {"Margin Cushion", 100.0},
            {"Margin Posted", 8229.74},
            {"Cash Available", 91322.05},
        };
        metrics.insert(book.begin(), book.end());
        return sender_.generate_trading_report_body(
            positions, std::nullopt, metrics, executions, "2026-06-24", true, closes, nullptr,
            settled, t1_close, t2_close, settled_metrics, "LIVE_EQUITY_MEAN_REVERSION",
            "EQUITY_MR_PORTFOLIO",
            with_book(context_of("2026-06-23", 55, 252.0), "2026-04-01", true));
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
    expect_body_is_fixture("equity_2026-06-24.html", equity_body());
}


// A body with every footnote number replaced by "^": the numbers follow the order the notes first
// appear in an email, so a row is compared without its number.
static std::string plain_notes(std::string body) {
    return std::regex_replace(body, std::regex("<sup style=\"font-size: 9px; color: #000;\">[0-9]+</sup>"), "^");
}
// T-8D D1 and HD 2026-10-12: the labels, on the bodies. The settled table's two price columns are
// named in plain words, with no "T-1" or "T-2".
TEST_F(EmailFixture, TheSleeveNamesItsKindAndTheSettledTableNamesItsTwoPrices) {
    const std::string futures = conservative_body();
    EXPECT_NE(futures.find(">Trend Following</h3>\n<div style=\"font-size: 11px; color: #999; margin: 0 0 8px 16px;\">Futures</div>\n<table>"),
              std::string::npos);
    const std::string base = base_body();
    EXPECT_NE(base.find(">Trend Following Fast</h3>"), std::string::npos);
    const std::string marks = "<th>Previous price</th><th>Latest price</th><th>Realized PnL</th>";
    // The sentence under the table is the book kind's (report_settled_marks.hpp): a futures row
    // multiplies out to its Realized PnL, the equity table has no column for the day's move.
    const std::string equity = equity_body();
    for (const auto& [body, equities] :
         {std::pair{futures, false}, std::pair{base, false}, std::pair{equity, true}}) {
        for (const char* gone : {"The two marks are raw closes", "raw closes",
                                 "need not multiply out", "not recomputed from the two marks"}) {
            EXPECT_EQ(body.find(gone), std::string::npos) << gone;
        }
        if (equities) {
            // HD 2026-10-14 (section 16): average cost first, then the day's move, then the split
            EXPECT_NE(body.find("<th>Symbol</th><th>Quantity</th><th>Average Cost</th><th>Previous price</th>"
                                "<th>Latest price</th><th>Day's PnL</th><th>Unrealized PnL</th><th>Realized PnL</th>"),
                      std::string::npos);
        } else {
            EXPECT_NE(body.find(marks), std::string::npos);
        }
        for (const char* gone : {"Entry Price", "Exit Price", "Prior mark", "Mark (", "T-1 close",
                                 "T-2 close", "T-1", "T-2"}) {
            EXPECT_EQ(body.find(gone), std::string::npos) << gone;
        }
    }
    // The executions footer of a futures book: today's, and the netting lines above it stay.
    EXPECT_NE(base.find("<strong>Today's Transaction Costs:</strong> $2.18<sup style=\"font-size: 9px; color: #000;\">"),
              std::string::npos);
    EXPECT_NE(base.find(" Netting between strategies: own costs $3.27, adjustment -$1.09, transaction costs $2.18.</div>"),
              std::string::npos);
    EXPECT_EQ(base.find("Netting Adjustment:</strong>"), std::string::npos) << "the arithmetic is one footnote";
}

// The lead's 2026-10-07 list: a price was printed to two fixed decimals (a yen future read $0.01)
// and a symbol with no bar read 0.00. A price prints every decimal it has, to eight at most and
// two at least.
TEST(ReportPriceText, APricePrintsEveryDecimalItHasAndNeverFewerThanTwo) {
    EXPECT_EQ(report_price_text(0.0062995), "0.0062995");
    EXPECT_EQ(report_price_text(103.73828125), "103.73828125");
    EXPECT_EQ(report_price_text(0.1998), "0.1998");
    EXPECT_EQ(report_price_text(677.0), "677.00");
    EXPECT_EQ(report_price_text(7217.25), "7217.25");
    EXPECT_EQ(report_price_text(2806.7), "2806.70");
    EXPECT_EQ(report_price_text(79070.0, true), "79,070.00");
    EXPECT_EQ(report_price_text(1234567.125, true), "1,234,567.125");
    EXPECT_EQ(report_price_text(-1234.5, true), "-1,234.50");
    EXPECT_EQ(report_price_text(0.0), "0.00");
    EXPECT_EQ(report_price_text(0.000000004), "0.00") << "below the eighth decimal";
    EXPECT_EQ(report_price_text(std::nan("")), "n/a");
}


// HD 2026-10-12: the settled table's two prices are the pair the settle booked the row on, so a
// row with a bar multiplies out to its P&L; a row whose symbol printed no bar shows its last known
// close and the words "no bar", and ONE footnote under the table says what the words mean, only on
// a body that has such a row.
TEST_F(EmailFixture, ASettledRowMultipliesOutAndANoBarRowSaysSoWithOneFootnote) {
    const std::string futures = plain_notes(conservative_body());
    const std::string base = plain_notes(base_body());
    const std::string equity = equity_body();
    // 1 x (677.00 - 668.50) x 50 = 425.00; -1 x (103.73828125 - 103.71484375) x 2,000 = -46.88
    EXPECT_NE(futures.find("<tr><td>QKE.v.0</td><td>1.00</td><td>$668.5000</td><td>$677.0000</td>"
                           "<td class=\"positive\">$425.00</td></tr>"),
              std::string::npos);
    EXPECT_NE(futures.find("<tr><td>QZT.v.0</td><td>-1.00</td><td>$103.7148</td>"
                           "<td>$103.7383</td><td class=\"negative\">-$46.88</td></tr>"),
              std::string::npos);
    // 2 x (0.1998 - 0.1994) x 100,000 = 80.00
    EXPECT_NE(futures.find("<tr><td>Q6L.v.0</td><td>2.00</td><td>$0.1994</td><td>$0.1998</td>"
                           "<td class=\"positive\">$80.00</td></tr>"),
              std::string::npos);
    // HE: the last known close 91.30, never the unused older 92.10, and a stored P&L of 0.00
    EXPECT_NE(futures.find("<tr><td>HE.v.0</td><td>1.00</td><td>$91.3000</td><td>\xE2\x80\x93^</td>"
                           "<td class=\"positive\">$0.00</td></tr>"),
              std::string::npos);
    EXPECT_EQ(futures.find("92.10"), std::string::npos);
    // BASE, a Monday: KE has no Sunday session. Friday's 668.50, not Thursday's 679.50.
    EXPECT_NE(base.find("<tr><td>KE.v.0</td><td>1.00</td><td>$668.5000</td><td>\xE2\x80\x93^</td>"
                        "<td class=\"positive\">$0.00</td></tr>"),
              std::string::npos);
    EXPECT_EQ(base.find("679.50"), std::string::npos);

    // HD 10-13: no sentence under the table; a no-bar row is a dash with footnote 1, once.
    for (const std::string& body : {futures, base}) {
        EXPECT_NE(body.find("\xE2\x80\x93^"), std::string::npos);
        EXPECT_NE(body.find(" No price received that day. P&amp;L is logged as 0 and updates on later days.</div>"), std::string::npos);
        EXPECT_EQ(body.find("no bar"), std::string::npos);
        EXPECT_EQ(body.find("Previous price and Latest price are the two closes"), std::string::npos);
    }
    // the equity body's one settled row printed a bar: no "no bar" cell and no footnote
    EXPECT_EQ(equity.find("no bar"), std::string::npos);
    EXPECT_NE(equity.find("<td>$337.3900</td><td>$353.6500</td>"), std::string::npos);
}



// READS SOURCE (a labelled guard): the equity runner is a `main()`, so what its day row holds
// cannot be executed here. The equity heading rests on four lines of it: the day's P&L as written
// is the realized of the day's fills less the costs plus the change in the value of the holdings,
// and the next run's update of that row restates the value of the holdings and never the
// realized. A change to any of them makes the heading's words false: this fails, it does not skip.
TEST(EmailBookHeadingSourceGuard, TheEquityDayRowBooksItsRealizedAndTheNextRunRestatesItsMark) {
    fs::path dir = fs::current_path();
    std::string src;
    for (int i = 0; i < 8 && !dir.empty(); ++i, dir = dir.parent_path()) {
        if (fs::exists(dir / "apps/strategies/live_equity_mean_reversion.cpp")) {
            src = read_file(dir / "apps/strategies/live_equity_mean_reversion.cpp");
            break;
        }
    }
    ASSERT_FALSE(src.empty()) << "runner source not found from " << fs::current_path().string();
    for (const char* present :
         {"double daily_pnl_for_today = daily_realized_pnl - total_daily_commissions;",
          "double daily_unrealized_pnl = total_unrealized_pnl - previous_total_unrealized_pnl;",
          "double daily_pnl = daily_pnl_for_today + daily_unrealized_pnl;",
          "\"total_unrealized_pnl = \" + std::to_string(yesterday_finalized_unrealized) + \", \"",
          "{\"daily_realized_pnl\", daily_realized_pnl},"}) {
        EXPECT_NE(src.find(present), std::string::npos) << present;
    }
    // the next run's update of the row reads its realized and does not write it
    const auto update = src.find("UPDATE trading.live_results SET \"");
    ASSERT_NE(update, std::string::npos);
    const auto where = src.find("\"WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION'", update);
    ASSERT_NE(where, std::string::npos);
    EXPECT_EQ(src.substr(update, where - update).find("\"daily_realized_pnl = "),
              std::string::npos);
}



// The runners are `main()`s: this guard READS SOURCE, to keep the two futures twins in step. What
// it guards (the return lines read the stored row; no day T "Daily ..." key) is shown on the real
// binary by the gate's email bodies.
TEST(EmailSettledSplitTwinSourceGuard, BothFuturesRunnersReadTheReturnLinesFromTheStoredRow) {
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        fs::path dir = fs::current_path();
        std::string src;
        for (int i = 0; i < 8 && !dir.empty(); ++i, dir = dir.parent_path()) {
            if (fs::exists(dir / runner)) {
                src = read_file(dir / runner);
                break;
            }
        }
        if (src.empty())
            GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_NE(src.find("strategy_metrics[\"Total Cumulative Return\"] =\n"
                           "                            today_row.total_cumulative_return;"),
                  std::string::npos)
            << runner;
        EXPECT_NE(src.find("strategy_metrics[\"Total Annualized Return\"] =\n"
                           "                            today_row.total_annualized_return;"),
                  std::string::npos)
            << runner;
        EXPECT_NE(src.find("report_context.statistics_sessions = today_row.total_days;"),
                  std::string::npos)
            << runner;
        for (const char* gone :
             {"strategy_metrics[\"Daily Return\"]", "strategy_metrics[\"Daily Total PnL\"]",
              "strategy_metrics[\"Total Annualized Return\"] = "
              "total_return_annualized"}) {
            EXPECT_EQ(src.find(gone), std::string::npos) << runner << ": " << gone;
        }
    }
}


// EXECUTES the sizing rule. The account value, the sizing capital and the high-water mark each
// futures fixture is fed are what half_compounded_capital gives for the fixture's settled history:
// the report is never rendered from a capital state the book cannot be in.
TEST_F(EmailFixture, TheCapitalInputsOfTheFuturesFixturesSatisfyTheSizingRule) {
    for (const FixtureCapital* fixture : {&kConservativeCapital, &kBaseCapital}) {
        const HalfCompounding rule =
            half_compounded_capital(fixture->starting_capital, fixture->settled_nets);
        EXPECT_NEAR(rule.account, fixture->account_value, 1e-6);
        EXPECT_NEAR(rule.capital, fixture->sizing_capital, 1e-6);
        EXPECT_NEAR(rule.peak, fixture->peak, 1e-6);
        // the rule's own closed form, on the three fixture figures
        EXPECT_NEAR(fixture->sizing_capital,
                    fixture->starting_capital -
                        (fixture->peak - (fixture->account_value - fixture->starting_capital)),
                    1e-6);
        EXPECT_LE(fixture->sizing_capital, fixture->starting_capital);
    }
    // The state the CONSERVATIVE body was first fed, sizing capital 501,250.34 with the account at
    // 503,505.07, is one no settled history gives: with the account 3,505.07 up and at its high
    // the rule sizes on the starting capital, and on less after any fall from a high.
    EXPECT_DOUBLE_EQ(half_compounded_capital(500000.0, {3505.07}).capital, 500000.0);
    EXPECT_LT(half_compounded_capital(500000.0, {5759.80, -2254.73}).capital, 501250.34);
}

// READS SOURCE: a twin guard. Both futures runners hand the two keys under the condition and
// from the values of their set_risk_detail call.
TEST(EmailCapitalLineTwinSourceGuard, BothFuturesRunnersHandTheTwoCapitalKeysOfRiskDetail) {
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        fs::path dir = fs::current_path();
        std::string src;
        for (int i = 0; i < 8 && !dir.empty(); ++i, dir = dir.parent_path()) {
            if (fs::exists(dir / runner)) {
                src = read_file(dir / runner);
                break;
            }
        }
        if (src.empty())
            GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_NE(src.find("risk_detail_json(one_pass_day, sizing_capital_read.capital.account)"),
                  std::string::npos)
            << runner;
        EXPECT_NE(src.find("if (one_pass_day.stores_detail()) {\n"
                           "                        report_context.has_sizing_capital = true;\n"
                           "                        report_context.sizing_capital = "
                           "one_pass_day.sizing_capital;\n"
                           "                        report_context.account_value_at_sizing =\n"
                           "                            sizing_capital_read.capital.account;"),
                  std::string::npos)
            << runner;
    }
}

// The lead, 2026-10-12: a report body carries no symbol of the specification, and the words
// "account value" name one figure only, the day's current portfolio value the leverage pair divides
// by.
TEST_F(EmailFixture, NoBodyCarriesASymbolOfTheSpecificationOrTwoAccountValues) {
    for (const std::string& body : {conservative_body(), base_body(), equity_body()}) {
        for (const char* symbol : {"V_t", "E_t", "L_max", "L_net_max", "cap L"}) {
            EXPECT_EQ(body.find(symbol), std::string::npos) << symbol;
        }
        size_t count = 0;
        for (size_t at = body.find("account value"); at != std::string::npos;
             at = body.find("account value", at + 1)) {
            ++count;
        }
        EXPECT_TRUE(count == 0 || count == 2) << count << " uses of the words";
    }
    const std::string body = conservative_body();
    EXPECT_NE(body.find("<strong>Gross Leverage:</strong>"), std::string::npos);
    EXPECT_NE(body.find("<strong>Net Leverage:</strong>"), std::string::npos);
}



// The names and the columns of the statistics lines, executed: each report key reads its own
// stored column, and flat days are total less winning less losing.
TEST(EmailEquityStatistics, EachReportKeyReadsItsStoredColumn) {
    LiveResultsRow row;
    row.total_cumulative_return = 1.0;
    row.total_annualized_return = 2.0;
    row.sharpe_ratio = 3.0;
    row.sortino_ratio = 4.0;
    row.max_drawdown = 5.0;
    row.volatility = 6.0;
    row.win_rate = 7.0;
    row.avg_win = 8.0;
    row.avg_loss = 9.0;
    row.profit_factor = 10.0;
    row.best_day = 11.0;
    row.worst_day = 12.0;
    row.downside_deviation = 13.0;
    row.gross_profit = 14.0;
    row.gross_loss = 15.0;
    row.winning_days = 16;
    row.losing_days = 17;
    row.total_days = 40;
    const std::map<std::string, double> expected = {{"Total Cumulative Return", 1.0},
                                                    {"Total Annualized Return", 2.0},
                                                    {"Sharpe Ratio", 3.0},
                                                    {"Sortino Ratio", 4.0},
                                                    {"Max Drawdown", 5.0},
                                                    {"Volatility", 6.0},
                                                    {"Win Rate", 7.0},
                                                    {"Average Win", 8.0},
                                                    {"Average Loss", 9.0},
                                                    {"Profit Factor", 10.0},
                                                    {"Best Day", 11.0},
                                                    {"Worst Day", 12.0},
                                                    {"Downside Deviation", 13.0},
                                                    {"Gross Profit", 14.0},
                                                    {"Gross Loss", 15.0},
                                                    {"Winning Days", 16.0},
                                                    {"Losing Days", 17.0},
                                                    {"Flat Days", 7.0},
                                                    {"Total Days", 40.0}};
    EXPECT_EQ(statistics_report_metrics(row), expected);
    EXPECT_EQ(
        dividend_line_html(6.565561),
        "<div class=\"metric\"><strong>Dividends Received:</strong> <span>$6.57</span></div>\n");
}

// READS SOURCE: the equity runner is a `main()`. What it guards (the keys the report reads, the
// stored row as the statistics' source, the stored dividend cell) is shown on the real binary by
// the equity chain's email bodies.
TEST(EmailEquitySourceGuard, TheEquityRunnerSetsTheKeysTheReportReads) {
    fs::path dir = fs::current_path();
    std::string src;
    for (int i = 0; i < 8 && !dir.empty(); ++i, dir = dir.parent_path()) {
        if (fs::exists(dir / "apps/strategies/live_equity_mean_reversion.cpp")) {
            src = read_file(dir / "apps/strategies/live_equity_mean_reversion.cpp");
            break;
        }
    }
    if (src.empty())
        GTEST_SKIP() << "runner source not found from the test working directory";
    for (const char* present :
         {"yesterday_daily_metrics_final[\"Daily Transaction Costs\"]",
          "strategy_metrics[\"Total Transaction Costs\"] = total_commissions_cumulative;",
          "trade_ngin::statistics_report_metrics(today_row_result.value())",
          "strategy_metrics[trade_ngin::kReportDividendIncome] = *dividend_income_today;",
          "double_metrics[\"total_dividend_income\"] = *dividend_income_today;"}) {
        EXPECT_NE(src.find(present), std::string::npos) << present;
    }
    for (const char* gone :
         {"[\"Daily Commissions\"]", "[\"Total Commissions\"]", "[\"Portfolio Leverage\"]",
          "risk_eval.value().portfolio_var * 100.0", "strategy_metrics[\"Daily Return\"]"}) {
        EXPECT_EQ(src.find(gone), std::string::npos) << gone;
    }
}


// What the report says about the day before is the runner's classification of it. A futures
// runner that carried its book (no session): no settled table, the two notes, in words that name
// no weekday; when the calendar names that day a holiday, the holiday banner too. A run on a
// Sunday whose runner found a session shows the settled table like any other day.
TEST_F(EmailFixture, TheDayBeforeBannerFollowsTheWeekdayForItsTitleAndThePricesForItsForm) {
    const StrategyPositionsMap settled = {
        {"TREND_FOLLOWING", {{"QMES.v.0", row("QMES.v.0", 1.0, 7153.5, 12.5)}}}};
    const std::unordered_map<std::string, double> close = {{"QMES.v.0", 7153.5}};
    const std::map<std::string, double> settled_metrics = {{"Daily Return", 0.01}};
    const auto body_on = [&](const std::string& date, bool no_session) {
        EmailReportContext context;
        context.prior_day_no_session = no_session;
        return sender_.generate_trading_report_body(
            StrategyPositionsMap{}, std::unordered_map<std::string, Position>{}, std::nullopt,
            futures_metrics(), StrategyExecutionsMap{}, date, "CONSERVATIVE_PORTFOLIO", true, {},
            nullptr, settled, close, close, settled_metrics, "LIVE_TREND_FOLLOWING", {}, context);
    };
    // Sunday 2026-04-26, the runner found a session on the Saturday (a contract that prints one)
    const std::string session = body_on("2026-04-26", false);
    EXPECT_NE(session.find("<h2>Yesterday's Finalized Position Results</h2>"), std::string::npos);
    EXPECT_EQ(session.find("No Executions"), std::string::npos);
    EXPECT_EQ(session.find("Yesterday was"), std::string::npos) << "nothing is missing: no banner";
    // the same Sunday, the runner carried its book
    const std::string carried = body_on("2026-04-26", true);
    EXPECT_EQ(carried.find("Yesterday's Finalized Position Results"), std::string::npos);
    EXPECT_NE(carried.find(">Yesterday was Saturday</h2>"), std::string::npos);
    EXPECT_NE(carried.find("<strong>No Executions:</strong> Futures markets were closed yesterday, so "
                           "no executions were generated and portfolio holdings are unchanged."),
              std::string::npos);
    EXPECT_NE(carried.find("<strong>Note:</strong> Yesterday's PnL data is not available."),
              std::string::npos);
    EXPECT_EQ(carried.find("NO SESSION - BOOK CARRIED"), std::string::npos);
    // a weekday run: carried or not is the runner's word; a carried weekday has no title
    EXPECT_NE(body_on("2026-04-28", false).find("Yesterday's Finalized Position Results"),
              std::string::npos);
    const std::string gap = body_on("2026-04-28", true);
    EXPECT_EQ(gap.find("Yesterday's Finalized Position Results"), std::string::npos);
    EXPECT_EQ(gap.find("Yesterday was"), std::string::npos);
    EXPECT_NE(gap.find("<strong>No Executions:</strong> No session was found for yesterday (2026-04-27)"),
              std::string::npos);
}

// READS SOURCE: the runners are `main()`s. The twins hand the report the same two facts; the
// equity runner sets its sender and its asset class.
TEST(EmailBookTextSourceGuard, TheRunnersHandTheReportTheirOwnBooksFacts) {
    const auto source_of = [](const char* relative) {
        fs::path dir = fs::current_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i, dir = dir.parent_path()) {
            if (fs::exists(dir / relative))
                return read_file(dir / relative);
        }
        return std::string();
    };
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        const std::string src = source_of(runner);
        if (src.empty())
            GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_NE(src.find("report_context.live_start_date = book_live_start_date;\n"
                           "                    report_context.prior_day_no_session = "
                           "skip_strategy_processing;"),
                  std::string::npos)
            << runner;
        EXPECT_NE(src.find("book_live_start_date = anchor_cell(anchor_q);"), std::string::npos)
            << runner;
    }
    const std::string equity = source_of("apps/strategies/live_equity_mean_reversion.cpp");
    if (equity.empty())
        GTEST_SKIP() << "equity runner source not found";
    for (const char* present : {"email_config.from_email = app_config.email.from_email;",
                                "report_context.equities = true;",
                                "report_context.live_start_date = book_live_start_date;",
                                "book_live_start_date = first_cell(anchor_q);",
                                "report_context.settled_date = yesterday_date_for_email;"}) {
        EXPECT_NE(equity.find(present), std::string::npos) << present;
    }
}

// `to_emails_production` was loaded and written back and read by nothing: every report goes to
// `to_emails`, and the template told the operator to fill the other list. The key is deleted. A
// config that still carries it loads as before (an unknown key is not read); the loaded object
// has one recipient list.
TEST(EmailConfigKey, TheConfigHasOneRecipientListAndAnOldFileStillLoads) {
    const nlohmann::json old_file = {{"smtp_host", "smtp.example.org"},
                                     {"smtp_port", 587},
                                     {"username", "u"},
                                     {"password", "p"},
                                     {"from_email", "reports@example.org"},
                                     {"to_emails", {"one@example.org", "two@example.org"}},
                                     {"to_emails_production", {"never-read@example.org"}},
                                     {"use_tls", true}};
    EmailConfig config;
    ASSERT_NO_THROW(config.from_json(old_file));
    EXPECT_EQ(config.to_emails, (std::vector<std::string>{"one@example.org", "two@example.org"}));
    EXPECT_EQ(config.from_email, "reports@example.org");
    const nlohmann::json written = config.to_json();
    EXPECT_FALSE(written.contains("to_emails_production")) << written.dump();
    EXPECT_EQ(written.dump().find("never-read@example.org"), std::string::npos);
    EXPECT_TRUE(written.contains("to_emails"));
}

// READS FILES: the three templates and their README no longer name the key.
TEST(EmailConfigKey, TheTemplatesDoNotNameTheDeletedKey) {
    fs::path root = fs::current_path();
    for (int i = 0; i < 8 && !root.empty(); ++i, root = root.parent_path()) {
        if (fs::exists(root / "config_template" / "README.md"))
            break;
    }
    if (!fs::exists(root / "config_template" / "README.md")) {
        GTEST_SKIP() << "config_template not found from the test working directory";
    }
    for (const char* file :
         {"README.md", "portfolios/base/email.json", "portfolios/conservative/email.json",
          "portfolios/equity_mr/email.json"}) {
        const std::string text = read_file(root / "config_template" / file);
        ASSERT_FALSE(text.empty()) << file;
        EXPECT_EQ(text.find("to_emails_production"), std::string::npos) << file;
        EXPECT_NE(text.find("to_emails"), std::string::npos) << file;
    }
    for (const char* file : {"portfolios/base/email.json", "portfolios/conservative/email.json",
                             "portfolios/equity_mr/email.json"}) {
        EXPECT_NO_THROW((void)nlohmann::json::parse(read_file(root / "config_template" / file)))
            << file;
    }
}

// A database that answers the Symbols Reference query with the metadata rows it is given (one
// JSON text per row, as the query builds it), and nothing else.
class SymbolsDatabase : public trade_ngin::testing::MockPostgresDatabase {
public:
    explicit SymbolsDatabase(std::vector<std::string> json_rows)
        : MockPostgresDatabase("mock://symbols"), rows_(std::move(json_rows)) {}
    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override {
        if (query.find("metadata.contract_metadata") == std::string::npos) {
            return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::DATABASE_ERROR,
                                                             "not asked");
        }
        arrow::StringBuilder symbol;
        for (const auto& row : rows_) (void)symbol.Append(row);
        std::shared_ptr<arrow::Array> a;
        (void)symbol.Finish(&a);
        return Result<std::shared_ptr<arrow::Table>>(
            arrow::Table::Make(arrow::schema({arrow::field("symbol", arrow::utf8())}), {a}));
    }

private:
    std::vector<std::string> rows_;
};

std::string metadata_json(const std::string& ib, const std::string& name,
                          const std::string& months) {
    return "{\"db\":\"" + ib + "\",\"ib\":\"" + ib + "\",\"name\":\"" + name +
           "\",\"months\":\"" + months + "\"}";
}

// T-EMAIL Q10 (HD): the Symbols Reference lists each contract's months and nothing about a front
// month, and the report carries no rollover notice. The month came from the calendar, not from the
// position, and was wrong for HO, NG and RB.
TEST_F(EmailFixture, TheSymbolsReferenceHasNoFrontMonthColumnAndTheReportNoRolloverNotice) {
    auto db = std::make_shared<SymbolsDatabase>(std::vector<std::string>{
        metadata_json("HO", "Heating Oil", "All Months"), metadata_json("ZS", "Soybeans", "FHKNQUX")});
    const std::unordered_map<std::string, Position> held = {{"HO.v.0", row("HO.v.0", 1.0, 2.5)},
                                                             {"ZS.v.0", row("ZS.v.0", 1.0, 1000.0)}};
    const std::string table = sender_.format_symbols_table_for_positions(held, db, "2026-04-10");
    EXPECT_EQ(table.find("Front Month"), std::string::npos) << table;
    EXPECT_EQ(table.find("HOK6"), std::string::npos) << table;
    EXPECT_NE(table.find("<th>Contract Months</th></tr>"), std::string::npos) << table;
    EXPECT_NE(table.find("<td>Heating Oil</td>"), std::string::npos) << table;
    EXPECT_EQ(conservative_body().find("Rollover"), std::string::npos);
    EXPECT_EQ(base_body().find("Rollover"), std::string::npos);
}

// R4b (per_contract_rolls on): the Front Month column comes back, showing the contract held, and the
// Contract Months column reads the root's cycle; a root with no outlook keeps the metadata's months and
// an empty Front Month cell. With the switch off (no roll view, or an empty one) the table is today's.
TEST_F(EmailFixture, WithTheSwitchOnTheSymbolsReferenceShowsTheContractHeldAndTheCycle) {
    auto db = std::make_shared<SymbolsDatabase>(std::vector<std::string>{
        metadata_json("HO", "Heating Oil", "All Months"), metadata_json("ZS", "Soybeans", "FHKNQUX")});
    const std::unordered_map<std::string, Position> held = {{"HO.v.0", row("HO.v.0", 1.0, 2.5)},
                                                             {"ZS.v.0", row("ZS.v.0", 1.0, 1000.0)}};
    ReportRolls rolls;
    rolls.per_contract_rolls = true;
    ReportRollOutlook zs;
    zs.root = "ZS";
    zs.held = "ZSX6";
    zs.cycle = "FHKNX";
    rolls.outlook["ZS"] = zs;
    const std::string table = sender_.format_symbols_table_for_positions(held, db, "2026-04-10", &rolls);
    EXPECT_NE(table.find("<th>Contract Months</th><th>Front Month</th></tr>"), std::string::npos) << table;
    EXPECT_NE(table.find("<td>Soybeans</td>\n<td>Jan Mar May Jul Nov</td>\n<td>ZSX6</td>\n</tr>"), std::string::npos)
        << table;
    EXPECT_NE(table.find("<td>Heating Oil</td>\n<td>All Months</td>\n<td></td>\n</tr>"), std::string::npos) << table;
    const std::string off = sender_.format_symbols_table_for_positions(held, db, "2026-04-10");
    EXPECT_EQ(off, sender_.format_symbols_table_for_positions(held, db, "2026-04-10", nullptr));
    const ReportRolls empty;
    EXPECT_EQ(off, sender_.format_symbols_table_for_positions(held, db, "2026-04-10", &empty));
    EXPECT_EQ(off.find("Front Month"), std::string::npos);
}

// R4b (per_contract_rolls on, ruling 18): the CONSERVATIVE body with MBT's roll a per-contract roll
// carrying its split prints one plain roll line under the roll legs; ZT's contract inside its window
// gets the Rollover Warning box; KE's settled row, a per-contract roll bar, carries the roll footnote
// with its new words; the footer's data sentence becomes the roll sentence. The switch-off body is the
// stored fixture (EmailFixture.Conservative...), so none of this is printed there.
TEST_F(EmailFixture, WithTheSwitchOnTheBodyPrintsTheRollLineTheBoxTheFootnoteAndTheFooter) {
    const std::unordered_map<std::string, Position> sleeve = {
        {"QZT.v.0", row("QZT.v.0", -1.0, 103.73828125)}, {"QKE.v.0", row("QKE.v.0", 1.0, 677.0)},
        {"QMBT.v.0", row("QMBT.v.0", 1.0, 77740.0)}};
    const StrategyPositionsMap book = {{"TREND_FOLLOWING", sleeve}};
    ExecutionReport rc = roll_leg("QMBT.v.0", true, Side::SELL, 77960.0, 2.45, "42185193");
    rc.roll_bar_move = 1.0 * (77960.0 - 78500.0) * 0.1;
    rc.roll_gap_points = 79070.0 - 77960.0;
    rc.roll_gap_value = (79070.0 - 77960.0) * 1.0 * 0.1;
    // R6: QMES rolls by the rule like every root, its closing leg carrying its split; a leg with no split
    // (QM2K's here) prints no line.
    ExecutionReport mes = roll_leg("QMES.v.0", true, Side::SELL, 7217.25, 1.30, "42140878");
    mes.roll_bar_move = 1.0 * (7217.25 - 7200.00) * 5.0;
    mes.roll_gap_points = 7262.50 - 7217.25;
    mes.roll_gap_value = (7262.50 - 7217.25) * 1.0 * 5.0;
    const StrategyExecutionsMap executions = {
        {"TREND_FOLLOWING", {rc, roll_leg("QMBT.v.0", false, Side::BUY, 79070.0, 2.45, "42013708"), mes,
                             roll_leg("QMES.v.0", false, Side::BUY, 7262.50, 1.32, "42003800"),
                             roll_leg("QM2K.v.0", true, Side::SELL, 2806.7, 1.10, "42005017"),
                             roll_leg("QM2K.v.0", false, Side::BUY, 2830.1, 1.10, "42002147")}}};
    const StrategyPositionsMap settled = {
        {"TREND_FOLLOWING",
         {{"QZT.v.0", row("QZT.v.0", -1.0, 103.73828125, -46.88)}, {"QKE.v.0", row("QKE.v.0", 1.0, 677.0, 425.0)}}}};
    const std::unordered_map<std::string, double> t1 = {{"QZT.v.0", 103.73828125}, {"QKE.v.0", 677.0}};
    const std::unordered_map<std::string, double> t2 = {{"QZT.v.0", 103.71484375}, {"QKE.v.0", 668.5}};
    EmailReportContext context = with_book(context_of("2026-04-27", 171, 311.0574), "2025-10-05");
    context.settled_zero_kinds.per_contract_roll.insert("QKE.v.0");
    context.rolls.per_contract_rolls = true;
    context.rolls.names["ROLL_QMBT.v.0_RC"] = ReportRollNames{"Micro Bitcoin Futures", "April", "May", 1};
    context.rolls.names["ROLL_QMES.v.0_RC"] = ReportRollNames{"Micro E-mini S&P500 Index", "September", "December", 2};
    context.rolls.names["ROLL_QM2K.v.0_RC"] = ReportRollNames{"Micro E-Mini Russell-2000 Index", "September", "December", 1};
    ReportRollOutlook zt;
    zt.root = "QZT";
    zt.held = "QZTM6";
    zt.cycle = "HMUZ";
    zt.in_window = true;
    zt.roll_by = "2026-05-28";
    zt.deadline = "2026-05-29";
    zt.first_notice = true;
    zt.target = "QZTU6";
    context.rolls.outlook["QZT"] = zt;
    const std::string body = sender_.generate_trading_report_body(
        book, sleeve, std::nullopt, futures_metrics(), executions, "2026-04-28", "CONSERVATIVE_PORTFOLIO", true,
        {}, nullptr, settled, t1, t2, {}, "LIVE_TREND_FOLLOWING",
        over_limit_of(kConservativeCapital.sizing_capital, 0.8678286, 0.2445012), context);
    EXPECT_NE(body.find("<p style=\"margin: 4px 0; font-size: 13px;\">Micro Bitcoin Futures, April to May: market "
                        "move booked -$54; contract price difference 1110.0 ($111, not profit or loss); roll cost "
                        "$4.90.</p>"),
              std::string::npos)
        << body;
    EXPECT_LT(body.find("<th>Roll Leg</th>"), body.find("Micro Bitcoin Futures, April to May")) << "under the legs";
    EXPECT_NE(body.find("<p style=\"margin: 4px 0; font-size: 13px;\">Micro E-mini S&P500 Index, September to December: "
                        "market move booked $86; contract price difference 45.25 ($226, not profit or loss); roll "
                        "cost $2.62.</p>"),
              std::string::npos)
        << "a micro's roll gets the full line (R6)";
    EXPECT_EQ(body.find("Micro E-Mini Russell-2000 Index, September"), std::string::npos) << "no split: no line";
    EXPECT_EQ(body.find("not shown for this contract yet"), std::string::npos) << "the cost-only line is gone";
    EXPECT_NE(body.find("<li><strong>QZT:</strong> Currently holding <strong>QZTM6</strong> \xE2\x86\x92 Switch to "
                        "<strong>QZTU6</strong> by 05-28, first notice 05-29</li>"),
              std::string::npos)
        << body;
    EXPECT_NE(body.find(kReportRollBarNote), std::string::npos) << body;
    EXPECT_NE(body.find(std::string("futures assets. ") + kReportRollsFooter), std::string::npos) << body;
    EXPECT_EQ(body.find("We currently only provide data for the front-month contract"), std::string::npos);
    EXPECT_EQ(body.find("P&amp;L is not booked on the roll."), std::string::npos)
        << "no row booked 0 on a roll: the old footnote is not printed";
    // The same body with the switch off: none of it.
    EmailReportContext off = context;
    off.rolls = ReportRolls{};
    off.settled_zero_kinds.per_contract_roll.clear();
    const std::string off_body = sender_.generate_trading_report_body(
        book, sleeve, std::nullopt, futures_metrics(), executions, "2026-04-28", "CONSERVATIVE_PORTFOLIO", true,
        {}, nullptr, settled, t1, t2, {}, "LIVE_TREND_FOLLOWING",
        over_limit_of(kConservativeCapital.sizing_capital, 0.8678286, 0.2445012), off);
    for (const char* absent : {"Micro Bitcoin Futures, April", "Rollover Notice", "rolled to the next one before",
                               "The old contract's move that day is booked", "not shown for this contract yet"}) {
        EXPECT_EQ(off_body.find(absent), std::string::npos) << absent;
    }
    EXPECT_NE(off_body.find("We currently only provide data for the front-month contract"), std::string::npos);
}

// N6: the note about a contract with no metadata sits after the table, not inside it.
TEST_F(EmailFixture, TheNoteAboutMissingMetadataIsOutsideTheTable) {
    auto db = std::make_shared<SymbolsDatabase>(
        std::vector<std::string>{metadata_json("HO", "Heating Oil", "All Months")});
    const std::unordered_map<std::string, Position> held = {{"HO.v.0", row("HO.v.0", 1.0, 2.5)},
                                                             {"QQ.v.0", row("QQ.v.0", 1.0, 5.0)}};
    const std::string table = sender_.format_symbols_table_for_positions(held, db, "2026-04-10");
    const auto note = table.find("metadata not found for: QQ");
    const auto close = table.find("</table>");
    ASSERT_NE(note, std::string::npos) << table;
    ASSERT_NE(close, std::string::npos) << table;
    EXPECT_GT(note, close) << "the note is inside the table";
}

// N7: a metadata row that cannot be read is not dropped silently: its cells print empty and the
// row stays, so the table is as long as the metadata.
TEST_F(EmailFixture, AMetadataRowThatCannotBeReadStillGetsItsRow) {
    auto db = std::make_shared<SymbolsDatabase>(std::vector<std::string>{
        metadata_json("HO", "Heating Oil", "All Months"), "{not json"});
    const std::unordered_map<std::string, Position> held = {{"HO.v.0", row("HO.v.0", 1.0, 2.5)}};
    const std::string table = sender_.format_symbols_table_for_positions(held, db, "2026-04-10");
    std::size_t rows = 0;
    for (auto at = table.find("<tr>\n<td>"); at != std::string::npos;
         at = table.find("<tr>\n<td>", at + 1))
        ++rows;
    EXPECT_EQ(rows, 2u) << table;
}

// The same inputs render the same bytes twice: the body carries no clock.
TEST_F(EmailFixture, EveryBodyRendersTheSameBytesTwice) {
    EXPECT_EQ(conservative_body(), conservative_body());
    EXPECT_EQ(base_body(), base_body());
    EXPECT_EQ(equity_body(), equity_body());
}

// HD 10-13 (EMAIL_FEEDBACK_HD_2026-10-13.md): the layout of the bodies. Plain and short: the headings,
// the blocks, the lines that are gone, and the one note at the bottom.
TEST_F(EmailFixture, TheBodiesFollowHDsLayout) {
    const std::string equity_text = equity_body();
    for (const std::string& body : {conservative_body(), base_body(), equity_text}) {
        const bool is_equity = &body == &equity_text || body == equity_text;
        const auto prev = body.find("<h2>Previous trading day (");
        const auto perf = body.find("<h2>Performance Metrics</h2>");
        ASSERT_NE(prev, std::string::npos);
        ASSERT_NE(perf, std::string::npos);
        EXPECT_LT(prev, perf);
        for (const char* gone :
             {"(settled)", "Statistics through", "Book as of", "Account as of", "Winning Days",
              "Flat Days", "Distribution", "Profit Factor", "Gross Profit", "Gross Loss", "Roll Costs",
              "95% VaR", "Expected Risk", "Sizing", "Over limit", "Per-name cap", "extrapolated",
              "Historical Volatility", "(on maintenance margin)", "(on the account value)",
              "rounded from exact", "(futures)", "As of March 1st", "no bar", "(average price",
              "Market Price", "NO SESSION", "(Efficiency", "stored days"}) {
            EXPECT_EQ(body.find(gone), std::string::npos) << gone;
        }
        // one block, in groups separated by a blank line
        const char* order[] = {"Unrealized PnL", "Realized PnL", "Transaction Costs", "Total PnL (Net)",
                               "Current Portfolio Value", "<br>", "Total Cumulative Return",
                               "Annualized Return", "Sharpe Ratio", "Sortino Ratio", "Volatility",
                               "Max Drawdown", "<br>", "Cash Available", "Margin Posted",
                               "Equity-to-Margin Ratio", "Margin Cushion", "<br>", "Gross Leverage",
                               "Net Leverage"};
        std::size_t at = perf;
        for (const char* label : order) {
            if (is_equity && (std::string(label) == "Equity-to-Margin Ratio" || std::string(label) == "Margin Cushion")) continue;
            const std::string needle = std::string(label) == "<br>" ? "<br>" : std::string("<strong>") + label;
            const auto found = body.find(needle, at + 1);
            ASSERT_NE(found, std::string::npos) << label;
            at = found;
        }
    }
}

TEST_F(EmailFixture, TheOneNoteAtTheBottomNamesTheAssetsAndTheStartDate) {
    EXPECT_NE(conservative_body().find(
                  "<strong>Note:</strong> This strategy is based on daily OHLCV data for futures "
                  "assets. We currently only provide data for the front-month contract and are "
                  "working on integrating roll data.<br><br>\nAll values reflect a trading start date "
                  "of October 5th, 2025.\n"),
              std::string::npos);
    EXPECT_NE(equity_body().find("<strong>Note:</strong> This strategy is based on daily OHLCV data "
                                 "for equity assets.<br><br>\nAll values reflect a trading start date "
                                 "of April 1st, 2026.\n"),
              std::string::npos);
}

TEST_F(EmailFixture, ARowWithNoPriceIsADashWithAFootnoteAndAMoneyAmountHasItsSign) {
    for (const std::string& body : {conservative_body(), base_body()}) {
        // the dash carries a number, and the line with that number sits below the block
        const std::regex dash("\xE2\x80\x93<sup style=\"font-size: 9px; color: #000;\">([0-9])</sup>");
        std::smatch m;
        ASSERT_TRUE(std::regex_search(body, m, dash));
        EXPECT_NE(body.find(m[1].str() + " No price received that day. P&amp;L is logged as 0 and updates on later days.</div>"),
                  std::string::npos);
        EXPECT_EQ(body.find("-$-"), std::string::npos);
        EXPECT_EQ(body.find("$-"), std::string::npos);
    }
    EXPECT_EQ(conservative_body().find("<td>2</td>"), std::string::npos);
}

// Writes the banner cases of EMAIL_FEEDBACK_HD_2026-10-13.md section 15 as stand-alone bodies, for the review
// folder, when TRADE_NGIN_BANNER_CASES_DIR names a directory. Synthetic: the clone holds no such day.
TEST_F(EmailFixture, WriteTheBannerCasesWhenAskedTo) {
    const char* dir = std::getenv("TRADE_NGIN_BANNER_CASES_DIR");
    if (dir == nullptr || std::string(dir).empty()) GTEST_SKIP() << "not asked to write";
    const StrategyPositionsMap settled = {
        {"TREND_FOLLOWING", {{"QMES.v.0", row("QMES.v.0", 1.0, 7153.5, 12.5)},
                              {"QKE.v.0", row("QKE.v.0", 1.0, 668.5, 0.0)}}}};
    const std::unordered_map<std::string, double> all = {{"QMES.v.0", 7160.0}, {"QKE.v.0", 670.0}};
    const std::unordered_map<std::string, double> from = {{"QMES.v.0", 7153.5}, {"QKE.v.0", 668.5}};
    const std::unordered_map<std::string, double> some = {{"QMES.v.0", 7160.0}};  // KE has no price
    const std::map<std::string, double> settled_metrics = {{"Daily Return", 0.01}};
    const auto write = [&](const std::string& name, const std::string& run_date, bool no_session,
                           const std::unordered_map<std::string, double>& latest) {
        EmailReportContext context;
        context.prior_day_no_session = no_session;
        context.live_start_date = "2025-10-05";
        const std::string body = sender_.generate_trading_report_body(
            StrategyPositionsMap{}, std::unordered_map<std::string, Position>{}, std::nullopt,
            futures_metrics(), StrategyExecutionsMap{}, run_date, "CONSERVATIVE_PORTFOLIO", true, {},
            nullptr, settled, latest, from, settled_metrics, "LIVE_TREND_FOLLOWING", {}, context);
        std::ofstream(std::string(dir) + "/" + name + ".html", std::ios::binary) << body;
    };
    write("case_holiday_some_missing_MLK_2026-01-20", "2026-01-20", false, some);
    write("case_good_friday_some_missing_2026-04-04", "2026-04-04", false, some);
    write("case_holiday_no_prices_2025-12-26", "2025-12-26", true, all);
    write("case_saturday_with_a_print_2026-06-21", "2026-06-21", false, some);
    write("case_weekday_feed_gap_2026-04-23", "2026-04-23", true, all);
    SUCCEED();
}
