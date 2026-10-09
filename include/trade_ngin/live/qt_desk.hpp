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
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/portfolio/desk_book.hpp"

namespace trade_ngin {
namespace qt {

/// The runner's mode (contract section 6 and the 2026-10-09 daily cutoff, contract C7). MODEL is
/// the daily model run. PUBLISH is the desk's approval (--publish: freeze the day; with
/// --send-now it also sends the e-mail). FALLBACK resets qt to the model's book and publishes it
/// (the 10:00 fallback and the catch-up of a past day). SEND e-mails a published day from its
/// stored rows, once.
enum class Mode { MODEL, DESK, OVERRIDE, PUBLISH, FINALIZE_SYSTEM, FALLBACK, SEND };

std::string mode_name(Mode mode);

/// Ruling 17: runs this binary (/proc/self/exe) with --finalize-system --portfolio-config <dir>
/// --date <date> and returns its exit status (-1: it could not be started).
int run_finalize_system(const std::string& portfolio_dir, const std::string& date);

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
    nlohmann::json result = nlohmann::json::object();  ///< what the engine wrote so far
};

Result<AuditRow> load_audit_row(PostgresDatabase& db, long id);

/// status 'running' and started_at (kept when already set), unless the row is already finished.
Result<void> start_audit_row(PostgresDatabase& db, long id);

/// The engine's outcome: status (done, refused or failed), message, finished_at = now(), and
/// `result` merged over the row's result (so a key written while the row ran, e.g. a publish's
/// email_sent_at, stays). Contract C4: only a pending or running row is finished; a row that is
/// already terminal is an error and is left as it is.
Result<void> finish_audit_row(PostgresDatabase& db, long id, const std::string& status,
                              const nlohmann::json& result, const std::string& message);

/// Contract C1: one row of an override request's snapshot of the qt_proposal book.
struct ProposalRow {
    std::string strategy_name;
    std::string symbol;
    long long quantity{0};
};

/// The snapshot's bytes, exactly as Python writes them:
/// json.dumps([{"strategy_name": ..., "symbol": ..., "quantity": ...}, ...],
///            separators=(",", ":"), ensure_ascii=True), rows in the order given.
std::string proposal_snapshot_json(const std::vector<ProposalRow>& rows);

/// SHA-256 of `bytes`, lower-case hex.
std::string sha256_hex(const std::string& bytes);

/// Contract C1's proposal_sha256: the rows sorted by (strategy_name, symbol, quantity), then
/// sha256_hex(proposal_snapshot_json(rows)).
std::string proposal_sha256(std::vector<ProposalRow> rows);

/// Every qt_proposal row of the portfolio and date (zero rows included), sorted by
/// (strategy_name, symbol, quantity). A quantity that is not a whole number is an error.
Result<std::vector<ProposalRow>> load_proposal_rows(PostgresDatabase& db,
                                                    const std::string& portfolio_id,
                                                    const std::string& date);

/// The refusal message of contract C1.
inline const char* kProposalChanged =
    "the proposal changed after the override was requested; request a new override";

/// Whether the decision row approves its override request and may be booked: the parent is an
/// override_request of the same portfolio and date and is done (it was e-mailed), no other
/// decision of that request is done (C2: one decision per request), the approver is not the
/// requester (case insensitive), the role is vp or president, and the request's token had not
/// expired when the decision was recorded. The request must carry its snapshot (C1; a request
/// without one is refused, approved or not), and for an approval the current qt_proposal rows
/// must hash to its proposal_sha256. On refusal the error says why.
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

/// The stored executions of one book on one date, per sleeve (strategy_name), in exec_id order.
Result<std::unordered_map<std::string, std::vector<ExecutionReport>>> load_book_executions(
    PostgresDatabase& db, const std::string& portfolio_id, const std::string& strategy_id,
    const std::string& book, const std::string& date);

/// The stored live_results row of one book on one date as a JSON object (column -> value), or an
/// error when there is none.
Result<nlohmann::json> load_book_results(PostgresDatabase& db, const std::string& portfolio_id,
                                         const std::string& strategy_id, const std::string& book,
                                         const std::string& date);

/// Review E#2: before a desk or override run writes its own qt day, every non-ROLL qt execution
/// of the date (the model's copied fills, or an earlier save's) is deleted, and the qt day's
/// completion marker (live_results.book_source) is cleared; mark_qt_day sets it again last.
/// One transaction.
Result<void> clear_desk_day(PostgresDatabase& db, const std::string& portfolio_id,
                            const std::string& strategy_id, const std::string& date);

/// Whether the qt day may be published (item 3): empty when it may, otherwise the reason.
/// It may when the qt live_results row of the date carries its completion marker (book_source
/// set) and the latest save or override_decision row of the date, if any, is done or refused.
Result<std::string> publish_blocker(PostgresDatabase& db, const std::string& portfolio_id,
                                    const std::string& strategy_id, const std::string& date);

/// When a publish row of the portfolio and date recorded that its e-mail was sent
/// (result.email_sent_at; the earliest), and that row's id. Empty: never sent.
struct EmailSent {
    std::string at;
    long row_id{0};
};
Result<EmailSent> publish_email_sent(PostgresDatabase& db, const std::string& portfolio_id,
                                     const std::string& date);

/// result.email_sent_at = now() on the (pending or running) publish row, committed on its own
/// right after the send; returns the time written.
Result<std::string> mark_email_sent(PostgresDatabase& db, long id);

/// The day's publish record on live_run_metadata (migrations 022 and 027). Empty strings: unset.
struct PublishState {
    std::string published_at;
    std::string published_by;
    std::string publish_source;  ///< desk, fallback or model-only
    std::string sent_at;
};
Result<PublishState> publish_state(PostgresDatabase& db, const std::string& portfolio_id,
                                   const std::string& strategy_id, const std::string& date);

/// Contract C7: the approval. One statement: live_run_metadata.published_by, published_at =
/// now() and publish_source for the date's row, only while it is unpublished, and the same
/// published_at and publish_source merged into the (pending or running) command row's result,
/// so a re-run of that row knows it already approved. Returns published_at. An already
/// published day, or a date without its metadata row, is an error.
Result<std::string> approve_day(PostgresDatabase& db, long row_id, const std::string& portfolio_id,
                                const std::string& strategy_id, const std::string& date,
                                const std::string& published_by, const std::string& source);

/// Contract C7: the day's e-mail went out. One statement: live_run_metadata.sent_at = now() while
/// it is NULL, and (row_id > 0) result.email_sent_at on the pending or running command row.
/// Returns sent_at. Committed on its own, right after the send.
Result<std::string> record_sent(PostgresDatabase& db, long row_id, const std::string& portfolio_id,
                                const std::string& strategy_id, const std::string& date);

/// The publish_source of an approval by the desk and of the fallback.
inline const char* kSourceDesk = "desk";
inline const char* kSourceFallback = "fallback";
/// The requested_by prefix of the scheduler's fallback rows (system:fallback-10am,
/// system:fallback-catchup); --fallback refuses any other row.
inline const char* kFallbackRequester = "system:fallback";

/// Contract C5: what kind of day a desk-editable portfolio's model run is on. Only the runner's
/// calendar closes a day (T-1 a Saturday, a Sunday or a holiday). A day the whole book is carried
/// on (no T-1 price) that the calendar says was a trading day is a feed hole.
enum class DayKind { TRADING, CALENDAR_CLOSED, FEED_HOLE };
DayKind classify_qt_day(int t1_weekday, bool t1_holiday, bool whole_book_carried);

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
