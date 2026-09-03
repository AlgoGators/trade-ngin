#include "trade_ngin/live/corporate_actions_audit_log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

CorporateActionsAuditLog::CorporateActionsAuditLog(std::string state_dir)
    : state_dir_(std::move(state_dir)) {}

CorporateActionsAuditLog::CorporateActionsAuditLog(std::string state_dir,
                                                   std::shared_ptr<PostgresDatabase> db,
                                                   std::string portfolio_id,
                                                   std::string strategy_id,
                                                   std::string strategy_name)
    : state_dir_(std::move(state_dir)),
      db_(std::move(db)),
      portfolio_id_(std::move(portfolio_id)),
      strategy_id_(std::move(strategy_id)),
      strategy_name_(std::move(strategy_name)) {}

bool CorporateActionsAuditLog::migrate_state_file_to_db() {
    // One-time import: only when the DB holds nothing for this strategy, so a
    // stale file can never resurrect events over a DB record that has since
    // moved on. Idempotent by that guard plus the table's ON CONFLICT.
    if (!db_backed()) return false;
    if (!std::filesystem::exists(file_path())) return false;

    CorporateActionsAuditLog file_log(state_dir_);
    auto file_loaded = file_log.load();
    // The file-only path never reports an error today (a corrupt file is
    // WARNed and treated as empty); handle it anyway so this stays correct if
    // that changes.
    if (file_loaded.is_error() || !file_loaded.value()) return false;
    if (file_log.applied_.empty()) return false;

    std::vector<PostgresDatabase::AppliedCorpActionRow> rows;
    rows.reserve(file_log.applied_.size());
    for (const auto& key : file_log.applied_) {
        PostgresDatabase::AppliedCorpActionRow r;
        r.symbol = std::get<0>(key);
        r.ex_date = std::get<1>(key);
        r.action_type = CorporateActionsApplier::type_to_string(std::get<2>(key));
        // Dividend detail attaches where the file carried it -- and ONLY to a
        // dividend. E2-F39 / BA-15: this used to match on (symbol, ex_date) alone,
        // so a SPLIT sharing an ex-date with a DIVIDEND inherited the dividend's
        // cash and cumulative dividend income counted it twice.
        if (const auto* de = dividend_detail_for(r.symbol, r.ex_date,
                                                 std::get<2>(key),
                                                 file_log.dividend_events_)) {
            r.qty_held = de->qty_held;
            r.dividend_per_share = de->dividend_per_share;
            r.total_cash = de->total_cash;
        }
        rows.push_back(std::move(r));
    }

    auto res = db_->store_applied_corp_actions(portfolio_id_, strategy_id_,
                                               strategy_name_, rows);
    if (res.is_error()) {
        ERROR("CorporateActionsAuditLog: legacy state-file import failed: " +
              std::string(res.error()->what()));
        return false;
    }

    INFO("CorporateActionsAuditLog: imported " + std::to_string(rows.size()) +
         " event(s) from legacy " + file_path() +
         " into trading.corp_action_applied; the file is now superseded and is "
         "left in place, not deleted");
    return true;
}

std::string CorporateActionsAuditLog::file_path() const {
    return state_dir_ + "/applied_corp_actions.json";
}

Result<bool> CorporateActionsAuditLog::load() {
    applied_.clear();
    dividend_events_.clear();
    pending_.clear();

    if (db_backed()) {
        // Import any legacy file BEFORE reading, so a first DB-backed run on a
        // host that still has the old state file does not re-apply its events.
        auto existing = db_->load_applied_corp_actions(portfolio_id_, strategy_id_, strategy_name_);
        if (existing.is_error()) {
            // Fail closed: an unreadable dedup record must not be mistaken for
            // "nothing applied yet", which would re-apply the whole window.
            // Returned as an error, not false, so the caller cannot conflate it
            // with a genuine first run and proceed.
            ERROR("CorporateActionsAuditLog: cannot read trading.corp_action_applied: " +
                  std::string(existing.error()->what()));
            return make_error<bool>(ErrorCode::DATABASE_ERROR,
                                    "cannot read trading.corp_action_applied: " +
                                        std::string(existing.error()->what()),
                                    "CorporateActionsAuditLog");
        }
        if (existing.value().empty() && migrate_state_file_to_db()) {
            existing = db_->load_applied_corp_actions(portfolio_id_, strategy_id_, strategy_name_);
            if (existing.is_error()) {
                ERROR("CorporateActionsAuditLog: re-read after import failed: " +
                      std::string(existing.error()->what()));
                return make_error<bool>(ErrorCode::DATABASE_ERROR,
                                        "re-read after legacy import failed: " +
                                            std::string(existing.error()->what()),
                                        "CorporateActionsAuditLog");
            }
        }

        for (const auto& r : existing.value()) {
            applied_.emplace(r.symbol, r.ex_date,
                             CorporateActionsApplier::type_from_type_string(r.action_type));
            if (r.total_cash != 0.0 || r.dividend_per_share != 0.0) {
                DividendEvent de;
                de.symbol = r.symbol;
                de.ex_date = r.ex_date;
                de.qty_held = r.qty_held;
                de.dividend_per_share = r.dividend_per_share;
                de.total_cash = r.total_cash;
                dividend_events_.push_back(std::move(de));
            }
        }
        // Rename bridge. Dedup rows are keyed to the symbol held when the event
        // was applied, but the vendor migrates a renamed symbol's ENTIRE history
        // to the new ticker -- verified in the live DB: AA carries 0 bars while
        // HWM carries 6,703 back to 2000-01-03, dividends included. So once
        // apply_renames re-keys a position, the same event resurfaces under the
        // new ticker, and a dedup keyed to the old one would not match: the split
        // would re-multiply the quantity, the dividend would re-rescale the cost
        // basis, permanently and compounding. Mirror every entry under its
        // current symbol so the check succeeds whichever side of a rename the
        // run is on. Read-side only -- no rows are rewritten, so this is
        // idempotent and cannot corrupt the record it protects.
        auto aliases = db_->get_ticker_aliases();
        if (aliases.is_error()) {
            // Fail closed for the same reason the read above does: a missing
            // alias map silently reopens the re-application path.
            ERROR("CorporateActionsAuditLog: cannot read ticker aliases: " +
                  std::string(aliases.error()->what()));
            return make_error<bool>(ErrorCode::DATABASE_ERROR,
                                    "cannot read ticker aliases for dedup rename bridge: " +
                                        std::string(aliases.error()->what()),
                                    "CorporateActionsAuditLog");
        }
        // effective_until is the date the rename took effect, so a ticker maps
        // to its successor only for events dated on or before it. Tickers ARE
        // reused: 33 historical_tickers in the live table carry two or more
        // successors (BBT -> BBT1 until 1998-12-10, and BBT -> TFC until
        // 2019-12-10). Keying a plain map on historical_ticker alone therefore
        // both picks an arbitrary winner and can mirror a later company's event
        // onto an unrelated symbol -- masking a genuine event that shares the
        // ex_date and action, which skips its adjustment and leaves the basis
        // wrong in the opposite direction. Resolve per event date instead.
        std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> renames;
        for (const auto& a : aliases.value()) {
            if (!a.historical_ticker.empty() && !a.current_symbol.empty() &&
                !a.effective_until.empty() && a.historical_ticker != a.current_symbol) {
                renames[a.historical_ticker].emplace_back(a.effective_until, a.current_symbol);
            }
        }
        // Ascending by date; ISO YYYY-MM-DD compares lexicographically.
        for (auto& entry : renames) {
            std::sort(entry.second.begin(), entry.second.end());
        }

        // The successor a symbol had at ex_date: the first rename on or after
        // that date. An event later than every rename belongs to whoever holds
        // the ticker now, not to the old company, so it maps nowhere.
        auto successor_at = [&renames](const std::string& sym,
                                       const std::string& ex_date) -> std::string {
            auto it = renames.find(sym);
            if (it == renames.end()) return {};
            for (const auto& [effective_until, current_symbol] : it->second) {
                if (ex_date <= effective_until) return current_symbol;
            }
            return {};
        };

        if (!renames.empty()) {
            std::vector<AppliedKey> mirrors;
            for (const auto& k : applied_) {
                // Follow the chain (A->B->C), bounded so a cyclic map cannot spin.
                std::string sym = std::get<0>(k);
                const std::string& ex_date = std::get<1>(k);
                for (int hop = 0; hop < 8; ++hop) {
                    std::string next = successor_at(sym, ex_date);
                    if (next.empty() || next == sym) break;
                    sym = next;
                    mirrors.emplace_back(sym, ex_date, std::get<2>(k));
                }
            }
            for (auto& m : mirrors) applied_.insert(std::move(m));
        }

        return Result<bool>(!existing.value().empty());
    }

    const std::string path = file_path();
    std::ifstream f(path);
    if (!f.is_open()) {
        // First run for this strategy. Not an error.
        return Result<bool>(false);
    }

    try {
        nlohmann::json j;
        f >> j;

        if (j.contains("applied") && j["applied"].is_array()) {
            for (const auto& entry : j["applied"]) {
                if (!entry.contains("symbol") || !entry.contains("ex_date") ||
                    !entry.contains("action")) {
                    continue;
                }
                applied_.emplace(
                    entry["symbol"].get<std::string>(),
                    entry["ex_date"].get<std::string>(),
                    CorporateActionsApplier::type_from_type_string(
                        entry["action"].get<std::string>()));
            }
        }
        if (j.contains("dividend_events") && j["dividend_events"].is_array()) {
            for (const auto& entry : j["dividend_events"]) {
                DividendEvent de;
                de.symbol = entry.value("symbol", "");
                de.ex_date = entry.value("ex_date", "");
                de.qty_held = entry.value("qty_held", 0.0);
                de.dividend_per_share = entry.value("dividend_per_share", 0.0);
                de.total_cash = entry.value("total_cash", 0.0);
                dividend_events_.push_back(std::move(de));
            }
        }
    } catch (const std::exception& e) {
        WARN("CorporateActionsAuditLog: failed to parse " + path + ": " + e.what() +
             " -- treating as empty");
        applied_.clear();
        dividend_events_.clear();
        return Result<bool>(false);
    }
    return Result<bool>(true);
}

bool CorporateActionsAuditLog::is_applied(const std::string& symbol,
                                          const std::string& ex_date,
                                          CorpActionType action) const {
    return applied_.find(AppliedKey{symbol, ex_date, action}) != applied_.end();
}

void CorporateActionsAuditLog::record(const PositionAdjustment& adj) {
    applied_.emplace(adj.symbol, adj.event_date, adj.type);

    if (db_backed()) {
        PostgresDatabase::AppliedCorpActionRow row;
        row.symbol = adj.symbol;
        row.ex_date = adj.event_date;
        row.action_type = CorporateActionsApplier::type_to_string(adj.type);
        if (adj.type == CorpActionType::DIVIDEND) {
            row.qty_held = adj.quantity_after;
            row.dividend_per_share = adj.event_value;
            row.total_cash = row.qty_held * row.dividend_per_share;
        }
        pending_.push_back(std::move(row));
    }

    if (adj.type == CorpActionType::DIVIDEND) {
        DividendEvent de;
        de.symbol = adj.symbol;
        de.ex_date = adj.event_date;
        de.qty_held = adj.quantity_after;  // unchanged for dividends
        de.dividend_per_share = adj.event_value;
        de.total_cash = de.qty_held * de.dividend_per_share;
        dividend_events_.push_back(std::move(de));
    }
}

double CorporateActionsAuditLog::total_cumulative_dividend_income() const {
    double sum = 0.0;
    for (const auto& de : dividend_events_) {
        sum += de.total_cash;
    }
    return sum;
}

Result<void> CorporateActionsAuditLog::save_in(DbTransaction& txn) const {
    if (!db_backed()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "save_in requires a DB-backed audit log: a state file cannot join a "
            "database transaction",
            "CorporateActionsAuditLog");
    }
    if (pending_.empty()) return Result<void>();

    auto res = db_->store_applied_corp_actions(txn, portfolio_id_, strategy_id_, strategy_name_,
                                               pending_);
    if (res.is_error()) {
        ERROR("CorporateActionsAuditLog: cannot persist dedup record: " +
              std::string(res.error()->what()));
        return res;
    }
    // Cleared only once the statements are in the transaction. If the caller's
    // commit later fails, the whole unit rolls back -- positions included -- so
    // there is nothing left to re-persist.
    pending_.clear();
    return Result<void>();
}

bool CorporateActionsAuditLog::save() const {
    if (db_backed()) {
        // Delta write. The natural key plus ON CONFLICT DO NOTHING makes a
        // repeated save harmless, so a retried run cannot double-record.
        if (pending_.empty()) return true;
        auto res = db_->store_applied_corp_actions(portfolio_id_, strategy_id_,
                                                   strategy_name_, pending_);
        if (res.is_error()) {
            ERROR("CorporateActionsAuditLog: cannot persist dedup record: " +
                  std::string(res.error()->what()));
            return false;
        }
        pending_.clear();
        return true;
    }

    try {
        std::filesystem::create_directories(state_dir_);
    } catch (const std::exception& e) {
        ERROR("CorporateActionsAuditLog: cannot create state dir " + state_dir_ +
              ": " + e.what());
        return false;
    }

    nlohmann::json out;
    out["applied"] = nlohmann::json::array();
    for (const auto& [sym, date, type] : applied_) {
        nlohmann::json entry;
        entry["symbol"] = sym;
        entry["ex_date"] = date;
        entry["action"] = CorporateActionsApplier::type_to_string(type);
        out["applied"].push_back(std::move(entry));
    }
    out["dividend_events"] = nlohmann::json::array();
    for (const auto& de : dividend_events_) {
        nlohmann::json entry;
        entry["symbol"] = de.symbol;
        entry["ex_date"] = de.ex_date;
        entry["qty_held"] = de.qty_held;
        entry["dividend_per_share"] = de.dividend_per_share;
        entry["total_cash"] = de.total_cash;
        out["dividend_events"].push_back(std::move(entry));
    }

    // Atomic write (ultrareview bug_003): writing directly to `path` leaves a
    // corrupt half-file if the process is killed mid-write, which makes the
    // next load() fail and silently zeroes the dedup state. Write to a temp
    // sibling, flush, then rename -- rename is atomic on the same filesystem,
    // so a reader either sees the old file or the new file but never a
    // partial one.
    const std::string path = file_path();
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream f(tmp_path, std::ios::out | std::ios::trunc);
        if (!f.is_open()) {
            ERROR("CorporateActionsAuditLog: cannot open " + tmp_path + " for writing");
            return false;
        }
        f << out.dump(2);
        f.flush();
        if (!f.good()) {
            ERROR("CorporateActionsAuditLog: write to " + tmp_path + " failed");
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        ERROR("CorporateActionsAuditLog: atomic rename " + tmp_path + " -> " +
              path + " failed: " + ec.message());
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

}  // namespace trade_ngin
