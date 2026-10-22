// include/trade_ngin/backtest/stored_run_refusal.hpp
#pragma once

#include <cstdint>
#include <string>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/database_interface.hpp"

namespace trade_ngin {

/**
 * A backtest whose run id is already stored is refused before its first cycle (T-8D-2 R49,
 * D2-BT-2).
 *
 * The run id is deterministic: the sorted strategy ids joined by '&', the window's END instant
 * (UTC) and a milliseconds field (RunIdGenerator::generate_portfolio_run_id). Two runs of the same
 * strategies to the same end date therefore carry ONE id whatever their lookback, their
 * configuration or their code. The id is kept that way on purpose: the A/B tooling compares two
 * runs by it. Without the refusal the second run's daily backtest.final_positions rows replace
 * the first run's as it goes (the writer deletes a date's rows of the key before it inserts),
 * and at the end its backtest.results INSERT fails on the primary key, so its curve, executions
 * and metadata are never written: the tables then hold the first run's results beside the second
 * run's positions.
 *
 * So: a run id that has a row in backtest.results, or a row in backtest.final_positions (the
 * writer stores the run id in its own column, the strategy id beside it), is refused with an
 * ERROR that names the id and the counts, and the run stores nothing. To run the same strategies to the same end date again, delete that
 * run's rows from the five backtest tables first (the test harness empties them before every
 * run, so no harness run is ever refused). A read that fails is not a refusal: the run goes on
 * as it did before this check, with a WARN.
 */
struct StoredRunRows {
    int64_t results{0};          ///< backtest.results rows of the run id
    int64_t final_positions{0};  ///< backtest.final_positions rows of the run id, any strategy
};

/// A value as a SQL text literal (embedded quotes doubled): execute_query binds no parameter.
inline std::string stored_run_sql_literal(const std::string& value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') out += '\'';
        out += ch;
    }
    out += "'";
    return out;
}

/// The statement that counts the run id's rows of backtest.results.
inline std::string stored_run_results_query(const std::string& run_id) {
    return "SELECT COUNT(*) FROM backtest.results WHERE run_id = " + stored_run_sql_literal(run_id);
}

/// The statement that counts the run id's rows of backtest.final_positions, every strategy's.
inline std::string stored_run_positions_query(const std::string& run_id) {
    return "SELECT COUNT(*) FROM backtest.final_positions WHERE run_id = " +
           stored_run_sql_literal(run_id);
}

/// The refusal's text for a run id with stored rows; empty when it has none.
inline std::string stored_run_refusal_text(const std::string& run_id, const StoredRunRows& rows) {
    if (rows.results <= 0 && rows.final_positions <= 0) return {};
    return "BACKTEST REFUSED: run_id " + run_id + " is already stored (backtest.results rows: " +
           std::to_string(rows.results) + ", backtest.final_positions rows: " +
           std::to_string(rows.final_positions) +
           "). The run id is the strategy ids and the window's end date, so this run would replace "
           "that run's positions and then fail to store its own results. Nothing was stored. "
           "Delete that run's rows from the backtest tables, or run to another end date.";
}

/// The run id's stored rows, or the error of the read that failed.
inline Result<StoredRunRows> count_stored_run_rows(DatabaseInterface& db,
                                                   const std::string& run_id) {
    StoredRunRows rows;
    const auto count = [&db](const std::string& query, const char* what) -> Result<int64_t> {
        auto result = db.execute_query(query);
        if (result.is_error()) {
            return make_error<int64_t>(ErrorCode::DATABASE_ERROR,
                                       std::string("the count of ") + what +
                                           " failed: " + result.error()->what(),
                                       "StoredRunRefusal");
        }
        const auto& table = result.value();
        if (!table || table->num_rows() != 1 || table->num_columns() != 1) {
            return make_error<int64_t>(ErrorCode::DATABASE_ERROR,
                                       std::string("the count of ") + what +
                                           " did not come back as one cell",
                                       "StoredRunRefusal");
        }
        return DataConversionUtils::safe_get_int64(table->column(0), 0, what);
    };
    auto results = count(stored_run_results_query(run_id), "backtest.results rows");
    if (results.is_error()) {
        return make_error<StoredRunRows>(results.error()->code(), results.error()->what(),
                                         "StoredRunRefusal");
    }
    auto positions = count(stored_run_positions_query(run_id), "backtest.final_positions rows");
    if (positions.is_error()) {
        return make_error<StoredRunRows>(positions.error()->code(), positions.error()->what(),
                                         "StoredRunRefusal");
    }
    rows.results = results.value();
    rows.final_positions = positions.value();
    return Result<StoredRunRows>(rows);
}

/// What the run does with its id: refused (an INVALID_ARGUMENT error carrying the refusal's text)
/// when the id has stored rows, ok otherwise. `unchecked` is set to the reason when a count could
/// not be read; the run is then NOT refused.
inline Result<void> refuse_stored_run(DatabaseInterface& db, const std::string& run_id,
                                      std::string* unchecked = nullptr) {
    auto rows = count_stored_run_rows(db, run_id);
    if (rows.is_error()) {
        if (unchecked) *unchecked = rows.error()->what();
        return Result<void>();
    }
    const std::string refusal = stored_run_refusal_text(run_id, rows.value());
    if (refusal.empty()) return Result<void>();
    return make_error<void>(ErrorCode::INVALID_ARGUMENT, refusal, "BacktestCoordinator");
}

}  // namespace trade_ngin
