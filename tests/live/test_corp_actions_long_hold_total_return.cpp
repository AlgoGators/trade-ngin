#include <gtest/gtest.h>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"

using namespace trade_ngin;

// Phase 4 audit test T4.3 — long-hold total-return regression guard for §1.15.
//
// Scenario: held 100 shares of a 4%-annual-yield equity for 1 year with no
// net price change. Provider uses backward (retroactive) closeadj
// adjustment, so each dividend rescales the closeadj curve. Pre-fix, the
// stored avg_price never updated, accumulating ~4% understatement of return.
// Post-fix, the avg_price /= (1 + d/close_t_minus_1) rescaling per
// dividend keeps the avg_price in the same closeadj frame as bar.close,
// so MTM correctly reflects total return.
//
// We simulate 4 quarterly dividends of $1 (total $4) on a $100 stock held
// at 100 shares for the year. End-of-year closeadj = $100 (provider has
// retroactively rescaled the original purchase price down by the same
// cumulative factor). Pre-fix: avg_price still $100, MTM = (100-100)*100 = $0
// (silently dropping the $400 of return). Post-fix: avg_price /= each
// quarter's ratio, ends ≈ $96.117, MTM = (100-96.117)*100 ≈ $388 ≈ 4%.

TEST(CorpActionsLongHoldTotalReturnTest, FourQuarterlyDividendsCaptureTotalReturn) {
    std::unordered_map<std::string, Position> positions;
    Position p;
    p.symbol = "DIVCO";
    p.quantity = Quantity(100.0);
    p.average_price = Decimal(100.0);
    positions["DIVCO"] = p;

    // 4 quarterly dividends of $1.00 each, close[T-1] = $100 each quarter
    // (no net price change over the year).
    std::vector<CorpActionEvent> events;
    for (int q = 0; q < 4; ++q) {
        CorpActionEvent ev;
        ev.symbol = "DIVCO";
        ev.ex_date = "2025-" + std::to_string(3 * (q + 1)) + "-15";
        ev.type = CorpActionType::DIVIDEND;
        ev.value = 1.00;
        ev.close_t_minus_1 = 100.0;
        events.push_back(ev);
    }

    auto log = CorporateActionsApplier::apply(positions, events);
    ASSERT_EQ(log.size(), 4u);

    // Each dividend scales avg_price by 1/(1 + 1/100) = 1/1.01.
    // After 4 quarters: avg_price = 100 / 1.01^4 = 100 / 1.04060401 ≈ 96.0980
    const double expected_avg = 100.0 / std::pow(1.01, 4);
    EXPECT_NEAR(positions["DIVCO"].average_price.as_double(), expected_avg, 1e-6);

    // MTM at year-end close $100 reflects the accumulated total return.
    const double mtm = (100.0 - positions["DIVCO"].average_price.as_double()) *
                       positions["DIVCO"].quantity.as_double();
    // 4% annual yield on 100 shares × $100 = $400 nominal, but the
    // compound-rescale formula gives slightly less than nominal:
    // mtm = (100 - 100/1.01^4) * 100 ≈ 100 - 96.0980) * 100 ≈ 390.20
    EXPECT_NEAR(mtm, 100.0 * (100.0 - expected_avg), 1e-6);
    EXPECT_GT(mtm, 380.0)
        << "Total return should be ~$390 (close to nominal 4% on $10K). "
           "Pre-fix MTM would be exactly $0 -- §1.15 silent understatement.";
    EXPECT_LT(mtm, 400.0);

    // Sanity: quantity is unchanged (dividends don't change share count).
    EXPECT_DOUBLE_EQ(positions["DIVCO"].quantity.as_double(), 100.0);
}
