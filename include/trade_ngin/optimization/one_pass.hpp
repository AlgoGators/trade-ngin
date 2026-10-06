// include/trade_ngin/optimization/one_pass.hpp
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

#include "trade_ngin/risk/overlay.hpp"

namespace trade_ngin {
namespace one_pass {

/**
 * The one optimisation pass of a rebalance (LOOP_SPEC sections 4 to 6) as pure functions of plain
 * vectors, in the order the rebalance applies them: the cap on the target, the optimiser's
 * covariance, the forecast-sign close, the search from the held book, the buffer and the rounding,
 * the clip to the cap, and the trim. Nothing here reads a book, a database or a clock, and nothing
 * is carried from one call to the next.
 *
 * Every vector is indexed by symbol in one fixed order. Positions are in contracts; `u[i]` is the
 * weight of ONE contract of symbol i on the sizing capital (multiplier x raw signal close /
 * capital), so a position n has the weight n u.
 */
using Vector = std::vector<double>;
using Matrix = std::vector<std::vector<double>>;
using Mask = std::vector<char>;

/// Section 4: the capped target sign(N) x min(|N|, L / u), fractional. Returns the target and the
/// rows the cap bound.
std::pair<Vector, Mask> cap_target(const Vector& target, const Vector& u, double cap);

/// Section 5.3: every row in `rows` with |n| u > L is clipped toward zero to floor(L / u) whole
/// contracts; a row outside `rows` (a held row) is never clipped. Returns the book and the rows
/// clipped.
std::pair<Vector, Mask> clip_to_cap(const Vector& book, const Vector& u, double cap,
                                    const Mask& rows);

/**
 * Section 5.1, the optimiser's covariance. `closes[d][i]` holds symbol i's last closes ending at
 * its signal close (NaN where it has none), the rows in date order on the dates `ordinals`;
 * `levels` the adjusted level on the same cells. A `stale` symbol leaves with the 0.01 diagonal;
 * the rest are intersected by date and the return over two intersected dates is the adjusted
 * change over the raw earlier close; while fewer than 20 returns remain the participant with the
 * fewest closes is dropped to the 0.01 diagonal; the sample covariance (n - 1) is annualised on
 * the intersected dates' own count.
 */
struct Covariance {
    Matrix matrix;            ///< over every symbol; a filled symbol has 0.01 on its diagonal alone
    Mask filled;              ///< the symbols on the 0.01 diagonal
    double bars_per_year{0.0};  ///< 0 when no symbol is estimated
};

Covariance optimiser_covariance(const Matrix& closes, const Matrix& levels, const Vector& ordinals,
                                const Mask& stale);

/// Section 5.2: TE(n) = sqrt((n u - x)' Sigma (n u - x)) + cost_mult x sum |n_i - h_i| c_i, c_i the
/// cost of one contract over the sizing capital.
double tracking_error(const Vector& n, const Vector& target_weights, const Matrix& covariance,
                      const Vector& u, const Vector& held, const Vector& cost, double cost_mult);

/// Section 5.2: whether the step from n_old to n_new is a candidate. `side` is the sign of the
/// target weight (0: a target of exactly zero, which steps only toward zero). A step away from
/// zero that lands strictly above the cap is refused; a step toward zero never is.
bool admissible(double n_new, double n_old, double side, double u, double cap);

/// Section 5.2: the pass cap, max(max_iterations, 2 x sum ceil(|x_i - h_i u_i| / u_i) + 1).
long pass_cap(const Vector& target_weights, const Vector& held, const Vector& u, long max_iterations);

/// Section 5.2: the search from the held book, one contract at a time: each pass tries +1 then -1
/// in every symbol in order, keeps the strictly lowest TE among the admissible steps (ties to the
/// earlier candidate) and takes it when it lowers TE by more than `threshold`.
struct Search {
    Vector book;
    double tracking_error{0.0};
    long passes{0};
    bool pass_capped{false};
};

Search search(const Vector& target_weights, const Matrix& covariance, const Vector& u,
              const Vector& held, const Vector& cost, double cost_mult, double cap,
              double threshold = 1e-6, long max_iterations = 100);

/// Rounding to the nearest whole number, a half away from zero (-2.5 to -3).
double round_half_away(double v);

/// Section 5.3: B_sigma = max(floor_ratio x tau, the largest u_i sqrt(Sigma_ii) over the eligible
/// symbols). Returns B and the index of that symbol, -1 when the floor governs.
std::pair<double, long> b_sigma(const Vector& u, const Matrix& covariance, const Mask& eligible,
                                double tau, double floor_ratio = 0.05);

/// Section 5.3: the symbols in the book, a non-zero search answer or a non-zero held position.
Mask book_eligible(const Vector& searched, const Vector& held);

/// Section 5.3: TE_h = sqrt((h - y)' Sigma (h - y)) in weights. At or below B: no trade. Otherwise
/// the book is h + a (y - h), a = (TE_h - B) / TE_h, rounded to nearest.
struct Buffered {
    Vector book;
    Vector unrounded;
    double te_held{0.0};
    double a{0.0};
    bool traded{false};
    bool returned_to_held{false};  ///< it traded and the rounding gave the held book back
};

Buffered buffer(const Vector& searched, const Vector& held, const Vector& u,
                const Matrix& covariance, double b);

/// Section 5.2: a held position on the opposite side of the forecast's sign is closed to flat
/// before the search. Returns the closed book and the rows closed.
std::pair<Vector, Mask> forecast_close(const Vector& held, const Vector& forecast_sign);

/**
 * Section 6.4, the trim. While a reading of the stored book is over its limit (the first breached
 * in the order R, R_jump, R_shock, L_g, L_n is served first), remove the one contract among the
 * candidate non-zero rows whose removal lowers that reading most (ties to the first symbol), toward
 * zero on a short as on a long; re-read; at most `max_contracts`. A removal that lowers nothing
 * stops the trim. `read` returns the overlay's readings of a book.
 */
struct Trimmed {
    Vector book;
    std::vector<std::pair<std::size_t, std::string>> removed;  ///< (symbol index, the term served)
    overlay::Readings readings;                                ///< of the returned book
    bool capped{false};  ///< still over a limit with max_contracts removed
};

Trimmed trim(const Vector& book, const Mask& candidates,
             const std::function<overlay::Readings(const Vector&)>& read,
             const overlay::Limits& limits, int max_contracts = 5);

/// The terms of `readings` over their limits, in the order R, R_jump, R_shock, L_g, L_n, each with
/// its excess.
std::vector<std::pair<std::string, double>> over_limit(const overlay::Readings& readings,
                                                       const overlay::Limits& limits);

/**
 * ONE REBALANCE (LOOP_SPEC sections 3.2 to 6.4): everything between the sleeves' unrounded targets
 * and the stored whole-contract book, in the one order the design fixes. A pure function of the
 * day's inputs; the caller supplies them and stores what comes back.
 *
 * Every per-symbol vector covers the same symbols in the same order: the book's symbols that have
 * a consumed bar (a symbol with none is not passed). The matrices are [date][symbol].
 */
struct DayInputs {
    // the book's constants (LOOP_SPEC section 12)
    double capital{0.0};            ///< E_t, the sizing capital
    double tau{0.20};               ///< the book's risk target (the first sleeve's)
    double cap{2.0};                ///< L, the per-name cap on the sizing capital
    double cost_multiplier{100.0};  ///< the search's cost multiplier
    double sign_band{2.0};          ///< the deferral band: |F| below it defers a sign close
    double b_sigma_floor{0.05};     ///< B_sigma's floor as a ratio to tau
    int trim_max{5};
    long max_iterations{100};
    overlay::Limits limits;         ///< the overlay's five limits, in reading units

    // per symbol
    Vector multiplier;      ///< M
    Vector close;           ///< the raw close at the symbol's last consumed bar
    Vector held;            ///< h, the held book in contracts (the filled ledger; live: the seeded book)
    Vector target;          ///< N*, the sum over the sleeves that signal the symbol (0 where none does)
    Vector first_forecast;  ///< the first sleeve's ruled forecast at the symbol's last consumed bar
    Vector cost;            ///< the cost of trading one contract, in currency
    Vector cost_adv;        ///< record only: the cost model's ADV behind `cost` (may be empty)
    Vector cost_vol_mult;   ///< record only: its volatility multiplier (may be empty)
    Mask signalling;        ///< some sleeve signals the symbol
    Mask first_signalling;  ///< the first sleeve signals it
    Mask hold;              ///< the engine's hold set: a non-SESSION bar, a withheld bar, a change bar (D37)
    Mask has_bar;           ///< the cycle consumed at least one bar of the symbol
    Mask ever_signalled;    ///< some sleeve signalled the symbol on an earlier rebalance

    // the overlay's window: the symbols' adjusted percentage returns ending at the signal date
    Matrix returns;
    Vector ordinals;
    Vector jump_sigma_daily;

    // the optimiser's covariance: each symbol's last 756 consumed closes and adjusted levels
    Matrix opt_closes;
    Matrix opt_levels;
    Vector opt_ordinals;
};

struct DayResult {
    // the classification
    Mask band;       ///< held by the deferral band (joins the hold set)
    Mask free;
    Mask fixed;      ///< a held row, or a hold-set symbol at zero: counted, never scaled or traded
    Mask closeout;   ///< a symbol nobody signals any more, closed to flat
    Mask participant;

    // the overlay on the capped target
    Vector u;               ///< the weight of one contract on the sizing capital
    Vector capped_target;   ///< N*c on the free rows, 0 elsewhere
    Mask cap_bound;
    overlay::GateWindow window;        ///< over the participants, in symbol order
    std::vector<std::size_t> participants;  ///< the participants' indices
    overlay::Readings readings;
    overlay::Multiplier multiplier;
    Vector scaled_target;   ///< N~ = m N*c on the free rows

    // the optimisation pass on the free rows (meaningful when `searched` is true)
    bool searched{false};
    Mask sign_closed;       ///< free rows closed to flat before the search
    Vector search_book;     ///< y on the free rows, the held quantity elsewhere
    double search_te{0.0};
    long passes{0};
    bool pass_capped{false};
    double b_sigma{0.0};
    long b_symbol{-1};      ///< the symbol that sets B_sigma, -1 when the floor governs
    double te_held{0.0};
    double a{0.0};
    bool traded{false};
    bool returned_to_held{false};
    Mask clipped;           ///< free rows clipped to the cap after the rounding
    Covariance covariance;  ///< over the free rows, in their order
    std::vector<std::size_t> free_rows;
    Mask stale;             ///< per free row

    // the stored book
    Vector pre_trim;        ///< the book before the trim
    Vector book;            ///< the stored whole-contract book
    Vector trimmed;         ///< contracts the trim removed, per symbol
    bool trim_capped{false};
    overlay::Readings stored_readings;  ///< the overlay's readings of the stored book
    std::vector<std::pair<std::string, double>> over_limit;      ///< terms still over after the trim
    double over_limit_excess_units{0.0};  ///< the largest excess in units of the largest non-zero stored u
    std::vector<std::string> by_hold_terms;  ///< terms the held rows keep over; "CAP" for a held row beyond the cap
    Mask by_hold;           ///< the held rows named

    // the fills, per symbol
    Vector sign_fill;       ///< the forecast-sign close (to flat), 0 where none
    Vector rest_fill;       ///< the move from the closed book to the stored book

    // the measures
    double target_gross{0.0};  ///< gross weight of the capped target and the held rows, before m
    double stored_gross{0.0};
    double risk_scale{0.0};    ///< stored gross / target gross (0 when the target is flat)

    // LOOP_SPEC section 4, the refusal: the overlay could not produce m (an input or a reading that
    // is not a finite number; a window with too few dates is BLIND, which is not a failure). The
    // book is then the held book on every row, with no search, no trim and no fill. `refusal` says
    // why (empty: no refusal); `refusal_on_reread` is true when section 6.4's re-read of the stored
    // book is what failed.
    std::string refusal;
    bool refusal_on_reread{false};
};

DayResult rebalance(const DayInputs& in);

}  // namespace one_pass
}  // namespace trade_ngin
