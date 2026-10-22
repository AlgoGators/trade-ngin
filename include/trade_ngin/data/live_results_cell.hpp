// One typed cell of a trading.live_results row that is not a plain double or integer: a date, a
// text, a jsonb object, or a number that may have no value (migration 030).
//
// The day's INSERT names a cell only when it has a value, so a cell without one stays NULL; the
// Day T-1 refresh assigns every cell and writes NULL where there is no value. The value travels
// as text and is cast by the statement to the cell's type.

#pragma once

#include <optional>
#include <string>

namespace trade_ngin {

struct LiveResultsCell {
    std::string column;
    std::string type;                  ///< numeric, integer, date, text or jsonb
    std::optional<std::string> value;  ///< no value: the cell is NULL
};

/// The types a cell may be cast to; the statement names the type, so it is checked against this
/// list before anything is built.
inline bool live_results_cell_type_known(const std::string& type) {
    return type == "numeric" || type == "integer" || type == "date" || type == "text" ||
           type == "jsonb";
}

}  // namespace trade_ngin
