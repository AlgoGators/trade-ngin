#include "trade_ngin/data/postgres_database.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace trade_ngin {
namespace {
std::string canonical_book(std::string book) {
    const auto first = book.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) throw std::runtime_error("runtime_scope_unsupported");
    book = book.substr(first, book.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(book.begin(), book.end(), book.begin(), [](unsigned char c) { return std::toupper(c); });
    return book;
}
}

bool PostgresDatabase::defer_live_write(const Timestamp& date, std::function<Result<void>()> write, unsigned part) {
    if (!pending_publication_ || publication_transaction_ ||
        format_timestamp(date).substr(0,10) != pending_publication_->date) return false;
    pending_publication_->writes.push_back(std::move(write));
    pending_publication_->parts |= part;
    return true;
}

bool PostgresDatabase::defer_live_write(std::function<Result<void>()> write, unsigned part) {
    if (!pending_publication_ || publication_transaction_) return false;
    pending_publication_->writes.push_back(std::move(write));
    pending_publication_->parts |= part;
    return true;
}

void PostgresDatabase::fence_live_write(pqxx::work& txn, const std::string& strategy_id,
                                       const std::string& portfolio_id, const std::string& stream) {
    const auto book = canonical_book(portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id);
    if (pending_publication_ &&
        (pending_publication_->strategy_id != strategy_id || pending_publication_->portfolio_id != book))
        throw std::runtime_error("runtime_publication_scope_mismatch");
    if (stream == "benchmark" || stream == "benchmark_rebench" || stream == "benchmark_frozen_shadow") {
        txn.exec("SELECT pg_advisory_xact_lock(hashtextextended($1,0))",
                 pqxx::params{"algolens:qt-book:" + book});
        return;
    }
    if (stream != "system" && stream != "qt")
        throw std::runtime_error("runtime_stream_unsupported");
    // Historical corrections commit separately during computation, but they
    // still belong to this captured run. Protect against retire/promote ABA
    // before taking the same canonical book lock as the final publisher.
    if (pending_publication_) {
        auto registry = txn.exec("SELECT runtime_revision,lifecycle,is_active,strategy_type "
            "FROM trading.strategy_registry WHERE id=$1 FOR UPDATE",
            pqxx::params{pending_publication_->registry_id});
        if (registry.size()!=1 || registry[0][0].as<long long>() != pending_publication_->registry_revision ||
            registry[0][1].as<std::string>() != "live" || !registry[0][2].as<bool>() ||
            registry[0][3].as<std::string>() != pending_publication_->strategy_id)
            throw std::runtime_error("runtime_scope_changed");
    }
    txn.exec("SELECT * FROM trading.lock_runtime_scope($1,$2,$3)",
             pqxx::params{strategy_id, book, stream == "qt"});
    if (pending_publication_ && pending_publication_->intent_id) {
        auto approved = txn.exec("SELECT id FROM trading.runtime_intents WHERE id=$1 AND status='approved' "
            "AND registry_revision=$2 AND config_snapshot=$3::jsonb FOR UPDATE",
            pqxx::params{pending_publication_->intent_id,pending_publication_->registry_revision,
                         pending_publication_->snapshot.dump()});
        if (approved.size()!=1) throw std::runtime_error("runtime_approval_changed");
    }
}

Result<bool> PostgresDatabase::begin_live_publication(const std::string& strategy_id,
    const std::string& portfolio_id, const Timestamp& date, const nlohmann::json& snapshot,
    bool controlled, const std::string& version) {
    if (pending_publication_ || publication_transaction_)
        return make_error<bool>(ErrorCode::INVALID_ARGUMENT,"runtime_publication_already_started");
    auto valid = validate_connection();
    if (valid.is_error()) return make_error<bool>(ErrorCode::DATABASE_ERROR,"runtime_database_unavailable");
    try {
        PendingPublication scope;
        scope.strategy_id = strategy_id;
        scope.portfolio_id = canonical_book(portfolio_id);
        scope.date = format_timestamp(date).substr(0,10);
        scope.snapshot = snapshot;
        pqxx::work txn(*connection_);
        auto registry = txn.exec(
            "SELECT r.id,r.runtime_revision,r.lifecycle,r.is_active FROM trading.strategy_registry r "
            "WHERE r.strategy_type=$1 AND upper(btrim(r.portfolio_id))=$2 "
            "ORDER BY r.id FOR UPDATE OF r",
            pqxx::params{strategy_id,scope.portfolio_id});
        if (registry.size()!=1) throw std::runtime_error("runtime_scope_unsupported");
        scope.registry_id = registry[0][0].as<std::string>();
        scope.registry_revision = registry[0][1].as<long long>();
        const auto lifecycle = registry[0][2].as<std::string>();
        const bool active = registry[0][3].as<bool>();
        bool stopped = false;
        if (controlled) {
            auto intent = txn.exec(
                "SELECT id,action FROM trading.runtime_intents WHERE registry_id=$1 "
                "AND portfolio_id=$2 AND engine_strategy_id=$3 AND status='approved' "
                "AND registry_revision=$4 AND config_snapshot=$5::jsonb FOR UPDATE",
                pqxx::params{scope.registry_id,scope.portfolio_id,strategy_id,
                             scope.registry_revision,snapshot.dump()});
            if(intent.size()!=1) throw std::runtime_error("runtime_approval_unavailable");
            scope.intent_id = intent[0][0].as<long long>();
            stopped = intent[0][1].as<std::string>() == "stop";
            if (stopped ? lifecycle != "retired" : lifecycle != "live" || !active)
                throw std::runtime_error("runtime_scope_ineligible");
            scope.attempt_id = txn.exec("SELECT gen_random_uuid()::text")[0][0].as<std::string>();
            txn.exec("INSERT INTO trading.runtime_attempts "
                     "(id,intent_id,registry_revision,config_snapshot,run_date,producer_version,status) "
                     "VALUES ($1,$2,$3,$4::jsonb,$5::date,$6,'running')",
                     pqxx::params{scope.attempt_id,scope.intent_id,scope.registry_revision,
                                  snapshot.dump(),scope.date,version});
            if (stopped) {
                txn.exec("SELECT pg_advisory_xact_lock(hashtextextended($1,0))",
                         pqxx::params{"algolens:qt-book:"+scope.portfolio_id});
                txn.exec("UPDATE trading.runtime_attempts SET status='applied',outcome='stopped',"
                         "finished_at=now() WHERE id=$1",pqxx::params{scope.attempt_id});
            }
        } else if (lifecycle != "live" || !active || scope.registry_revision != 0) {
            throw std::runtime_error("runtime_scope_ineligible");
        }
        txn.commit();
        if (!stopped) pending_publication_ = std::move(scope);
        return stopped;
    } catch(const std::exception&) {
        return make_error<bool>(ErrorCode::DATABASE_ERROR,"runtime_start_refused");
    }
}

Result<void> PostgresDatabase::publish_live_publication() {
    if (!pending_publication_ || publication_transaction_)
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_publication_not_started");
    if (pending_publication_->parts != CompletePublication || pending_publication_->invalid_payload) {
        abandon_live_publication("publication_incomplete");
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_publication_incomplete");
    }
    try {
        // Persisted same-day rows may belong to an earlier attempt. Only a
        // freshly captured system batch can satisfy a configured member here.
        std::vector<std::string> expected_members;
        for (const auto& [name,definition] : pending_publication_->snapshot.at("strategies").items())
            if (definition.value("enabled_live",false)) expected_members.push_back(name);
        auto fresh_members = pending_publication_->fresh_system_members;
        std::sort(expected_members.begin(),expected_members.end());
        std::sort(fresh_members.begin(),fresh_members.end());
        if (expected_members.empty() || expected_members != fresh_members)
            throw std::runtime_error("runtime_scope_fresh_positions_incomplete");
        pqxx::work txn(*connection_);
        auto registry = txn.exec("SELECT runtime_revision,lifecycle,is_active,strategy_type "
            "FROM trading.strategy_registry WHERE id=$1 FOR UPDATE",
            pqxx::params{pending_publication_->registry_id});
        if(registry.size()!=1 || registry[0][0].as<long long>() != pending_publication_->registry_revision ||
           registry[0][1].as<std::string>() != "live" || !registry[0][2].as<bool>() ||
           registry[0][3].as<std::string>() != pending_publication_->strategy_id)
            throw std::runtime_error("runtime_scope_changed");
        fence_live_write(txn,pending_publication_->strategy_id,pending_publication_->portfolio_id);
        if (pending_publication_->intent_id) {
            auto approved = txn.exec("SELECT id FROM trading.runtime_intents WHERE id=$1 AND status='approved' "
                "AND registry_revision=$2 AND config_snapshot=$3::jsonb FOR UPDATE",
                pqxx::params{pending_publication_->intent_id,pending_publication_->registry_revision,
                             pending_publication_->snapshot.dump()});
            if(approved.size()!=1) throw std::runtime_error("runtime_approval_changed");
        }
        publication_transaction_ = &txn;
        // Callbacks hold copies of all payload values. No market work happens here.
        for (const auto& write : pending_publication_->writes) {
            auto result = write();
            if(result.is_error()) throw std::runtime_error("runtime_publication_write_failed");
        }
        // A composite scope is only complete when every configured member has
        // current system and effective-QT evidence, including explicit zeros.
        for (const auto& [name,definition] : pending_publication_->snapshot.at("strategies").items()) {
            if (!definition.value("enabled_live",false)) continue;
            auto coverage = txn.exec("SELECT count(DISTINCT portfolio_type) FROM trading.positions "
                "WHERE portfolio_id=$1 AND strategy_id=$2 AND date=$3::date AND strategy_name=$4 "
                "AND portfolio_type IN ('system','qt')",
                pqxx::params{pending_publication_->portfolio_id,pending_publication_->strategy_id,
                             pending_publication_->date,name});
            if (coverage[0][0].as<int>() != 2)
                throw std::runtime_error("runtime_scope_positions_incomplete");
        }
        if (!pending_publication_->attempt_id.empty()) {
            auto updated = txn.exec("UPDATE trading.runtime_attempts SET status='applied',"
                "outcome='published',publication_id=id,finished_at=now() WHERE id=$1 AND status='running'",
                pqxx::params{pending_publication_->attempt_id});
            if(updated.affected_rows()!=1) throw std::runtime_error("runtime_attempt_changed");
        }
        publication_transaction_ = nullptr;
        txn.commit();
        pending_publication_.reset();
        return Result<void>();
    } catch(const std::exception&) {
        publication_transaction_ = nullptr;
        abandon_live_publication("publication_failed");
        return make_error<void>(ErrorCode::DATABASE_ERROR,"runtime_publication_failed");
    }
}

void PostgresDatabase::abandon_live_publication(const std::string& failure_code) {
    if (!pending_publication_ || publication_transaction_) return;
    try {
        if (!pending_publication_->attempt_id.empty() && connection_ && connection_->is_open()) {
            pqxx::work txn(*connection_);
            txn.exec("UPDATE trading.runtime_attempts SET status='failed',failure_code=$2,"
                "finished_at=now() WHERE id=$1 AND status='running'",
                pqxx::params{pending_publication_->attempt_id,failure_code});
            txn.commit();
        }
    } catch (...) { /* Remains unacknowledged; never manufacture success. */ }
    pending_publication_.reset();
}

Result<void> PostgresDatabase::store_live_run_inputs(const std::string& strategy_id,
    const std::string& portfolio_id,const Timestamp& date,const nlohmann::json& row) {
    if(defer_live_write(date,[this,strategy_id,portfolio_id,date,row]() {
        return store_live_run_inputs(strategy_id,portfolio_id,date,row); }, InputsPart)) return Result<void>();
    try {
        PublicationTransaction txn(*connection_,publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id);
        const auto day = format_timestamp(date).substr(0,10);
        auto existing = txn.exec("SELECT config_snapshot,universe,data_window,engine_flags,trade_ngin_sha "
            "FROM trading.run_inputs WHERE portfolio_id=$1 "
            "AND strategy_id=$2 AND date=$3::date FOR UPDATE",pqxx::params{portfolio_id,strategy_id,day});
        if (!existing.empty()) {
            const std::vector<std::string> fields{"config_snapshot","universe","data_window","engine_flags"};
            for (size_t index=0;index<fields.size();++index)
                if (nlohmann::json::parse(existing[0][index].as<std::string>()) != row.at(fields[index]))
                    return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_historical_snapshot_conflict");
            if (existing[0][4].as<std::string>() != row.at("trade_ngin_sha").get<std::string>())
                return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_historical_snapshot_conflict");
        }
        txn.exec("INSERT INTO trading.run_inputs (portfolio_id,strategy_id,date,trade_ngin_sha,"
            "config_snapshot,universe,data_window,risk_limits_id,engine_flags) "
            "VALUES ($1,$2,$3::date,$4,$5::jsonb,$6::jsonb,$7::jsonb,NULL,$8::jsonb) "
            "ON CONFLICT (portfolio_id,strategy_id,date) DO NOTHING",
            pqxx::params{portfolio_id,strategy_id,day,row.at("trade_ngin_sha").get<std::string>(),
                         row.at("config_snapshot").dump(),row.at("universe").dump(),
                         row.at("data_window").dump(),row.at("engine_flags").dump()});
        txn.commit();
        return Result<void>();
    } catch (...) { return make_error<void>(ErrorCode::DATABASE_ERROR,"runtime_snapshot_write_failed"); }
}

Result<void> PostgresDatabase::execute_scoped_live_update(const std::string& query,
    const std::string& strategy_id,const std::string& portfolio_id) {
    try {
        PublicationTransaction txn(*connection_,publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id);
        txn.exec(query);
        txn.commit();
        return Result<void>();
    } catch (...) { return make_error<void>(ErrorCode::DATABASE_ERROR,"runtime_scoped_update_failed"); }
}
}  // namespace trade_ngin
