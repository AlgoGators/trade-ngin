#pragma once

// The six overlay columns of trading.live_results (migration 030; HD 2026-10-10): the overlay's
// own readings of the STORED book on the sizing capital, and a one-day VaR from the same
// covariance. Nothing here computes a reading: the readings come from overlay::readings, run by
// the pass on its stored book (OnePassDay) or by the PortfolioManager on the book a live runner
// stores (PortfolioManager::overlay_readings_for_book).
//
// LOOP_SPEC v6.2 sections 7.2 and 7.3 say the readings stay on the OVERLAY line and are never
// columns; HD's later decision stores them and governs.

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "trade_ngin/data/live_results_cell.hpp"
#include "trade_ngin/risk/overlay.hpp"
#include "trade_ngin/risk/risk_detail.hpp"

namespace trade_ngin {

/// The overlay's readings of one book, with how much of the book the covariance covers.
struct StoredBookReadings {
    overlay::Readings readings;
    int contracts_held{0};     ///< the book's non-zero symbols
    int contracts_in_risk{0};  ///< those of them inside the covariance (in R and R_jump)
};

/// The one-sided 95 percent point of a standard normal: var_95_1d's multiple of the daily sigma.
inline constexpr double kVar95Sigmas = 1.645;

/// The six cells. A field with no value is a NULL cell.
struct OverlayColumns {
    std::optional<double> overlay_risk;            ///< R, annualised, a fraction of the sizing capital
    std::optional<double> overlay_risk_jump;       ///< R_jump
    std::optional<double> overlay_risk_shock;      ///< R_shock
    std::optional<double> overlay_gross_leverage;  ///< L
    std::optional<double> overlay_net_leverage;    ///< L_net, signed
    std::optional<double> var_95_1d;               ///< dollars, positive
};

/**
 * @brief The six cells of a row.
 *
 * @param stores_detail      the row carries risk_detail (OnePassDay::stores_detail): a sized
 *                           rebalance the overlay answered. False: all six are NULL.
 * @param readings           the overlay's readings of the stored book
 * @param window_bars_per_year the gate window's own annualisation factor
 * @param sizing_capital     E_t, the capital the readings are weights on
 *
 * On a blind window (`covariance_readings` false) the three risk readings and the VaR have no
 * value (the code's 0.0 is never stored); the two leverage readings are stored.
 * var_95_1d = 1.645 x R / sqrt(bars a year) x E_t: the stored book's expected one-day sigma in
 * dollars, from the covariance R is read on, times the 95 percent normal point.
 */
inline OverlayColumns overlay_columns_of(bool stores_detail, const overlay::Readings& readings,
                                         double window_bars_per_year, double sizing_capital) {
    OverlayColumns out;
    if (!stores_detail) return out;
    out.overlay_gross_leverage = readings.gross;
    out.overlay_net_leverage = readings.net;
    if (!readings.covariance_readings) return out;
    out.overlay_risk = readings.risk;
    out.overlay_risk_jump = readings.jump;
    out.overlay_risk_shock = readings.shock;
    if (window_bars_per_year > 0.0) {
        out.var_95_1d =
            kVar95Sigmas * readings.risk / std::sqrt(window_bars_per_year) * sizing_capital;
    }
    return out;
}

/**
 * @brief The OVERLAY_STORED log line of a live run: the readings of the book the runner stores
 *        (the accessor's), the window's bars a year, the sizing capital and the VaR, how many of
 *        the book's contracts the covariance covers, and beside them the pass's own readings of
 *        its stored book with whether the five are equal to the bit (they are on a day the
 *        runner changed no row after the pass; `rolled_back` counts the rows it did change).
 */
inline std::string overlay_stored_line(const std::string& date, const OnePassDay& day,
                                       const StoredBookReadings& stored,
                                       const OverlayColumns& columns, std::size_t rolled_back) {
    auto num = [](double v) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        return std::string(buf);
    };
    const overlay::Readings& r = stored.readings;
    const bool equal = r.covariance_readings == day.covariance_readings &&
                       r.risk == day.overlay_risk && r.jump == day.overlay_risk_jump &&
                       r.shock == day.overlay_risk_shock && r.gross == day.overlay_gross_leverage &&
                       r.net == day.overlay_net_leverage;
    return "OVERLAY_STORED date=" + date + " covariance=" + (r.covariance_readings ? "1" : "0") +
           " R=" + num(r.risk) + " R_jump=" + num(r.jump) + " R_shock=" + num(r.shock) +
           " L_g=" + num(r.gross) + " L_n=" + num(r.net) +
           " bars_per_year=" + num(day.window_bars_per_year) +
           " capital=" + num(day.sizing_capital) +
           " var_95_1d=" + (columns.var_95_1d ? num(*columns.var_95_1d) : std::string("null")) +
           " in_risk=" + std::to_string(stored.contracts_in_risk) +
           " held=" + std::to_string(stored.contracts_held) +
           " pass_R=" + num(day.overlay_risk) + " pass_R_jump=" + num(day.overlay_risk_jump) +
           " pass_R_shock=" + num(day.overlay_risk_shock) +
           " pass_L_g=" + num(day.overlay_gross_leverage) +
           " pass_L_n=" + num(day.overlay_net_leverage) +
           " equal_to_pass=" + (equal ? "1" : "0") +
           " rolled_back=" + std::to_string(rolled_back);
}

/// The six as typed cells, in the migration's order; `number` renders a value as a cell's text.
template <typename Number>
std::vector<LiveResultsCell> overlay_cells(const OverlayColumns& c, Number number) {
    auto cell = [&](const std::optional<double>& v) {
        return v ? number(*v) : std::optional<std::string>();
    };
    return {
        {"overlay_risk", "numeric", cell(c.overlay_risk)},
        {"overlay_risk_jump", "numeric", cell(c.overlay_risk_jump)},
        {"overlay_risk_shock", "numeric", cell(c.overlay_risk_shock)},
        {"overlay_gross_leverage", "numeric", cell(c.overlay_gross_leverage)},
        {"overlay_net_leverage", "numeric", cell(c.overlay_net_leverage)},
        {"var_95_1d", "numeric", cell(c.var_95_1d)},
    };
}

}  // namespace trade_ngin
