// tests/live/test_execution_price_integrity.cpp
//
// The zero-price chain, and the invariants that now break it.
//
// A synthetic fill has to be priced at something. When the T-1 close was missing the
// answer used to be Position::average_price -- a COST BASIS. That is a category error
// with a self-sustaining failure mode:
//
//   missing T-1 close
//     -> day-T placeholder leaves average_price at its incoming value (0 for a new
//        position, whose basis is not established until the fill is processed)
//     -> ExecutionManager prices the fill off average_price          => fill at 0
//     -> on_execution records cost basis 0 from that fill
//     -> carried_basis is also 0, so resolve_day_t_cost_basis returns 0
//     -> the runner's `if (cost_basis > 0)` guard SKIPS the write, leaving the
//        placeholder -- the previous close -- masquerading as a cost basis
//     -> that persists and reloads next session as a carried basis of 0
//
// These tests exercise the chain through LiveDailyCycle, which is the same code the
// runner calls, rather than through the individual helpers in isolation. That
// distinction matters: Wave 2 tested resolve_day_t_cost_basis as a pure function and
// left the runner's guard around it with no coverage at all, which is exactly where
// the residual defect was hiding.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/live/execution_price_resolver.hpp"
#include "trade_ngin/live/live_daily_cycle.hpp"

using namespace trade_ngin;

namespace {

constexpr int kDay = 24;

Timestamp days_before(const Timestamp& t, int n) {
    return t - std::chrono::hours(kDay * n);
}

Position held(const std::string& symbol, double qty, double basis) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(basis);
    p.unrealized_pnl = Decimal(0.0);
    p.realized_pnl = Decimal(0.0);
    p.last_update = std::chrono::system_clock::now();
    return p;
}

Bar bar_at(const std::string& symbol, const Timestamp& ts, double close) {
    return Bar(ts, close, close, close, close, 1000.0, symbol);
}

const ExecutionReport* find_exec(const std::vector<ExecutionReport>& execs,
                                 const std::string& symbol) {
    for (const auto& e : execs) {
        if (e.symbol == symbol) return &e;
    }
    return nullptr;
}

class ExecutionPriceIntegrity : public ::testing::Test {
protected:
    Timestamp now_ = std::chrono::system_clock::now();
    ExecutionManager em_;
};

// ---------------------------------------------------------------------------
// The poisoned chain, end to end.
// ---------------------------------------------------------------------------

// PRE-FIX: fill_price = 0, because average_price (0 for a brand-new position) was
// used as the market price. This is the defect in one assertion.
TEST_F(ExecutionPriceIntegrity, NewPositionWithNoPriceIsNeverFilledAtZero) {
    std::unordered_map<std::string, Position> positions;
    positions["NEWCO"] = held("NEWCO", 100.0, 0.0);  // opened today: no basis yet
    std::unordered_map<std::string, Position> previous;

    std::unordered_map<std::string, double> t1;  // deliberately empty
    std::vector<Bar> bars;                       // no history either

    auto r = LiveDailyCycle::execute_day_t(em_, positions, previous, t1, bars, now_);
    ASSERT_TRUE(r.is_ok());

    for (const auto& e : r.value().executions) {
        EXPECT_GT(e.fill_price.as_double(), 0.0) << "a fill was booked at a non-positive price";
    }
    EXPECT_EQ(find_exec(r.value().executions, "NEWCO"), nullptr)
        << "an unpriceable symbol must not generate an execution at all";
}

// The phantom: a target we could not price must not survive into the persisted book.
TEST_F(ExecutionPriceIntegrity, UnpriceableNewTargetIsRolledOutOfTheBook) {
    std::unordered_map<std::string, Position> positions;
    positions["NEWCO"] = held("NEWCO", 100.0, 0.0);
    std::unordered_map<std::string, Position> previous;

    auto r = LiveDailyCycle::execute_day_t(em_, positions, previous, {}, {}, now_);
    ASSERT_TRUE(r.is_ok());

    EXPECT_EQ(positions.count("NEWCO"), 0u)
        << "a position that never executed was left in the day-T book";
    ASSERT_EQ(r.value().unpriced.size(), 1u);
    EXPECT_EQ(r.value().unpriced[0], "NEWCO");
}

// A held position we cannot price keeps the quantity it actually has -- not the
// target the optimizer wanted and not zero.
TEST_F(ExecutionPriceIntegrity, UnpriceableHeldPositionRevertsToCarriedQuantity) {
    std::unordered_map<std::string, Position> previous;
    previous["HALTED"] = held("HALTED", 50.0, 120.0);

    std::unordered_map<std::string, Position> positions;
    positions["HALTED"] = held("HALTED", 200.0, 0.0);  // optimizer wants to add

    auto r = LiveDailyCycle::execute_day_t(em_, positions, previous, {}, {}, now_);
    ASSERT_TRUE(r.is_ok());

    ASSERT_EQ(positions.count("HALTED"), 1u);
    EXPECT_DOUBLE_EQ(positions["HALTED"].quantity.as_double(), 50.0)
        << "book must show what we hold, not what we wanted";
    EXPECT_DOUBLE_EQ(positions["HALTED"].average_price.as_double(), 120.0)
        << "the carried basis must survive a day with no execution";
    EXPECT_EQ(find_exec(r.value().executions, "HALTED"), nullptr);
}

// A close-out we cannot price must not book a fill at the position's own basis,
// which would report zero realized PnL on the exit.
TEST_F(ExecutionPriceIntegrity, UnpriceableCloseOutDoesNotFillAtCostBasis) {
    std::unordered_map<std::string, Position> previous;
    previous["GONE"] = held("GONE", 40.0, 88.0);
    std::unordered_map<std::string, Position> positions;  // target: flat

    auto r = LiveDailyCycle::execute_day_t(em_, positions, previous, {}, {}, now_);
    ASSERT_TRUE(r.is_ok());

    const auto* exec = find_exec(r.value().executions, "GONE");
    EXPECT_EQ(exec, nullptr) << "closed at its own cost basis rather than a market price";
}

// ---------------------------------------------------------------------------
// The widened path: a real older close, not an invented one.
// ---------------------------------------------------------------------------

TEST_F(ExecutionPriceIntegrity, MissingT1IsPricedFromTheMostRecentRealClose) {
    std::unordered_map<std::string, Position> previous;
    previous["THIN"] = held("THIN", 10.0, 45.0);
    std::unordered_map<std::string, Position> positions;
    positions["THIN"] = held("THIN", 30.0, 45.0);

    // No T-1 print, but the symbol traded two sessions ago at 51.25.
    std::vector<Bar> bars{
        bar_at("THIN", days_before(now_, 4), 49.00),
        bar_at("THIN", days_before(now_, 2), 51.25),
    };

    auto r = LiveDailyCycle::execute_day_t(em_, positions, previous, {}, bars, now_);
    ASSERT_TRUE(r.is_ok());

    const auto* exec = find_exec(r.value().executions, "THIN");
    ASSERT_NE(exec, nullptr) << "a symbol with a usable older close must still trade";
    EXPECT_DOUBLE_EQ(exec->fill_price.as_double(), 51.25) << "must use the most recent close, not the oldest";
    ASSERT_EQ(r.value().widened_prices.size(), 1u);
    EXPECT_NE(r.value().widened_prices[0].find("THIN"), std::string::npos);
    EXPECT_NE(r.value().widened_prices[0].find("2 days stale"), std::string::npos)
        << "the substitution must record how old the price is: " << r.value().widened_prices[0];
}

TEST_F(ExecutionPriceIntegrity, WidenedCloseBeyondTheStalenessBoundIsRefused) {
    std::unordered_map<std::string, Position> previous;
    previous["DELISTED"] = held("DELISTED", 10.0, 45.0);
    std::unordered_map<std::string, Position> positions;
    positions["DELISTED"] = held("DELISTED", 30.0, 45.0);

    std::vector<Bar> bars{bar_at("DELISTED", days_before(now_, 40), 51.25)};

    auto r = LiveDailyCycle::execute_day_t(em_, positions, previous, {}, bars, now_);
    ASSERT_TRUE(r.is_ok());

    EXPECT_EQ(find_exec(r.value().executions, "DELISTED"), nullptr)
        << "a 40-day-old close must not be presented as today's market price";
    ASSERT_EQ(r.value().unpriced.size(), 1u);
    EXPECT_EQ(r.value().unpriced[0], "DELISTED");
    EXPECT_DOUBLE_EQ(positions["DELISTED"].quantity.as_double(), 10.0);
}

// A real T-1 close always wins. This is the control that bounds the blast radius:
// the normal path must be untouched by any of the above.
TEST_F(ExecutionPriceIntegrity, PresentT1CloseIsUsedUnchanged) {
    std::unordered_map<std::string, Position> previous;
    previous["AAPL"] = held("AAPL", 10.0, 150.0);
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = held("AAPL", 25.0, 150.0);

    std::unordered_map<std::string, double> t1{{"AAPL", 190.0}};
    // An older bar exists too and must be ignored in favour of the T-1 close.
    std::vector<Bar> bars{bar_at("AAPL", days_before(now_, 3), 177.0)};

    auto r = LiveDailyCycle::execute_day_t(em_, positions, previous, t1, bars, now_);
    ASSERT_TRUE(r.is_ok());

    const auto* exec = find_exec(r.value().executions, "AAPL");
    ASSERT_NE(exec, nullptr);
    EXPECT_DOUBLE_EQ(exec->fill_price.as_double(), 190.0);
    EXPECT_TRUE(r.value().widened_prices.empty()) << "T-1 was present; nothing should widen";
    EXPECT_TRUE(r.value().unpriced.empty());
    EXPECT_TRUE(r.value().rolled_back.empty());
    EXPECT_DOUBLE_EQ(positions["AAPL"].quantity.as_double(), 25.0) << "normal path must be intact";
}

// A bar dated after the reference session must never price today's fill.
TEST_F(ExecutionPriceIntegrity, FutureBarsAreNotUsedAsPrices) {
    std::unordered_map<std::string, Position> positions;
    positions["FWD"] = held("FWD", 10.0, 0.0);
    std::unordered_map<std::string, Position> previous;

    std::vector<Bar> bars{bar_at("FWD", now_ + std::chrono::hours(48), 99.0)};

    auto r = LiveDailyCycle::execute_day_t(em_, positions, previous, {}, bars, now_);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(find_exec(r.value().executions, "FWD"), nullptr)
        << "a fill was priced from a session that has not happened yet";
}

// A non-positive close in the feed is not a price and must not mask a good older one.
TEST_F(ExecutionPriceIntegrity, NonPositiveCloseDoesNotMaskAnOlderGoodClose) {
    std::unordered_map<std::string, Position> positions;
    positions["BADROW"] = held("BADROW", 10.0, 0.0);
    std::unordered_map<std::string, Position> previous;

    std::vector<Bar> bars{
        bar_at("BADROW", days_before(now_, 3), 62.50),
        bar_at("BADROW", days_before(now_, 1), 0.0),  // bad row, more recent
    };

    auto r = LiveDailyCycle::execute_day_t(em_, positions, previous, {}, bars, now_);
    ASSERT_TRUE(r.is_ok());

    const auto* exec = find_exec(r.value().executions, "BADROW");
    ASSERT_NE(exec, nullptr);
    EXPECT_DOUBLE_EQ(exec->fill_price.as_double(), 62.50);
}

// A T-1 entry that is present but zero is not a usable price either.
TEST_F(ExecutionPriceIntegrity, ZeroValuedT1EntryIsTreatedAsMissing) {
    std::unordered_map<std::string, Position> positions;
    positions["ZED"] = held("ZED", 10.0, 0.0);
    std::unordered_map<std::string, Position> previous;

    std::unordered_map<std::string, double> t1{{"ZED", 0.0}};
    std::vector<Bar> bars{bar_at("ZED", days_before(now_, 2), 33.0)};

    auto r = LiveDailyCycle::execute_day_t(em_, positions, previous, t1, bars, now_);
    ASSERT_TRUE(r.is_ok());

    const auto* exec = find_exec(r.value().executions, "ZED");
    ASSERT_NE(exec, nullptr) << "a zero T-1 entry should fall through to the widened close";
    EXPECT_DOUBLE_EQ(exec->fill_price.as_double(), 33.0);
}

// ---------------------------------------------------------------------------
// The basis residual -- the branch Wave 2 left uncovered.
// ---------------------------------------------------------------------------

// PRE-FIX: the runner's `if (cost_basis > 0)` guard skipped the write, so
// average_price kept the day-T placeholder (the previous close) and a MARK was
// persisted as a COST BASIS.
TEST_F(ExecutionPriceIntegrity, UnresolvableBasisIsNotSilentlyLeftAsAMark) {
    std::unordered_map<std::string, Position> positions;
    // The placeholder has already written yesterday's close into average_price.
    positions["ORPHAN"] = held("ORPHAN", 100.0, 250.0);

    std::unordered_map<std::string, Position> strategy;   // no record
    std::unordered_map<std::string, Position> previous;   // not carried
    std::unordered_map<std::string, double> marks{{"ORPHAN", 250.0}};

    auto unresolved =
        LiveDailyCycle::resolve_and_apply_basis(positions, strategy, previous, marks);

    ASSERT_EQ(unresolved.size(), 1u) << "the residual must be reported, not swallowed";
    EXPECT_EQ(unresolved[0], "ORPHAN");
    EXPECT_DOUBLE_EQ(positions["ORPHAN"].average_price.as_double(), 0.0)
        << "250.0 is today's mark, not what the position cost";
    EXPECT_DOUBLE_EQ(positions["ORPHAN"].unrealized_pnl.as_double(), 0.0);
}

// The two legitimate sources still win, and in the right order.
TEST_F(ExecutionPriceIntegrity, StrategyBasisWinsAndCarriedBasisBacksItUp) {
    std::unordered_map<std::string, Position> positions;
    positions["TRADED"] = held("TRADED", 10.0, 999.0);  // placeholder mark
    positions["HELD"] = held("HELD", 20.0, 999.0);      // placeholder mark

    std::unordered_map<std::string, Position> strategy;
    strategy["TRADED"] = held("TRADED", 10.0, 101.0);

    std::unordered_map<std::string, Position> previous;
    previous["HELD"] = held("HELD", 20.0, 55.0);

    std::unordered_map<std::string, double> marks{{"TRADED", 110.0}, {"HELD", 60.0}};

    auto unresolved =
        LiveDailyCycle::resolve_and_apply_basis(positions, strategy, previous, marks);

    EXPECT_TRUE(unresolved.empty());
    EXPECT_DOUBLE_EQ(positions["TRADED"].average_price.as_double(), 101.0);
    EXPECT_DOUBLE_EQ(positions["HELD"].average_price.as_double(), 55.0);
    // Unrealized must be measured from the basis, not from the placeholder.
    EXPECT_DOUBLE_EQ(positions["TRADED"].unrealized_pnl.as_double(), 10.0 * (110.0 - 101.0));
    EXPECT_DOUBLE_EQ(positions["HELD"].unrealized_pnl.as_double(), 20.0 * (60.0 - 55.0));
}

// A flat row must not be reported as an unresolved holding.
TEST_F(ExecutionPriceIntegrity, FlatPositionIsNotReportedUnresolved) {
    std::unordered_map<std::string, Position> positions;
    positions["FLAT"] = held("FLAT", 0.0, 0.0);

    auto unresolved = LiveDailyCycle::resolve_and_apply_basis(positions, {}, {}, {});
    EXPECT_TRUE(unresolved.empty());
}

// ---------------------------------------------------------------------------
// Marks and fills must come from the same map.
// ---------------------------------------------------------------------------

TEST_F(ExecutionPriceIntegrity, MarksMatchThePricesTheFillsUsed) {
    std::unordered_map<std::string, Position> previous;
    previous["THIN"] = held("THIN", 10.0, 40.0);
    std::unordered_map<std::string, Position> positions;
    positions["THIN"] = held("THIN", 10.0, 40.0);  // unchanged: no execution

    std::vector<Bar> bars{bar_at("THIN", days_before(now_, 2), 47.0)};

    auto r = LiveDailyCycle::execute_day_t(em_, positions, previous, {}, bars, now_);
    ASSERT_TRUE(r.is_ok());

    ASSERT_EQ(r.value().execution_prices.count("THIN"), 1u)
        << "the widened price must be exposed so marking uses the same number";
    EXPECT_DOUBLE_EQ(r.value().execution_prices.at("THIN"), 47.0);

    auto unresolved = LiveDailyCycle::resolve_and_apply_basis(
        positions, {}, previous, r.value().execution_prices);
    EXPECT_TRUE(unresolved.empty());
    EXPECT_DOUBLE_EQ(positions["THIN"].average_price.as_double(), 40.0);
    EXPECT_DOUBLE_EQ(positions["THIN"].unrealized_pnl.as_double(), 10.0 * (47.0 - 40.0));
}

// ---------------------------------------------------------------------------
// Resolver units -- the staleness arithmetic the bound depends on.
// ---------------------------------------------------------------------------

TEST_F(ExecutionPriceIntegrity, StalenessIsWholeCalendarDaysAndNeverNegative) {
    EXPECT_EQ(ExecutionPriceResolver::staleness_days(now_, now_), 0);
    EXPECT_EQ(ExecutionPriceResolver::staleness_days(now_, days_before(now_, 3)), 3);
    EXPECT_EQ(ExecutionPriceResolver::staleness_days(now_, now_ + std::chrono::hours(48)), 0)
        << "a future observation must not produce a negative age";
}

TEST_F(ExecutionPriceIntegrity, LatestCloseIgnoresOtherSymbols) {
    std::vector<Bar> bars{
        bar_at("A", days_before(now_, 1), 10.0),
        bar_at("B", days_before(now_, 1), 20.0),
    };
    auto m = ExecutionPriceResolver::latest_close_at_or_before(bars, now_);
    ASSERT_EQ(m.size(), 2u);
    EXPECT_DOUBLE_EQ(m.at("A").price, 10.0);
    EXPECT_DOUBLE_EQ(m.at("B").price, 20.0);
}

}  // namespace
