// include/trade_ngin/live/stored_book_ownership.hpp
#pragma once

// A live futures run stores its day under the strategy id it builds from the sleeves it LOADS, and
// seeds each loaded sleeve from that sleeve's own stored rows. Stored positions that belong to a
// strategy id or a sleeve the run does not load are therefore read by nobody: the run would size a
// fresh book beside them, store a clean-looking day under another id and leave them held with no
// fill (a sleeve set enabled_live = false is enough). The runners check this before any row is
// written and refuse the run, naming the rows.
//
// Header-only: the runners call it, the tests feed it a mock database.

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/database_interface.hpp"

namespace trade_ngin {

/// The portfolio's stored non-zero positions on its latest book date before the run date that the
/// run will not load: a row whose strategy_id is not `strategy_id`, or whose sleeve
/// (strategy_name) is not one of `sleeves`. Each entry reads
/// "<book date> <strategy_id>/<sleeve> <symbol> <quantity>", in the query's order (strategy id,
/// sleeve, symbol). Empty: every stored position of the previous book belongs to the run (or the
/// portfolio has no stored book). A failed read is an error: the run cannot show whose book it is.
inline Result<std::vector<std::string>> stored_positions_outside_run(
    DatabaseInterface& db, const std::string& portfolio_id, const std::string& strategy_id,
    const std::vector<std::string>& sleeves, const Timestamp& now,
    const std::string& schema = "trading") {
    using Rows = std::vector<std::string>;
    const std::string run_date = core::format_utc_date(now);
    const std::string query =
        "SELECT to_char(date, 'YYYY-MM-DD') AS stored_book_date, strategy_id, strategy_name, symbol, "
        "quantity::text AS quantity FROM " + schema + ".positions "
        "WHERE portfolio_id = '" + portfolio_id + "' AND quantity <> 0 "
        "AND DATE(date) = (SELECT MAX(DATE(date)) FROM " + schema + ".positions "
        "WHERE portfolio_id = '" + portfolio_id + "' AND DATE(date) < '" + run_date + "') "
        "ORDER BY strategy_id, strategy_name, symbol";
    auto result = db.execute_query(query);
    if (result.is_error()) {
        return make_error<Rows>(ErrorCode::DATABASE_ERROR,
                                "the portfolio's previous stored book could not be read (" +
                                    std::string(result.error()->what()) + ")",
                                "StoredBookOwnership");
    }
    Rows outside;
    const auto table = result.value();
    if (!table || table->num_rows() == 0 || table->num_columns() < 5) return Result<Rows>(outside);
    // the generic converter builds every column as arrow::utf8()
    auto column = [&](int c) {
        return std::static_pointer_cast<arrow::StringArray>(table->column(c)->chunk(0));
    };
    const auto dates = column(0), ids = column(1), names = column(2), symbols = column(3),
               quantities = column(4);
    for (int64_t i = 0; i < table->num_rows(); ++i) {
        const std::string id = ids->GetString(i), sleeve = names->GetString(i);
        const bool loaded = id == strategy_id &&
                            std::find(sleeves.begin(), sleeves.end(), sleeve) != sleeves.end();
        if (loaded) continue;
        outside.push_back(dates->GetString(i) + " " + id + "/" + sleeve + " " +
                          symbols->GetString(i) + " " + quantities->GetString(i));
    }
    return Result<Rows>(outside);
}

/// The refusal's line: how many rows, under which strategy ids and sleeves, and the first rows.
inline std::string stored_positions_outside_run_line(const std::string& portfolio_id,
                                                    const std::string& strategy_id,
                                                    const std::vector<std::string>& outside) {
    std::string rows;
    for (std::size_t i = 0; i < outside.size() && i < 12; ++i) {
        rows += (rows.empty() ? "" : "; ") + outside[i];
    }
    if (outside.size() > 12) rows += "; and " + std::to_string(outside.size() - 12) + " more";
    return "STORED_BOOK_NOT_LOADED portfolio " + portfolio_id + ": " +
           std::to_string(outside.size()) +
           " stored non-zero position(s) of the previous book belong to a strategy or sleeve this "
           "run does not load (it runs as " + strategy_id + "): " + rows +
           ". Refusing to run: the run would store a new book beside them and leave them held "
           "with no fill. Check that the config's portfolio_id names this book's own portfolio, "
           "then load every sleeve the stored book holds a position under.";
}

}  // namespace trade_ngin
