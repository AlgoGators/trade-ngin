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
    double bars_per_year{0.0};  ///< 0 when no symbol is estimated; 256 (16 x 16) when carver
    bool carver{false};       ///< built by strategy25_covariance
};

Covariance optimiser_covariance(const Matrix& closes, const Matrix& levels, const Vector& ordinals,
                                const Mask& stale);

/// The closes the search holds per symbol (the trend sleeve's last 756 consumed bars), and so
/// the default and the largest sample_cov_window.
inline constexpr std::size_t kSampleCovWindow = 756;

/// defaults.json optimization.sample_cov_window: each symbol (column) keeps only its last
/// `window` closes, its earlier closes and levels set to NaN, so optimiser_covariance estimates on
/// that span exactly as it does on the full one. A column with no more than `window` closes is
/// unchanged.
void keep_last_closes(Matrix& closes, Matrix& levels, std::size_t window);

/**
 * Section 5.1 under defaults.json optimization.optimiser_covariance "carver": AFTS strategy 25's
 * covariance (p.417; Appendix B p.641), sigma_i sigma_j rho_ij with sigma_i the symbol's standard
 * deviation by strategy three's method (0.7 x the 32-day EWMA + 0.3 x its long-run mean, p.90)
 * annualised by 16 (`sigma`, per symbol) and rho_ij the
 * exponentially weighted correlation of weekly returns, six-month (26-week) span, no shrinkage: the
 * overlay's carver estimator (overlay::carver_matrix) on `returns`, the symbols' daily percentage
 * returns on the dates `ordinals`. A symbol whose sigma is not a positive number is on the 0.01
 * diagonal alone (filled). There is no stale rule and no date intersection.
 */
Covariance strategy25_covariance(const Matrix& returns, const Vector& ordinals,
                                 const Vector& sigma);

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

/**
 * How B_sigma is set (defaults.json optimization.b_sigma_rule; absent: granularity).
 *   - kGranularity (today, D24): b_sigma over book_eligible(searched, held), the floor or the
 *     largest one-contract risk of the names wanted or held, whichever is larger.
 *   - kFixed (AFTS strategy 25, p.431): floor_ratio x tau alone, whatever the names; at the
 *     floor 0.05 it is the book's 1% at tau 20%. Any other ratio departs from the book.
 *   - kMinimal (the AFTS gap list's minimum fix for C-1): kGranularity with the held-only rows (a
 *     held position the search answers zero) left out of the names, so a held lumpy contract no
 *     longer sets the buffer that protects it. Departs from the book.
 */
enum class BSigmaRule { kGranularity, kFixed, kMinimal };

/// B_sigma under `rule` from the search answer and the held book. Returns B and the index of the
/// symbol that sets it, -1 when the floor governs (always under kFixed).
std::pair<double, long> b_sigma(const Vector& u, const Matrix& covariance, const Vector& searched,
                                const Vector& held, double tau, double floor_ratio,
                                BSigmaRule rule);

/**
 * How the buffer moves a book whose TE_h is above B (defaults.json optimization.buffer_step_rule;
 * absent: fraction). The no-trade check is the same under both.
 *   - kFraction (today, AFTS strategy 25 as written): every row moves the one shared fraction
 *     a = (TE_h - B) / TE_h of the way to the search's answer and is rounded to nearest.
 *   - kEdge (Carver's stated aim, "trade until the tracking error is equal to the buffer"): from
 *     the held book, one contract at a time toward the search's answer in the row whose move gives
 *     the lowest tracking error against the answer (ties: the lower cost per contract, then the
 *     row order), stopping as soon as the error is at or below B or when no move lowers it. A row
 *     never passes or leaves its answer. Two errors within 1e-12 of each other (relative) are
 *     equal: a tie, a move that does not lower the error, an error at B. Under kEdge a TE_h within
 *     that of B is at the buffer (no trade); kFraction's no-trade check is TE_h <= B exactly.
 */
enum class BufferStepRule { kFraction, kEdge };

/// Section 5.3: TE_h = sqrt((h - y)' Sigma (h - y)) in weights. At or below B: no trade. Otherwise
/// the book is h + a (y - h), a = (TE_h - B) / TE_h, rounded to nearest (kFraction), or the held
/// book stepped toward y one contract at a time to the buffer's edge (kEdge, where `a` is still
/// set, as the fraction the book would have used, and `unrounded` is the book itself).
struct Buffered {
    Vector book;
    Vector unrounded;
    double te_held{0.0};
    double a{0.0};
    bool traded{false};
    bool returned_to_held{false};  ///< it traded and the rounding gave the held book back
    // kEdge only (empty or zero under kFraction):
    std::vector<std::pair<std::size_t, double>> steps;  ///< (row, +1 or -1) in the order taken
    double te_book{0.0};    ///< TE of the returned book against y
    bool inside{false};     ///< it stopped at or below B (false: no move lowered the error)
};

/// `cost` (per row, one contract's cost; empty reads as zero) breaks kEdge's ties only.
Buffered buffer(const Vector& searched, const Vector& held, const Vector& u,
                const Matrix& covariance, double b,
                BufferStepRule rule = BufferStepRule::kFraction, const Vector& cost = {});

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
    double b_sigma_floor{0.05};     ///< B_sigma's floor as a ratio to tau (kFixed: its width)
    BSigmaRule b_sigma_rule{BSigmaRule::kGranularity};
    /// defaults.json optimization.wrong_sign_rule "buffer" (AFTS strategy 25, pp.430-432): a held
    /// position on the wrong side of its forecast is neither closed before the search nor held by
    /// the deferral band; it is a free row like any other and the buffer moves it a of the way to
    /// the search's answer. false ("close", today): the forecast-sign close and the band.
    bool wrong_sign_buffer{false};
    /// defaults.json optimization.buffer_step_rule: how the buffer reaches its edge (Buffered).
    BufferStepRule buffer_step_rule{BufferStepRule::kFraction};
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
    /// risk.json's carver module overlay_covariance "carver": the overlay's covariance and shock
    /// sigmas are built from `sizing_sigma` (per symbol, the annualised standard deviation the
    /// sleeve sizes on) and weekly correlations (overlay::carver_covariance); false is "sample".
    bool overlay_carver{false};
    Vector sizing_sigma;

    // the optimiser's covariance: each symbol's last 756 consumed closes and adjusted levels
    Matrix opt_closes;
    Matrix opt_levels;
    Vector opt_ordinals;
    /// defaults.json optimization.sample_cov_window: the sample covariance reads each free row's
    /// last this many of the closes above (keep_last_closes), after the stale rule. Ignored when
    /// opt_carver.
    std::size_t sample_cov_window{kSampleCovWindow};
    /// defaults.json optimization.optimiser_covariance "carver" (AFTS strategy 25): the optimiser's
    /// covariance is strategy25_covariance on `returns` and `ordinals` (the overlay's window) with
    /// `opt_sigma` (per symbol, strategy three's blended sd annualised by 16); false ("sample"):
    /// optimiser_covariance on the closes above.
    bool opt_carver{false};
    Vector opt_sigma;
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
    Vector sigma_jump;                 ///< per participant: its jump sigma on the window's factor
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
    /// buffer_step_rule edge: the buffer's book per symbol (the held quantity off the free rows),
    /// its one-contract steps as (symbol, +1 or -1) in the order taken, the TE of its book against
    /// the search's answer and whether it stopped at or below B. Empty or zero under fraction.
    bool edge{false};
    Vector edge_book;
    std::vector<std::pair<std::size_t, double>> edge_steps;
    double edge_te{0.0};
    bool edge_inside{false};
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
    /// For the record only (nothing reads them to decide): the capped target with the held rows,
    /// and the stored book, read on the carver covariance (overlay::carver_covariance) whatever the
    /// book's overlay_covariance, so books on either rule compare. Equal to `readings` and
    /// `stored_readings` on a carver book.
    overlay::Readings carver_readings;
    overlay::Readings carver_stored_readings;
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

/**
 * QT plan E5 (rulings 10 and 12, contract section 5): the step of the pass that moved each symbol
 * from what was asked (`in.target`, the desk's quantity on a desk run) to what was stored
 * (`out.book`). One of: none, hold, cap, overlay, sign_close, search, buffer, rounding, clip,
 * trim. Pure; nothing is re-run.
 *
 *   - none: the stored quantity is the asked one (within 1e-9).
 *   - hold: the row was not free (the engine's hold set, the deferral band, a fixed or
 *     unweighed row) or the day was refused, so the held quantity was stored.
 *   - sign_close: the held position was closed to flat before the search and the row stored
 *     flat against a non-zero ask.
 *   - otherwise the step whose own change to the row was largest, ties to the earlier step, along
 *     the chain asked -> capped (cap) -> scaled (overlay) -> searched (search) -> buffered
 *     unrounded (buffer) -> rounded (rounding) -> clipped (clip) -> trimmed (trim). Under
 *     buffer_step_rule edge the buffered book is the edge book, whole contracts, so the rounding
 *     never moves a row.
 */
std::vector<std::string> attribute(const DayInputs& in, const DayResult& out);

}  // namespace one_pass
}  // namespace trade_ngin
