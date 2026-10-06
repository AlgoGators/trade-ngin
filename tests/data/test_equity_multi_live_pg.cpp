#include <cstdlib>
#include <fstream>
#include <iterator>
#include "trade_ngin/apps/equity_multi_consumption.hpp"
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "trade_ngin/apps/equity_multi_live_runner.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/risk/risk_module_config.hpp"
#include "trade_ngin/strategy/equity_strategy_builder.hpp"

using namespace trade_ngin;
using namespace trade_ngin::apps;

namespace {

constexpr const char* kStrategy = "LIVE_EQUITY_ALPHA_BETA";
constexpr const char* kBookA = "INVESTOR_P2_A";
constexpr const char* kBookB = "INVESTOR_P2_B";
constexpr const char* kT2 = "2026-11-02";
constexpr const char* kT1 = "2026-11-03";
constexpr const char* kDay1 = "2026-11-04";
constexpr const char* kDay2 = "2026-11-05";
constexpr const char* kDay3 = "2026-11-06";

struct LiveDay {
    const char* source_day;
    const char* data_end_day;
};

constexpr std::array<LiveDay, 10> kLiveDays{{
    {"2026-11-04", "2026-11-03"},
    {"2026-11-05", "2026-11-04"},
    {"2026-11-06", "2026-11-05"},
    {"2026-11-09", "2026-11-06"},
    {"2026-11-10", "2026-11-09"},
    {"2026-11-11", "2026-11-10"},
    {"2026-11-12", "2026-11-11"},
    {"2026-11-13", "2026-11-12"},
    {"2026-11-16", "2026-11-13"},
    {"2026-11-17", "2026-11-16"},
}};

std::string owned_dsn() {
    const char* raw = std::getenv("TRADE_NGIN_121124_TEST_DSN");
    if (raw == nullptr)
        throw std::runtime_error("TRADE_NGIN_121124_TEST_DSN is required");
    const std::string dsn(raw);
    if (!dsn.starts_with("host=/tmp/algolens-repair-pg-") ||
        dsn.find(" dbname=trade_ngin_121124_p2_test ") == std::string::npos ||
        dsn.find("hostaddr") != std::string::npos ||
        dsn.find("service=") != std::string::npos) {
        throw std::runtime_error(
            "TRADE_NGIN_121124_TEST_DSN is not the owned disposable P2/P3 database");
    }
    return dsn;
}

void expect_ok(const Result<void>& result) {
    ASSERT_TRUE(result.is_ok()) << (result.is_error() ? result.error()->what() : "");
}

EquityLiveBookPlan make_plan(double alpha_exit = 0.1) {
    const auto definition = [](double exit_threshold) {
        return nlohmann::json{
            {"type", "MeanReversionStrategy"},
            {"enabled_live", true},
            {"default_allocation", 0.5},
            {"symbols", nlohmann::json::array({"SYN"})},
            {"config",
             {{"lookback_period", 3},
              {"entry_threshold", 0.5},
              {"exit_threshold", exit_threshold},
              {"risk_target", 0.1},
              {"position_size", 1.0},
              {"vol_lookback", 3},
              {"use_stop_loss", false},
              {"allow_fractional_shares", false}}}};
    };
    const std::vector<EquityStrategyEntry> entries{
        {"ALPHA", "MeanReversionStrategy", 0.5, definition(alpha_exit)},
        {"BETA", "MeanReversionStrategy", 0.5, definition(2.0)}};
    auto built = build_equity_live_book_plan(entries);
    if (built.is_error()) throw std::runtime_error(built.error()->what());
    return built.value();
}

AppConfig make_config(const std::string& portfolio_id, double capital, double alpha_exit=0.1) {
    AppConfig config;
    config.portfolio_id = portfolio_id;
    config.initial_capital = capital;
    config.opt_config.capital = capital;
    config.reserve_capital_pct = 0.1;
    config.benchmark_mode = "deferred";
    config.execution.commission_rate = 0.0005;
    config.execution.position_limit_live = 500.0;
    config.max_drawdown = 0.9;
    config.max_leverage = 10.0;
    config.risk_config.max_gross_leverage = 10.0;
    config.risk_config.max_net_leverage = 10.0;
    auto none = make_none_module(
        "synthetic issue 121/124 completion gate", "codex", "2026-10-01");
    if (none.is_error()) throw std::runtime_error(none.error()->what());
    config.risk_schema.portfolio = {none.value()};
    config.risk_schema.max_drawdown=config.max_drawdown;
    config.risk_schema.max_leverage=config.max_leverage;
    config.risk_schema.reporting={"carver","all_bars",.15,.10,.7,10,10,.99,252};
    config.risk_schema.attribution={{"_ruled_by","codex"},{"_ruled_on","2026-10-01"}};
    config.live.record_equity_policy_snapshot();
    for(const auto& sleeve:make_plan(alpha_exit).sleeves)
        {
        config.strategies_config[sleeve.source_id]=sleeve.definition;
        // Raw file weights intentionally require the existing plan normalization.
        config.strategies_config[sleeve.source_id]["default_allocation"]=2.0;
    }
    auto snap=build_runtime_trading_snapshot(config);
    if(snap.is_error())throw std::runtime_error(snap.error()->what());
    auto parsed=ConfigLoader::parse_trading_config(snap.value());
    if(parsed.is_error())throw std::runtime_error(parsed.error()->what());
    if(build_runtime_trading_snapshot(parsed.value()).value()!=snap.value())throw std::runtime_error("fixture snapshot roundtrip mismatch: "+nlohmann::json::diff(snap.value(),build_runtime_trading_snapshot(parsed.value()).value()).dump());
    return config;
}

Timestamp day(const std::string& value) {
    Timestamp parsed;
    if (!core::parse_utc_date(value, parsed))
        throw std::runtime_error("invalid synthetic date: " + value);
    return parsed;
}

class EquityMultiLivePg : public ::testing::Test {
protected:
    void SetUp() override {
        LoggerConfig logger_config;
        logger_config.destination = LogDestination::NONE;
        logger_config.min_level = LogLevel::FATAL;
        Logger::instance().initialize(logger_config);
        dsn_ = owned_dsn();
        prepare_substrate();
        reset_scope();
        db_ = std::make_shared<PostgresDatabase>(dsn_);
        expect_ok(db_->connect());
        auto& registry = InstrumentRegistry::instance();
        expect_ok(registry.initialize(db_));
        expect_ok(registry.load_equity_instruments({"SYN"}));
    }

    void TearDown() override { Logger::reset_for_tests(); }

    void prepare_substrate() {
        pqxx::connection connection(dsn_);
        pqxx::work tx(connection);
        tx.exec(R"SQL(
            CREATE SCHEMA IF NOT EXISTS equities_data;
            CREATE TABLE IF NOT EXISTS equities_data.ohlcv_1d(
                symbol text NOT NULL, time timestamptz NOT NULL,
                open double precision NOT NULL, high double precision NOT NULL,
                low double precision NOT NULL, close double precision NOT NULL,
                volume double precision NOT NULL, div_cash double precision,
                split_factor double precision, delisting_date date,
                PRIMARY KEY(symbol,time));
            CREATE TABLE IF NOT EXISTS equities_data.corporate_action(
                date text NOT NULL, action text NOT NULL, ticker text NOT NULL,
                value text, contraticker text, contraname text, name text);
            CREATE TABLE IF NOT EXISTS equities_data.ticker_aliases(
                historical_ticker text PRIMARY KEY, current_symbol text NOT NULL,
                effective_until text, note text);
            DELETE FROM equities_data.ohlcv_1d WHERE symbol='SYN';
            DELETE FROM equities_data.corporate_action WHERE ticker='SYN';
            INSERT INTO equities_data.ohlcv_1d
                (symbol,time,open,high,low,close,volume,div_cash,split_factor,delisting_date)
            VALUES
                ('SYN','2026-10-30T00:00:00Z',200,200,200,200,1000000,0,1,NULL),
                ('SYN','2026-11-02T00:00:00Z',200,200,200,200,1000000,0,1,NULL),
                ('SYN','2026-11-03T00:00:00Z',90,90,90,90,1000000,0,2,NULL),
                ('SYN','2026-11-04T00:00:00Z',80,80,80,80,1000000,0,1,NULL),
                ('SYN','2026-11-05T00:00:00Z',110,110,110,110,1000000,0,1,NULL),
                ('SYN','2026-11-06T00:00:00Z',100,100,100,100,1000000,0,1,NULL),
                ('SYN','2026-11-09T00:00:00Z',75,75,75,75,1000000,0,1,NULL),
                ('SYN','2026-11-10T00:00:00Z',115,115,115,115,1000000,0,1,NULL),
                ('SYN','2026-11-11T00:00:00Z',100,100,100,100,1000000,0,1,NULL),
                ('SYN','2026-11-12T00:00:00Z',70,70,70,70,1000000,0,1,NULL),
                ('SYN','2026-11-13T00:00:00Z',120,120,120,120,1000000,0,1,NULL),
                ('SYN','2026-11-16T00:00:00Z',100,100,100,100,1000000,0,1,NULL);
        )SQL");
        tx.commit();
    }

    void onboard_and_seed(const std::string& config_key,
                          const std::string& portfolio_id, double capital,
                          double quantity_scale) {
        const InvestorBookOnboarding request{
            config_key, portfolio_id, capital, "2026-10-30", {kStrategy},
            "issues-121-124-pg-gate"};
        auto onboarded = db_->onboard_investor_book(request);
        ASSERT_TRUE(onboarded.is_ok())
            << (onboarded.is_error() ? onboarded.error()->what() : "");

        pqxx::connection connection(dsn_);
        pqxx::work tx(connection);
        for (const std::string owner : {"ALPHA", "BETA"}) {
            const double quantity =
                (owner == "ALPHA" ? 10.0 : 20.0) * quantity_scale;
            for (const std::string source_day : {kT2, kT1}) {
                tx.exec(
                    "INSERT INTO trading.positions "
                    "(symbol,quantity,average_price,daily_unrealized_pnl,"
                    "daily_realized_pnl,last_update,updated_at,strategy_id,"
                    "strategy_name,date,portfolio_id,portfolio_type) "
                    "VALUES('SYN',$1,100,0,0,$2::date,$2::date,$3,$4,$2::date,$5,'system') "
                    "ON CONFLICT(portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type) "
                    "DO UPDATE SET quantity=excluded.quantity,average_price=100,"
                    "daily_unrealized_pnl=0,daily_realized_pnl=0,last_update=excluded.last_update",
                    pqxx::params{quantity, source_day, kStrategy, owner, portfolio_id});
            }
        }
        for (const std::string source_day : {kT2, kT1}) {
            tx.exec(
                "INSERT INTO trading.live_results "
                "(portfolio_id,strategy_id,date,portfolio_type,current_portfolio_value,"
                "total_pnl,total_realized_pnl,total_unrealized_pnl,total_transaction_costs,"
                "daily_realized_pnl,daily_unrealized_pnl,daily_transaction_costs,"
                "gross_notional,net_notional,margin_posted,cash_available) "
                "VALUES($1,$2,$3::date,'system',$4::numeric,0,0,0,0,0,0,0,"
                "3000,3000,3000,$4::numeric-3000) "
                "ON CONFLICT(portfolio_id,strategy_id,date,portfolio_type) DO UPDATE SET "
                "current_portfolio_value=$4::numeric,total_pnl=0,total_realized_pnl=0,"
                "total_unrealized_pnl=0,total_transaction_costs=0,daily_realized_pnl=0,"
                "daily_unrealized_pnl=0,daily_transaction_costs=0,gross_notional=3000,"
                "net_notional=3000,margin_posted=3000,cash_available=$4::numeric-3000",
                pqxx::params{portfolio_id, kStrategy, source_day, capital});
            tx.exec(
                "INSERT INTO trading.equity_curve "
                "(strategy_id,portfolio_id,timestamp,equity,portfolio_type) "
                "VALUES($1,$2,$3::date,$4,'system') "
                "ON CONFLICT(portfolio_id,strategy_id,timestamp,portfolio_type) "
                "DO UPDATE SET equity=$4",
                pqxx::params{kStrategy, portfolio_id, source_day, capital});
        }
        tx.commit();
    }

    void run_day(const std::string& portfolio_id, double capital,
                 const std::string& source_day,
                 const std::string& data_end_day,
                 double alpha_exit = 0.1) {
        auto result = run_multi_sleeve_equity_live_day(
            make_config(portfolio_id, capital, alpha_exit), make_plan(alpha_exit), db_,
            InstrumentRegistry::instance(),
            HolidayChecker(std::string(TRADE_NGIN_SOURCE_DIR) +
                           "/include/trade_ngin/core/holidays.json"),
            day(source_day), day("2026-10-30"), day(data_end_day), true);
        ASSERT_TRUE(result.is_ok())
            << "portfolio=" << portfolio_id << " source_day=" << source_day
            << " data_end_day=" << data_end_day << " error="
            << (result.is_error() ? result.error()->what() : "");
        pqxx::connection connection(dsn_);pqxx::work tx(connection);
        const auto rows=tx.exec("SELECT m.portfolio_config->'config_inspection',i.config_snapshot,"
            "p.publication_id::text,s.publication_id,s.lifecycle FROM trading.live_run_metadata m "
            "JOIN trading.run_inputs i USING(portfolio_id,strategy_id,date) "
            "JOIN trading.investor_book_publications p ON p.portfolio_id=m.portfolio_id AND p.source_day=m.date "
            "JOIN trading.live_config_attempt_safety s ON s.attempt_id=m.portfolio_config->'config_inspection'->'identity'->>'config_attempt_id' "
            "WHERE m.portfolio_id=$1 AND m.date=$2::date",pqxx::params{portfolio_id,source_day});
        ASSERT_EQ(rows.size(),1u);
        const auto capture=nlohmann::json::parse(rows[0][0].as<std::string>());
        const auto snapshot=nlohmann::json::parse(rows[0][1].as<std::string>());
        EXPECT_EQ(capture.at("publication_schema_version"),5);
        EXPECT_EQ(capture.at("identity").at("publication_id"),rows[0][2].as<std::string>());
        EXPECT_EQ(rows[0][2].as<std::string>(),rows[0][3].as<std::string>());
        EXPECT_EQ(rows[0][4].as<std::string>(),"published");
        EXPECT_EQ(capture.at("supplied").at("effective_snapshot"),snapshot);
        EXPECT_EQ(snapshot.at("strategies").at("ALPHA").at("default_allocation"),2.0);
        EXPECT_TRUE(validate_live_config_projection_for_publication(capture.at("supplied")));
        EXPECT_TRUE(validate_equity_multi_consumption(capture.at("equity_multi_consumption"),snapshot,kStrategy,
            portfolio_id,source_day,capture.at("configuration_selection").at("effective_sha256")));
        // Mutate actual captured invocations, including real no-charge/history branches.
        const auto& observed=capture.at("equity_multi_consumption");
        const auto rejects=[&](const nlohmann::json& malformed){
            EXPECT_FALSE(validate_equity_multi_consumption(malformed,snapshot,kStrategy,portfolio_id,source_day,
                capture.at("configuration_selection").at("effective_sha256")));
        };
        auto malformed=observed;malformed["portfolio_invocation"]["passes"][0]["risk_helper"]="fabricated_call";rejects(malformed);
        malformed=observed;malformed["portfolio_invocation"]["passes"][0]["risk"]["manager_source"]={{"unbounded","object"}};rejects(malformed);
        for(const auto& [owner,invocation]:observed.at("strategy_invocations").items()) {
            for(const auto& [symbol,unused]:invocation.at("symbols").items()) {
                (void)unused;malformed=observed;
                malformed["strategy_invocations"][owner]["symbols"][symbol]["reads"]={{"invented_observation",true}};rejects(malformed);
            }
        }
        for(const char* charges:{"strategy_charges","compatibility_charges"}) {
            if(!observed.at("portfolio_invocation").is_null() && !observed.at("portfolio_invocation").at(charges).empty()) {
                malformed=observed;malformed["portfolio_invocation"][charges][0]["reads"].erase("quantity");rejects(malformed);
                malformed=observed;malformed["portfolio_invocation"][charges][0]["index"]=-1;rejects(malformed);
                malformed=observed;malformed["portfolio_invocation"]["skip_execution_generation"]=true;rejects(malformed);
            }
        }
        for(const char* field:{"full_run_certification","coverage","source_to_storage_owners","portfolio_invocation"}) {
            auto malformed=capture.at("equity_multi_consumption");malformed[field]=true;
            EXPECT_FALSE(validate_equity_multi_consumption(malformed,snapshot,kStrategy,portfolio_id,source_day,
                capture.at("configuration_selection").at("effective_sha256")));
        }
    }

    long scalar_long(const std::string& query) const {
        pqxx::connection connection(dsn_);
        pqxx::read_transaction tx(connection);
        return tx.exec(query)[0][0].as<long>();
    }

    double scalar_double(const std::string& query) const {
        pqxx::connection connection(dsn_);
        pqxx::read_transaction tx(connection);
        return tx.exec(query)[0][0].as<double>();
    }

    std::string scalar_text(const std::string& query) const {
        pqxx::connection connection(dsn_);
        pqxx::read_transaction tx(connection);
        return tx.exec(query)[0][0].as<std::string>();
    }

    void run_chain() {
        for (const auto& live_day : kLiveDays) {
            run_day(kBookA, 100000.0, live_day.source_day, live_day.data_end_day);
            run_day(kBookB, 200000.0, live_day.source_day, live_day.data_end_day);
        }
    }

    std::unordered_map<std::string, std::string> normalized_scope_digests() const {
        pqxx::connection connection(dsn_);
        pqxx::read_transaction tx(connection);
        const auto result = tx.exec(R"SQL(
            WITH scope AS (
                SELECT ARRAY['INVESTOR_P2_A','INVESTOR_P2_B']::text[] books
            ), rows(kind, value) AS (
                SELECT 'positions', (to_jsonb(p) - 'updated_at')::text
                  FROM trading.positions p, scope
                 WHERE p.portfolio_id = ANY(scope.books)
                UNION ALL
                SELECT 'executions',
                       (to_jsonb(e) - 'created_at' - 'exec_id' - 'order_id')::text
                  FROM trading.executions e, scope
                 WHERE e.portfolio_id = ANY(scope.books)
                UNION ALL
                SELECT 'signals', to_jsonb(s)::text
                  FROM trading.signals s, scope
                 WHERE s.portfolio_id = ANY(scope.books)
                UNION ALL
                SELECT 'live_results',
                       (to_jsonb(r) - 'id' - 'created_at')::text
                  FROM trading.live_results r, scope
                 WHERE r.portfolio_id = ANY(scope.books)
                UNION ALL
                SELECT 'equity_curve', (to_jsonb(e) - 'id')::text
                  FROM trading.equity_curve e, scope
                 WHERE e.portfolio_id = ANY(scope.books)
                UNION ALL
                SELECT 'corp_action',
                       (to_jsonb(c) - 'id' - 'applied_at')::text
                  FROM trading.corp_action_applied c, scope
                 WHERE c.portfolio_id = ANY(scope.books)
                UNION ALL
                SELECT 'run_inputs',
                       (to_jsonb(i) - 'id' - 'recorded_at')::text
                  FROM trading.run_inputs i, scope
                 WHERE i.portfolio_id = ANY(scope.books)
                UNION ALL
                SELECT 'live_metadata',
                       (to_jsonb(m) - 'id' - 'created_at'
                        #- '{portfolio_config,config_inspection,captured_at}'
                        #- '{portfolio_config,config_inspection,publication_recorded_at}'
                        #- '{portfolio_config,config_inspection,identity,capture_id}'
                        #- '{portfolio_config,config_inspection,identity,publication_id}'
                        #- '{portfolio_config,config_inspection,identity,config_attempt_id}')::text
                  FROM trading.live_run_metadata m, scope
                 WHERE m.portfolio_id = ANY(scope.books)
                UNION ALL
                SELECT 'publication',
                       (to_jsonb(p) - 'publication_id' - 'book_id' - 'published_at' - 'content_digest')::text
                  FROM trading.investor_book_publications p, scope
                 WHERE p.portfolio_id = ANY(scope.books)
            )
            SELECT kind, md5(string_agg(value, E'\n' ORDER BY value))
              FROM rows GROUP BY kind ORDER BY kind
        )SQL");
        std::unordered_map<std::string, std::string> digests;
        for (const auto& row : result)
            digests.emplace(row[0].as<std::string>(), row[1].as<std::string>());
        return digests;
    }

    void reset_scope() {
        pqxx::connection connection(dsn_);
        pqxx::work tx(connection);
        tx.exec(R"SQL(
            DELETE FROM trading.corp_action_applied
             WHERE portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B');
            DELETE FROM trading.run_inputs
             WHERE portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B');
            DELETE FROM trading.live_run_metadata
             WHERE portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B');
            DELETE FROM trading.equity_curve
             WHERE portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B');
            DELETE FROM trading.live_results
             WHERE portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B');
            DELETE FROM trading.signals
             WHERE portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B');
            DELETE FROM trading.executions
             WHERE portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B');
            DELETE FROM trading.positions
             WHERE portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B');
            -- These rows are intentionally immutable to application DML. This
            -- is an owned disposable database, so reset the test registry with
            -- TRUNCATE only after fenced operational rows have been removed.
            TRUNCATE TABLE trading.live_config_attempt_selections CASCADE;
            TRUNCATE TABLE trading.investor_book_publications,
                           trading.investor_book_strategies,
                           trading.investor_books,
                           trading.strategy_trading_days_metadata CASCADE;
        )SQL");
        tx.commit();
    }

    std::string dsn_;
    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(EquityMultiLivePg,
       P2AndP3PublishTwoOwnerSystemBooksAtomicallyAndRerunSafely) {
    onboard_and_seed("investor_p2_a", kBookA, 100000.0, 1.0);
    onboard_and_seed("investor_p2_b", kBookB, 200000.0, 2.0);

    EXPECT_EQ(scalar_long(
        "SELECT count(DISTINCT book_id) FROM trading.investor_books WHERE "
        "portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B')"), 2);
    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.strategy_trading_days_metadata WHERE "
        "portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B') "
        "AND strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND live_start_date='2026-10-30'"), 2);

    // A fully scoped delete cannot cross investor books. Restore the deleted
    // synthetic seed before running so the financial path remains identical.
    {
        pqxx::connection connection(dsn_);
        pqxx::work tx(connection);
        tx.exec("DELETE FROM trading.positions WHERE portfolio_id=$1 AND strategy_id=$2 "
                "AND strategy_name='ALPHA' AND date=$3::date AND symbol='SYN' "
                "AND portfolio_type='system'",
                pqxx::params{kBookA, kStrategy, kT2});
        tx.commit();
    }
    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.positions WHERE portfolio_id='INVESTOR_P2_A' "
        "AND strategy_id='LIVE_EQUITY_ALPHA_BETA' AND date='2026-11-02'"), 1);
    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.positions WHERE portfolio_id='INVESTOR_P2_B' "
        "AND strategy_id='LIVE_EQUITY_ALPHA_BETA' AND date='2026-11-02'"), 2);
    onboard_and_seed("investor_p2_a", kBookA, 100000.0, 1.0);

    run_day(kBookA, 100000.0, kDay1, kT1);
    run_day(kBookB, 200000.0, kDay1, kT1);

    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.investor_book_publications WHERE "
        "portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B') AND source_day='2026-11-04'"), 2);
    EXPECT_EQ(scalar_long(
        "SELECT count(DISTINCT publication_id) FROM trading.investor_book_publications WHERE "
        "portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B') AND source_day='2026-11-04'"), 2);
    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.investor_book_publications WHERE "
        "portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B') AND model_stream<>'system'"), 0);
    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.positions WHERE portfolio_id IN "
        "('INVESTOR_P2_A','INVESTOR_P2_B') AND portfolio_type='qt'"), 0);

    // The T-1 split was independently applied and durably audited for both owners.
    EXPECT_DOUBLE_EQ(scalar_double(
        "SELECT sum(quantity) FROM trading.positions WHERE portfolio_id='INVESTOR_P2_A' "
        "AND strategy_id='LIVE_EQUITY_ALPHA_BETA' AND date='2026-11-03'"), 60.0);
    EXPECT_DOUBLE_EQ(scalar_double(
        "SELECT sum(daily_unrealized_pnl) FROM trading.positions WHERE "
        "portfolio_id='INVESTOR_P2_A' AND strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND date='2026-11-03'"), 2400.0);
    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.corp_action_applied WHERE portfolio_id IN "
        "('INVESTOR_P2_A','INVESTOR_P2_B') AND strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND symbol='SYN' AND action_type='SPLIT' AND ex_date='2026-11-03'"), 4);

    // ALPHA increases a long while BETA exits. Only their net reaches the account.
    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.executions WHERE portfolio_id='INVESTOR_P2_A' "
        "AND strategy_id='LIVE_EQUITY_ALPHA_BETA' AND date='2026-11-04' "
        "AND strategy_name='ALPHA' AND side='BUY'"), 1);
    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.executions WHERE portfolio_id='INVESTOR_P2_A' "
        "AND strategy_id='LIVE_EQUITY_ALPHA_BETA' AND date='2026-11-04' "
        "AND strategy_name='BETA' AND side='SELL'"), 1);
    EXPECT_EQ(scalar_long(
        "SELECT jsonb_array_length(engine_flags->'account_executions') FROM trading.run_inputs "
        "WHERE portfolio_id='INVESTOR_P2_A' AND strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND date='2026-11-04'"), 1);

    // W12 accounting identities hold against the durable owner rows and fills.
    EXPECT_NEAR(scalar_double(
        "SELECT abs(r.daily_realized_pnl-p.realized) FROM trading.live_results r "
        "CROSS JOIN LATERAL (SELECT coalesce(sum(daily_realized_pnl),0) realized "
        "FROM trading.positions p WHERE p.portfolio_id=r.portfolio_id "
        "AND p.strategy_id=r.strategy_id AND p.date=r.date AND p.portfolio_type='system') p "
        "WHERE r.portfolio_id='INVESTOR_P2_A' AND r.strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND r.date='2026-11-04' AND r.portfolio_type='system'"), 0.0, 1e-6);
    EXPECT_NEAR(scalar_double(
        "SELECT abs(r.total_unrealized_pnl-p.unrealized) FROM trading.live_results r "
        "CROSS JOIN LATERAL (SELECT coalesce(sum(daily_unrealized_pnl),0) unrealized "
        "FROM trading.positions p WHERE p.portfolio_id=r.portfolio_id "
        "AND p.strategy_id=r.strategy_id AND p.date=r.date AND p.portfolio_type='system') p "
        "WHERE r.portfolio_id='INVESTOR_P2_A' AND r.strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND r.date='2026-11-04' AND r.portfolio_type='system'"), 0.0, 1e-6);
    EXPECT_NEAR(scalar_double(
        "SELECT abs(r.daily_transaction_costs-e.costs) FROM trading.live_results r "
        "CROSS JOIN LATERAL (SELECT coalesce(sum(total_transaction_costs-netting_adjustment),0) costs "
        "FROM trading.executions e WHERE e.portfolio_id=r.portfolio_id "
        "AND e.strategy_id=r.strategy_id AND e.date=r.date AND e.portfolio_type='system') e "
        "WHERE r.portfolio_id='INVESTOR_P2_A' AND r.strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND r.date='2026-11-04' AND r.portfolio_type='system'"), 0.0, 1e-6);
    EXPECT_NEAR(scalar_double(
        "SELECT abs(total_pnl-(total_realized_pnl-total_transaction_costs+total_unrealized_pnl)) "
        "FROM trading.live_results WHERE portfolio_id='INVESTOR_P2_A' "
        "AND strategy_id='LIVE_EQUITY_ALPHA_BETA' AND date='2026-11-04'"), 0.0, 1e-6);
    EXPECT_NEAR(scalar_double(
        "SELECT abs(r.current_portfolio_value-(b.initial_capital+r.total_pnl)) "
        "FROM trading.live_results r JOIN trading.investor_books b USING(portfolio_id) "
        "WHERE r.portfolio_id='INVESTOR_P2_A' AND r.strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND r.date='2026-11-04'"), 0.0, 1e-6);

    const double a_alpha = scalar_double(
        "SELECT quantity FROM trading.positions WHERE portfolio_id='INVESTOR_P2_A' "
        "AND strategy_id='LIVE_EQUITY_ALPHA_BETA' AND strategy_name='ALPHA' "
        "AND date='2026-11-04' AND symbol='SYN'");
    const double b_alpha = scalar_double(
        "SELECT quantity FROM trading.positions WHERE portfolio_id='INVESTOR_P2_B' "
        "AND strategy_id='LIVE_EQUITY_ALPHA_BETA' AND strategy_name='ALPHA' "
        "AND date='2026-11-04' AND symbol='SYN'");
    ASSERT_NE(a_alpha, 0.0);
    EXPECT_LE(std::abs(b_alpha - 2.0 * a_alpha), 1.0);

    // Continue both books through two more live days. This exercises the
    // open/hold-close continuation path, including atomic T-1 finalization on
    // each subsequent publication, rather than proving only an isolated day.
    run_day(kBookA, 100000.0, kDay2, kDay1);
    run_day(kBookB, 200000.0, kDay2, kDay1);
    run_day(kBookA, 100000.0, kDay3, kDay2);
    run_day(kBookB, 200000.0, kDay3, kDay2);

    for (size_t index = 3; index < kLiveDays.size(); ++index) {
        run_day(kBookA, 100000.0, kLiveDays[index].source_day,
                kLiveDays[index].data_end_day);
        run_day(kBookB, 200000.0, kLiveDays[index].source_day,
                kLiveDays[index].data_end_day);
    }

    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.investor_book_publications WHERE "
        "portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B') AND source_day BETWEEN "
        "'2026-11-04' AND '2026-11-17'"), 20);
    EXPECT_EQ(scalar_long(
        "SELECT count(DISTINCT publication_id) FROM trading.investor_book_publications WHERE "
        "portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B') AND source_day BETWEEN "
        "'2026-11-04' AND '2026-11-17'"), 20);
    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.live_results WHERE portfolio_id IN "
        "('INVESTOR_P2_A','INVESTOR_P2_B') AND strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND date BETWEEN '2026-11-04' AND '2026-11-17' AND portfolio_type='system'"), 20);
    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.executions WHERE portfolio_id IN "
        "('INVESTOR_P2_A','INVESTOR_P2_B') AND strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND date='2026-11-05' AND side='BUY'"), 4);
    EXPECT_EQ(scalar_long(
        "SELECT count(*) FROM trading.executions WHERE portfolio_id IN "
        "('INVESTOR_P2_A','INVESTOR_P2_B') AND strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND date='2026-11-06' AND side='SELL'"), 4);
    EXPECT_NEAR(scalar_double(
        "SELECT coalesce(sum(abs(quantity)),0) FROM trading.positions WHERE "
        "portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B') "
        "AND strategy_id='LIVE_EQUITY_ALPHA_BETA' AND date='2026-11-06' "
        "AND portfolio_type='system'"), 0.0, 1e-6);
    EXPECT_NEAR(scalar_double(
        "SELECT coalesce(max(abs(r.daily_realized_pnl-p.realized)),0) FROM "
        "trading.live_results r CROSS JOIN LATERAL (SELECT "
        "coalesce(sum(daily_realized_pnl),0) realized FROM trading.positions p "
        "WHERE p.portfolio_id=r.portfolio_id AND p.strategy_id=r.strategy_id "
        "AND p.date=r.date AND p.portfolio_type='system') p WHERE "
        "r.portfolio_id IN ('INVESTOR_P2_A','INVESTOR_P2_B') "
        "AND r.strategy_id='LIVE_EQUITY_ALPHA_BETA' AND r.date BETWEEN "
        "'2026-11-04' AND '2026-11-17' AND r.portfolio_type='system'"), 0.0, 1e-6);

    const std::string publication_before = scalar_text(
        "SELECT publication_id::text||':'||content_digest FROM "
        "trading.investor_book_publications WHERE portfolio_id='INVESTOR_P2_A' "
        "AND source_day='2026-11-17'");
    run_day(kBookA, 100000.0, kLiveDays.back().source_day,
            kLiveDays.back().data_end_day);
    EXPECT_EQ(scalar_text(
        "SELECT publication_id::text||':'||content_digest FROM "
        "trading.investor_book_publications WHERE portfolio_id='INVESTOR_P2_A' "
        "AND source_day='2026-11-17'"), publication_before);

    // A changed same-day computation must fail closed and preserve the publication.
    auto conflict = run_multi_sleeve_equity_live_day(
        make_config(kBookA, 100000.0,0.2), make_plan(0.2), db_,
        InstrumentRegistry::instance(),
        HolidayChecker(std::string(TRADE_NGIN_SOURCE_DIR) +
                       "/include/trade_ngin/core/holidays.json"),
        day(kLiveDays.back().source_day), day("2026-10-30"),
        day(kLiveDays.back().data_end_day), true);
    ASSERT_TRUE(conflict.is_error());
    EXPECT_EQ(scalar_text(
        "SELECT publication_id::text||':'||content_digest FROM "
        "trading.investor_book_publications WHERE portfolio_id='INVESTOR_P2_A' "
        "AND source_day='2026-11-17'"), publication_before);

    // The ten-day chain is deterministic from a clean book scope. Recreate
    // onboarding, anchors, seeds, and every daily publication, then compare
    // all durable financial content while excluding generated UUID/timestamp
    // columns that are deliberately not replay identities.
    const auto first_digests = normalized_scope_digests();
    reset_scope();
    onboard_and_seed("investor_p2_a", kBookA, 100000.0, 1.0);
    onboard_and_seed("investor_p2_b", kBookB, 200000.0, 2.0);
    run_chain();
    const auto replay_digests = normalized_scope_digests();
    EXPECT_EQ(replay_digests.size(), first_digests.size());
    for (const auto& [table, digest] : first_digests) {
        ASSERT_EQ(replay_digests.count(table), 1u) << table;
        EXPECT_EQ(replay_digests.at(table), digest) << table;
    }
}


TEST_F(EquityMultiLivePg, NonTradingCompositePublishesExplicitSkippedCoverage) {
    onboard_and_seed("investor_p2_a",kBookA,100000.0,1.0);
    run_day(kBookA,100000.0,kDay1,kT1);
    run_day(kBookA,100000.0,kDay2,kDay1);
    run_day(kBookA,100000.0,kDay3,kDay2);
    run_day(kBookA,100000.0,"2026-11-07",kDay3);
    EXPECT_EQ(scalar_text("SELECT portfolio_config->'config_inspection'->'equity_multi_consumption'->'coverage'->>'primary' FROM trading.live_run_metadata WHERE portfolio_id='INVESTOR_P2_A' AND date='2026-11-07'"),"skipped_non_trading_day");
    EXPECT_EQ(scalar_text("SELECT portfolio_config->'config_inspection'->'equity_multi_consumption'->>'portfolio_invocation' IS NULL FROM trading.live_run_metadata WHERE portfolio_id='INVESTOR_P2_A' AND date='2026-11-07'"),"t");
}


TEST_F(EquityMultiLivePg, InvestorPublicationWrapperSurvivesGuardedRollback) {
    onboard_and_seed("investor_p2_a",kBookA,100000.0,1.0);
    run_day(kBookA,100000.0,kDay1,kT1);
    const auto sql_file=[](const char* name){std::ifstream in(std::string(TRADE_NGIN_SOURCE_DIR)+"/migrations/"+name);return std::string(std::istreambuf_iterator<char>(in),{});};
    const auto migration=sql_file("031_live_config_overrides.sql");
    const auto rollback=sql_file("031_live_config_overrides_rollback.sql");
    pqxx::connection connection(dsn_);
    {pqxx::nontransaction tx(connection);EXPECT_THROW(tx.exec(rollback),pqxx::sql_error);tx.exec("ROLLBACK");}
    const auto before=scalar_text("SELECT publication_id::text FROM trading.investor_book_publications WHERE portfolio_id='INVESTOR_P2_A'");
    {pqxx::nontransaction tx(connection);tx.exec("TRUNCATE trading.live_config_attempt_selections CASCADE");tx.exec(rollback);}
    const auto call="SELECT trading.publish_system_investor_day('INVESTOR_P2_A','LIVE_EQUITY_ALPHA_BETA','2026-11-04','fixture')::text";
    const auto invoke4=[&](){pqxx::work tx(connection);auto value=tx.exec(call)[0][0].as<std::string>();tx.commit();return value;};
    EXPECT_EQ(invoke4(),before);
    {pqxx::nontransaction tx(connection);tx.exec(migration);tx.exec(migration);}
    EXPECT_EQ(invoke4(),before);
    EXPECT_EQ(scalar_long("SELECT count(*) FROM trading.investor_book_publications WHERE portfolio_id='INVESTOR_P2_A'"),1);
}

TEST_F(EquityMultiLivePg, InsufficientHistoryPreservesPartialReadsWithoutCharges) {
    onboard_and_seed("investor_p2_a",kBookA,100000.0,0.0);
    auto config=make_config(kBookA,100000.0);
    for(auto& [owner,definition]:config.strategies_config.items()) {
        (void)owner;definition["config"]["lookback_period"]=20;
    }
    const auto selected=collect_enabled_equity_strategies(config.strategies_config,"enabled_live");
    ASSERT_TRUE(selected.is_ok());const auto plan=build_equity_live_book_plan(selected.value());ASSERT_TRUE(plan.is_ok());
    auto result=run_multi_sleeve_equity_live_day(config,plan.value(),db_,InstrumentRegistry::instance(),
        HolidayChecker(std::string(TRADE_NGIN_SOURCE_DIR)+"/include/trade_ngin/core/holidays.json"),
        day(kDay1),day("2026-10-30"),day(kT1),true);
    ASSERT_TRUE(result.is_ok())<<(result.is_error()?result.error()->what():"");
    const auto capture=nlohmann::json::parse(scalar_text("SELECT (portfolio_config->'config_inspection')::text FROM trading.live_run_metadata WHERE portfolio_id='INVESTOR_P2_A'"));
    const auto& observed=capture.at("equity_multi_consumption");
    EXPECT_TRUE(validate_equity_multi_consumption(observed,capture.at("supplied").at("effective_snapshot"),
        kStrategy,kBookA,kDay1,capture.at("configuration_selection").at("effective_sha256")));
    for(const auto* key:{"strategy_charges","compatibility_charges"})EXPECT_TRUE(observed.at("portfolio_invocation").at(key).empty());
    for(const auto& [owner,invocation]:observed.at("strategy_invocations").items()) {
        (void)owner;const auto& row=invocation.at("symbols").at("SYN");
        EXPECT_EQ(row.at("reads").at("lookback_period"),20);
        EXPECT_FALSE(row.at("reads").contains("entry_threshold"));
        EXPECT_FALSE(row.at("reads").contains("capital_allocation"));
        EXPECT_TRUE(row.at("observed_state").empty());
    }
}

TEST_F(EquityMultiLivePg, HouseNonemptyPublishesButLegacyEmptyHouseBoundaryStaysAtomic) {
    onboard_and_seed("investor_p2_a",kBookA,100000.0,1.0);
    {
        pqxx::connection connection(dsn_);pqxx::work tx(connection);
        tx.exec("TRUNCATE trading.investor_books,trading.runtime_intents CASCADE");
        tx.exec("INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id) VALUES('task4-composite',$1,$2) ON CONFLICT DO NOTHING",pqxx::params{kStrategy,kBookA});
        const auto snapshot=build_runtime_trading_snapshot(make_config(kBookA,100000.0)).value();
        tx.exec("INSERT INTO trading.runtime_intents(registry_id,portfolio_id,engine_strategy_id,action,registry_revision,config_snapshot,status,requested_by,request_reason,approved_by,approval_reason,approved_at) VALUES('task4-composite',$1,$2,'run',0,$3::jsonb,'approved','fixture-submit','run','fixture-approve','run',now())",pqxx::params{kBookA,kStrategy,snapshot.dump()});
        tx.commit();
    }
    const auto invoke=[&](const char* date,const char* previous){
        return run_multi_sleeve_equity_live_day(make_config(kBookA,100000.0),make_plan(),db_,
            InstrumentRegistry::instance(),HolidayChecker(std::string(TRADE_NGIN_SOURCE_DIR)+"/include/trade_ngin/core/holidays.json"),
            day(date),day("2026-10-30"),day(previous),true);
    };
    expect_ok(invoke(kDay1,kT1));expect_ok(invoke(kDay2,kDay1));expect_ok(invoke(kDay3,kDay2));
    EXPECT_EQ(scalar_long("SELECT count(*) FROM trading.qt_model_seed_publications WHERE portfolio_id='INVESTOR_P2_A'"),3);
    const auto before=scalar_text("SELECT trading.live_config_financial_state('LIVE_EQUITY_ALPHA_BETA','INVESTOR_P2_A')::text");
    auto empty=invoke("2026-11-09",kDay3);ASSERT_TRUE(empty.is_error());
    EXPECT_EQ(scalar_text("SELECT trading.live_config_financial_state('LIVE_EQUITY_ALPHA_BETA','INVESTOR_P2_A')::text"),before);
    EXPECT_EQ(scalar_long("SELECT count(*) FROM trading.run_inputs WHERE portfolio_id='INVESTOR_P2_A' AND date='2026-11-09'"),0);
    EXPECT_EQ(scalar_long("SELECT count(*) FROM trading.qt_empty_model_owner_publications WHERE portfolio_id='INVESTOR_P2_A'"),0);
    EXPECT_EQ(scalar_long("SELECT count(*) FROM trading.live_run_metadata WHERE portfolio_id='INVESTOR_P2_A' AND portfolio_config->'config_inspection'->>'profile'='live_equity_mean_reversion'"),0);
}



}  // namespace
