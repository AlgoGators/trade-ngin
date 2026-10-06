#include "trade_ngin/data/live_config_selection.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/strategy/equity_strategy_builder.hpp"
#include <stdexcept>

namespace trade_ngin {
namespace {
using Json=nlohmann::json;
void require(bool condition) {
    if (!condition) throw std::runtime_error("live_config_selection_refused");
}
std::string engine_identity(const AppConfig& config) {
    auto selected=select_enabled_live_strategies(config.strategies_config);
    require(selected.is_ok());
    bool equity=false, futures=false;
    for (const auto& name:selected.value().names) {
        const auto type=config.strategies_config.at(name).value("type",std::string{});
        if (type=="MeanReversionStrategy") equity=true;
        else futures=true;
    }
    require(!(equity && futures));
    if (equity) {
        const auto entries=apps::collect_enabled_equity_strategies(config.strategies_config,"enabled_live");
        require(entries.is_ok());
        const auto plan=apps::build_equity_live_book_plan(entries.value());
        require(plan.is_ok());
        return plan.value().combined_strategy_id;
    }
    return build_combined_strategy_id(selected.value().names);
}
Json lock_scope(pqxx::work& txn,const std::string& engine,const std::string& book) {
    const auto rows=txn.exec("SELECT trading.lock_live_config_scope($1,$2)",pqxx::params{engine,book});
    require(rows.size()==1 && !rows[0][0].is_null());
    return Json::parse(rows[0][0].as<std::string>());
}
pqxx::result active_version(pqxx::work& txn,const Json& scope) {
    // Query even investor scopes: missing storage/permissions can never masquerade
    // as a successful file-only lookup. Inner joins are not used: corrupt missing
    // audit/version rows must refuse rather than disappear into a no-row result.
    return txn.exec("SELECT a.version_id::text,v.registry_id,v.registry_revision,v.validator_build,"
        "v.base_sha256,v.effective_sha256,v.changes::text,v.effective_snapshot::text,"
        "v.portfolio_id,v.engine_strategy_id,x.version_id::text,v.validator_sha256,v.operation "
        "FROM trading.live_config_active a LEFT JOIN trading.live_config_versions v ON v.version_id=a.version_id "
        "LEFT JOIN trading.live_config_activations x ON x.version_id=a.version_id "
        "WHERE a.portfolio_id=$1 AND a.engine_strategy_id=$2",
        pqxx::params{scope.at("portfolio_id").get<std::string>(),scope.at("engine_strategy_id").get<std::string>()});
}
void check_version_source(const pqxx::row& row,const Json& scope,const std::string& build) {
    for (const auto& field:row) require(!field.is_null());
    require(scope.at("investor_book_id").is_null()); // No controlled investor approval contract yet.
    require(row[1].as<std::string>()==scope.at("registry_id").get<std::string>() &&
        row[2].as<long long>()==scope.at("registry_revision").get<long long>() &&
        row[3].as<std::string>()==build &&
        row[8].as<std::string>()==scope.at("portfolio_id").get<std::string>() &&
        row[9].as<std::string>()==scope.at("engine_strategy_id").get<std::string>() &&
        row[0].as<std::string>()==row[10].as<std::string>() && row[11].as<std::string>().size()==64);
}
}

Result<ConfigSelection> select_live_configuration(PostgresDatabase& db,const AppConfig& base,
                                                  const std::string& engine_build) {
    if (db.validate_connection().is_error())
        return make_error<ConfigSelection>(ErrorCode::DATABASE_ERROR,"live_config_selection_refused");
    try {
        require(!engine_build.empty());
        auto base_snapshot=build_runtime_trading_snapshot(base);
        auto base_hash=live_config_snapshot_sha256(base); // Preserve legacy unnormalized file weights.
        require(base_snapshot.is_ok() && base_hash.is_ok());
        pqxx::work txn(*db.connection_);
        auto scope=lock_scope(txn,engine_identity(base),base.portfolio_id);
        auto rows=active_version(txn,scope);
        require(rows.size()<=1);
        ConfigSelection result{base,{{"schema","live-config-selection/v1"},{"source","file"},
            {"version_id",nullptr},{"base_sha256",base_hash.value()},
            {"effective_sha256",base_hash.value()},{"scope",scope},{"engine_build",engine_build}}};
        if (!rows.empty()) {
            const auto row=rows[0];
            check_version_source(row,scope,engine_build);
            auto changes=Json::parse(row[6].as<std::string>());
            auto approved=Json::parse(row[7].as<std::string>());
            const auto operation=row[12].as<std::string>();
            require(operation=="override" || operation=="reset_to_baseline");
            const bool reset=operation=="reset_to_baseline";
            require(!reset || (changes.is_object() && changes.empty()));
            auto validated=reset
                ? validate_live_config_baseline_request({{"schema","live-config-baseline-validation/v1"},
                    {"base_snapshot",base_snapshot.value()}})
                : validate_live_config_request({{"schema","live-config-validation/v1"},
                    {"base_snapshot",base_snapshot.value()},{"changes",changes}});
            require(validated.is_ok());
            const auto& output=validated.value();
            require(output.at("base_sha256")==row[4].as<std::string>() &&
                output.at("effective_sha256")==row[5].as<std::string>() &&
                output.at("effective_snapshot")==approved);
            auto approved_hash=live_config_snapshot_sha256(approved);
            require(approved_hash.is_ok() && approved_hash.value()==row[5].as<std::string>());
            if (!reset) {
                auto effective=apply_live_config_override(base,changes);
                require(effective.is_ok());
                result.config=std::move(effective.value());
            } // An approved reset returns the validated baseline with private credentials intact.
            result.receipt["source"]="approved_override";
            result.receipt["version_id"]=row[0].as<std::string>();
            result.receipt["base_sha256"]=output.at("base_sha256");
            result.receipt["effective_sha256"]=output.at("effective_sha256");
        }
        txn.commit();
        return result;
    } catch(const std::exception&) {
        return make_error<ConfigSelection>(ErrorCode::DATABASE_ERROR,"live_config_selection_refused");
    }
}

void PostgresDatabase::admit_live_config_selection(pqxx::work& txn,PendingPublication& pending,
                                                  const Json& receipt,bool controlled) {
    require(receipt.is_object() && receipt.size()==7 &&
        receipt.at("schema")=="live-config-selection/v1" && receipt.at("engine_build")==pending.producer_version);
    const auto scope=lock_scope(txn,pending.strategy_id,pending.portfolio_id);
    require(scope==receipt.at("scope"));
    const auto rows=active_version(txn,scope);
    require(rows.size()<=1);
    if (rows.empty()) {
        require(receipt.at("source")=="file" && receipt.at("version_id").is_null() &&
            receipt.at("base_sha256")==receipt.at("effective_sha256"));
        // The typed hash path intentionally permits legacy nonnormalized weights.
        auto parsed=ConfigLoader::parse_trading_config(pending.snapshot);
        require(parsed.is_ok());
        auto snapshot=build_runtime_trading_snapshot(parsed.value());
        auto hash=live_config_snapshot_sha256(parsed.value());
        require(snapshot.is_ok() && snapshot.value()==pending.snapshot && hash.is_ok() &&
            receipt.at("effective_sha256")==hash.value());
    } else {
        require(controlled && pending.mode!=LivePublicationMode::SystemInvestor && pending.intent_id!=0);
        const auto row=rows[0];
        check_version_source(row,scope,pending.producer_version);
        require(receipt.at("source")=="approved_override" && receipt.at("version_id")==row[0].as<std::string>() &&
            receipt.at("base_sha256")==row[4].as<std::string>() &&
            receipt.at("effective_sha256")==row[5].as<std::string>() &&
            pending.snapshot==Json::parse(row[7].as<std::string>()));
        auto hash=live_config_snapshot_sha256(pending.snapshot);
        require(hash.is_ok() && hash.value()==row[5].as<std::string>());
    }
    pending.config_attempt_id=pending.attempt_id.empty()
        ? txn.exec("SELECT gen_random_uuid()::text")[0][0].as<std::string>() : pending.attempt_id;
    pending.configuration_selection=receipt;
    txn.exec("INSERT INTO trading.live_config_attempt_selections "
        "(attempt_id,portfolio_id,engine_strategy_id,run_date,engine_build,selection,config_snapshot) "
        "VALUES($1,$2,$3,$4::date,$5,$6::jsonb,$7::jsonb)",
        pqxx::params{pending.config_attempt_id,pending.portfolio_id,pending.strategy_id,pending.date,
                     pending.producer_version,receipt.dump(),pending.snapshot.dump()});
    txn.exec("INSERT INTO trading.live_config_attempt_safety(attempt_id) VALUES($1)",
             pqxx::params{pending.config_attempt_id});
}
} // namespace trade_ngin
