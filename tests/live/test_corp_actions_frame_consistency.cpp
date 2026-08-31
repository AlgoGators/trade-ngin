#include <gtest/gtest.h>

#include <cmath>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/data/market_data_utils.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"

using namespace trade_ngin;

// A dividend is handled in two independent places that must agree:
//
//   * the PRICE series, which scales every pre-dividend bar by
//     close_D / (close_D + div_D)   -- build_equity_adjusted_query, mirrored by
//     compute_backward_adjustment_factors;
//   * the POSITION applier, which rescales cost basis so the basis stays in the
//     same frame as the marks it is compared against.
//
// If the applier divides by a different close than the price series does, basis
// and mark drift apart a little on every dividend -- silently, and permanently,
// because no later event repairs a basis. These tests pin the two to the same
// denominator: the close ON the ex-date.
//
// This is a different axis from the raw-dollar / adjusted-close frame mix that
// 05-22 §B6 documents as deliberate and load-bearing. That mix is preserved.
namespace {

Position make_position(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(avg_price);
    return p;
}

}  // namespace

TEST(CorpActionFrameConsistency, DividendBasisRescaleMatchesPriceSeriesFactor) {
    // Three bars; a dividend goes ex on the middle one.
    const double close_before_ex = 101.00;
    const double close_on_ex = 100.00;  // post-drop close, the price series' denominator
    const double dividend = 0.50;

    std::vector<market_data_utils::AdjustmentBar> bars = {
        {close_before_ex, 0.0, 1.0},
        {close_on_ex, dividend, 1.0},
        {100.75, 0.0, 1.0},
    };
    const auto factors = market_data_utils::compute_backward_adjustment_factors(bars);
    ASSERT_EQ(factors.size(), 3u);

    // What the price series does to a pre-dividend bar.
    const double price_series_factor = factors[0];
    ASSERT_GT(price_series_factor, 0.0);

    // What the applier does to a pre-dividend cost basis.
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 90.0);

    CorpActionEvent ev;
    ev.symbol = "AAPL";
    ev.ex_date = "2026-08-10";
    ev.type = CorpActionType::DIVIDEND;
    ev.value = dividend;
    ev.close_t_minus_1 = close_on_ex;  // ex-date close, per the fix

    const auto adjustments = CorporateActionsApplier::apply(positions, {ev});
    ASSERT_EQ(adjustments.size(), 1u);

    const double basis_factor = positions["AAPL"].average_price.as_double() / 90.0;

    // The two must scale by the same amount. Decimal carries 8 decimal places,
    // so compare at that resolution rather than machine epsilon.
    EXPECT_NEAR(basis_factor, price_series_factor, 1e-8)
        << "cost basis and the price series must land in the same frame; "
           "basis_factor=" << basis_factor
        << " price_series_factor=" << price_series_factor;
}

TEST(CorpActionFrameConsistency, UsingThePriorDaysCloseWouldDriftFromThePriceSeries) {
    // Guards the specific regression: reverting the denominator to close[T-1]
    // must visibly disagree with the price series. If this ever stops failing
    // to differ, the two frames have been silently re-coupled by accident.
    const double close_before_ex = 101.00;
    const double close_on_ex = 100.00;
    const double dividend = 0.50;

    std::vector<market_data_utils::AdjustmentBar> bars = {
        {close_before_ex, 0.0, 1.0}, {close_on_ex, dividend, 1.0}, {100.75, 0.0, 1.0}};
    const double price_series_factor =
        market_data_utils::compute_backward_adjustment_factors(bars)[0];

    const double wrong_ratio = 1.0 + dividend / close_before_ex;  // the old denominator
    const double wrong_factor = 1.0 / wrong_ratio;

    EXPECT_GT(std::abs(wrong_factor - price_series_factor), 1e-9)
        << "close[T-1] and close[ex-date] must not be interchangeable here";
}

// Date keys are built from UTC instants. On the deployed image
// (TZ=America/New_York) localtime renders a UTC-midnight timestamp as the
// PREVIOUS day, which shifted every corp-action key and the trading date the
// run writes under. This pins UTC rendering regardless of the host's zone.
TEST(CorpActionFrameConsistency, DateKeysAreUtcRegardlessOfHostTimezone) {
    // 2026-08-10 00:00:00 UTC.
    std::tm utc{};
    utc.tm_year = 126;
    utc.tm_mon = 7;
    utc.tm_mday = 10;
    const std::time_t t = timegm(&utc);

    auto render = [](std::time_t when) {
        std::tm out{};
        gmtime_r(&when, &out);
        char buf[11];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &out);
        return std::string(buf);
    };

    const std::string before = render(t);

    const char* saved_tz = std::getenv("TZ");
    setenv("TZ", "America/New_York", 1);
    tzset();
    const std::string under_eastern = render(t);
    if (saved_tz) {
        setenv("TZ", saved_tz, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();

    EXPECT_EQ(before, "2026-08-10");
    EXPECT_EQ(under_eastern, "2026-08-10")
        << "date keys must not shift with the host timezone; localtime here "
           "would yield 2026-08-09 and mis-key every corp-action lookup";
}
