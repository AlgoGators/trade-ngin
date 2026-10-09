// src/live/qt_desk.cpp
#include "trade_ngin/live/qt_desk.hpp"

#include <arrow/api.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <sys/wait.h>
#include <unistd.h>

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
        case Mode::FINALIZE_SYSTEM: return "finalize-system";
        case Mode::FALLBACK: return "fallback";
        case Mode::SEND: return "send";
    }
    return "model";
}

int run_finalize_system(const std::string& portfolio_dir, const std::string& date) {
    std::vector<std::string> args = {"live_portfolio", "--finalize-system", "--portfolio-config",
                                     portfolio_dir, "--date", date};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv("/proc/self/exe", argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
        "COALESCE(approver_role, ''), COALESCE(token_expires_at::text, ''), created_at::text, "
        "COALESCE(result::text, '{}') FROM trading.position_overrides WHERE id = " +
            std::to_string(id));
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
    try {
        row.result = nlohmann::json::parse(cell(r[12]).empty() ? "{}" : cell(r[12]));
        if (!row.result.is_object()) row.result = nlohmann::json::object();
    } catch (const std::exception&) {
        row.result = nlohmann::json::object();
    }
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
    // C4: terminal rows are final, so only a pending or running row is finished; migration 025
    // allows pending -> running -> outcome only, so a pending row (refused before it was started)
    // is moved to running first, in the same transaction.
    auto updated = db.execute_direct_query(
        "UPDATE trading.position_overrides SET status = 'running', started_at = "
        "COALESCE(started_at, now()) WHERE id = " + std::to_string(id) +
        " AND status = 'pending'; UPDATE trading.position_overrides SET status = " +
        sql_literal(status) +
        ", result = COALESCE(result, '{}'::jsonb) || " + sql_literal(result.dump()) +
        "::jsonb, message = " + sql_literal(message) +
        ", started_at = COALESCE(started_at, now()), finished_at = now() WHERE id = " +
        std::to_string(id) + " AND status = 'running'");
    if (updated.is_error()) {
        return make_error<void>(updated.error()->code(), updated.error()->what(), "QtDesk");
    }
    if (updated.value() != 1) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "command row " + std::to_string(id) +
                                    " is no longer pending or running; its outcome (" + status +
                                    ") was not written",
                                "QtDesk");
    }
    return Result<void>();
}

namespace {

// SHA-256 (FIPS 180-4).
constexpr std::array<uint32_t, 64> kSha256K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline uint32_t byte_at(const std::string& s, std::size_t i) {
    return static_cast<uint32_t>(static_cast<unsigned char>(s[i]));
}

// Python's json encoder with ensure_ascii=True (py_encode_basestring_ascii): the two-character
// escapes for " \ \n \r \t \b \f, every other character outside ' '..'~' as \uXXXX (lower-case
// hex), a code point above U+FFFF as a surrogate pair. `text` is UTF-8.
void json_string(std::string& out, const std::string& text) {
    static const char* hex = "0123456789abcdef";
    auto u = [&](uint32_t cp) {
        out += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4) out += hex[(cp >> shift) & 0xF];
    };
    out += '"';
    for (std::size_t i = 0; i < text.size();) {
        const uint32_t c = byte_at(text, i);
        uint32_t cp = c;
        std::size_t len = 1;
        if (c >= 0xF0 && i + 3 < text.size()) {
            cp = ((c & 0x07u) << 18) | ((byte_at(text, i + 1) & 0x3Fu) << 12) |
                 ((byte_at(text, i + 2) & 0x3Fu) << 6) | (byte_at(text, i + 3) & 0x3Fu);
            len = 4;
        } else if (c >= 0xE0 && i + 2 < text.size()) {
            cp = ((c & 0x0Fu) << 12) | ((byte_at(text, i + 1) & 0x3Fu) << 6) |
                 (byte_at(text, i + 2) & 0x3Fu);
            len = 3;
        } else if (c >= 0xC0 && i + 1 < text.size()) {
            cp = ((c & 0x1Fu) << 6) | (byte_at(text, i + 1) & 0x3Fu);
            len = 2;
        }
        i += len;
        switch (cp) {
            case '"': out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '\n': out += "\\n"; continue;
            case '\r': out += "\\r"; continue;
            case '\t': out += "\\t"; continue;
            case '\b': out += "\\b"; continue;
            case '\f': out += "\\f"; continue;
            default: break;
        }
        if (cp >= 0x20 && cp <= 0x7E) {
            out += static_cast<char>(cp);
        } else if (cp < 0x10000) {
            u(cp);
        } else {
            const uint32_t v = cp - 0x10000;
            u(0xD800 | (v >> 10));
            u(0xDC00 | (v & 0x3FF));
        }
    }
    out += '"';
}

// AlgoLens sorts by the full (strategy_name, symbol, quantity) tuple: two rows can share
// (strategy_name, symbol) across strategy_ids, and the quantity breaks the tie.
bool proposal_order(const ProposalRow& a, const ProposalRow& b) {
    if (a.strategy_name != b.strategy_name) return a.strategy_name < b.strategy_name;
    if (a.symbol != b.symbol) return a.symbol < b.symbol;
    return a.quantity < b.quantity;
}

}  // namespace

std::string proposal_snapshot_json(const std::vector<ProposalRow>& rows) {
    std::string out = "[";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) out += ',';
        out += "{\"strategy_name\":";
        json_string(out, rows[i].strategy_name);
        out += ",\"symbol\":";
        json_string(out, rows[i].symbol);
        out += ",\"quantity\":" + std::to_string(rows[i].quantity) + "}";
    }
    out += "]";
    return out;
}

std::string sha256_hex(const std::string& bytes) {
    std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg = bytes;
    const uint64_t bit_length = static_cast<uint64_t>(bytes.size()) * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 64 != 56) msg += static_cast<char>(0x00);
    for (int shift = 56; shift >= 0; shift -= 8) {
        msg += static_cast<char>((bit_length >> shift) & 0xFF);
    }
    std::array<uint32_t, 64> w{};
    for (std::size_t block = 0; block < msg.size(); block += 64) {
        for (std::size_t t = 0; t < 16; ++t) {
            const std::size_t i = block + 4 * t;
            w[t] = (byte_at(msg, i) << 24) | (byte_at(msg, i + 1) << 16) |
                   (byte_at(msg, i + 2) << 8) | byte_at(msg, i + 3);
        }
        for (std::size_t t = 16; t < 64; ++t) {
            const uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (std::size_t t = 0; t < 64; ++t) {
            const uint32_t big_s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = k + big_s1 + ch + kSha256K[t] + w[t];
            const uint32_t big_s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = big_s0 + maj;
            k = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += k;
    }
    std::ostringstream hex;
    for (uint32_t word : h) hex << std::hex << std::setw(8) << std::setfill('0') << word;
    return hex.str();
}

std::string proposal_sha256(std::vector<ProposalRow> rows) {
    std::sort(rows.begin(), rows.end(), proposal_order);
    return sha256_hex(proposal_snapshot_json(rows));
}

Result<std::vector<ProposalRow>> load_proposal_rows(PostgresDatabase& db,
                                                    const std::string& portfolio_id,
                                                    const std::string& date) {
    using Out = std::vector<ProposalRow>;
    if (!valid_date(date)) {
        return make_error<Out>(ErrorCode::INVALID_ARGUMENT, "bad date '" + date + "'", "QtDesk");
    }
    auto rows = query_rows(
        db, "SELECT strategy_name, symbol, trunc(quantity)::bigint::text, "
            "(quantity = trunc(quantity))::text, quantity::text FROM trading.positions "
            "WHERE portfolio_id = " + sql_literal(portfolio_id) + " AND date = " +
                sql_literal(date) + "::date AND portfolio_type = 'qt_proposal'");
    if (rows.is_error()) {
        return make_error<Out>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    Out out;
    for (const auto& r : rows.value()) {
        if (cell(r[3]) != "true") {
            return make_error<Out>(ErrorCode::INVALID_DATA,
                                   "qt_proposal quantity " + cell(r[4]) + " of " + cell(r[1]) +
                                       " is not a whole number of contracts",
                                   "QtDesk");
        }
        out.push_back({cell(r[0]), cell(r[1]), std::stoll(cell(r[2]))});
    }
    std::sort(out.begin(), out.end(), proposal_order);
    return Result<Out>(out);
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
    if (request.status != "done") {
        return refuse("the override request " + std::to_string(request.id) + " is " +
                      request.status + ", not done (it was never e-mailed)");
    }
    // C2: one decision per request.
    auto decided = query_rows(
        db, "SELECT COALESCE(string_agg(id::text, ', ' ORDER BY id), '') FROM "
            "trading.position_overrides WHERE kind = 'override_decision' AND status = 'done' "
            "AND parent_id = " + std::to_string(request.id) +
                " AND id <> " + std::to_string(decision.id));
    if (decided.is_error()) return refuse(decided.error()->what());
    if (!decided.value().empty() && !cell(decided.value()[0][0]).empty()) {
        return refuse("the override request " + std::to_string(request.id) +
                      " was already decided (decision " + cell(decided.value()[0][0]) + ")");
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
    // C1: a request without a snapshot predates the rule; any decision on it is refused and the
    // desk requests again.
    const auto& p = request.payload;
    if (!p.is_object() || !p.contains("proposal_sha256") || !p.at("proposal_sha256").is_string() ||
        !p.contains("proposal") || !p.at("proposal").is_array()) {
        return refuse("the override request " + std::to_string(request.id) +
                      " carries no proposal snapshot (it predates the snapshot rule); "
                      "request a new override");
    }
    if (approved) {
        // The approver approved the snapshot in the request; the book booked is the current
        // qt_proposal, so the two must be the same rows.
        auto current = load_proposal_rows(db, decision.portfolio_id, decision.date);
        if (current.is_error()) return refuse(current.error()->what());
        if (proposal_sha256(current.value()) !=
            lower(trim(p.at("proposal_sha256").get<std::string>()))) {
            return refuse(kProposalChanged);
        }
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

Result<std::unordered_map<std::string, std::vector<ExecutionReport>>> load_book_executions(
    PostgresDatabase& db, const std::string& portfolio_id, const std::string& strategy_id,
    const std::string& book, const std::string& date) {
    using Out = std::unordered_map<std::string, std::vector<ExecutionReport>>;
    if (auto b = check_book(book); b.is_error()) {
        return make_error<Out>(b.error()->code(), b.error()->what(), "QtDesk");
    }
    auto rows = query_rows(
        db,
        "SELECT COALESCE(strategy_name, ''), exec_id, order_id, symbol, side, quantity::text, "
        "price::text, (extract(epoch FROM execution_time) * 1000000)::bigint::text, "
        "COALESCE(commissions_fees, 0)::text, COALESCE(implicit_price_impact, 0)::text, "
        "COALESCE(slippage_market_impact, 0)::text, COALESCE(total_transaction_costs, 0)::text, "
        "COALESCE(netting_adjustment, 0)::text, COALESCE(is_partial, false)::text, "
        "COALESCE(execution_type, 'STRATEGY'), COALESCE(instrument_id, '') "
        "FROM trading.executions WHERE portfolio_id = " + sql_literal(portfolio_id) +
            " AND strategy_id = " + sql_literal(strategy_id) + " AND portfolio_type = " +
            sql_literal(book) + " AND date = " + sql_literal(date) +
            "::date ORDER BY strategy_name, exec_id, order_id");
    if (rows.is_error()) {
        return make_error<Out>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    auto num = [](const Cell& c) { return c && !c->empty() ? std::stod(*c) : 0.0; };
    Out out;
    for (const auto& r : rows.value()) {
        ExecutionReport e;
        e.exec_id = cell(r[1]);
        e.order_id = cell(r[2]);
        e.symbol = cell(r[3]);
        const std::string side = cell(r[4]);
        e.side = side == "BUY" ? Side::BUY : (side == "SELL" ? Side::SELL : Side::NONE);
        e.filled_quantity = Quantity(num(r[5]));
        e.fill_price = Price(num(r[6]));
        e.fill_time = Timestamp(std::chrono::duration_cast<Timestamp::duration>(
            std::chrono::microseconds(r[7] && !r[7]->empty() ? std::stoll(*r[7]) : 0LL)));
        e.commissions_fees = Decimal(num(r[8]));
        e.implicit_price_impact = Decimal(num(r[9]));
        e.slippage_market_impact = Decimal(num(r[10]));
        e.total_transaction_costs = Decimal(num(r[11]));
        e.netting_adjustment = Decimal(num(r[12]));
        e.is_partial = cell(r[13]) == "true";
        const std::string type = cell(r[14]);
        e.execution_type = type == "ROLL"     ? ExecutionType::ROLL
                           : type == "BORROW" ? ExecutionType::BORROW
                                              : ExecutionType::STRATEGY;
        e.instrument_id = cell(r[15]);
        out[cell(r[0])].push_back(std::move(e));
    }
    return Result<Out>(out);
}

Result<nlohmann::json> load_book_results(PostgresDatabase& db, const std::string& portfolio_id,
                                         const std::string& strategy_id, const std::string& book,
                                         const std::string& date) {
    if (auto b = check_book(book); b.is_error()) {
        return make_error<nlohmann::json>(b.error()->code(), b.error()->what(), "QtDesk");
    }
    auto rows = query_rows(
        db, "SELECT (to_jsonb(r) - 'config' - 'risk_detail')::text FROM trading.live_results r "
            "WHERE portfolio_id = " + sql_literal(portfolio_id) + " AND strategy_id = " +
                sql_literal(strategy_id) + " AND portfolio_type = " + sql_literal(book) +
                " AND date = " + sql_literal(date) + "::date");
    if (rows.is_error()) {
        return make_error<nlohmann::json>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    if (rows.value().size() != 1) {
        return make_error<nlohmann::json>(ErrorCode::DATA_NOT_FOUND,
                                          "expected one " + book + " live_results row for " +
                                              portfolio_id + " on " + date + ", found " +
                                              std::to_string(rows.value().size()),
                                          "QtDesk");
    }
    try {
        return Result<nlohmann::json>(nlohmann::json::parse(cell(rows.value()[0][0])));
    } catch (const std::exception& e) {
        return make_error<nlohmann::json>(ErrorCode::INVALID_DATA, e.what(), "QtDesk");
    }
}

Result<void> clear_desk_day(PostgresDatabase& db, const std::string& portfolio_id,
                            const std::string& strategy_id, const std::string& date) {
    if (!valid_date(date)) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "bad date '" + date + "'", "QtDesk");
    }
    const std::string where = " WHERE portfolio_id = " + sql_literal(portfolio_id) +
                              " AND strategy_id = " + sql_literal(strategy_id) + " AND date = " +
                              sql_literal(date) + "::date AND portfolio_type = 'qt'";
    return exec(db, "UPDATE trading.live_results SET book_source = NULL" + where +
                        "; DELETE FROM trading.executions" + where +
                        " AND execution_type IS DISTINCT FROM 'ROLL'");
}

Result<std::string> publish_blocker(PostgresDatabase& db, const std::string& portfolio_id,
                                    const std::string& strategy_id, const std::string& date) {
    auto marker = query_rows(
        db, "SELECT COALESCE(book_source, '') FROM trading.live_results WHERE portfolio_id = " +
                sql_literal(portfolio_id) + " AND strategy_id = " + sql_literal(strategy_id) +
                " AND date = " + sql_literal(date) + "::date AND portfolio_type = 'qt'");
    if (marker.is_error()) {
        return make_error<std::string>(marker.error()->code(), marker.error()->what(), "QtDesk");
    }
    if (marker.value().empty()) {
        return Result<std::string>("no qt book for " + portfolio_id + " on " + date);
    }
    if (cell(marker.value()[0][0]).empty()) {
        return Result<std::string>("the qt book of " + date +
                                   " is incomplete (a desk or override run did not finish "
                                   "writing it, live_results.book_source is not set); save again");
    }
    auto last = query_rows(
        db, "SELECT id::text, kind, status FROM trading.position_overrides WHERE portfolio_id = " +
                sql_literal(portfolio_id) + " AND date = " + sql_literal(date) +
                "::date AND kind IN ('save', 'override_decision') ORDER BY id DESC LIMIT 1");
    if (last.is_error()) {
        return make_error<std::string>(last.error()->code(), last.error()->what(), "QtDesk");
    }
    if (!last.value().empty()) {
        const auto& r = last.value().front();
        const std::string status = cell(r[2]);
        if (status != "done" && status != "refused") {
            return Result<std::string>("the last desk command of " + date + " (" + cell(r[1]) +
                                       " " + cell(r[0]) + ") is " + status +
                                       ", not done: the qt book may not be the desk's");
        }
    }
    return Result<std::string>(std::string());
}

Result<EmailSent> publish_email_sent(PostgresDatabase& db, const std::string& portfolio_id,
                                     const std::string& date) {
    auto rows = query_rows(
        db, "SELECT id::text, result->>'email_sent_at' FROM trading.position_overrides WHERE "
            "kind = 'publish' AND portfolio_id = " + sql_literal(portfolio_id) + " AND date = " +
                sql_literal(date) +
                "::date AND result ? 'email_sent_at' ORDER BY id LIMIT 1");
    if (rows.is_error()) {
        return make_error<EmailSent>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    EmailSent sent;
    if (!rows.value().empty()) {
        sent.row_id = std::stol(cell(rows.value()[0][0]));
        sent.at = cell(rows.value()[0][1]);
    }
    return Result<EmailSent>(sent);
}

Result<std::string> mark_email_sent(PostgresDatabase& db, long id) {
    auto rows = query_rows(
        db, "UPDATE trading.position_overrides SET result = COALESCE(result, '{}'::jsonb) || "
            "jsonb_build_object('email_sent_at', now()::text) WHERE id = " + std::to_string(id) +
                " AND kind = 'publish' AND status IN ('pending', 'running') "
                "RETURNING result->>'email_sent_at'");
    if (rows.is_error()) {
        return make_error<std::string>(rows.error()->code(), rows.error()->what(), "QtDesk");
    }
    if (rows.value().empty()) {
        return make_error<std::string>(ErrorCode::DATABASE_ERROR,
                                       "publish row " + std::to_string(id) +
                                           " is not pending or running; email_sent_at not written",
                                       "QtDesk");
    }
    return Result<std::string>(cell(rows.value()[0][0]));
}

DayKind classify_qt_day(int t1_weekday, bool t1_holiday, bool whole_book_carried) {
    if (t1_weekday == 0 || t1_weekday == 6 || t1_holiday) return DayKind::CALENDAR_CLOSED;
    return whole_book_carried ? DayKind::FEED_HOLE : DayKind::TRADING;
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

namespace {
// A missing 027 column names the migration in the error.
std::string with_027_hint(const std::string& what) {
    if (what.find("publish_source") != std::string::npos ||
        what.find("sent_at") != std::string::npos) {
        return what + " (trading.live_run_metadata needs migration 027)";
    }
    return what;
}

std::string metadata_where(const std::string& portfolio_id, const std::string& strategy_id,
                           const std::string& date) {
    return " WHERE portfolio_id = " + sql_literal(portfolio_id) + " AND strategy_id = " +
           sql_literal(strategy_id) + " AND date = " + sql_literal(date) + "::date";
}
}  // namespace

Result<PublishState> publish_state(PostgresDatabase& db, const std::string& portfolio_id,
                                   const std::string& strategy_id, const std::string& date) {
    if (!valid_date(date)) {
        return make_error<PublishState>(ErrorCode::INVALID_ARGUMENT, "bad date '" + date + "'",
                                        "QtDesk");
    }
    auto rows = query_rows(
        db, "SELECT COALESCE(published_at::text, ''), COALESCE(published_by, ''), "
            "COALESCE(publish_source, ''), COALESCE(sent_at::text, '') FROM "
            "trading.live_run_metadata" + metadata_where(portfolio_id, strategy_id, date));
    if (rows.is_error()) {
        return make_error<PublishState>(rows.error()->code(),
                                        with_027_hint(rows.error()->what()), "QtDesk");
    }
    PublishState state;
    if (!rows.value().empty()) {
        const auto& r = rows.value().front();
        state.published_at = cell(r[0]);
        state.published_by = cell(r[1]);
        state.publish_source = cell(r[2]);
        state.sent_at = cell(r[3]);
    }
    return Result<PublishState>(state);
}

Result<std::string> approve_day(PostgresDatabase& db, long row_id, const std::string& portfolio_id,
                                const std::string& strategy_id, const std::string& date,
                                const std::string& published_by, const std::string& source) {
    if (!valid_date(date)) {
        return make_error<std::string>(ErrorCode::INVALID_ARGUMENT, "bad date '" + date + "'",
                                       "QtDesk");
    }
    const std::string id = std::to_string(row_id);
    const std::string open_row = "(SELECT 1 FROM trading.position_overrides WHERE id = " + id +
                                 " AND status IN ('pending', 'running'))";
    auto rows = query_rows(
        db, "WITH m AS (UPDATE trading.live_run_metadata SET published_by = " +
                sql_literal(published_by) + ", published_at = now(), publish_source = " +
                sql_literal(source) + metadata_where(portfolio_id, strategy_id, date) +
                " AND published_at IS NULL" +
                (row_id > 0 ? " AND EXISTS " + open_row : std::string()) +
                " RETURNING published_at), r AS (UPDATE trading.position_overrides SET result = "
                "COALESCE(result, '{}'::jsonb) || jsonb_build_object('published_at', "
                "(SELECT max(published_at) FROM m)::text, 'publish_source', " +
                sql_literal(source) + ") WHERE id = " + id +
                " AND status IN ('pending', 'running') AND EXISTS (SELECT 1 FROM m) RETURNING id) "
                "SELECT (SELECT count(*) FROM m)::text, COALESCE((SELECT max(published_at) FROM "
                "m)::text, ''), (SELECT count(*) FROM r)::text");
    if (rows.is_error()) {
        return make_error<std::string>(rows.error()->code(), with_027_hint(rows.error()->what()),
                                       "QtDesk");
    }
    if (rows.value().empty() || cell(rows.value()[0][0]) != "1") {
        return make_error<std::string>(
            ErrorCode::DATABASE_ERROR,
            portfolio_id + " " + date +
                " was not approved: its live_run_metadata row is missing or already published" +
                (row_id > 0 ? ", or command row " + id + " is not open" : std::string()),
            "QtDesk");
    }
    return Result<std::string>(cell(rows.value()[0][1]));
}

Result<std::string> record_sent(PostgresDatabase& db, long row_id, const std::string& portfolio_id,
                                const std::string& strategy_id, const std::string& date) {
    if (!valid_date(date)) {
        return make_error<std::string>(ErrorCode::INVALID_ARGUMENT, "bad date '" + date + "'",
                                       "QtDesk");
    }
    auto rows = query_rows(
        db, "WITH m AS (UPDATE trading.live_run_metadata SET sent_at = now()" +
                metadata_where(portfolio_id, strategy_id, date) +
                " AND sent_at IS NULL RETURNING sent_at), r AS (UPDATE trading.position_overrides "
                "SET result = COALESCE(result, '{}'::jsonb) || jsonb_build_object('email_sent_at', "
                "now()::text) WHERE id = " + std::to_string(row_id) +
                " AND kind = 'publish' AND status IN ('pending', 'running') RETURNING id) "
                "SELECT COALESCE((SELECT max(sent_at) FROM m)::text, ''), "
                "(SELECT count(*) FROM r)::text");
    if (rows.is_error()) {
        return make_error<std::string>(rows.error()->code(), with_027_hint(rows.error()->what()),
                                       "QtDesk");
    }
    std::string at = rows.value().empty() ? std::string() : cell(rows.value()[0][0]);
    if (at.empty()) {
        // Already recorded (the caller checks sent_at before it sends), or no metadata row.
        auto state = publish_state(db, portfolio_id, strategy_id, date);
        if (state.is_error()) {
            return make_error<std::string>(state.error()->code(), state.error()->what(), "QtDesk");
        }
        if (state.value().sent_at.empty()) {
            return make_error<std::string>(ErrorCode::DATABASE_ERROR,
                                           "no live_run_metadata row for " + portfolio_id +
                                               " on " + date + "; sent_at not written",
                                           "QtDesk");
        }
        at = state.value().sent_at;
    }
    return Result<std::string>(at);
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
