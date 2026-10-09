// src/live/qt_desk.cpp
#include "trade_ngin/live/qt_desk.hpp"

#include <arrow/api.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace trade_ngin {
namespace qt {

namespace {

using Cell = std::optional<std::string>;
using Rows = std::vector<std::vector<Cell>>;

const std::set<std::string> kBooks = {"system", "qt_proposal", "qt"};

Result<Rows> query_rows(PostgresDatabase& db, const std::string& sql) {
    auto result = db.execute_query(sql);
    if (result.is_error()) {
        return make_error<Rows>(result.error()->code(), result.error()->what(), "QtDesk");
    }
    Rows rows;
    const auto table = result.value();
    if (!table || table->num_rows() == 0) return Result<Rows>(rows);
    const int columns = table->num_columns();
    std::vector<std::shared_ptr<arrow::StringArray>> arrays;
    for (int c = 0; c < columns; ++c) {
        if (table->column(c)->num_chunks() == 0) {
            return make_error<Rows>(ErrorCode::DATABASE_ERROR, "empty column chunk", "QtDesk");
        }
        arrays.push_back(std::static_pointer_cast<arrow::StringArray>(table->column(c)->chunk(0)));
    }
    for (int64_t r = 0; r < table->num_rows(); ++r) {
        std::vector<Cell> row;
        for (int c = 0; c < columns; ++c) {
            if (arrays[c]->IsNull(r)) {
                row.emplace_back(std::nullopt);
            } else {
                row.emplace_back(arrays[c]->GetString(r));
            }
        }
        rows.push_back(std::move(row));
    }
    return Result<Rows>(rows);
}

Result<void> exec(PostgresDatabase& db, const std::string& sql) {
    auto result = db.execute_direct_query(sql);
    if (result.is_error()) {
        return make_error<void>(result.error()->code(), result.error()->what(), "QtDesk");
    }
    return Result<void>();
}

std::string cell(const Cell& c) { return c ? *c : std::string(); }

Result<void> check_book(const std::string& book) {
    if (kBooks.count(book) == 0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "unknown book '" + book + "'",
                                "QtDesk");
    }
    return Result<void>();
}

bool valid_date(const std::string& date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return false;
    for (std::size_t i = 0; i < date.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (!std::isdigit(static_cast<unsigned char>(date[i]))) return false;
    }
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::string mode_name(Mode mode) {
    switch (mode) {
        case Mode::MODEL: return "model";
        case Mode::DESK: return "desk";
        case Mode::OVERRIDE: return "override";
        case Mode::PUBLISH: return "publish";
    }
    return "model";
}

std::string sql_literal(const std::string& text) {
    std::string out = "'";
    for (char ch : text) {
        if (ch == '\'') out += '\'';
        out += ch;
    }
    out += "'";
    return out;
}

Result<bool> read_desk_editable(const std::string& portfolio_json_path) {
    std::ifstream in(portfolio_json_path);
    if (!in) {
        return make_error<bool>(ErrorCode::FILE_NOT_FOUND,
                                "cannot read " + portfolio_json_path, "QtDesk");
    }
    try {
        nlohmann::json j;
        in >> j;
        if (!j.contains("qt")) return Result<bool>(false);
        const auto& block = j.at("qt");
        if (!block.is_object()) {
            return make_error<bool>(ErrorCode::INVALID_ARGUMENT,
                                    portfolio_json_path + ": \"qt\" is not an object", "QtDesk");
        }
        if (!block.contains("desk_editable")) return Result<bool>(false);
        if (!block.at("desk_editable").is_boolean()) {
            return make_error<bool>(ErrorCode::INVALID_ARGUMENT,
                                    portfolio_json_path + ": qt.desk_editable is not a boolean",
                                    "QtDesk");
        }
        return Result<bool>(block.at("desk_editable").get<bool>());
    } catch (const std::exception& e) {
        return make_error<bool>(ErrorCode::INVALID_ARGUMENT,
                                portfolio_json_path + ": " + e.what(), "QtDesk");
    }
}

Result<AuditRow> load_audit_row(PostgresDatabase& db, long id) {
    auto rows = query_rows(
        db,
        "SELECT id::text, portfolio_id, to_char(date, 'YYYY-MM-DD'), kind, status, requested_by, "
        "COALESCE(reason, ''), payload::text, COALESCE(parent_id::text, '0'), "
        "COALESCE(approver_role, ''), COALESCE(token_expires_at::text, ''), created_at::text "
        "FROM trading.position_overrides WHERE id = " + std::to_string(id));
    if (rows.is_error()) {
        return make_error<AuditRow>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    if (rows.value().empty()) {
        return make_error<AuditRow>(ErrorCode::DATA_NOT_FOUND,
                                    "trading.position_overrides has no row " + std::to_string(id),
                                    "QtDesk");
    }
    const auto& r = rows.value().front();
    AuditRow row;
    row.id = std::stol(cell(r[0]));
    row.portfolio_id = cell(r[1]);
    row.date = cell(r[2]);
    row.kind = cell(r[3]);
    row.status = cell(r[4]);
    row.requested_by = cell(r[5]);
    row.reason = cell(r[6]);
    try {
        row.payload = nlohmann::json::parse(cell(r[7]).empty() ? "{}" : cell(r[7]));
    } catch (const std::exception&) {
        row.payload = nlohmann::json::object();
    }
    row.parent_id = std::stol(cell(r[8]));
    row.approver_role = cell(r[9]);
    row.token_expires_at = cell(r[10]);
    row.created_at = cell(r[11]);
    return Result<AuditRow>(row);
}

Result<void> start_audit_row(PostgresDatabase& db, long id) {
    return exec(db, "UPDATE trading.position_overrides SET status = 'running', "
                    "started_at = COALESCE(started_at, now()) WHERE id = " +
                        std::to_string(id) + " AND status IN ('pending', 'running')");
}

Result<void> finish_audit_row(PostgresDatabase& db, long id, const std::string& status,
                              const nlohmann::json& result, const std::string& message) {
    if (status != "done" && status != "refused" && status != "failed") {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "unknown outcome '" + status + "'",
                                "QtDesk");
    }
    return exec(db, "UPDATE trading.position_overrides SET status = " + sql_literal(status) +
                        ", result = " + sql_literal(result.dump()) + "::jsonb, message = " +
                        sql_literal(message) +
                        ", started_at = COALESCE(started_at, now()), finished_at = now() "
                        "WHERE id = " + std::to_string(id));
}

Result<bool> check_override_decision(PostgresDatabase& db, const AuditRow& decision) {
    auto refuse = [](const std::string& why) {
        return make_error<bool>(ErrorCode::INVALID_ARGUMENT, why, "QtDesk");
    };
    if (decision.kind != "override_decision") return refuse("row is not an override_decision");
    if (!decision.payload.is_object() || !decision.payload.contains("approved") ||
        !decision.payload.at("approved").is_boolean()) {
        return refuse("the decision's payload has no boolean \"approved\"");
    }
    const bool approved = decision.payload.at("approved").get<bool>();
    if (decision.parent_id <= 0) return refuse("the decision names no override request");
    auto parent = load_audit_row(db, decision.parent_id);
    if (parent.is_error()) return refuse(parent.error()->what());
    const AuditRow& request = parent.value();
    if (request.kind != "override_request") {
        return refuse("the decision's parent " + std::to_string(request.id) +
                      " is not an override_request");
    }
    if (request.portfolio_id != decision.portfolio_id || request.date != decision.date) {
        return refuse("the decision and its request are for different portfolios or dates");
    }
    if (lower(trim(request.requested_by)) == lower(trim(decision.requested_by))) {
        return refuse("the requester may not approve their own request");
    }
    if (decision.approver_role != "vp" && decision.approver_role != "president") {
        return refuse("the approver role must be vp or president");
    }
    if (request.token_expires_at.empty()) {
        return refuse("the override request was never e-mailed (no token)");
    }
    auto expired = query_rows(db, "SELECT (" + sql_literal(request.token_expires_at) +
                                      "::timestamptz < " + sql_literal(decision.created_at) +
                                      "::timestamptz)::text");
    if (expired.is_error()) return refuse(expired.error()->what());
    if (!expired.value().empty() && cell(expired.value()[0][0]) == "true") {
        return refuse("the override link had expired when the decision was recorded");
    }
    return Result<bool>(approved);
}

Result<bool> desk_edit_done(PostgresDatabase& db, const std::string& portfolio_id,
                            const std::string& date) {
    auto rows = query_rows(
        db, "SELECT count(*)::text FROM trading.position_overrides WHERE portfolio_id = " +
                sql_literal(portfolio_id) + " AND date = " + sql_literal(date) +
                "::date AND status = 'done' AND (kind = 'save' OR (kind = 'override_decision' "
                "AND COALESCE((payload->>'approved')::boolean, false)))");
    if (rows.is_error()) {
        return make_error<bool>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    return Result<bool>(!rows.value().empty() && std::stol(cell(rows.value()[0][0])) > 0);
}

namespace {
Result<bool> count_days(PostgresDatabase& db, const std::string& portfolio_id,
                        const std::string& strategy_id, const std::string& book,
                        const std::string& date, const char* op) {
    if (auto b = check_book(book); b.is_error()) {
        return make_error<bool>(b.error()->code(), b.error()->what(), "QtDesk");
    }
    auto rows = query_rows(
        db, "SELECT count(*)::text FROM trading.live_results WHERE portfolio_id = " +
                sql_literal(portfolio_id) + " AND strategy_id = " + sql_literal(strategy_id) +
                " AND portfolio_type = " + sql_literal(book) + " AND date " + op + " " +
                sql_literal(date) + "::date");
    if (rows.is_error()) {
        return make_error<bool>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    return Result<bool>(!rows.value().empty() && std::stol(cell(rows.value()[0][0])) > 0);
}
}  // namespace

Result<bool> book_has_day_before(PostgresDatabase& db, const std::string& portfolio_id,
                                 const std::string& strategy_id, const std::string& book,
                                 const std::string& date) {
    return count_days(db, portfolio_id, strategy_id, book, date, "<");
}

Result<bool> book_has_day(PostgresDatabase& db, const std::string& portfolio_id,
                          const std::string& strategy_id, const std::string& book,
                          const std::string& date) {
    return count_days(db, portfolio_id, strategy_id, book, date, "=");
}

Result<SleeveQuantities> load_book(PostgresDatabase& db, const std::string& portfolio_id,
                                   const std::string& strategy_id, const std::string& book,
                                   const std::string& date) {
    if (auto b = check_book(book); b.is_error()) {
        return make_error<SleeveQuantities>(b.error()->code(), b.error()->what(), "QtDesk");
    }
    auto rows = query_rows(
        db, "SELECT strategy_name, symbol, quantity::text FROM trading.positions "
            "WHERE portfolio_id = " + sql_literal(portfolio_id) +
                " AND strategy_id = " + sql_literal(strategy_id) +
                " AND portfolio_type = " + sql_literal(book) + " AND date = " +
                sql_literal(date) + "::date ORDER BY strategy_name, symbol");
    if (rows.is_error()) {
        return make_error<SleeveQuantities>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    SleeveQuantities out;
    for (const auto& r : rows.value()) {
        out[cell(r[0])][cell(r[1])] = std::stod(cell(r[2]).empty() ? "0" : cell(r[2]));
    }
    return Result<SleeveQuantities>(out);
}

std::map<std::string, double> book_totals(const SleeveQuantities& book) {
    std::map<std::string, double> totals;
    for (const auto& [sleeve, rows] : book) {
        (void)sleeve;
        for (const auto& [symbol, quantity] : rows) totals[symbol] += quantity;
    }
    return totals;
}

Result<void> copy_book_day(PostgresDatabase& db, const std::string& portfolio_id,
                           const std::string& strategy_id, const std::string& date,
                           const std::string& from_book, const std::string& to_book,
                           bool with_results, const std::string& book_source) {
    if (auto b = check_book(from_book); b.is_error()) return b;
    if (auto b = check_book(to_book); b.is_error()) return b;
    if (from_book == to_book || !valid_date(date)) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "bad copy " + from_book + " -> " +
                                    to_book + " on " + date, "QtDesk");
    }
    struct Table {
        std::string name;
        std::string date_predicate;
    };
    std::vector<Table> tables = {{"positions", "date = " + sql_literal(date) + "::date"}};
    if (with_results) {
        tables.push_back({"executions", "date = " + sql_literal(date) + "::date"});
        tables.push_back({"live_results", "date = " + sql_literal(date) + "::date"});
        tables.push_back({"equity_curve", "DATE(\"timestamp\") = " + sql_literal(date) + "::date"});
    }
    std::string sql;
    for (const auto& table : tables) {
        auto columns = query_rows(
            db, "SELECT column_name FROM information_schema.columns WHERE table_schema = "
                "'trading' AND table_name = " + sql_literal(table.name) +
                    " AND column_name <> 'id' ORDER BY ordinal_position");
        if (columns.is_error()) {
            return make_error<void>(columns.error()->code(), columns.error()->what(), "QtDesk");
        }
        std::string names, values;
        bool has_book = false;
        for (const auto& c : columns.value()) {
            const std::string name = cell(c[0]);
            const std::string quoted = "\"" + name + "\"";
            std::string value = quoted;
            if (name == "portfolio_type") {
                value = sql_literal(to_book);
                has_book = true;
            } else if (name == "moved_by") {
                value = "NULL";
            } else if (name == "order_id" && table.name == "executions") {
                // the order id carries the book (master document, final schema)
                value = sql_literal(to_book + "-") + " || " + quoted;
            } else if (name == "book_source") {
                value = to_book == "qt" && !book_source.empty() ? sql_literal(book_source) : "NULL";
            }
            names += (names.empty() ? "" : ", ") + quoted;
            values += (values.empty() ? "" : ", ") + value;
        }
        if (!has_book) {
            return make_error<void>(ErrorCode::DATABASE_ERROR,
                                    "trading." + table.name +
                                        " has no portfolio_type column (migration 021)",
                                    "QtDesk");
        }
        const std::string where = " WHERE portfolio_id = " + sql_literal(portfolio_id) +
                                  " AND strategy_id = " + sql_literal(strategy_id) + " AND " +
                                  table.date_predicate + " AND portfolio_type = ";
        sql += "DELETE FROM trading." + table.name + where + sql_literal(to_book) + "; ";
        sql += "INSERT INTO trading." + table.name + " (" + names + ") SELECT " + values +
               " FROM trading." + table.name + where + sql_literal(from_book) + "; ";
    }
    return exec(db, sql);
}

Result<void> write_moved_by(PostgresDatabase& db, const std::string& portfolio_id,
                            const std::string& strategy_id, const std::string& date,
                            const std::map<std::string, std::string>& moved_by) {
    if (moved_by.empty()) return Result<void>();
    std::string cases;
    for (const auto& [symbol, step] : moved_by) {
        cases += " WHEN " + sql_literal(symbol) + " THEN " + sql_literal(step);
    }
    return exec(db, "UPDATE trading.positions SET moved_by = CASE symbol" + cases +
                        " ELSE moved_by END WHERE portfolio_id = " + sql_literal(portfolio_id) +
                        " AND strategy_id = " + sql_literal(strategy_id) + " AND date = " +
                        sql_literal(date) + "::date AND portfolio_type = 'qt'");
}

Result<void> mark_qt_day(PostgresDatabase& db, const std::string& portfolio_id,
                         const std::string& strategy_id, const std::string& date,
                         const std::string& book_source, const nlohmann::json& detail) {
    auto rows = db.execute_direct_query(
        "UPDATE trading.live_results SET book_source = " + sql_literal(book_source) +
        ", risk_detail = COALESCE(risk_detail, '{}'::jsonb) || jsonb_build_object('desk', " +
        sql_literal(detail.dump()) + "::jsonb) WHERE portfolio_id = " + sql_literal(portfolio_id) +
        " AND strategy_id = " + sql_literal(strategy_id) + " AND date = " + sql_literal(date) +
        "::date AND portfolio_type = 'qt'");
    if (rows.is_error()) {
        return make_error<void>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    if (rows.value() != 1) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "expected one qt live_results row on " + date + ", updated " +
                                    std::to_string(rows.value()),
                                "QtDesk");
    }
    return Result<void>();
}

Result<std::string> earliest_unpublished_before(PostgresDatabase& db,
                                                const std::string& portfolio_id,
                                                const std::string& strategy_id,
                                                const std::string& date) {
    const std::string pid = sql_literal(portfolio_id), sid = sql_literal(strategy_id);
    auto rows = query_rows(
        db,
        "WITH first AS (SELECT MIN(date) AS d FROM trading.live_results WHERE portfolio_id = " +
            pid + " AND strategy_id = " + sid +
            " AND portfolio_type = 'qt'), days AS (SELECT generate_series((SELECT d FROM first), " +
            sql_literal(date) +
            "::date - 1, interval '1 day')::date AS day) "
            "SELECT COALESCE(to_char(MIN(day), 'YYYY-MM-DD'), '') FROM days WHERE "
            "NOT EXISTS (SELECT 1 FROM trading.live_results r WHERE r.portfolio_id = " +
            pid + " AND r.strategy_id = " + sid +
            " AND r.portfolio_type = 'qt' AND r.date = days.day) OR NOT EXISTS (SELECT 1 FROM "
            "trading.live_run_metadata m WHERE m.portfolio_id = " +
            pid + " AND m.strategy_id = " + sid +
            " AND m.date = days.day AND m.published_at IS NOT NULL)");
    if (rows.is_error()) {
        return make_error<std::string>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    return Result<std::string>(rows.value().empty() ? std::string() : cell(rows.value()[0][0]));
}

Result<std::string> published_at(PostgresDatabase& db, const std::string& portfolio_id,
                                  const std::string& strategy_id, const std::string& date) {
    auto rows = query_rows(
        db, "SELECT COALESCE(published_at::text, '') FROM trading.live_run_metadata WHERE "
            "portfolio_id = " + sql_literal(portfolio_id) + " AND strategy_id = " +
                sql_literal(strategy_id) + " AND date = " + sql_literal(date) + "::date");
    if (rows.is_error()) {
        return make_error<std::string>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    return Result<std::string>(rows.value().empty() ? std::string() : cell(rows.value()[0][0]));
}

Result<void> set_published(PostgresDatabase& db, const std::string& portfolio_id,
                           const std::string& strategy_id, const std::string& date,
                           const std::string& published_by) {
    auto rows = db.execute_direct_query(
        "UPDATE trading.live_run_metadata SET published_by = " + sql_literal(published_by) +
        ", published_at = now() WHERE portfolio_id = " + sql_literal(portfolio_id) +
        " AND strategy_id = " + sql_literal(strategy_id) + " AND date = " + sql_literal(date) +
        "::date");
    if (rows.is_error()) {
        return make_error<void>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    if (rows.value() != 1) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "no live_run_metadata row for " + portfolio_id + " on " + date,
                                "QtDesk");
    }
    return Result<void>();
}

nlohmann::json desk_result_json(const std::vector<DeskSymbolOutcome>& outcomes,
                                const std::string& book_source) {
    nlohmann::json symbols = nlohmann::json::array();
    for (const auto& o : outcomes) {
        symbols.push_back({{"symbol", o.symbol},
                           {"asked", o.asked},
                           {"given", o.given},
                           {"moved_by", o.moved_by}});
    }
    return {{"symbols", symbols}, {"book_source", book_source}};
}

Result<void> strip_model_columns(const std::string& csv_path) {
    std::ifstream in(csv_path);
    if (!in) {
        return make_error<void>(ErrorCode::FILE_NOT_FOUND, "cannot read " + csv_path, "QtDesk");
    }
    std::vector<std::string> lines;
    std::string line;
    std::size_t keep = std::string::npos;  // the number of fields kept, once the header is seen
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] != '#') {
            std::vector<std::string> fields;
            std::size_t start = 0;
            while (true) {
                const std::size_t comma = line.find(',', start);
                fields.push_back(line.substr(start, comma - start));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            if (keep == std::string::npos) {
                const auto f = std::find(fields.begin(), fields.end(), "forecast");
                if (f != fields.end()) keep = static_cast<std::size_t>(f - fields.begin());
            }
            if (keep != std::string::npos && fields.size() > keep) {
                fields.resize(keep);
                line.clear();
                for (std::size_t i = 0; i < fields.size(); ++i) {
                    line += (i == 0 ? "" : ",") + fields[i];
                }
            }
        }
        lines.push_back(line);
    }
    in.close();
    std::ofstream out(csv_path, std::ios::trunc);
    for (const auto& l : lines) out << l << "\n";
    return Result<void>();
}

bool email_disabled(const std::string& username, const std::string& password) {
    const char* flag = std::getenv("QT_EMAIL_DISABLED");
    if (flag != nullptr && std::string(flag) == "1") return true;
    if (username.empty() || password.empty()) return true;
    return username.rfind("YOUR_", 0) == 0 || password.rfind("YOUR_", 0) == 0;
}

}  // namespace qt
}  // namespace trade_ngin
