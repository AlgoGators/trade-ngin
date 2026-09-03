#pragma once

#include <string>
#include <vector>

namespace trade_ngin {

/**
 * @brief F-8 -- the arithmetic that relates the BOOK frame to the BROKER frame.
 *
 * `trading.positions.average_price` for an equity is an ADJUSTED cost basis: every
 * class-1 ex-date the book held through restates it (split by F, dividend by
 * 1 + d/c with c the RAW ex-date close). A broker restates for splits and NEVER for
 * dividends; it credits the dividend as cash on the pay date instead. So the two
 * numbers differ by construction, by an amount that is knowable exactly:
 *
 *     B_broker = B_book x PRODUCT_i r_i        over the applied DIVIDENDS only
 *
 * -- splits cancel, because both frames divide by F.
 *
 * The rule, its tolerances and what may NOT be reconciled dollar-for-dollar are in
 * docs/BROKER_BASIS_RECONCILIATION.md. This header is only its arithmetic: pure, no
 * database, no I/O, no logging, so it is directly unit-testable and so the runner and
 * a future broker adapter cannot each grow their own version of it.
 */
namespace broker_frame {

/**
 * @brief One class-1 event as the ledger records it.
 *
 * `ratio` is `trading.corp_action_applied.basis_ratio` (migration 006), which is
 * `PositionAdjustment.ratio_change`: F for a split, 1 + d/c for a dividend.
 *
 * `action_type` is the stored enum-name form ("SPLIT", "ADR_SPLIT", "DIVIDEND",
 * "TERMINATION") -- the same string `CorporateActionsApplier::type_to_string` writes.
 * It is the discriminator rather than "did this row carry cash", because a dividend
 * whose detail was never populated must still be recognised as a dividend rather than
 * silently demoted to a split, which would move the answer by the whole factor.
 */
struct AppliedEvent {
    std::string symbol;
    std::string ex_date;               ///< YYYY-MM-DD
    std::string action_type;           ///< "SPLIT" | "ADR_SPLIT" | "DIVIDEND" | ...
    double ratio{1.0};                 ///< basis_ratio: F, or 1 + d/c
    double dividend_per_share{0.0};    ///< d; 0 on a split
    bool ratio_known{true};            ///< false for a pre-migration-006 row (NULL)
};

/** @brief Does this event move the basis in the BOOK frame but not the broker's? */
bool is_dividend(const AppliedEvent& ev);

/** @brief The factor a dividend divides the basis by: 1 + d/c. 0 on unusable input. */
double dividend_basis_ratio(double dividend_per_share, double close_at_ex_date);

/**
 * @brief The raw ex-date close implied by a stored dividend row: c = d / (r - 1).
 *
 * The ledger stores the factor and the cash but not the close, and the P&L-gap formula
 * wants the close. Inverting the factor is exact and needs no join to ohlcv_1d.
 * Returns 0 when the row cannot imply one (r <= 1, or no cash).
 */
double implied_close_at_ex_date(const AppliedEvent& ev);

/**
 * @brief Invert the book's adjusted basis back to the broker's frame.
 *
 * `B_book x PRODUCT r_i` over the DIVIDEND events in `applied`. Split ratios in the
 * list are ignored on purpose -- both frames already divide by F, so including one
 * would move the answer by exactly the split factor.
 *
 * Returns `basis_unknown()` -- a sentinel, never a plausible-looking number -- when
 * the chain cannot be inverted: a dividend whose ratio was not recorded (a row written
 * before migration 006; NULL is UNKNOWN, not 1.0), a non-positive ratio (impossible
 * from 1 + d/c with c > 0), or a non-positive basis. A caller must test
 * `basis_is_known()` before printing the result.
 */
double raw_basis(double adjusted_basis, const std::vector<AppliedEvent>& applied);

/** @brief The sentinel `raw_basis` returns when the chain cannot be inverted. */
constexpr double basis_unknown() { return -1.0; }

/** @brief Is a `raw_basis` result usable? */
inline bool basis_is_known(double v) { return v > 0.0; }

/**
 * @brief The P&L difference the two frames MUST show, and which is not a break.
 *
 *     U_book - (U_broker + D) = q * B_broker * (1 - 1 / PRODUCT r_i) - q * SUM d_i
 *
 * For a single dividend this is the closed form in the rule,
 * `q * d * (B_broker - (c + d)) / (c + d)` -- the dividend times the relative distance
 * between the raw basis and the cum-dividend close -- and it is ZERO only when the
 * position was bought at the cum close.
 *
 * Report this as an informational line. Do NOT drive an alert off the P&L difference
 * itself; drive it off the difference between the measured gap and this number.
 *
 * Returns 0.0 when the chain carries no dividend, and when it cannot be evaluated
 * (an unknown ratio, an unusable basis) -- the caller has already been told that by
 * `raw_basis`.
 */
double expected_pnl_gap(double quantity, double raw_basis_value,
                        const std::vector<AppliedEvent>& applied);

}  // namespace broker_frame
}  // namespace trade_ngin
