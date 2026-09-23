// include/trade_ngin/live/carried_day.hpp
#pragma once

#include <string>
#include <unordered_map>

namespace trade_ngin {

/**
 * @brief The values a futures runner CARRIES into its positions file on a day with no session.
 *
 * HD 2026-09-18 (the Sunday positions CSV): with the MarketDataBus off at the load, a day with
 * no session (the previous calendar day was a Saturday or a holiday, so no T-1 close exists)
 * feeds the strategies nothing and runs no rebalance, so a strategy's forecast, volatility and
 * EMAs are empty that day. The per-strategy positions file keeps the real details instead of
 * zero rows: the held quantities, each symbol's last mark, and each sleeve's last computed
 * forecasts from the previous session, with a note that no session occurred and the values are
 * carried, not computed.
 */

/// A symbol's last mark: its latest loaded close and that bar's UTC date (YYYY-MM-DD).
struct CarriedMark {
    double close{0.0};
    std::string date;
};

/// One sleeve's last computed forecasts: its trading.signals rows of the latest stored run date
/// before today, and that date (YYYY-MM-DD, UTC). Empty when the sleeve has no stored signals.
struct CarriedForecasts {
    std::string session_date;
    std::unordered_map<std::string, double> forecasts;
};

}  // namespace trade_ngin
