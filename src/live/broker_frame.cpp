#include "trade_ngin/live/broker_frame.hpp"

#include <cmath>

namespace trade_ngin {
namespace broker_frame {

bool is_dividend(const AppliedEvent& ev) { return ev.action_type == "DIVIDEND"; }

double dividend_basis_ratio(double dividend_per_share, double close_at_ex_date) {
    // The denominator is the RAW close ON the ex-date. A non-positive close cannot
    // produce a meaningful ratio, and 0 is the value the runner deliberately leaves in
    // place when it could not establish one -- the applier then skips the event, so a
    // ratio must never be manufactured from it here either.
    if (!(close_at_ex_date > 0.0) || !std::isfinite(close_at_ex_date)) return 0.0;
    if (!std::isfinite(dividend_per_share)) return 0.0;
    return 1.0 + dividend_per_share / close_at_ex_date;
}

double implied_close_at_ex_date(const AppliedEvent& ev) {
    // r = 1 + d/c  =>  c = d / (r - 1). Exact; no join to ohlcv_1d.
    if (!ev.ratio_known) return 0.0;
    if (!(ev.dividend_per_share > 0.0) || !std::isfinite(ev.dividend_per_share)) return 0.0;
    const double step = ev.ratio - 1.0;
    if (!(step > 0.0) || !std::isfinite(step)) return 0.0;
    return ev.dividend_per_share / step;
}

double raw_basis(double adjusted_basis, const std::vector<AppliedEvent>& applied) {
    if (!(adjusted_basis > 0.0) || !std::isfinite(adjusted_basis)) return basis_unknown();

    double product = 1.0;
    for (const auto& ev : applied) {
        // Splits cancel: the broker divides the basis by F exactly as the book does.
        // Including one here would move the answer by the whole split factor.
        if (!is_dividend(ev)) continue;

        // NULL basis_ratio means UNKNOWN, not 1.0. A pre-migration-006 row in the chain
        // makes the whole chain uninvertible, and saying so is the point: silently
        // dropping the event reports an adjusted basis as broker-equivalent.
        if (!ev.ratio_known) return basis_unknown();
        if (!(ev.ratio > 0.0) || !std::isfinite(ev.ratio)) return basis_unknown();

        product *= ev.ratio;
    }

    if (!(product > 0.0) || !std::isfinite(product)) return basis_unknown();
    const double raw = adjusted_basis * product;
    return std::isfinite(raw) ? raw : basis_unknown();
}

double expected_pnl_gap(double quantity, double raw_basis_value,
                        const std::vector<AppliedEvent>& applied) {
    if (!basis_is_known(raw_basis_value) || !std::isfinite(quantity)) return 0.0;

    double product = 1.0;
    double cash_per_share = 0.0;
    bool any_dividend = false;

    for (const auto& ev : applied) {
        if (!is_dividend(ev)) continue;
        if (!ev.ratio_known || !(ev.ratio > 0.0) || !std::isfinite(ev.ratio)) return 0.0;
        if (!std::isfinite(ev.dividend_per_share)) return 0.0;
        product *= ev.ratio;
        cash_per_share += ev.dividend_per_share;
        any_dividend = true;
    }

    if (!any_dividend) return 0.0;
    if (!(product > 0.0) || !std::isfinite(product)) return 0.0;

    // U_book - (U_broker + D)
    //   = q(M - B_book) - q(M - B_broker) - q*SUM d
    //   = q(B_broker - B_book) - q*SUM d
    //   = q*B_broker*(1 - 1/PRODUCT r) - q*SUM d
    //
    // The mark M cancels, which is why this needs no price: the gap is a property of the
    // two BASES and the cash, not of where the stock is trading today.
    const double gap =
        quantity * raw_basis_value * (1.0 - 1.0 / product) - quantity * cash_per_share;
    return std::isfinite(gap) ? gap : 0.0;
}

}  // namespace broker_frame
}  // namespace trade_ngin
