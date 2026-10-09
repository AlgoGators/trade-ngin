// include/trade_ngin/risk/overlay.hpp
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace trade_ngin {
namespace overlay {

/**
 * The risk overlay's arithmetic in capital terms (LOOP_SPEC section 4), as pure functions of the
 * participants' returns and position weights. Nothing here reads a book, a database or a clock.
 *
 * THE GATE WINDOW. `returns[d][i]` is participant i's adjusted percentage return on date d (NaN
 * where it has none), the rows in date order ENDING AT the signal date; `ordinals[d]` is the date
 * as a day number. The window is the last 252 rows on which ANY participant has a return. Then:
 *   - a participant with NO return in the window is dropped from R, R_jump and R_shock and stays in
 *     leverage;
 *   - a participant with fewer than 120 returns in the window is left out of the complete-date
 *     intersection and of R and R_jump, and stays in R_shock on its own window sigma (at least two
 *     returns, else it is dropped as above) and in leverage;
 *   - the complete dates are the window rows on which every remaining participant has a return;
 *   - below 21 complete dates the window is BLIND: no covariance reading; leverage still applies;
 *   - one annualisation: bars a year = (complete dates - 1) / (the calendar days they span /
 *     365.25);
 *   - with at least 120 complete dates the covariance is the sample covariance (n - 1) of the
 *     complete dates; below that, of every window row with the missing returns zero-filled.
 * The covariance is annualised and indexed over the participants that are in R, in their order.
 */
inline constexpr std::size_t kWindowDates = 252;
inline constexpr std::size_t kCompleteDatesFloor = 120;  // the complete-date covariance engages
inline constexpr std::size_t kBlindBelow = 21;
inline constexpr std::size_t kParticipantReturnsFloor = 120;
inline constexpr double kDaysPerYear = 365.25;

struct GateWindow {
    enum class Mode { kBlind, kComplete, kZeroFill };
    Mode mode{Mode::kBlind};
    std::size_t window_dates{0};
    std::size_t complete_dates{0};
    double first_ordinal{0.0};  ///< the window's first and last rows (0 when the window is empty)
    double last_ordinal{0.0};
    double bars_per_year{0.0};  ///< 0 while blind
    std::vector<char> in_r;         ///< per participant: in R and R_jump
    std::vector<char> in_shock;     ///< per participant: in R_shock
    std::vector<char> no_return;    ///< per participant: no return in the window
    std::vector<char> short_history;  ///< per participant: fewer than 120 returns in the window
    std::vector<double> shock_sigma;  ///< per participant: its annualised sigma (0 where not in_shock)
    std::vector<std::vector<double>> covariance;  ///< annualised, over the in_r participants

    bool blind() const { return mode == Mode::kBlind; }
    static const char* name(Mode mode);
};

GateWindow gate_window(const std::vector<std::vector<double>>& returns,
                       const std::vector<double>& ordinals);

/**
 * THE READINGS on the participants' weights x_i = N_i M_i P_i FX_i / E (signed, on the sizing
 * capital): R = sqrt(x' Sigma x) and R_jump = sqrt(x' Sigma_jump x) over the participants in R,
 * Sigma_jump the window's correlations on each participant's own jump sigma (`sigma_jump`,
 * annualised); R_shock = sum |x_i| sigma_i over the participants in R_shock; L = sum |x_i| and
 * L_net = sum x_i (signed) over every participant. On a BLIND window the three covariance readings
 * are not computed (`covariance_readings` false, the values 0) and the two leverage readings are.
 */
struct Readings {
    bool covariance_readings{false};
    double risk{0.0};
    double jump{0.0};
    double shock{0.0};
    double gross{0.0};
    double net{0.0};  ///< signed
};

Readings readings(const std::vector<double>& weights, const GateWindow& window,
                  const std::vector<double>& sigma_jump);

/**
 * THE MULTIPLIER: m = the smallest of min(1, limit / reading) over the five terms in the order
 * risk, jump, shock, gross leverage, net leverage (the net term on |L_net|); a reading that is not
 * computed or is not positive asks for nothing. `binding` is the first term at m, or "none" when
 * m is 1.
 */
struct Limits {
    double risk{0.0};
    double jump{0.0};
    double shock{0.0};
    double gross{0.0};
    double net{0.0};
};

struct Multiplier {
    double m{1.0};
    std::string binding{"none"};  ///< R, R_jump, R_shock, L_g, L_n or none
    double risk{1.0};
    double jump{1.0};
    double shock{1.0};
    double gross{1.0};
    double net{1.0};
};

Multiplier multiplier(const Readings& readings, const Limits& limits);

/// Section 6.4: the rows of the weights that KEEP `term` (R, R_jump, R_shock, L_g, L_n) over: for R
/// and R_jump the rows with a positive Euler contribution x_i (Sigma x)_i among the participants in
/// R (a hedging row is not named); for R_shock every non-zero row in R_shock; for L_g every
/// non-zero row; for L_n the rows on the net's side.
std::vector<char> contributors(const std::vector<double>& weights, const GateWindow& window,
                               const std::vector<double>& sigma_jump, const std::string& term);

/// The p-th percentile (0..100) of the values by linear interpolation between order statistics.
double percentile(std::vector<double> values, double pct);

/**
 * THE OVERLAY'S INPUTS for one rebalance, built once from the participants' own series and read by
 * every evaluation of that rebalance. `symbols` are the participants in sorted order; row d of
 * `returns` holds each participant's adjusted percentage return on the date `ordinals[d]` (NaN
 * where it has none); the rows are the union of the participants' dates, in date order, ending at
 * the signal date. `close`, `multiplier` and `jump_sigma_daily` are per participant; a participant
 * with no series (`has_series` 0) has no return, no price and no weight.
 */
struct Inputs {
    double tau{0.0};  ///< the book's risk target, the unit the three risk limits are ratios to
    std::vector<std::string> symbols;
    std::vector<std::vector<double>> returns;
    std::vector<double> ordinals;
    std::vector<double> close;
    std::vector<double> multiplier;
    std::vector<double> jump_sigma_daily;
    std::vector<char> has_series;
};

/// One participant's series as its sleeve holds it (StrategyInterface::OverlaySeries).
struct ParticipantSeries {
    bool present{false};
    const std::vector<double>* day{nullptr};
    const std::vector<double>* returns{nullptr};
    double close{0.0};
    double multiplier{1.0};
    double jump_sigma_daily{0.0};
};

/// Aligns the participants' series on the union of their dates.
Inputs build_inputs(double tau, const std::vector<std::string>& symbols,
                    const std::vector<ParticipantSeries>& series);

/// The three risk limits as ratios to tau, and the two leverage limits as they are.
struct LimitRatios {
    double risk{0.0};   ///< R_max / tau
    double jump{0.0};   ///< R_jump_max / tau
    double shock{0.0};  ///< R_shock_max / tau
    double gross{0.0};  ///< L_max
    double net{0.0};    ///< L_net_max
    bool set() const { return risk > 0.0 && jump > 0.0 && shock > 0.0 && gross > 0.0 && net > 0.0; }
};

/**
 * One evaluation: the weights of a book on the sizing capital, the window, the readings, the
 * limits in force and the multiplier. `weights[i]` = quantity x multiplier x close / capital for
 * participant i (FX is 1 on this book); a participant the book does not hold has weight 0 and
 * still counts in the window's dates. `outside` lists the book's non-zero symbols that are no
 * participant or have no series: they are in no reading, and the caller says so.
 */
struct Evaluation {
    GateWindow window;
    Readings readings;
    Limits limits;
    Multiplier multiplier;
    std::vector<double> weights;
    std::vector<double> sigma_jump;  ///< per participant, annualised on the window's factor
    std::vector<std::string> outside;
};

Evaluation evaluate(const Inputs& inputs, const std::vector<std::pair<std::string, double>>& book,
                    double capital, const LimitRatios& ratios);

}  // namespace overlay
}  // namespace trade_ngin
