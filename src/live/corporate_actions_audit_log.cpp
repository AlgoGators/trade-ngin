#include "trade_ngin/live/corporate_actions_audit_log.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

namespace {

CorpActionType parse_type(const std::string& s) {
    if (s == "SPLIT") return CorpActionType::SPLIT;
    if (s == "ADR_SPLIT") return CorpActionType::ADR_SPLIT;
    if (s == "DIVIDEND") return CorpActionType::DIVIDEND;
    return CorpActionType::UNKNOWN;
}

}  // namespace

CorporateActionsAuditLog::CorporateActionsAuditLog(std::string state_dir)
    : state_dir_(std::move(state_dir)) {}

std::string CorporateActionsAuditLog::file_path() const {
    return state_dir_ + "/applied_corp_actions.json";
}

bool CorporateActionsAuditLog::load() {
    applied_.clear();
    dividend_events_.clear();

    const std::string path = file_path();
    std::ifstream f(path);
    if (!f.is_open()) {
        // First run for this strategy. Not an error.
        return false;
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
                    parse_type(entry["action"].get<std::string>()));
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
        return false;
    }
    return true;
}

bool CorporateActionsAuditLog::is_applied(const std::string& symbol,
                                          const std::string& ex_date,
                                          CorpActionType action) const {
    return applied_.find(AppliedKey{symbol, ex_date, action}) != applied_.end();
}

void CorporateActionsAuditLog::record(const PositionAdjustment& adj) {
    applied_.emplace(adj.symbol, adj.event_date, adj.type);

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

bool CorporateActionsAuditLog::save() const {
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
