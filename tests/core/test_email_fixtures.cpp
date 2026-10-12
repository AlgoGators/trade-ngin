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
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
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
            with_book(with_capital(context_of("2026-04-27", 171, 311.0574),
                                   kConservativeCapital.sizing_capital,
                                   kConservativeCapital.account_value),
                      "2025-10-05"));
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
            positions, std::nullopt, metrics, executions, "2026-06-22", true, closes, nullptr,
            settled, t1_close, t2_close, settled_metrics, "LIVE_EQUITY_MEAN_REVERSION",
            "EQUITY_MR_PORTFOLIO",
            with_book(context_of("2026-06-18", 55, 252.0), "2026-04-01", true));
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

// T-8D D1 and HD 2026-10-12: the labels, on the bodies. The settled table's two price columns are
// named in plain words, with no "T-1" or "T-2".
TEST_F(EmailFixture, TheSleeveNamesItsKindAndTheSettledTableNamesItsTwoPrices) {
    const std::string futures = conservative_body();
    EXPECT_NE(futures.find(">Trend Following (futures)</h3>\n<table>\n<tr><th>Symbol</th>"
                           "<th>Quantity</th><th>Market Price</th>"),
              std::string::npos);
    const std::string base = base_body();
    EXPECT_NE(base.find(">Trend Following Fast (futures)</h3>"), std::string::npos);
    const std::string marks = "<th>Previous price</th><th>Latest price</th><th>Realized PnL</th>";
    const std::string note =
        "The two marks are raw closes. Realized PnL is the settled day's "
        "stored figure; it is not recomputed from the two marks";
    for (const std::string& body : {futures, base, equity_body()}) {
        EXPECT_NE(body.find(marks), std::string::npos);
        for (const char* gone : {"Entry Price", "Exit Price", "Prior mark", "Mark (", "T-1 close",
                                 "T-2 close", "T-1", "T-2"}) {
            EXPECT_EQ(body.find(gone), std::string::npos) << gone;
        }
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

TEST_F(EmailFixture, ARowsPriceMultipliesOutAndASymbolWithNoBarHasNoMark) {
    const std::string body = conservative_body();
    // today's positions: -1 x 0.0062995 x 12,500,000 = 78,743.75; 2 x 0.1998 x 100,000 = 39,960
    EXPECT_NE(body.find("<td>Q6J.v.0</td>\n<td>-1</td>\n<td>$0.0062995</td>\n<td>$78,743.75</td>"),
              std::string::npos);
    EXPECT_NE(body.find("<td>Q6L.v.0</td>\n<td>2</td>\n<td>$0.1998</td>\n<td>$39,960.00</td>"),
              std::string::npos);
    EXPECT_NE(body.find("<td>QZT.v.0</td>\n<td>-1</td>\n<td>$103.73828125</td>\n"
                        "<td>$207,476.56</td>"),
              std::string::npos);
    EXPECT_EQ(body.find("$0.01<"), std::string::npos) << "a price cut to the cent";
    // the settled table: both prices at their decimals; HE printed no bar on the settled day
    EXPECT_NE(body.find("<tr><td>Q6J.v.0</td><td>-1.00</td><td>0.006286</td><td>0.0062995</td>"),
              std::string::npos);
    EXPECT_NE(body.find("<tr><td>HE.v.0</td><td>1.00</td><td>91.30</td><td>no bar</td>"),
              std::string::npos);
    EXPECT_EQ(body.find("<td>0.00</td>"), std::string::npos) << "a missing mark printed as zero";
    // a roll leg and a fill
    EXPECT_NE(body.find("<td>SELL</td><td>1</td><td>$77960.00</td><td>42185193</td>"),
              std::string::npos);
    EXPECT_NE(body.find("<td class=\"positive\">BUY</td>\n<td>1</td>\n<td>$2806.70</td>"),
              std::string::npos);
}

// HD 2026-10-12: the settled table's two prices are the pair the settle booked the row on, so a
// row with a bar multiplies out to its P&L; a row whose symbol printed no bar shows its last known
// close and the words "no bar", and ONE footnote under the table says what the words mean, only on
// a body that has such a row.
TEST_F(EmailFixture, ASettledRowMultipliesOutAndANoBarRowSaysSoWithOneFootnote) {
    const std::string futures = conservative_body();
    const std::string base = base_body();
    const std::string equity = equity_body();
    // 1 x (677.00 - 668.50) x 50 = 425.00; -1 x (103.73828125 - 103.71484375) x 2,000 = -46.88
    EXPECT_NE(futures.find("<tr><td>QKE.v.0</td><td>1.00</td><td>668.50</td><td>677.00</td>"
                           "<td class=\"positive\">$425.00</td></tr>"),
              std::string::npos);
    EXPECT_NE(futures.find("<tr><td>QZT.v.0</td><td>-1.00</td><td>103.71484375</td>"
                           "<td>103.73828125</td><td class=\"negative\">$-46.88</td></tr>"),
              std::string::npos);
    // 2 x (0.1998 - 0.1994) x 100,000 = 80.00
    EXPECT_NE(futures.find("<tr><td>Q6L.v.0</td><td>2.00</td><td>0.1994</td><td>0.1998</td>"
                           "<td class=\"positive\">$80.00</td></tr>"),
              std::string::npos);
    // HE: the last known close 91.30, never the unused older 92.10, and a stored P&L of 0.00
    EXPECT_NE(futures.find("<tr><td>HE.v.0</td><td>1.00</td><td>91.30</td><td>no bar</td>"
                           "<td class=\"positive\">$0.00</td></tr>"),
              std::string::npos);
    EXPECT_EQ(futures.find("92.10"), std::string::npos);
    // BASE, a Monday: KE has no Sunday session. Friday's 668.50, not Thursday's 679.50.
    EXPECT_NE(base.find("<tr><td>KE.v.0</td><td>1.00</td><td>668.50</td><td>no bar</td>"
                        "<td class=\"positive\">$0.00</td></tr>"),
              std::string::npos);
    EXPECT_EQ(base.find("679.50"), std::string::npos);

    const std::string footnote =
        ">no bar: the market did not print a price that day; the "
        "position's P&amp;L for the day is 0</div>\n";
    const std::string note = "The two marks are raw closes.";
    for (const std::string& body : {futures, base}) {
        const auto at = body.find(footnote);
        ASSERT_NE(at, std::string::npos);
        EXPECT_EQ(body.find(footnote, at + 1), std::string::npos) << "the footnote is printed once";
        // under the table (after the last sleeve's table of a book of several) and above the note
        EXPECT_GT(at, body.rfind("</table>", body.find(note)));
        EXPECT_LT(at, body.find(note));
        EXPECT_EQ(body.find("n/a (no bar)"), std::string::npos);
    }
    // the equity body's one settled row printed a bar: no "no bar" cell and no footnote
    EXPECT_EQ(equity.find("no bar"), std::string::npos);
    EXPECT_NE(equity.find("<td>337.39</td><td>353.65</td>"), std::string::npos);
}

// T-8D-2 R61: the snapshot is two blocks, because its figures are of two days. The statistics run
// through the settled day and say so; the book lines are the day T row as written and say that.
// The settled block is headed with the settled day; the nine distribution statistics are printed.
TEST_F(EmailFixture, TheSnapshotIsTheSettledStatisticsAndTheBookAsOfToday) {
    const std::string body = conservative_body();
    EXPECT_EQ(body.find("Portfolio Snapshot"), std::string::npos);
    EXPECT_EQ(body.find(" Metrics</h2>"), std::string::npos);
    const auto settled_block = body.find("<h2>2026-04-27 (settled)</h2>");
    const auto statistics = body.find("<h2>Statistics through 2026-04-27 (settled)</h2>");
    const auto distribution =
        body.find("<div class=\"metric\"><strong>Distribution (through 2026-04-27)</strong></div>");
    const auto book = body.find(
        "<h2>Book as of 2026-04-28 (today's costs booked; P&amp;L settles "
        "tomorrow)</h2>");
    ASSERT_NE(settled_block, std::string::npos);
    ASSERT_NE(statistics, std::string::npos);
    ASSERT_NE(distribution, std::string::npos);
    ASSERT_NE(book, std::string::npos);
    EXPECT_TRUE(settled_block < statistics && statistics < distribution && distribution < book);
    const auto between = [&](const std::string& line, size_t from, size_t to) {
        const auto at = body.find(line, from);
        return at != std::string::npos && at < to;
    };
    // a statistic sits in the statistics block, a level in the book block
    EXPECT_TRUE(
        between("<strong>Sharpe Ratio:</strong> <span>0.20</span>", statistics, distribution));
    EXPECT_TRUE(between("<strong>Total Days:</strong> <span>171</span>", statistics, distribution));
    EXPECT_TRUE(between("<strong>Total Cumulative Return:</strong>", book, body.size()));
    EXPECT_TRUE(between("<strong>Current Portfolio Value:</strong>", book, body.size()));
    EXPECT_TRUE(
        between("<strong>Gross Leverage (on the sizing capital):</strong>", book, body.size()));
    // the nine, with their units
    const std::string nine =
        "<div class=\"metric\"><strong>Win Rate:</strong> <span>50.88%</span></div>\n"
        "<div class=\"metric\"><strong>Average Win:</strong> <span>0.31%</span></div>\n"
        "<div class=\"metric\"><strong>Average Loss:</strong> <span>0.30%</span></div>\n"
        "<div class=\"metric\"><strong>Profit Factor:</strong> <span>1.07</span></div>\n"
        "<div class=\"metric\"><strong>Best Day:</strong> <span>1.23%</span></div>\n"
        "<div class=\"metric\"><strong>Worst Day:</strong> <span>-1.41%</span></div>\n"
        "<div class=\"metric\"><strong>Downside Deviation:</strong> <span>5.77%</span></div>\n"
        "<div class=\"metric\"><strong>Gross Profit:</strong> <span>$27,152.70</span></div>\n"
        "<div class=\"metric\"><strong>Gross Loss:</strong> <span>$25,292.40</span></div>\n";
    EXPECT_TRUE(between(nine, distribution, book)) << body.substr(distribution, 1200);
    // the equity book's settled day is its previous session, not the calendar day before
    const std::string equity = equity_body();
    EXPECT_NE(equity.find("<h2>2026-06-18 (settled)</h2>"), std::string::npos);
    EXPECT_NE(equity.find("<h2>Statistics through 2026-06-18 (settled)</h2>"), std::string::npos);
    EXPECT_NE(equity.find("<h2>Book as of 2026-06-22 (today's trades and costs booked"),
              std::string::npos);
    EXPECT_EQ(equity.find("2026-06-21 Metrics"), std::string::npos);
}

// The lead's audit of 2026-10-12 (A3): the heading of the book block said "today's costs booked;
// P&L settles tomorrow" on every book. That is the futures day row. The equity day row is written
// with the P&L its own fills realized, its costs and its holdings valued at the latest close; the
// next run settles the day's price move only. Each book has its own heading, from one function.
TEST_F(EmailFixture, TheEquityBookBlockSaysWhatItsDayRowBooksAndTheFuturesHeadingIsUnchanged) {
    const std::string futures_heading =
        "<h2>Book as of 2026-04-28 (today's costs booked; P&amp;L settles tomorrow)</h2>\n";
    const std::string equity_heading =
        "<h2>Book as of 2026-06-22 (today's trades and costs booked, holdings valued at the "
        "latest close; today's price move settles on the next run)</h2>\n";
    EXPECT_EQ(book_heading_html("2026-04-28", false), futures_heading);
    EXPECT_EQ(book_heading_html("2026-06-22", true), equity_heading);

    const std::string equity = equity_body();
    const auto at = equity.find(equity_heading);
    ASSERT_NE(at, std::string::npos);
    EXPECT_EQ(equity.find("<h2>Book as of", at + 1), std::string::npos) << "one book block";
    // the futures sentence is on no equity body: its day row is not costs only
    EXPECT_EQ(equity.find("today's costs booked"), std::string::npos);
    EXPECT_EQ(equity.find("settles tomorrow"), std::string::npos);
    // plain words: no day notation
    for (const char* gone : {"T-1", "T-2", "day T", "Day T"}) {
        EXPECT_EQ(equity_heading.find(gone), std::string::npos) << gone;
    }

    // both futures books keep their heading, byte for byte
    EXPECT_NE(conservative_body().find(futures_heading), std::string::npos);
    const std::string base = base_body();
    EXPECT_NE(base.find(book_heading_html("2026-04-27", false)), std::string::npos);
    for (const std::string& body : {conservative_body(), base}) {
        EXPECT_EQ(body.find("holdings valued at the latest close"), std::string::npos);
    }
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

// T-8D-2 R74: the annualised return is printed for every n and labelled while n is under one year
// of the grid's sessions (R40's NULL is on a held branch).
TEST_F(EmailFixture, TheAnnualisedReturnIsLabelledWhileTheBookHasUnderOneYearOfSessions) {
    EXPECT_EQ(annualized_return_label(context_of("", 171, 311.0574)),
              "Total Annualized Return (extrapolated: under one year of sessions)");
    EXPECT_EQ(annualized_return_label(context_of("", 311, 311.0574)),
              "Total Annualized Return (extrapolated: under one year of sessions)");
    EXPECT_EQ(annualized_return_label(context_of("", 312, 311.0574)), "Total Annualized Return");
    EXPECT_EQ(annualized_return_label(context_of("", 252, 252.0)), "Total Annualized Return");
    EXPECT_EQ(annualized_return_label(context_of("", 0, 252.0)), "Total Annualized Return")
        << "a caller that hands no n gets no label";
    EXPECT_EQ(annualized_return_label(EmailReportContext{}), "Total Annualized Return");
    // 171 sessions of 311.0574: labelled; 320: not
    EXPECT_NE(conservative_body().find(
                  "<div class=\"metric\"><strong>Total Annualized Return (extrapolated: under one "
                  "year of sessions):</strong> <span class=\"positive\">1.29%</span></div>"),
              std::string::npos);
    EXPECT_NE(base_body().find("<div class=\"metric\"><strong>Total Annualized Return:</strong> "
                               "<span class=\"positive\">1.29%</span></div>"),
              std::string::npos);
}

// The profit factor has no value while the series has no losing session: the stored cell is NULL
// and the row loader reads a NULL as 0. The line says so; it never prints the 0.
TEST_F(EmailFixture, AProfitFactorWithNoLosingSessionIsNotPrintedAsANumber) {
    std::map<std::string, double> metrics = futures_metrics();
    metrics["Profit Factor"] = 0.0;
    metrics["Gross Loss"] = 0.0;
    const std::string body = sender_.generate_trading_report_body(
        StrategyPositionsMap{}, std::unordered_map<std::string, Position>{}, std::nullopt, metrics,
        StrategyExecutionsMap{}, "2026-04-28", "CONSERVATIVE_PORTFOLIO");
    EXPECT_NE(body.find("<strong>Profit Factor:</strong> <span>n/a (no losing session)</span>"),
              std::string::npos);
    EXPECT_EQ(body.find("<strong>Profit Factor:</strong> <span>0.00</span>"), std::string::npos);
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

// The futures report prints the sizing capital and the portfolio value the sizing read, each from
// its own key of the day's risk_detail and each under its own plain words. No line prints their
// difference (HD 2026-10-12: "Capital above the starting capital" is dropped: the difference is
// the high-water mark less the starting capital, positive with the account under 500,000). The
// portfolio value when the day was SIZED is a different figure from the day's current portfolio
// value the leverage lines call the account value, so the line does not use those words for it.
TEST_F(EmailFixture, AFuturesReportPrintsTheSizingCapitalAndTheSizedValueAndNoDifferenceOfThem) {
    // CONSERVATIVE 2026-04-24 as stored: account_value 497,440.873363, sizing_capital
    // 495,186.141684. Their difference, 2,254.731679, is on no line.
    EmailReportContext stored = with_capital({}, 495186.141684, 497440.873363);
    const std::string lines = capital_lines_html(stored);
    EXPECT_EQ(lines,
              "<div class=\"metric\"><strong>Sizing capital:</strong> "
              "<span>$495,186.14</span></div>\n"
              "<div class=\"metric\"><strong>Portfolio value when the day was sized:</strong> "
              "<span>$497,440.87</span></div>\n");
    for (const char* gone : {"V_t", "E_t", "account value", "starting capital", "Capital above",
                             "2,254.73", "less the sizing capital"}) {
        EXPECT_EQ(lines.find(gone), std::string::npos) << gone;
    }
    // a row with no risk_detail: one line, no figure
    const std::string none = capital_lines_html(EmailReportContext{});
    EXPECT_EQ(none,
              "<div class=\"metric\"><strong>Sizing capital and portfolio value when the day "
              "was sized:</strong> no reading (no sized rebalance this run: no risk reading "
              "was stored for this day)</div>\n");
    EXPECT_EQ(none.find("$"), std::string::npos);
    EXPECT_EQ(none.find("starting capital"), std::string::npos);

    // on the bodies: in the book block, after the cash line, the two figures of the fixture
    const std::string body = conservative_body();
    EXPECT_NE(body.find("<strong>Cash Available:</strong> <span>$487,405.07</span></div>\n"
                        "<div class=\"metric\"><strong>Sizing capital:</strong> "
                        "<span>$497,745.27</span></div>\n"
                        "<div class=\"metric\"><strong>Portfolio value when the day was "
                        "sized:</strong> <span>$503,505.07</span></div>\n</div>\n"),
              std::string::npos);
    const std::string base = base_body();
    EXPECT_NE(base.find("<strong>Sizing capital:</strong> <span>$499,357.14</span></div>\n"
                        "<div class=\"metric\"><strong>Portfolio value when the day was "
                        "sized:</strong> <span>$499,357.14</span></div>\n</div>\n"),
              std::string::npos);
    // no body of either futures book carries the dropped line, nor its figure
    for (const std::string& futures : {body, base}) {
        EXPECT_EQ(futures.find("Capital above"), std::string::npos);
        EXPECT_EQ(futures.find("starting capital"), std::string::npos);
        EXPECT_EQ(futures.find("less the sizing capital"), std::string::npos);
    }
    EXPECT_EQ(body.find("2,254.73"), std::string::npos);
    // the equity book has no sizing capital: no line of either form
    const std::string equity = equity_body();
    EXPECT_EQ(equity.find("Sizing capital"), std::string::npos);
    EXPECT_EQ(equity.find("when the day was sized"), std::string::npos);
    EXPECT_EQ(equity.find("starting capital"), std::string::npos);
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
    EXPECT_NE(body.find("<strong>Gross Leverage (on the account value):</strong>"),
              std::string::npos);
    EXPECT_NE(body.find("<strong>Net Leverage (on the account value):</strong>"),
              std::string::npos);
}

// The settled day's roll costs print on a futures report, under its transaction costs (they are
// the ROLL part of them); the read stood in the overload no futures runner reaches. The equity
// report has no such line.
TEST_F(EmailFixture, TheSettledDaysRollCostsPrintOnAFuturesReport) {
    EXPECT_NE(conservative_body().find(
                  "<div class=\"metric\"><strong>Daily Transaction Costs:</strong> "
                  "<span>$3.93</span></div>\n"
                  "<div class=\"metric\"><strong>Daily Roll Costs (upper bound, inside the "
                  "transaction costs):</strong> <span>$2.84</span></div>\n"
                  "<div class=\"metric\"><strong>Daily Total PnL (Net):</strong>"),
              std::string::npos);
    EXPECT_NE(base_body().find("<strong>Daily Roll Costs (upper bound, inside the transaction "
                               "costs):</strong> <span>$0.00</span>"),
              std::string::npos);
    EXPECT_EQ(equity_body().find("Roll Costs"), std::string::npos);
}

// T-8D-2 R57, R58, R44 (c), T-8D R15: the equity report prints its settled day's and cumulative
// transaction costs (the keys were "Commissions" and never read), a statistics block from the
// day's stored row with the stored realised volatility, and one dividend line.
TEST_F(EmailFixture, TheEquityReportPrintsItsCostsItsStatisticsAndItsDividendLine) {
    const std::string body = equity_body();
    EXPECT_NE(body.find("<div class=\"metric\"><strong>Daily Transaction Costs:</strong> "
                        "<span>$1.09</span></div>"),
              std::string::npos);
    EXPECT_NE(body.find("<div class=\"metric\"><strong>Total Transaction Costs:</strong> "
                        "<span>$69.42</span></div>"),
              std::string::npos);
    EXPECT_NE(body.find("<div class=\"metric\"><strong>Total PnL (Net):</strong> "
                        "<span class=\"negative\">$-448.21</span></div>\n"
                        "<div class=\"metric\"><strong>Dividend Income (cumulative, not in "
                        "P&amp;L):</strong> <span>$6.57</span></div>\n"),
              std::string::npos);
    EXPECT_EQ(body.find("Dividend Income", body.find("Dividend Income") + 1), std::string::npos)
        << "one dividend line";
    EXPECT_EQ(body.find("Commissions"), std::string::npos);
    EXPECT_EQ(body.find("Portfolio Leverage"), std::string::npos);
    // the statistics block, in the settled statistics, under one year of the 252 sessions
    const std::string statistics =
        "<h2>Statistics through 2026-06-18 (settled)</h2>\n<div class=\"metrics-category\">\n"
        "<div class=\"metric\"><strong>Total Annualized Return (extrapolated: under one year of "
        "sessions):</strong> <span class=\"negative\">-1.86%</span></div>\n"
        "<div class=\"metric\"><strong>Sharpe Ratio:</strong> <span>-0.42</span></div>\n"
        "<div class=\"metric\"><strong>Sortino Ratio:</strong> <span>-0.55</span></div>\n"
        "<div class=\"metric\"><strong>Max Drawdown:</strong> <span>1.12%</span></div>\n"
        "<div class=\"metric\"><strong>Historical Volatility:</strong> <span>1.43%</span></div>\n"
        "<br>\n"
        "<div class=\"metric\"><strong>Winning Days:</strong> <span>20</span></div>\n"
        "<div class=\"metric\"><strong>Losing Days:</strong> <span>30</span></div>\n"
        "<div class=\"metric\"><strong>Flat Days:</strong> <span>5</span></div>\n"
        "<div class=\"metric\"><strong>Total Days:</strong> <span>55</span></div>\n";
    EXPECT_NE(body.find(statistics), std::string::npos);
    EXPECT_NE(body.find("<strong>Distribution (through 2026-06-18)</strong>"), std::string::npos);
    EXPECT_EQ(body.find("19.24%"), std::string::npos) << "the reporter's ex-ante figure";
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
        "<div class=\"metric\"><strong>Dividend Income (cumulative, not in P&amp;L):</strong> "
        "<span>$6.57</span></div>\n");
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

// T-8D-2 R59: the text of a report is its own book's. The header names the book; no banner and
// no cell is derived from the weekday or from a list of agricultural roots; the footer carries
// the book's start date and, on the equity book, no futures sentence; the equity book has no
// Symbols Reference; a share quantity prints to six decimals.
TEST_F(EmailFixture, TheTextOfAReportIsItsOwnBooks) {
    const std::string base = base_body();      // a Monday
    const std::string equity = equity_body();  // a Monday, the previous session a Thursday
    const std::string futures = conservative_body();
    for (const std::string& body : {base, equity, futures}) {
        EXPECT_EQ(body.find("Yesterday was"), std::string::npos);
        EXPECT_EQ(body.find("Agricultural"), std::string::npos);
        EXPECT_EQ(body.find("agricultural"), std::string::npos);
        EXPECT_EQ(body.find("<td>N/A</td>"), std::string::npos);
    }
    // the header
    EXPECT_NE(equity.find("<div class=\"header-info\">2026-06-22 | Equity Mr Portfolio</div>"),
              std::string::npos);
    EXPECT_EQ(equity.find("Trend Following"), std::string::npos);
    // an agricultural root with a true zero on a Monday prints its zero, its last known close
    // and the words for a day with no bar
    EXPECT_NE(base.find("<tr><td>KE.v.0</td><td>1.00</td><td>668.50</td><td>no bar</td>"
                        "<td class=\"positive\">$0.00</td></tr>"),
              std::string::npos);
    // the footers
    EXPECT_NE(futures.find("<strong>Note:</strong> This strategy is based on daily OHLCV data. We "
                           "currently only provide data for the front-month contract.<br><br>\n"
                           "All values reflect a trading start date of October 5th, 2025.<br><br>\n"
                           "As of March 1st, 2026"),
              std::string::npos);
    EXPECT_NE(base.find("All values reflect a trading start date of November 11th, 2025."),
              std::string::npos);
    EXPECT_EQ(base.find("October 5th, 2025"), std::string::npos) << "another book's start date";
    EXPECT_EQ(equity.find("October 5th, 2025"), std::string::npos);
    EXPECT_NE(equity.find("<strong>Note:</strong> This strategy is based on daily OHLCV data."
                          "<br><br>\nAll values reflect a trading start date of April 1st, 2026.\n"
                          "</div>"),
              std::string::npos);
    EXPECT_EQ(equity.find("front-month"), std::string::npos);
    EXPECT_EQ(equity.find("Heating Oil"), std::string::npos);
    EXPECT_EQ(equity.find("Symbols Reference"), std::string::npos);
    EXPECT_NE(futures.find("<h2>Symbols Reference</h2>"), std::string::npos);
    // shares to six decimals, contracts whole
    EXPECT_NE(equity.find("<td>QTMUS</td>\n<td>25.791441</td>"), std::string::npos);
    EXPECT_NE(equity.find("<td class=\"negative\">SELL</td>\n<td>2.370356</td>"),
              std::string::npos);
    EXPECT_NE(futures.find("<td>Q6L.v.0</td>\n<td>2</td>"), std::string::npos);
    EXPECT_EQ(report_long_date("2025-10-05"), "October 5th, 2025");
    EXPECT_EQ(report_long_date("2026-04-01"), "April 1st, 2026");
    EXPECT_EQ(report_long_date("2025-11-11"), "November 11th, 2025");
    EXPECT_EQ(report_long_date("2026-03-22"), "March 22nd, 2026");
    EXPECT_EQ(report_long_date("2026-03-23"), "March 23rd, 2026");
    EXPECT_EQ(report_long_date("not a date"), "not a date");
}

// What the report says about the day before is the runner's classification of it. A futures
// runner that carried its book (no session): no settled table, the two notes, in words that name
// no weekday; when the calendar names that day a holiday, the holiday banner too. A run on a
// Sunday whose runner found a session shows the settled table like any other day.
TEST_F(EmailFixture, TheDayBeforeIsWhatTheRunnerFoundNeverTheWeekday) {
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
    EXPECT_EQ(session.find("Saturday"), std::string::npos);
    // the same Sunday, the runner carried its book
    const std::string carried = body_on("2026-04-26", true);
    EXPECT_EQ(carried.find("Yesterday's Finalized Position Results"), std::string::npos);
    EXPECT_NE(carried.find("<strong>No Executions:</strong> No session was found for yesterday "
                           "(2026-04-25), so no new market data is available."),
              std::string::npos);
    EXPECT_NE(carried.find("<strong>Note:</strong> Yesterday's PnL data is not available."),
              std::string::npos);
    EXPECT_EQ(carried.find("Saturday"), std::string::npos);
    EXPECT_EQ(carried.find("Yesterday was"), std::string::npos);
    // a weekday run: carried or not is the runner's word, the weekday changes nothing
    EXPECT_NE(body_on("2026-04-28", false).find("Yesterday's Finalized Position Results"),
              std::string::npos);
    EXPECT_EQ(body_on("2026-04-28", true).find("Yesterday's Finalized Position Results"),
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

// A database that answers the rollover notice's one query (the held symbols' contract metadata)
// with the rows it is given, and nothing else.
class ContractMonthsDatabase : public trade_ngin::testing::MockPostgresDatabase {
public:
    struct Row {
        std::string databento, ib, months;
    };
    explicit ContractMonthsDatabase(std::vector<Row> rows)
        : MockPostgresDatabase("mock://contract-months"), rows_(std::move(rows)) {}
    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override {
        if (query.find("metadata.contract_metadata") == std::string::npos) {
            return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::DATABASE_ERROR,
                                                             "not asked");
        }
        arrow::StringBuilder databento, ib, months;
        for (const auto& row : rows_) {
            (void)databento.Append(row.databento);
            (void)ib.Append(row.ib);
            (void)months.Append(row.months);
        }
        std::shared_ptr<arrow::Array> a, b, c;
        (void)databento.Finish(&a);
        (void)ib.Finish(&b);
        (void)months.Finish(&c);
        return Result<std::shared_ptr<arrow::Table>>(
            arrow::Table::Make(arrow::schema({arrow::field("Databento Symbol", arrow::utf8()),
                                              arrow::field("IB Symbol", arrow::utf8()),
                                              arrow::field("Contract Months", arrow::utf8())}),
                               {a, b, c}));
    }

private:
    std::vector<Row> rows_;
};

// Since migration 019, HO and NG list "All Months" and fall in the monthly branch of the rollover
// notice, which took the contract of the run date's own month as the front. An energy contract
// expires about the 20th of the month BEFORE its contract month, so that expiry was always behind
// the run date and the notice never printed. 2026-04-10: the front is the May contract (K6),
// which expires on 2026-04-22 (three business days before Saturday 04-25), twelve days ahead.
TEST_F(EmailFixture, AnEnergyContractWithinFifteenDaysOfItsExpiryIsInTheRolloverNotice) {
    auto db = std::make_shared<ContractMonthsDatabase>(std::vector<ContractMonthsDatabase::Row>{
        {"HO", "HO", "All Months"}, {"NG", "NG", "All Months"}, {"MBT", "MBT", "All Months"}});
    const std::unordered_map<std::string, Position> held = {
        {"HO.v.0", row("HO.v.0", 1.0, 2.5)},
        {"NG.v.0", row("NG.v.0", -1.0, 3.1)},
        {"MBT.v.0", row("MBT.v.0", 1.0, 77740.0)}};
    const std::string notice = sender_.format_rollover_warning(held, "2026-04-10", db, "");
    EXPECT_NE(notice.find("<li><strong>HO:</strong> Currently holding <strong>HOK6</strong>"),
              std::string::npos)
        << notice;
    EXPECT_NE(notice.find("Switch to <strong>HOM6</strong>"), std::string::npos) << notice;
    EXPECT_NE(notice.find("<li><strong>NG:</strong> Currently holding <strong>NGK6</strong>"),
              std::string::npos)
        << notice;
    // a monthly contract outside the energy rule is as it was: the April contract, third Friday
    EXPECT_NE(notice.find("<li><strong>MBT:</strong> Currently holding <strong>MBTJ6</strong>"),
              std::string::npos)
        << notice;
    // after that expiry the front is the June contract, about the 20th of May: no notice yet
    const std::string later = sender_.format_rollover_warning(held, "2026-04-24", db, "");
    EXPECT_EQ(later.find("<strong>HO:</strong>"), std::string::npos) << later;
    EXPECT_EQ(later.find("<strong>NG:</strong>"), std::string::npos) << later;
    // December: the front is the January contract of the next year
    const std::string december = sender_.format_rollover_warning(held, "2026-12-10", db, "");
    EXPECT_NE(december.find("<strong>HO:</strong> Currently holding <strong>HOF7</strong>"),
              std::string::npos)
        << december;
    EXPECT_NE(december.find("Switch to <strong>HOG7</strong>"), std::string::npos) << december;
}

// The same inputs render the same bytes twice: the body carries no clock.
TEST_F(EmailFixture, EveryBodyRendersTheSameBytesTwice) {
    EXPECT_EQ(conservative_body(), conservative_body());
    EXPECT_EQ(base_body(), base_body());
    EXPECT_EQ(equity_body(), equity_body());
}
