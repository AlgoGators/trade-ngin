// src/data/postgres_strategy_config.cpp
//
// QT plan E2 (migration 022): the read of a portfolio's active trading.strategy_config row and
// the write of a live run's settings used into trading.live_run_metadata.settings_used.

#include <string>

#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/data/postgres_database.hpp"

namespace trade_ngin {

Result<std::optional<StrategyConfigRow>> PostgresDatabase::get_active_strategy_config(
    const std::string& portfolio_id) {
    using Out = std::optional<StrategyConfigRow>;
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto v = validate_connection(); v.is_error()) {
        return make_error<Out>(v.error()->code(), v.error()->what(), component_id_);
    }
    if (portfolio_id.empty()) {
        return make_error<Out>(ErrorCode::INVALID_ARGUMENT,
                               "strategy_config lookup needs a portfolio_id", component_id_);
    }
    try {
        pqxx::read_transaction txn(*connection_);
        const pqxx::result r = txn.exec(
            "SELECT version, overrides::text, reason, created_by FROM trading.strategy_config "
            "WHERE portfolio_id = $1 AND is_active",
            pqxx::params{portfolio_id});
        if (r.empty()) return Out{};
        if (r.size() > 1) {
            // The partial unique index makes this impossible; a database without it is not
            // the one migration 022 built, and no row of it can be trusted to be "the" one.
            return make_error<Out>(ErrorCode::DATABASE_ERROR,
                                   std::to_string(r.size()) +
                                       " active strategy_config rows for " + portfolio_id +
                                       " (migration 022 allows one)",
                                   component_id_);
        }
        StrategyConfigRow row;
        row.portfolio_id = portfolio_id;
        row.version = r[0][0].as<int>();
        row.overrides = nlohmann::json::parse(r[0][1].as<std::string>());
        row.reason = r[0][2].is_null() ? std::string() : r[0][2].as<std::string>();
        row.created_by = r[0][3].is_null() ? std::string() : r[0][3].as<std::string>();
        return Out{std::move(row)};
    } catch (const std::exception& e) {
        return make_error<Out>(ErrorCode::DATABASE_ERROR,
                               "strategy_config lookup for " + portfolio_id +
                                   " failed: " + std::string(e.what()),
                               component_id_);
    }
}

Result<void> PostgresDatabase::store_settings_used(const Timestamp& date,
                                                   const std::string& strategy_id,
                                                   const std::string& portfolio_id,
                                                   const nlohmann::json& settings_used) {
    std::string secret;
    if (ConfigLoader::find_secret_key(settings_used, "", &secret)) {
        // Only the key's path is reported, never a value.
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "settings_used not written: key " + secret +
                                    " looks like a credential",
                                component_id_);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto v = validate_connection(); v.is_error()) return v;
    try {
        pqxx::work txn(*connection_);
        // The same date text store_live_run_metadata writes, so the UPDATE finds its row.
        const std::string date_str = format_timestamp(date).substr(0, 10);
        const pqxx::result r = txn.exec(
            "UPDATE trading.live_run_metadata SET settings_used = $4::jsonb "
            "WHERE date = $1::date AND strategy_id = $2 AND portfolio_id = $3",
            pqxx::params{date_str, strategy_id, portfolio_id, settings_used.dump()});
        if (r.affected_rows() != 1) {
            return make_error<void>(ErrorCode::DATABASE_ERROR,
                                    "settings_used not written: " +
                                        std::to_string(r.affected_rows()) +
                                        " live_run_metadata rows for (" + date_str + ", " +
                                        strategy_id + ", " + portfolio_id + "), expected 1",
                                    component_id_);
        }
        txn.commit();
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "settings_used not written: " + std::string(e.what()),
                                component_id_);
    }
}

}  // namespace trade_ngin
