#include "trade_ngin/data/postgres_database.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/data/qt_empty_model_owner_storage.hpp"
#include "trade_ngin/data/qt_equity_model_prior_binding.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include <optional>

namespace trade_ngin {
namespace {
std::string canonical_book(std::string book) {
    const auto first = book.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) throw std::runtime_error("runtime_scope_unsupported");
    book = book.substr(first, book.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(book.begin(), book.end(), book.begin(), [](unsigned char c) { return std::toupper(c); });
    return book;
}
// Model (system-stream) publication admits a live or an incubating scope: incubating
// strategies run the same model on mock capital (AlgoLens migration 002). Retired and
// any other lifecycle stay closed, and every caller still requires is_active and the
// revision checks. Desk ownership stays live-only (qt_desk_owner_scope.hpp,
// qt_desk_current_facts.cpp). Migration 025 applies the same rule in the database.
bool publishes_model(const std::string& lifecycle) {
    return lifecycle == "live" || lifecycle == "incubating";
}
}

bool PostgresDatabase::defer_live_write(const Timestamp& date, std::function<Result<void>()> write, unsigned part) {
    if (!pending_publication_ || publication_transaction_ ||
        format_timestamp(date).substr(0,10) != pending_publication_->date) return false;
    pending_publication_->writes.push_back(std::move(write));
    pending_publication_->parts |= part;
    return true;
}

Result<void> PostgresDatabase::validate_portfolio_id(
    const std::string& portfolio_id) const {
    if (portfolio_id.empty() ||
        portfolio_id.find_first_not_of(" \t\r\n") == std::string::npos) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "portfolio_id must not be empty");
    }
    return Result<void>();
}

Result<std::string> PostgresDatabase::onboard_investor_book(
    const InvestorBookOnboarding& request) {
    const auto valid_key = [](const std::string& value, std::size_t maximum,
                              bool lowercase_only) {
        if (value.empty() || value.size() > maximum ||
            !std::isalnum(static_cast<unsigned char>(value.front()))) {
            return false;
        }
        return std::all_of(value.begin(), value.end(), [lowercase_only](unsigned char ch) {
            if (lowercase_only) {
                return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                       ch == '_' || ch == '-';
            }
            return std::isalnum(ch) != 0 || ch == '_' || ch == '-';
        });
    };
    if (!valid_key(request.config_key, 64, true) ||
        !valid_key(request.portfolio_id, 100, false) ||
        !std::isfinite(request.initial_capital) || request.initial_capital <= 0.0 ||
        request.created_by.empty() || request.created_by.size() > 200 ||
        request.created_by.find_first_not_of(" \t\r\n") == std::string::npos ||
        request.strategy_ids.empty()) {
        return make_error<std::string>(ErrorCode::INVALID_ARGUMENT,
                                       "investor_book_onboarding_input_invalid");
    }
    Timestamp parsed_opening_date;
    if (!core::parse_utc_date(request.opening_date, parsed_opening_date)) {
        return make_error<std::string>(ErrorCode::INVALID_ARGUMENT,
                                       "investor_book_onboarding_input_invalid");
    }
    std::set<std::string> unique_strategies;
    for (const auto& strategy_id : request.strategy_ids) {
        auto valid = validate_strategy_id(strategy_id);
        if (valid.is_error() || !unique_strategies.insert(strategy_id).second) {
            return make_error<std::string>(ErrorCode::INVALID_ARGUMENT,
                                           "investor_book_strategy_ids_invalid");
        }
    }
    auto connection_validation = validate_connection();
    if (connection_validation.is_error()) {
        return make_error<std::string>(connection_validation.error()->code(),
                                       connection_validation.error()->what());
    }
    try {
        pqxx::work transaction(*connection_);
        nlohmann::json strategies = nlohmann::json::array();
        for (const auto& strategy_id : request.strategy_ids) {
            strategies.push_back(strategy_id);
        }
        auto result = transaction.exec(
            "SELECT trading.onboard_investor_book($1,$2,$3,$4::date,$5::jsonb,$6)::text",
            pqxx::params{request.config_key, request.portfolio_id, request.initial_capital,
                         request.opening_date, strategies.dump(), request.created_by});
        if (result.size() != 1 || result[0][0].is_null()) {
            return make_error<std::string>(ErrorCode::DATABASE_ERROR,
                                           "investor_book_onboarding_result_invalid");
        }
        const std::string book_id = result[0][0].as<std::string>();
        transaction.commit();
        return Result<std::string>(book_id);
    } catch (const pqxx::sql_error& error) {
        const std::string message = error.what();
        const auto code = message.find("investor_book_") != std::string::npos
                              ? ErrorCode::INVALID_ARGUMENT
                              : ErrorCode::DATABASE_ERROR;
        return make_error<std::string>(code, message, "PostgresDatabase");
    } catch (const std::exception& error) {
        return make_error<std::string>(ErrorCode::DATABASE_ERROR, error.what(),
                                       "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::validate_operational_stream(const std::string& portfolio_id,
                                                         const std::string& stream) {
    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) return portfolio_validation;
    if (stream == "qt_proposal") {
        poison_proposal_refusal();
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"proposal_operational_stream_unsupported");
    }
    if (stream != "system" && stream != "qt")
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"operational_stream_scope_invalid");
    // Configuration approval is not authorization for a desk publication.
    if (pending_publication_ && stream != "system") {
        pending_publication_->invalid_payload = true;
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_system_publication_requires_system_stream");
    }
    return Result<void>();
}

void PostgresDatabase::poison_proposal_refusal() {
    if (pending_publication_) pending_publication_->invalid_payload = true;
}

void PostgresDatabase::require_proposal_capability(pqxx::work& txn) {
    // ACCESS SHARE holds the checked catalog generation through the seed/read;
    // migration015 needs ACCESS EXCLUSIVE. Ordinary row writes remain possible.
    txn.exec("LOCK TABLE trading.positions IN ACCESS SHARE MODE");
    auto status = txn.exec(R"SQL(
      SELECT
        (SELECT string_agg(a.attname,',' ORDER BY k.ordinality)
           FROM pg_constraint c CROSS JOIN LATERAL
             unnest(c.conkey) WITH ORDINALITY k(attnum,ordinality)
           JOIN pg_attribute a ON a.attrelid=c.conrelid AND a.attnum=k.attnum
           WHERE c.conrelid='trading.positions'::regclass AND c.contype='p'
             AND c.conname='positions_pkey') =
          'portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type'
        AND (SELECT count(*) FROM pg_constraint WHERE conrelid='trading.positions'::regclass
             AND contype='p')=1
        AND EXISTS (SELECT 1 FROM pg_attribute a JOIN pg_attrdef d
          ON d.adrelid=a.attrelid AND d.adnum=a.attnum
          WHERE a.attrelid='trading.positions'::regclass AND a.attname='portfolio_type'
            AND a.atttypid='text'::regtype AND a.attnotnull
            AND pg_get_expr(d.adbin,d.adrelid)='''system''::text')
        AND EXISTS (SELECT 1 FROM pg_constraint c
          WHERE c.conrelid='trading.positions'::regclass
            AND c.conname='positions_portfolio_type_check' AND c.contype='c'
            AND c.convalidated AND regexp_replace(pg_get_constraintdef(c.oid),'\s','','g')=
            'CHECK((portfolio_type=ANY(ARRAY[''system''::text,''qt''::text,''benchmark''::text,''benchmark_rebench''::text,''benchmark_frozen_shadow''::text,''qt_proposal''::text])))')
        AND (SELECT count(*) FROM pg_constraint WHERE conrelid='trading.positions'::regclass
             AND conname='positions_portfolio_type_check')=1
        AND EXISTS (SELECT 1 FROM pg_proc f JOIN pg_language l ON l.oid=f.prolang
          WHERE f.oid=to_regprocedure('trading.fence_runtime_publication_row()')
            AND f.prorettype='pg_catalog.trigger'::regtype AND f.pronargs=0
            AND l.lanname='plpgsql' AND md5(f.prosrc)='419771cec97836560ae952c120aac4d1')
        AND EXISTS (SELECT 1 FROM pg_trigger t
          WHERE t.tgrelid='trading.positions'::regclass
            AND t.tgname='runtime_publication_fence' AND t.tgenabled='O'
            AND t.tgtype=31 AND NOT t.tgisinternal AND t.tgqual IS NULL
            AND t.tgattr::text='' AND t.tgnargs=0 AND octet_length(t.tgargs)=0
            AND t.tgconstraint=0 AND NOT t.tgdeferrable AND NOT t.tginitdeferred
            AND t.tgparentid=0 AND t.tgoldtable IS NULL AND t.tgnewtable IS NULL
            AND t.tgfoid=to_regprocedure('trading.fence_runtime_publication_row()'))
        AND (SELECT count(*) FROM pg_trigger WHERE tgrelid='trading.positions'::regclass
             AND tgname='runtime_publication_fence')=1 AS supported
    )SQL");
    if (status.size()!=1 || !status[0][0].as<bool>())
        throw std::runtime_error("proposal015_schema_capability_missing_or_unsupported");
}

void PostgresDatabase::require_qt_exact_storage_capability(pqxx::work& txn) {
    // ROW EXCLUSIVE is also needed for the two seed tables: ENABLE/DISABLE
    // TRIGGER uses SHARE ROW EXCLUSIVE, which ACCESS SHARE does not exclude.
    // Hold these relation locks through the final publication commit.
    txn.exec("LOCK TABLE trading.positions IN ACCESS SHARE MODE");
    txn.exec("LOCK TABLE trading.qt_storage_capabilities, "
             "trading.qt_model_seed_publications IN ROW EXCLUSIVE MODE");
    const bool empty_capability=require_qt_empty_owner_storage_capability(txn);
    const auto status = txn.exec(R"SQL(
      WITH expected_columns(rel,name,kind,required) AS (VALUES
        ('trading.positions'::regclass,'qt_proposal_revision','uuid',false),
        ('trading.qt_model_seed_publications'::regclass,'publication_id','uuid',true),
        ('trading.qt_model_seed_publications'::regclass,'attempt_id','uuid',false),
        ('trading.qt_model_seed_publications'::regclass,'portfolio_id','text',true),
        ('trading.qt_model_seed_publications'::regclass,'strategy_id','text',true),
        ('trading.qt_model_seed_publications'::regclass,'source_day','date',true),
        ('trading.qt_model_seed_publications'::regclass,'publication_version','bigint',true),
        ('trading.qt_model_seed_publications'::regclass,'system_components','jsonb',true),
        ('trading.qt_model_seed_publications'::regclass,'seed_digest','text',true),
        ('trading.qt_model_seed_publications'::regclass,'proposal_components','jsonb',true),
        ('trading.qt_model_seed_publications'::regclass,'proposal_manifest_digest','text',true),
        ('trading.qt_model_seed_publications'::regclass,'producer_version','text',true),
        ('trading.qt_model_seed_publications'::regclass,'created_at','timestamp with time zone',true),
        ('trading.qt_storage_capabilities'::regclass,'capability_name','text',true),
        ('trading.qt_storage_capabilities'::regclass,'capability_version','bigint',true),
        ('trading.qt_storage_capabilities'::regclass,'installed_at','timestamp with time zone',true)
      ), expected_keys(rel,kind,columns) AS (VALUES
        ('trading.qt_model_seed_publications'::regclass,'p',ARRAY['publication_id']::text[]),
        ('trading.qt_model_seed_publications'::regclass,'u',ARRAY['attempt_id']::text[]),
        ('trading.qt_model_seed_publications'::regclass,'u',
          ARRAY['portfolio_id','source_day','publication_version']::text[]),
        ('trading.qt_model_seed_publications'::regclass,'u',
          ARRAY['portfolio_id','strategy_id','source_day','attempt_id']::text[]),
        ('trading.qt_storage_capabilities'::regclass,'p',ARRAY['capability_name']::text[])
      ), expected_checks(rel,name,definition) AS (VALUES
        ('trading.positions'::regclass,
          'positions_qt_proposal_revision_check',
          'CHECK(((portfolio_type=''qt_proposal''::text)OR(qt_proposal_revisionISNULL)))'),
        ('trading.qt_model_seed_publications'::regclass,
          'qt_model_seed_publications_portfolio_id_check',
          'CHECK((length(btrim(portfolio_id))>0))'),
        ('trading.qt_model_seed_publications'::regclass,
          'qt_model_seed_publications_strategy_id_check',
          'CHECK((length(btrim(strategy_id))>0))'),
        ('trading.qt_model_seed_publications'::regclass,
          'qt_model_seed_publications_publication_version_check',
          'CHECK((publication_version>0))'),
        ('trading.qt_model_seed_publications'::regclass,
          'qt_model_seed_publications_system_components_check',
          'CHECK(((jsonb_typeof(system_components)=''array''::text)AND(jsonb_array_length(system_components)>0)))'),
        ('trading.qt_model_seed_publications'::regclass,
          'qt_model_seed_publications_seed_digest_check',
          'CHECK((seed_digest~''^[0-9a-f]{64}$''::text))'),
        ('trading.qt_model_seed_publications'::regclass,
          'qt_model_seed_publications_proposal_components_check',
          'CHECK((jsonb_typeof(proposal_components)=''array''::text))'),
        ('trading.qt_model_seed_publications'::regclass,
          'qt_model_seed_publications_proposal_manifest_digest_check',
          'CHECK((proposal_manifest_digest~''^[0-9a-f]{64}$''::text))'),
        ('trading.qt_model_seed_publications'::regclass,
          'qt_model_seed_publications_producer_version_check',
          'CHECK((length(btrim(producer_version))>0))'),
        ('trading.qt_storage_capabilities'::regclass,
          'qt_storage_capabilities_capability_version_check',
          'CHECK((capability_version>0))')
      ), expected_triggers(rel,name,events) AS (VALUES
        ('trading.positions'::regclass,'qt_proposal_revision_stamp',23),
        ('trading.qt_model_seed_publications'::regclass,'qt_model_seed_immutable',27),
        ('trading.qt_model_seed_publications'::regclass,'qt_model_seed_no_truncate',34),
        ('trading.qt_storage_capabilities'::regclass,'qt_storage_capability_immutable',27),
        ('trading.qt_storage_capabilities'::regclass,'qt_storage_capability_no_truncate',34)
      )
      SELECT
        (SELECT count(*) FROM pg_attribute a
          WHERE a.attrelid='trading.positions'::regclass
            AND a.attname IN ('quantity','average_price')
            AND a.attnum>0 AND NOT a.attisdropped AND a.attnotnull
            AND format_type(a.atttypid,a.atttypmod)='numeric(20,8)')=2
        AND (SELECT count(*) FROM pg_class r WHERE r.oid IN
          ('trading.qt_model_seed_publications'::regclass,
           'trading.qt_storage_capabilities'::regclass)
          AND r.relkind='r' AND NOT r.relispartition
          AND NOT r.relrowsecurity AND NOT r.relforcerowsecurity)=2
        AND NOT EXISTS (SELECT 1 FROM expected_columns e
          LEFT JOIN pg_attribute a ON a.attrelid=e.rel AND a.attname=e.name
            AND a.attnum>0 AND NOT a.attisdropped
          WHERE a.attnum IS NULL OR format_type(a.atttypid,a.atttypmod)<>e.kind
            OR a.attnotnull IS DISTINCT FROM e.required)
        AND (SELECT count(*) FROM pg_attribute a WHERE
          a.attrelid='trading.qt_model_seed_publications'::regclass
          AND a.attnum>0 AND NOT a.attisdropped)=12
        AND (SELECT count(*) FROM pg_attribute a WHERE
          a.attrelid='trading.qt_storage_capabilities'::regclass
          AND a.attnum>0 AND NOT a.attisdropped)=3
        AND (SELECT count(*) FROM pg_attribute a JOIN pg_attrdef d
          ON d.adrelid=a.attrelid AND d.adnum=a.attnum
          WHERE (a.attrelid,a.attname) IN
            (('trading.qt_model_seed_publications'::regclass,'created_at'),
             ('trading.qt_storage_capabilities'::regclass,'installed_at'))
          AND pg_get_expr(d.adbin,d.adrelid)='now()')=2
        AND (SELECT count(*) FROM trading.qt_storage_capabilities
          WHERE capability_name='qt_exact_decimal8' AND capability_version=1)=1
        AND NOT EXISTS (SELECT 1 FROM expected_keys e WHERE NOT EXISTS (
          SELECT 1 FROM pg_constraint c JOIN pg_index i ON i.indexrelid=c.conindid
          WHERE c.conrelid=e.rel AND c.contype=e.kind
            AND c.convalidated AND NOT c.condeferrable AND NOT c.condeferred
            AND c.coninhcount=0 AND c.conparentid=0
            AND i.indisunique AND i.indisvalid AND i.indisready AND i.indislive
            AND i.indisprimary=(e.kind='p') AND i.indpred IS NULL
            AND i.indexprs IS NULL AND i.indnatts=i.indnkeyatts
            AND (SELECT array_agg(a.attname::text ORDER BY k.ordinality)
              FROM unnest(c.conkey) WITH ORDINALITY k(attnum,ordinality)
              JOIN pg_attribute a ON a.attrelid=c.conrelid AND a.attnum=k.attnum)
              =e.columns))
        AND (SELECT count(*) FROM pg_constraint WHERE
          conrelid='trading.qt_model_seed_publications'::regclass
          AND contype IN ('p','u'))=4
        AND (SELECT count(*) FROM pg_constraint WHERE
          conrelid='trading.qt_storage_capabilities'::regclass
          AND contype IN ('p','u'))=1
        AND NOT EXISTS (SELECT 1 FROM expected_checks e
          LEFT JOIN pg_constraint c ON c.conrelid=e.rel AND c.conname=e.name
          WHERE c.oid IS NULL OR c.contype<>'c' OR NOT c.convalidated
            OR regexp_replace(pg_get_constraintdef(c.oid),'\s','','g')<>e.definition)
        AND (SELECT count(*) FROM pg_constraint WHERE
          conrelid='trading.qt_model_seed_publications'::regclass AND contype='c')=8
        AND (SELECT count(*) FROM pg_constraint WHERE
          conrelid='trading.qt_storage_capabilities'::regclass AND contype='c')=1
        AND EXISTS (SELECT 1 FROM pg_proc f JOIN pg_language l ON l.oid=f.prolang
          WHERE f.oid=to_regprocedure('trading.refuse_qt_seed_mutation()')
            AND f.prorettype='pg_catalog.trigger'::regtype AND f.pronargs=0
            AND f.prokind='f' AND f.provolatile='v' AND NOT f.prosecdef
            AND NOT f.proisstrict AND f.proconfig IS NULL
            AND l.lanname='plpgsql'
            AND md5(f.prosrc)='e2ecd4f309bfd52dd1ef3506b9d3b0e3')
        AND EXISTS (SELECT 1 FROM pg_proc f JOIN pg_language l ON l.oid=f.prolang
          WHERE f.oid=to_regprocedure('trading.stamp_qt_proposal_revision()')
            AND f.prorettype='pg_catalog.trigger'::regtype AND f.pronargs=0
            AND f.prokind='f' AND f.provolatile='v' AND NOT f.prosecdef
            AND NOT f.proisstrict AND f.proconfig IS NULL
            AND l.lanname='plpgsql'
            AND md5(f.prosrc)='a93838668d52663a89ea31debe9b1bc3')
        AND EXISTS (SELECT 1 FROM pg_class c JOIN pg_index i ON i.indexrelid=c.oid
          JOIN pg_attribute a ON a.attrelid=i.indrelid AND a.attnum=i.indkey[0]
          WHERE c.oid=to_regclass('trading.qt_proposal_revision_unique')
            AND i.indrelid='trading.positions'::regclass
            AND a.attname='qt_proposal_revision'
            AND i.indisunique AND i.indisvalid AND i.indisready AND i.indislive
            AND NOT i.indisprimary AND i.indnkeyatts=1 AND i.indnatts=1
            AND i.indexprs IS NULL
            AND regexp_replace(pg_get_expr(i.indpred,i.indrelid),'\s','','g')=
                '(qt_proposal_revisionISNOTNULL)')
        AND NOT EXISTS (SELECT 1 FROM pg_attrdef d JOIN pg_attribute a
          ON a.attrelid=d.adrelid AND a.attnum=d.adnum
          WHERE d.adrelid='trading.positions'::regclass
            AND a.attname='qt_proposal_revision')
        AND NOT EXISTS (SELECT 1 FROM expected_triggers e
          LEFT JOIN pg_trigger t ON t.tgrelid=e.rel AND t.tgname=e.name
          WHERE t.oid IS NULL OR t.tgenabled<>'O' OR t.tgtype<>e.events
            OR t.tgisinternal OR t.tgqual IS NOT NULL OR t.tgattr::text<>''
            OR t.tgnargs<>0 OR octet_length(t.tgargs)<>0
            OR t.tgconstraint<>0 OR t.tgdeferrable OR t.tginitdeferred
            OR t.tgparentid<>0 OR t.tgoldtable IS NOT NULL
            OR t.tgnewtable IS NOT NULL
            OR t.tgfoid<>CASE WHEN e.name='qt_proposal_revision_stamp'
              THEN to_regprocedure('trading.stamp_qt_proposal_revision()')
              ELSE to_regprocedure('trading.refuse_qt_seed_mutation()') END)
        AND (SELECT count(*) FROM pg_trigger WHERE
          tgrelid='trading.qt_model_seed_publications'::regclass
          AND NOT tgisinternal)=CASE WHEN $1 THEN 3 ELSE 2 END
        AND (SELECT count(*) FROM pg_trigger WHERE
          tgrelid='trading.qt_storage_capabilities'::regclass
          AND NOT tgisinternal)=2
        AS supported
    )SQL",pqxx::params{empty_capability});
    if (status.size()!=1 || !status[0][0].as<bool>())
        throw std::runtime_error("qt_exact_storage_capability_missing_or_unsupported");
}

bool PostgresDatabase::defer_live_write(std::function<Result<void>()> write, unsigned part) {
    if (!pending_publication_ || publication_transaction_) return false;
    pending_publication_->writes.push_back(std::move(write));
    pending_publication_->parts |= part;
    return true;
}

void PostgresDatabase::fence_live_write(pqxx::work& txn, const std::string& strategy_id,
                                       const std::string& portfolio_id, const std::string& stream,
                                       bool proposal_seed_operation) {
    if (portfolio_id.empty() ||
        portfolio_id.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::runtime_error("portfolio_id_required");
    }
    const auto book = canonical_book(portfolio_id);
    if (pending_publication_ &&
        (pending_publication_->strategy_id != strategy_id || pending_publication_->portfolio_id != book))
        throw std::runtime_error("runtime_publication_scope_mismatch");
    if (stream == "benchmark" || stream == "benchmark_rebench" || stream == "benchmark_frozen_shadow") {
        txn.exec("SELECT pg_advisory_xact_lock(hashtextextended($1,0))",
                 pqxx::params{"algolens:qt-book:" + book});
        return;
    }
    if (stream != "system" && stream != "qt" &&
        !(stream == "qt_proposal" && proposal_seed_operation))
        throw std::runtime_error("runtime_stream_unsupported");
    // Historical corrections commit separately during computation, but they
    // still belong to this captured run. Protect against retire/promote ABA
    // before taking the same canonical book lock as the final publisher.
    if (pending_publication_) {
        auto registry = txn.exec("SELECT runtime_revision,lifecycle,is_active,strategy_type "
            "FROM trading.strategy_registry WHERE id=$1 FOR UPDATE",
            pqxx::params{pending_publication_->registry_id});
        if (registry.size()!=1 || registry[0][0].as<long long>() != pending_publication_->registry_revision ||
            !publishes_model(registry[0][1].as<std::string>()) || !registry[0][2].as<bool>() ||
            registry[0][3].as<std::string>() != pending_publication_->strategy_id)
            throw std::runtime_error("runtime_scope_changed");
    }
    txn.exec("SELECT * FROM trading.lock_runtime_scope($1,$2,$3)",
             pqxx::params{strategy_id, book, stream == "qt" || stream == "qt_proposal"});
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
    bool controlled, const std::string& version, PublicationEvidenceRequirement requirement,
    PublicationEvidenceToken* token_out, PublicationPriorRequirement prior_requirement) {
    if (token_out) *token_out = {};
    if (pending_publication_ || publication_transaction_)
        return make_error<bool>(ErrorCode::INVALID_ARGUMENT,"runtime_publication_already_started");
    if ((requirement != PublicationEvidenceRequirement::LegacyNotCollected &&
         requirement != PublicationEvidenceRequirement::RequiredFinalObservations) ||
        (requirement == PublicationEvidenceRequirement::RequiredFinalObservations && !token_out))
        return make_error<bool>(ErrorCode::INVALID_ARGUMENT,"runtime_evidence_requirement_invalid");
    if ((prior_requirement != PublicationPriorRequirement::None &&
         prior_requirement != PublicationPriorRequirement::VerifiedEquity) ||
        (prior_requirement == PublicationPriorRequirement::VerifiedEquity &&
         (strategy_id != "LIVE_EQUITY_MEAN_REVERSION" ||
          requirement != PublicationEvidenceRequirement::RequiredFinalObservations)))
        return make_error<bool>(ErrorCode::INVALID_ARGUMENT,"runtime_prior_requirement_invalid");
    auto valid = validate_connection();
    if (valid.is_error()) return make_error<bool>(ErrorCode::DATABASE_ERROR,"runtime_database_unavailable");
    try {
        auto scope = std::make_unique<PendingPublication>();
        PublicationEvidenceToken token;
        token.marker_ = std::make_shared<const unsigned char>(0);
        scope->evidence_marker = token.marker_;
        scope->evidence_requirement = requirement;
        scope->prior_requirement = prior_requirement;
        scope->strategy_id = strategy_id;
        scope->portfolio_id = canonical_book(portfolio_id);
        scope->date = format_timestamp(date).substr(0,10);
        scope->producer_version = version;
        scope->snapshot = snapshot;
        pqxx::work txn(*connection_);
        auto registry = txn.exec(
            "SELECT r.id,r.runtime_revision,r.lifecycle,r.is_active FROM trading.strategy_registry r "
            "WHERE r.strategy_type=$1 AND (upper(btrim(r.portfolio_id))=$2 OR EXISTS ("
            "SELECT 1 FROM trading.strategy_book_memberships m WHERE m.strategy_id=r.id "
            "AND upper(btrim(m.portfolio_id))=$2)) "
            "ORDER BY r.id FOR UPDATE OF r",
            pqxx::params{strategy_id,scope->portfolio_id});
        if (registry.size()!=1) throw std::runtime_error("runtime_scope_unsupported");
        scope->registry_id = registry[0][0].as<std::string>();
        scope->registry_revision = registry[0][1].as<long long>();
        const auto lifecycle = registry[0][2].as<std::string>();
        const bool active = registry[0][3].as<bool>();
        bool stopped = false;
        if (controlled) {
            auto intent = txn.exec(
                "SELECT id,action FROM trading.runtime_intents WHERE registry_id=$1 "
                "AND portfolio_id=$2 AND engine_strategy_id=$3 AND status='approved' "
                "AND registry_revision=$4 AND config_snapshot=$5::jsonb FOR UPDATE",
                pqxx::params{scope->registry_id,scope->portfolio_id,strategy_id,
                             scope->registry_revision,snapshot.dump()});
            if(intent.size()!=1) throw std::runtime_error("runtime_approval_unavailable");
            scope->intent_id = intent[0][0].as<long long>();
            stopped = intent[0][1].as<std::string>() == "stop";
            if (stopped ? lifecycle != "retired" : !publishes_model(lifecycle) || !active)
                throw std::runtime_error("runtime_scope_ineligible");
            scope->attempt_id = txn.exec("SELECT gen_random_uuid()::text")[0][0].as<std::string>();
            scope->publication_id = scope->attempt_id;
            txn.exec("INSERT INTO trading.runtime_attempts "
                     "(id,intent_id,registry_revision,config_snapshot,run_date,producer_version,status) "
                     "VALUES ($1,$2,$3,$4::jsonb,$5::date,$6,'running')",
                     pqxx::params{scope->attempt_id,scope->intent_id,scope->registry_revision,
                                  snapshot.dump(),scope->date,version});
            if (stopped) {
                txn.exec("SELECT pg_advisory_xact_lock(hashtextextended($1,0))",
                         pqxx::params{"algolens:qt-book:"+scope->portfolio_id});
                txn.exec("UPDATE trading.runtime_attempts SET status='applied',outcome='stopped',"
                         "finished_at=now() WHERE id=$1",pqxx::params{scope->attempt_id});
            }
        } else if (!publishes_model(lifecycle) || !active || scope->registry_revision != 0) {
            throw std::runtime_error("runtime_scope_ineligible");
        }
        txn.commit();
        if (!stopped) {
            pending_publication_ = std::move(scope);
            if (token_out) *token_out = std::move(token);
        }
        return stopped;
    } catch(const std::exception&) {
        return make_error<bool>(ErrorCode::DATABASE_ERROR,"runtime_start_refused");
    }
}

Result<void> PostgresDatabase::store_model_position_batch(const QtModelPositionBatch& batch) {
    try {
    if(!pending_publication_ || publication_transaction_ || pending_publication_->invalid_payload ||
        pending_publication_->strategy_id!="LIVE_EQUITY_MEAN_REVERSION" ||
        batch.strategy_id!=pending_publication_->strategy_id || batch.portfolio_id!=pending_publication_->portfolio_id ||
        batch.source_day!=pending_publication_->date || batch.strategy_name.empty()) {
        poison_proposal_refusal();return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_model_batch_scope_invalid");
    }
    auto& pending=*pending_publication_;
    const auto& configured=pending.snapshot.at("strategies");
    if(!configured.contains(batch.strategy_name) || !configured.at(batch.strategy_name).is_object() ||
        !configured.at(batch.strategy_name).contains("enabled_live") ||
        !configured.at(batch.strategy_name).at("enabled_live").is_boolean() ||
        !configured.at(batch.strategy_name).at("enabled_live").get<bool>() ||
        pending.proposal_sealed_members.contains(batch.strategy_name)) {
        poison_proposal_refusal();return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_model_batch_owner_invalid");
    }
    if(!batch.positions.empty()) {
        for(const auto& position:batch.positions)
            if(format_timestamp(position.last_update).substr(0,10)!=batch.source_day) {
                poison_proposal_refusal();return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_model_batch_day_invalid");
            }
        return store_positions(batch.positions,batch.strategy_id,batch.strategy_name,batch.portfolio_id,"trading.positions","system");
    }
    if(std::find(pending.fresh_system_members.begin(),pending.fresh_system_members.end(),batch.strategy_name)!=pending.fresh_system_members.end()) {
        poison_proposal_refusal();return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_model_batch_duplicate");
    }
    // Native typed callback, not caller JSON/Boolean. All writes remain deferred.
    pending.fresh_system_members.push_back(batch.strategy_name);
    pending.fresh_empty_batches.push_back({batch.portfolio_id,batch.strategy_id,batch.strategy_name,batch.source_day});
    pending.parts|=PositionsPart;
    pending.writes.push_back([this,batch]() {
        try {
            if(!publication_transaction_ || !pending_publication_ || pending_publication_->invalid_payload)
                throw std::runtime_error("runtime_model_batch_transaction_missing");
            auto& tx=*publication_transaction_;
            if(!require_qt_empty_owner_storage_capability(tx))throw std::runtime_error("runtime_empty_owner_capability_missing");
            fence_live_write(tx,batch.strategy_id,batch.portfolio_id,"system");
            tx.exec("DELETE FROM trading.positions WHERE portfolio_id=$1 AND strategy_id=$2 AND strategy_name=$3 AND date=$4::date AND portfolio_type='system'",
                pqxx::params{batch.portfolio_id,batch.strategy_id,batch.strategy_name,batch.source_day});
            return Result<void>();
        } catch(const std::exception&) {
            poison_proposal_refusal();return make_error<void>(ErrorCode::DATABASE_ERROR,"runtime_model_batch_write_refused");
        }
    });
    return Result<void>();
    } catch(const std::exception&) {
        // Any allocation/JSON failure during staging invalidates the entire
        // pending publication; partially recorded callback metadata is unusable.
        poison_proposal_refusal();
        return make_error<void>(ErrorCode::DATABASE_ERROR,"runtime_model_batch_staging_refused");
    }
}

Result<void> PostgresDatabase::clear_equity_model_current_positions(
    const std::string& portfolio_id,const Timestamp& date) {
    if(!pending_publication_ || pending_publication_->invalid_payload ||
        pending_publication_->strategy_id!="LIVE_EQUITY_MEAN_REVERSION" ||
        pending_publication_->portfolio_id!=portfolio_id ||
        pending_publication_->date!=format_timestamp(date).substr(0,10)) {
        if(pending_publication_)pending_publication_->invalid_payload=true;
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"equity_current_clear_scope_invalid");
    }
    if(defer_live_write(date,[this,portfolio_id,date](){
        return clear_equity_model_current_positions(portfolio_id,date);}))return Result<void>();
    if(!publication_transaction_) {
        pending_publication_->invalid_payload=true;
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"equity_current_clear_requires_publication");
    }
    try {
        auto& tx=*publication_transaction_;
        fence_live_write(tx,"LIVE_EQUITY_MEAN_REVERSION",portfolio_id,"system");
        tx.exec("DELETE FROM trading.positions WHERE portfolio_id=$1 AND strategy_id='LIVE_EQUITY_MEAN_REVERSION' "
            "AND strategy_name='EQUITY_MEAN_REVERSION' AND portfolio_type='system' AND date=$2::date",
            pqxx::params{portfolio_id,pending_publication_->date});
        return Result<void>();
    }catch(const std::exception&) {
        pending_publication_->invalid_payload=true;
        return make_error<void>(ErrorCode::DATABASE_ERROR,"equity_current_clear_failed");
    }
}

Result<VerifiedEquityModelPrior> PostgresDatabase::capture_equity_model_prior(
    const EquityModelPriorSelection& selection,const EquityModelPriorOwner& owner) {
    if (!pending_publication_)
        return make_error<VerifiedEquityModelPrior>(ErrorCode::INVALID_ARGUMENT,"runtime_prior_not_started");
    auto& pending=*pending_publication_;
    try {
        if (publication_transaction_ || pending.invalid_payload || pending.equity_prior ||
            pending.prior_requirement != PublicationPriorRequirement::VerifiedEquity ||
            selection.mode != EquityModelPriorMode::VerifiedDeskPrior ||
            pending.strategy_id != "LIVE_EQUITY_MEAN_REVERSION" ||
            owner.strategy_id != pending.strategy_id || owner.strategy_name != "EQUITY_MEAN_REVERSION" ||
            owner.portfolio_id != pending.portfolio_id || owner.valuation_day != pending.date ||
            owner.source_day >= owner.valuation_day)
            throw std::runtime_error("runtime_prior_scope_invalid");
        auto connected=validate_connection();
        if(connected.is_error())throw std::runtime_error("runtime_prior_database_unavailable");
        // Admission already committed. This is a SHORT read transaction;
        // release its book lock before computing or invoking another DB method.
        pqxx::work tx(*connection_);
        auto prior=load_verified_equity_model_prior(tx,selection,owner);
        if(prior.is_error())throw std::runtime_error("runtime_prior_unavailable");
        tx.commit();
        pending.equity_prior.emplace(CapturedEquityModelPrior{selection,owner,prior.value()});
        return prior.value();
    } catch(const std::exception&) {
        pending.invalid_payload=true;
        return make_error<VerifiedEquityModelPrior>(ErrorCode::INVALID_DATA,"runtime_prior_capture_refused");
    }
}

Result<void> PostgresDatabase::attach_equity_run_consumption(
    const PublicationEvidenceToken& token,const EquityRunProjection& projection) {
    if(!pending_publication_)
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_equity_consumption_not_started");
    auto& pending=*pending_publication_;
    const bool same_marker=token.marker_ && pending.evidence_marker &&
        !token.marker_.owner_before(pending.evidence_marker) &&
        !pending.evidence_marker.owner_before(token.marker_);
    const auto& document=projection.document();
    if(publication_transaction_ || pending.invalid_payload || !same_marker ||
        pending.strategy_id!="LIVE_EQUITY_MEAN_REVERSION" ||
        pending.evidence_requirement!=PublicationEvidenceRequirement::RequiredFinalObservations ||
        !pending.inspection_capture_queued || pending.final_consumption || pending.equity_final_consumption ||
        !document.is_object() || !document.contains("run_key") ||
        document.at("run_key")!=nlohmann::json{{"portfolio_id",pending.portfolio_id},
            {"strategy_id",pending.strategy_id},{"strategy_name","EQUITY_MEAN_REVERSION"},{"date",pending.date}}) {
        pending.invalid_payload=true;
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_equity_consumption_protocol_invalid");
    }
    try {
        const auto& prior_reads=document.at("stages").at("prior").at("reads");
        if(pending.prior_requirement==PublicationPriorRequirement::VerifiedEquity) {
            if(!pending.equity_prior || prior_reads.value("mode","")!="verified_desk_prior")
                throw std::runtime_error("runtime_equity_prior_trace_missing");
            const auto& reference=pending.equity_prior->observed.replay_reference;
            for(const auto* key:{"mode","source_day","valuation_day","decision_id","finalization_id",
                "finalization_digest","finalization_source_digest","accounting_input_digest","observation_digest","results_digest"})
                if(!prior_reads.contains(key) || prior_reads.at(key)!=reference.at(key))
                    throw std::runtime_error("runtime_equity_prior_trace_mismatch");
            const auto& action_reads=document.at("stages").at("corporate_actions").at("reads");
            const auto& eod_reads=document.at("stages").at("eod").at("reads");
            if(reference.at("schema_version")=="qt-equity-model-prior/v2") {
                if(reference.at("action_admission")!="proved_action_adjusted_prior" ||
                    document.at("schema_version")!="qt-equity-run-consumption/v2" ||
                    document.at("catalog_version")!="qt-equity-main08b15c-run/v2" ||
                    action_reads.value("path","")!="proved_action_adjusted_prior" || action_reads.size()!=7)
                    throw std::runtime_error("runtime_equity_action_frame_trace_mismatch");
                const auto& frame=reference.at("action_frame");
                for(const auto* key:{"original_action_count","successor_action_count",
                    "original_action_digest","successor_action_digest"})
                    if(action_reads.at(key)!=frame.at(key))
                        throw std::runtime_error("runtime_equity_action_frame_trace_mismatch");
                if(action_reads.at("basis_frame_digest")!=reference.at("action_frame_digest"))
                    throw std::runtime_error("runtime_equity_action_frame_trace_mismatch");
            } else if(reference.at("schema_version")!="qt-equity-model-prior/v1" ||
                action_reads.value("path","")!="proved_action_free_prior")
                throw std::runtime_error("runtime_equity_prior_trace_path_mismatch");
            if(action_reads.at("effective_event_count")!=0 || eod_reads.value("path","")!="proved_desk_successor")
                throw std::runtime_error("runtime_equity_prior_trace_path_mismatch");
            const auto& rows=pending.equity_prior->observed.financial.at("live_results");
            if(!rows.is_array() || rows.size()!=1)
                throw std::runtime_error("runtime_equity_prior_financial_shape_invalid");
            const std::pair<const char*,const char*> fields[] = {
                {"previous_equity_exact","current_portfolio_value_exact"},
                {"previous_total_pnl_exact","total_pnl_exact"},
                {"previous_total_realized_pnl_exact","total_realized_pnl_exact"},
                {"previous_total_transaction_costs_exact","total_transaction_costs_exact"},
                {"initial_capital_exact","initial_capital_exact"}};
            for(const auto& [trace_key,source_key]:fields) {
                if(!eod_reads.contains(trace_key) || !eod_reads.at(trace_key).is_string() ||
                    !rows.at(0).contains(source_key) || !rows.at(0).at(source_key).is_string())
                    throw std::runtime_error("runtime_equity_prior_financial_missing");
                const auto traced=parse_qt_quantity_exact(eod_reads.at(trace_key).get<std::string>());
                const auto proved=parse_qt_quantity_exact(rows.at(0).at(source_key).get<std::string>());
                if(traced.is_error() || proved.is_error() || traced.value()!=proved.value())
                    throw std::runtime_error("runtime_equity_prior_financial_mismatch");
            }
        } else if(prior_reads.value("mode","")!="system_reference") {
            throw std::runtime_error("runtime_equity_prior_trace_requirement_mismatch");
        }
        pending.equity_final_consumption.emplace(projection);return Result<void>();
    }
    catch(const std::exception&) {pending.invalid_payload=true;
        return make_error<void>(ErrorCode::DATABASE_ERROR,"runtime_equity_consumption_attachment_failed");}
}

Result<void> PostgresDatabase::attach_live_consumption(
    const PublicationEvidenceToken& token, const ConsumptionProjection& projection) {
    if (!pending_publication_)
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_consumption_not_started");
    auto& pending = *pending_publication_;
    const bool same_marker = token.marker_ && pending.evidence_marker &&
        !token.marker_.owner_before(pending.evidence_marker) &&
        !pending.evidence_marker.owner_before(token.marker_);
    if (pending.strategy_id == "LIVE_EQUITY_MEAN_REVERSION" ||
        publication_transaction_ || pending.invalid_payload || !same_marker ||
        pending.evidence_requirement != PublicationEvidenceRequirement::RequiredFinalObservations ||
        !pending.inspection_capture_queued || pending.final_consumption ||
        !projection.document().is_object()) {
        pending.invalid_payload = true;
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_consumption_protocol_invalid");
    }
    try {
        pending.final_consumption.emplace(projection);
        return Result<void>();
    } catch (const std::exception&) {
        pending.invalid_payload = true;
        return make_error<void>(ErrorCode::DATABASE_ERROR,"runtime_consumption_attachment_failed");
    }
}

Result<void> PostgresDatabase::publish_live_publication() {
    if (!pending_publication_ || publication_transaction_)
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_publication_not_started");
    if (pending_publication_->parts != CompletePublication || pending_publication_->invalid_payload ||
        (pending_publication_->evidence_requirement ==
             PublicationEvidenceRequirement::RequiredFinalObservations &&
         (!pending_publication_->inspection_capture_queued ||
          (pending_publication_->strategy_id == "LIVE_EQUITY_MEAN_REVERSION" ?
            !pending_publication_->equity_final_consumption : !pending_publication_->final_consumption))) ||
        (pending_publication_->prior_requirement == PublicationPriorRequirement::VerifiedEquity &&
         !pending_publication_->equity_prior)) {
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
           !publishes_model(registry[0][1].as<std::string>()) || !registry[0][2].as<bool>() ||
           registry[0][3].as<std::string>() != pending_publication_->strategy_id)
            throw std::runtime_error("runtime_scope_changed");
        fence_live_write(txn,pending_publication_->strategy_id,pending_publication_->portfolio_id);
        // VerifiedEquity only: the prior re-proved in THIS transaction, kept for the binding row.
        std::optional<VerifiedEquityModelPrior> reproved_prior;
        if (pending_publication_->prior_requirement == PublicationPriorRequirement::VerifiedEquity) {
            const auto& captured=*pending_publication_->equity_prior;
            auto current=load_verified_equity_model_prior(txn,captured.selection,captured.owner);
            if(current.is_error() ||
                current.value().replay_reference != captured.observed.replay_reference ||
                current.value().financial != captured.observed.financial ||
                current.value().basis_positions != captured.observed.basis_positions)
                throw std::runtime_error("runtime_prior_changed");
            reproved_prior.emplace(current.value());
        }
        if (pending_publication_->intent_id) {
            auto approved = txn.exec("SELECT id FROM trading.runtime_intents WHERE id=$1 AND status='approved' "
                "AND registry_revision=$2 AND config_snapshot=$3::jsonb FOR UPDATE",
                pqxx::params{pending_publication_->intent_id,pending_publication_->registry_revision,
                             pending_publication_->snapshot.dump()});
            if(approved.size()!=1) throw std::runtime_error("runtime_approval_changed");
        }
        // EQ schema3 and immutable MODEL seed use one identity in this transaction.
        if(pending_publication_->strategy_id=="LIVE_EQUITY_MEAN_REVERSION" &&
            pending_publication_->publication_id.empty())
            pending_publication_->publication_id=txn.exec("SELECT gen_random_uuid()::text")[0][0].as<std::string>();
        publication_transaction_ = &txn;
        // Callbacks hold copies of all payload values. No market work happens here.
        for (const auto& write : pending_publication_->writes) {
            auto result = write();
            if(result.is_error() || pending_publication_->invalid_payload)
                throw std::runtime_error("runtime_publication_write_failed");
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
            const bool empty_owner=pending_publication_->strategy_id=="LIVE_EQUITY_MEAN_REVERSION" &&
                pending_publication_->fresh_system_components.empty() &&
                pending_publication_->fresh_empty_batches.size()==expected_members.size();
            if(!empty_owner && coverage[0][0].as<int>() != 2)
                throw std::runtime_error("runtime_scope_positions_incomplete");
            if(empty_owner) {
                auto count=txn.exec("SELECT count(*) FROM trading.positions WHERE portfolio_id=$1 AND date=$2::date AND portfolio_type='system'",
                    pqxx::params{pending_publication_->portfolio_id,pending_publication_->date});
                if(count[0][0].as<long long>()!=0)throw std::runtime_error("runtime_empty_owner_system_changed");
            }
        }
        // Uncontrolled legacy publications have no runtime_attempts row. Give
        // their source an identity only inside this atomic final transaction.
        if (pending_publication_->publication_id.empty())
            pending_publication_->publication_id =
                txn.exec("SELECT gen_random_uuid()::text")[0][0].as<std::string>();
        QtModelSeedPublication seed{pending_publication_->publication_id,
            pending_publication_->portfolio_id,pending_publication_->strategy_id,
            pending_publication_->date,pending_publication_->fresh_system_components,
            {},pending_publication_->producer_version,{}};
        const bool empty_owner=seed.strategy_id=="LIVE_EQUITY_MEAN_REVERSION" && seed.system_components.empty() &&
            pending_publication_->fresh_empty_batches.size()==expected_members.size();
        const auto seed_digest = empty_owner ? qt_digest_v1(nlohmann::json{{"seed_rows",nlohmann::json::array()}}) : qt_model_seed_digest(seed);
        if (seed_digest.is_error())
            throw std::runtime_error("runtime_model_seed_digest_invalid");
        seed.seed_digest = seed_digest.value();
        if (record_qt_model_seed_publication(seed).is_error())
            throw std::runtime_error("runtime_model_seed_record_failed");
        // Migration 024: the immutable MODEL -> verified-prior binding for this
        // publication, in the same transaction. Every other mode writes nothing here.
        if (pending_publication_->prior_requirement == PublicationPriorRequirement::VerifiedEquity) {
            if (!reproved_prior) throw std::runtime_error("runtime_prior_changed");
            auto binding=derive_qt_equity_model_prior_binding(seed.publication_id,seed.portfolio_id,
                seed.source_day,reproved_prior->replay_reference);
            if (binding.is_error() || record_qt_equity_model_prior_binding(txn,binding.value()).is_error())
                throw std::runtime_error("runtime_model_prior_binding_failed");
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
    if(pending_publication_ && pending_publication_->prior_requirement == PublicationPriorRequirement::VerifiedEquity) {
        if(!pending_publication_->equity_prior ||
            strategy_id != pending_publication_->strategy_id ||
            portfolio_id != pending_publication_->portfolio_id ||
            format_timestamp(date).substr(0,10) != pending_publication_->date ||
            !row.is_object() || !row.contains("engine_flags") || !row.at("engine_flags").is_object() ||
            !row.at("engine_flags").contains("equity_model_prior") ||
            row.at("engine_flags").at("equity_model_prior") != pending_publication_->equity_prior->observed.replay_reference) {
            pending_publication_->invalid_payload=true;
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_prior_replay_capture_invalid");
        }
    }
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
