// include/trade_ngin/live/dividend_counter.hpp
//
// trading.live_results.total_dividend_income (T-8D-2 R44 (b)): what the equity runner reads to
// count each dividend on the shares of the position row dated its ex-date, and which cells a run
// writes when one of those reads fails.
//
// "The symbol has no row on its ex-date" and "the row could not be read" are not the same. The
// first is an answer: the event is counted on the quantity the corp-action record holds. The
// second is a failed read: the run logs an ERROR and writes no new figure, so its own row carries
// the figure of the Day T-1 row and the Day T-1 row is left as it is.
//
// Header-only: the runner calls it, the tests execute it without a database.
#pragma once

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/// The shares of a symbol on the position row dated an ex-date, read once per ex-date. It is
/// the lookup CorporateActionsAuditLog::dividend_income_through takes; an ex-date whose rows
/// could not be read is remembered in failed_reads() and answered as "no row".
class ExDateRowShares {
public:
    using Rows = std::unordered_map<std::string, Position>;
    using LoadRows = std::function<Result<Rows>(const std::string& ex_date)>;

    explicit ExDateRowShares(LoadRows load) : load_(std::move(load)) {}

    std::optional<double> operator()(const std::string& symbol, const std::string& ex_date) {
        auto cached = rows_.find(ex_date);
        if (cached == rows_.end()) {
            if (failed_dates_.count(ex_date)) return std::nullopt;
            auto loaded = load_(ex_date);
            if (loaded.is_error()) {
                failed_dates_.insert(ex_date);
                failed_reads_.push_back(ex_date + " (" + std::string(loaded.error()->what()) + ")");
                return std::nullopt;
            }
            cached = rows_.emplace(ex_date, loaded.value()).first;
        }
        const auto row = cached->second.find(symbol);
        if (row == cached->second.end()) return std::nullopt;
        return row->second.quantity.as_double();
    }

    /// One entry per ex-date whose position rows could not be read, in the order met.
    const std::vector<std::string>& failed_reads() const { return failed_reads_; }

private:
    LoadRows load_;
    std::unordered_map<std::string, Rows> rows_;
    std::set<std::string> failed_dates_;
    std::vector<std::string> failed_reads_;
};

/// The two cells a run writes: the Day T-1 row's (no value: the UPDATE does not name the column
/// and the row keeps what it has) and the run's own row's (no value: the INSERT does not name it,
/// so the cell takes the column's default, which is 0).
struct DividendCounterCells {
    std::optional<double> day_t1;
    std::optional<double> today;
};

/// A run whose reads all answered writes the two figures it computed. A run with a failed read
/// (the dividend record, or an ex-date's position rows) computed nothing it can trust: the Day T-1
/// row is left as it is and the run's own row carries the figure stored on the Day T-1 row, the
/// previous value, when there is one. When there is none (no Day T-1 row, a NULL cell, or the
/// carry read failed too) the run's own row is left to the column's default, 0, and the ERROR
/// line is the record that the figure was not computed.
inline DividendCounterCells dividend_counter_cells(bool read_failed, double through_day_t1,
                                                   double through_today,
                                                   const std::optional<double>& stored_on_day_t1) {
    if (read_failed) return {std::nullopt, stored_on_day_t1};
    return {through_day_t1, through_today};
}

}  // namespace trade_ngin
