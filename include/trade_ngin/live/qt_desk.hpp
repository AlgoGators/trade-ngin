// include/trade_ngin/live/qt_desk.hpp
//
// QT plan E3 and E6 to E8 (docs/design/qt-contract.md): what the live futures runner reads and
// writes around the three books (system, qt_proposal, qt) and the command log
// trading.position_overrides. The runner's own trading code is unchanged; these are the reads that
// decide which book a run starts from and writes to, the copy of the model's day into the desk's
// books, and the command row's outcome.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/portfolio/desk_book.hpp"

namespace trade_ngin {
namespace qt {

/// The runner's mode (contract section 6). MODEL is the daily model run.
enum class Mode { MODEL, DESK, OVERRIDE, PUBLISH };

std::string mode_name(Mode mode);

/// `"text"` as a SQL string literal (quotes doubled).
std::string sql_literal(const std::string& text);

/// The portfolio's desk-editing flag: `qt.desk_editable` in its portfolio.json (absent: false).
/// A file that cannot be read or parsed is an error.
Result<bool> read_desk_editable(const std::string& portfolio_json_path);

/// One trading.position_overrides row (contract section 4).
struct AuditRow {
    long id{0};
    std::string portfolio_id;
    std::string date;  ///< YYYY-MM-DD
    std::string kind;
    std::string status;
    std::string requested_by;
    std::string reason;
    nlohmann::json payload = nlohmann::json::object();
    long parent_id{0};  ///< 0: none
    std::string approver_role;
    std::string token_expires_at;  ///< empty: none
    std::string created_at;
};

Result<AuditRow> load_audit_row(PostgresDatabase& db, long id);

/// status 'running' and started_at (kept when already set), unless the row is already finished.
Result<void> start_audit_row(PostgresDatabase& db, long id);

/// The engine's outcome: status (done, refused or failed), result, message, finished_at = now().
Result<void> finish_audit_row(PostgresDatabase& db, long id, const std::string& status,
                              const nlohmann::json& result, const std::string& message);

/// Whether the decision row approves its override request and may be booked: the parent is an
/// override_request of the same portfolio and date, the approver is not the requester (case
/// insensitive), the role is vp or president, and the request's token had not expired when the
/// decision was recorded. On refusal the error says why.
Result<bool> check_override_decision(PostgresDatabase& db, const AuditRow& decision);

/// A desk edit stands for the date: a 'save' or an approved 'override_decision' row is done.
Result<bool> desk_edit_done(PostgresDatabase& db, const std::string& portfolio_id,
                            const std::string& date);

/// Whether the book has a live_results row for the strategy before `date` (any earlier day).
Result<bool> book_has_day_before(PostgresDatabase& db, const std::string& portfolio_id,
                                 const std::string& strategy_id, const std::string& book,
                                 const std::string& date);

/// Whether the book has a live_results row for the strategy on `date`.
Result<bool> book_has_day(PostgresDatabase& db, const std::string& portfolio_id,
                          const std::string& strategy_id, const std::string& book,
                          const std::string& date);

/// The stored positions of one book on one date: sleeve -> symbol -> quantity.
Result<SleeveQuantities> load_book(PostgresDatabase& db, const std::string& portfolio_id,
                                   const std::string& strategy_id, const std::string& book,
                                   const std::string& date);

/// Sum over the sleeves: symbol -> quantity.
std::map<std::string, double> book_totals(const SleeveQuantities& book);

/**
 * Contract section 3: the model's day copied into another book of the same portfolio and date,
 * replacing that book's rows of the date. `with_results` copies executions, live_results (with
 * `book_source`) and the equity curve too; without it, positions only (the qt_proposal seed).
 * One statement, one transaction. moved_by is NULL on the copy.
 */
Result<void> copy_book_day(PostgresDatabase& db, const std::string& portfolio_id,
                           const std::string& strategy_id, const std::string& date,
                           const std::string& from_book, const std::string& to_book,
                           bool with_results, const std::string& book_source);

/// moved_by on the qt rows of the date, per symbol (every sleeve's row of the symbol).
Result<void> write_moved_by(PostgresDatabase& db, const std::string& portfolio_id,
                            const std::string& strategy_id, const std::string& date,
                            const std::map<std::string, std::string>& moved_by);

/// The qt live_results row of the date: book_source, and `detail` merged into risk_detail under
/// the key "desk".
Result<void> mark_qt_day(PostgresDatabase& db, const std::string& portfolio_id,
                         const std::string& strategy_id, const std::string& date,
                         const std::string& book_source, const nlohmann::json& detail);

/// Ruling 29: the earliest day before `date` from the portfolio's first qt day on that has no qt
/// book or is not published. Empty: none (the first desk day has nothing earlier).
Result<std::string> earliest_unpublished_before(PostgresDatabase& db,
                                                const std::string& portfolio_id,
                                                const std::string& strategy_id,
                                                const std::string& date);

/// When the date was published already (empty: not yet).
Result<std::string> published_at(PostgresDatabase& db, const std::string& portfolio_id,
                                  const std::string& strategy_id, const std::string& date);

/// live_run_metadata.published_by / published_at = now() for the date's row.
Result<void> set_published(PostgresDatabase& db, const std::string& portfolio_id,
                           const std::string& strategy_id, const std::string& date,
                           const std::string& published_by);

/// The JSON a desk run writes into its command row (contract section 6).
nlohmann::json desk_result_json(const std::vector<DeskSymbolOutcome>& outcomes,
                                const std::string& book_source);

/// The desk book's positions CSV without the model's columns (forecast and after; ruling 15).
Result<void> strip_model_columns(const std::string& csv_path);

/// True when the e-mail block cannot send: QT_EMAIL_DISABLED=1, or no username/password, or a
/// template placeholder ("YOUR_...").
bool email_disabled(const std::string& username, const std::string& password);

}  // namespace qt
}  // namespace trade_ngin
