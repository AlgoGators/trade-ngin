// Owned PostgreSQL only. No engine, strategies, config loader, network or email.
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace trade_ngin;
int main(int argc, char** argv) {
    try {
        AppConfig synthetic;
        synthetic.portfolio_id = "BOOK";
        synthetic.strategies_config = {{"TREND",{{"enabled_live",true},
            {"default_allocation",1.0},{"type","TrendFollowingStrategy"}}}};
        const std::string mode = argc == 2 ? argv[1] : "";
        const bool missing_member = mode.find("missing_member") == 0;
        const std::string engine = missing_member ? "LIVE_MISSING_TREND" : "LIVE_TREND";
        if (missing_member) {
            synthetic.strategies_config["TREND"]["default_allocation"] = 0.5;
            synthetic.strategies_config["MISSING"] = synthetic.strategies_config["TREND"];
        }
        const auto snapshot = build_runtime_trading_snapshot(synthetic);
        if (snapshot.is_error()) return 19;
        if (argc == 2 && std::string(argv[1]) == "snapshot") {
            std::cout << "RUNTIME_SNAPSHOT=" << snapshot.value().dump() << '\n';
            return 0;
        }
        const char* raw = std::getenv("ALGOLENS_TEST_DB");
        if (!raw || argc != 2) return 2;
        std::string dsn(raw);
        if (dsn.rfind("host=/tmp/algolens-repair-pg-", 0) != 0 ||
            dsn.find(" dbname=algolens_test_") == std::string::npos ||
            dsn.find("hostaddr") != std::string::npos || dsn.find("service=") != std::string::npos)
            return 2;
        PostgresDatabase db(dsn);
        if (db.connect().is_error()) return 3;
        const bool controlled = mode.find("controlled") != std::string::npos;
        auto date = std::chrono::system_clock::from_time_t(1790035200);
        auto started = db.begin_live_publication(engine, mode == "membership" ? "SECOND" : "BOOK", date,
            snapshot.value(), controlled, "local-test");
        if (mode == "legacy_changed" || mode == "membership") {
            if (!started.is_error()) return 20;
            std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
            return 0;
        }
        if (mode == "stop_controlled") {
            if (started.is_error() || !started.value()) return 21;
            std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
            return 0;
        }
        if (started.is_error() || started.value()) return 4;
        if (mode.find("historical_") == 0) {
            pqxx::connection other(dsn);
            {
                pqxx::work editor(other);
                if (mode.find("historical_aba") == 0) {
                    editor.exec("UPDATE trading.strategy_registry SET lifecycle='retired';"
                                "UPDATE trading.strategy_registry SET lifecycle='live'");
                } else if (mode == "historical_superseded_controlled") {
                    editor.exec("UPDATE trading.runtime_intents SET status='superseded' WHERE status='approved'");
                }
                editor.commit();
            }
            auto raw_result = db.execute_scoped_live_update(
                "UPDATE trading.positions SET quantity=42 WHERE date='2026-09-21'",
                engine,"BOOK");
            std::vector<Position> previous{Position("ES",Quantity(99),Price(100),Decimal(0),Decimal(0),
                date-std::chrono::hours(24))};
            auto typed_result = db.store_positions(previous,engine,"TREND","BOOK","trading.positions");
            const bool allowed = mode == "historical_valid";
            if (raw_result.is_ok() != allowed || typed_result.is_ok() != allowed) return 23;
            pqxx::read_transaction observer(other);
            if (observer.exec("SELECT quantity FROM trading.positions WHERE date='2026-09-21'")[0][0].as<int>() !=
                (allowed ? 99 : 5)) return 24;
            db.abandon_live_publication();
            std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
            return 0;
        }
        std::vector<Position> rows{Position("ES",Quantity(12),Price(100),Decimal(0),Decimal(0),date)};
        if (db.store_positions(rows,engine,"TREND","BOOK","trading.positions").is_error()) return 5;
        rows[0].quantity = Quantity(99); // the publication must retain the captured 12
        if (mode != "incomplete" && db.store_risk_limits(engine,"BOOK",{{"max_gross_leverage",4.0}}).is_error()) return 6;
        const std::string column = mode.find("rollback") == 0 ? "does_not_exist" : "total_pnl";
        if (db.store_live_results_complete(engine,date,{{column,123}}, {}, {},"BOOK").is_error()) return 7;
        if (db.store_live_run_metadata(date,engine,"BOOK",{{"TREND",1.0}},{},{}).is_error()) return 14;
        if (db.store_trading_equity_curve(engine,date,500000,"BOOK").is_error()) return 15;
        if (db.seed_qt_positions_from_system(engine,"TREND","BOOK","2026-09-22").is_error()) return 16;
        if (db.store_live_run_inputs(engine,"BOOK",date,
            {{"trade_ngin_sha","local-test"},{"config_snapshot",snapshot.value()},
             {"universe",{"ES"}},{"data_window",{}},{"engine_flags",{}}}).is_error()) return 17;
        pqxx::connection other(dsn);
        {
            pqxx::work observer(other);
            if (mode != "publish_concurrent" && mode != "missing_member_stale" &&
                observer.exec("SELECT count(*) FROM trading.positions")[0][0].as<int>() != 0) return 8;
            if (mode.find("retire") == 0) observer.exec("UPDATE trading.strategy_registry SET lifecycle='retired' WHERE id='trend'");
            observer.commit();
        }
        if (mode == "publish_concurrent") {
            std::cout << "RUNTIME_READY" << std::endl;
            std::string command;
            if (!std::getline(std::cin,command) || command != "publish") return 22;
        }
        auto result = db.publish_live_publication();
        const bool expect_success = mode.find("publish") == 0;
        if (result.is_ok() != expect_success) return 9;
        pqxx::read_transaction observer(other);
        auto published = observer.exec("SELECT quantity FROM trading.positions WHERE portfolio_type='system' AND strategy_name='TREND'");
        if (expect_success) {
            if (published.size() != 1 || published[0][0].as<double>() != 12) return 10;
            if (observer.exec("SELECT count(*) FROM trading.live_results")[0][0].as<int>() != 1) return 11;
        } else if (!published.empty() ||
            observer.exec("SELECT count(*) FROM trading.risk_limits")[0][0].as<int>() != 0) return 12;
        if (controlled && observer.exec("SELECT status FROM trading.runtime_attempts")[0][0].as<std::string>() !=
            (expect_success ? "applied" : "failed")) return 18;
        std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
        return 0;
    } catch (...) { std::cerr << "runtime probe failed\n"; return 13; }
}
