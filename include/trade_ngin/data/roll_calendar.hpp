// include/trade_ngin/data/roll_calendar.hpp
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

#include "trade_ngin/core/holiday_checker.hpp"

namespace trade_ngin::contract_rolls {

/// A calendar date (the UTC date of a daily bar).
using Day = std::chrono::sys_days;

/// "YYYY-MM-DD".
std::string ymd(Day day);
/// Parses "YYYY-MM-DD"; throws std::invalid_argument on anything else.
Day parse_ymd(const std::string& text);

/// Thrown when a date the rule needs falls outside the holiday calendar's coverage. The calendar
/// cannot tell "open" from "unknown" there, so the per-contract roll code fails closed; the API
/// (contract_rolls.hpp) turns it into an error Result.
class CalendarCoverageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief The exchange trading days the roll rule counts (HD 2026-10-23, ruling 1).
 *
 * Monday to Friday less the exchange holidays of `HolidayChecker`
 * (include/trade_ngin/core/holidays.json: the US market closures, ad-hoc closures included).
 * A Saturday bar (MBT) and a Sunday bar are never trading days, and neither is a Globex session on
 * a holiday. Outside the checker's coverage every query throws CalendarCoverageError.
 */
class TradingCalendar {
public:
    /// `is_holiday` and `covers` take a "YYYY-MM-DD" string, as HolidayChecker does.
    TradingCalendar(std::function<bool(const std::string&)> is_holiday,
                    std::function<bool(const std::string&)> covers);

    /// The calendar of a loaded HolidayChecker (kept by reference: it must outlive this object).
    static TradingCalendar from_checker(const HolidayChecker& checker);

    bool is_trading_day(Day day) const;
    /// The n-th trading day strictly before `day` (n >= 1).
    Day trading_day_before(Day day, int n = 1) const;
    /// The first trading day on or after `day`.
    Day trading_day_on_or_after(Day day) const;
    /// The last trading day of a month.
    Day last_trading_day_of_month(int year, unsigned month) const;
    /// The n-th trading day of a month (n >= 1).
    Day nth_trading_day_of_month(int year, unsigned month, int n) const;

private:
    void require_covered(Day day) const;
    std::function<bool(const std::string&)> is_holiday_;
    std::function<bool(const std::string&)> covers_;
};

/**
 * @brief The exchange rules for a contract's last trading day and first notice day, by root.
 *
 * Ported from step 0's audited `r0lib.py` (docs/after_equities/stage3/lead2/evidence/R0/scripts),
 * on this calendar instead of its own holiday list (the two differ only on the ad-hoc closures,
 * which holidays.json has and r0lib.py does not). The last trading day is READ from
 * contract_ohlcv_1d.expiration; this rule only cross-checks it (a mismatch is a WARN). The first
 * notice day has no column and comes from the rule.
 *
 * `month` is the contract's delivery month (the month code), `year` its delivery year.
 */
struct ExchangeRules {
    /// The last trading day by the exchange's rule; throws std::invalid_argument for an unknown root.
    static Day last_trading_day(const std::string& root, int year, unsigned month,
                                const TradingCalendar& cal);
    /// The first notice day, for the physically delivered contracts whose notice period opens
    /// before the last trading day (metals, grains, Treasuries, live cattle); nullopt for the rest
    /// (cash settled, or delivery only after the last trade).
    static std::optional<Day> first_notice_day(const std::string& root, int year, unsigned month,
                                               const TradingCalendar& cal);
    /// Whether the root is one the rules know.
    static bool knows(const std::string& root);
};

}  // namespace trade_ngin::contract_rolls
