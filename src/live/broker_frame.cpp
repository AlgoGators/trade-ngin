#include "trade_ngin/live/broker_frame.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace trade_ngin {
namespace broker_frame {

bool is_dividend(const AppliedEvent& ev) { return ev.action_type == "DIVIDEND"; }

namespace {
// B-vii: which events actually change the SHARE COUNT.
//
// Only these two. A SPINOFF restates the parent's cost basis and explicitly leaves its
// share count alone ("a spinoff never changes the PARENT share count",
// corporate_actions_lifecycle.cpp), and a TERMINATION closes a position without restating
// anything -- its basis_ratio is NULL for that reason. Treating either as a split divides
// every EARLIER dividend's per-share cash by a factor that never touched the share count.
bool is_share_count_split(const AppliedEvent& ev) {
    return ev.action_type == "SPLIT" || ev.action_type == "ADR_SPLIT";
}
}  // namespace

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

    // BA-23: per-share cash may NOT be summed across a split.
    //
    // `quantity` is the share count TODAY. A dividend paid before a 4:1 split was paid on a
    // quarter of today's shares, so charging it at today's count over-states the cash by the
    // split factor -- and the split factor is exactly the size of error this whole comparison
    // exists to detect. The cash for event i is `q_i * d_i` with `q_i` the count held then;
    // in today's units `q_i = q / S_i`, where `S_i` is the product of the split factors
    // applied AFTER event i. So the per-today's-share cash is `SUM d_i / S_i`.
    //
    // Walk the chain from the END so `S_i` is just the running product of splits already
    // passed. That needs a defined order, so sort a local copy by ex-date, and on a tie put
    // SPLITS FIRST: the per-bar feed emits a bar's split row before its dividend row and the
    // applier applies them in that order, so a dividend sharing an ex-date with a split was
    // paid at the POST-split count and its own bar's split must not be in `S_i`.
    std::vector<const AppliedEvent*> chain;
    chain.reserve(applied.size());
    for (const auto& ev : applied) chain.push_back(&ev);
    std::stable_sort(chain.begin(), chain.end(),
                     [](const AppliedEvent* a, const AppliedEvent* b) {
                         if (a->ex_date != b->ex_date) return a->ex_date < b->ex_date;
                         return is_share_count_split(*a) && is_dividend(*b);
                     });

    double product = 1.0;
    double cash_per_share = 0.0;
    double splits_after = 1.0;
    bool any_dividend = false;

    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const AppliedEvent& ev = **it;
        const bool dividend_row = is_dividend(ev);
        const bool split_row = is_share_count_split(ev);
        // B-vii: classify BEFORE validating the ratio. A TERMINATION carries a NULL
        // basis_ratio because it restates nothing, and validating it here returned 0.0 for
        // the whole chain -- so the reconciliation went silent for any symbol whose history
        // contains a close-out. A row that is neither a dividend nor a share-count split
        // contributes to neither term and its ratio is none of this function's business.
        if (!dividend_row && !split_row) continue;
        if (!ev.ratio_known || !(ev.ratio > 0.0) || !std::isfinite(ev.ratio)) return 0.0;
        if (split_row) {
            // A split, and every dividend BEFORE it was paid at a share count this factor
            // has since multiplied.
            splits_after *= ev.ratio;
            if (!(splits_after > 0.0) || !std::isfinite(splits_after)) return 0.0;
            continue;
        }
        if (!std::isfinite(ev.dividend_per_share)) return 0.0;
        product *= ev.ratio;
        cash_per_share += ev.dividend_per_share / splits_after;
        any_dividend = true;
    }

    if (!any_dividend) return 0.0;
    if (!(product > 0.0) || !std::isfinite(product)) return 0.0;

    // U_book - (U_broker + D)
    //   = q(M - B_book) - q(M - B_broker) - D
    //   = q(B_broker - B_book) - D
    //   = q*B_broker*(1 - 1/PRODUCT r) - q*SUM (d_i / S_i)
    //
    // The mark M cancels, which is why this needs no price: the gap is a property of the
    // two BASES and the cash, not of where the stock is trading today. The first term needs
    // no split correction -- both bases are per share in TODAY's units, so the split cancels
    // inside it as it does in raw_basis.
    const double gap =
        quantity * raw_basis_value * (1.0 - 1.0 / product) - quantity * cash_per_share;
    return std::isfinite(gap) ? gap : 0.0;
}

}  // namespace broker_frame
}  // namespace trade_ngin
