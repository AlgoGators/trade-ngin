// Disposable PostgreSQL only. No application runner or transport.
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/qt_seed_publication.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace trade_ngin;

int main(int argc, char** argv) {
    const char* raw = std::getenv("ALGOLENS_TEST_DB");
    if (!raw || (argc != 2 && argc != 3)) return 2;
    if (argc == 3 &&
        (std::string(argv[1]) != "--scenario" ||
         std::string(argv[2]) != "seed-provenance")) return 2;
    const std::string dsn(raw), mode(argc == 3 ? "handoff_success" : argv[1]);
    if (dsn.rfind("host=/tmp/algolens-repair-pg-",0) != 0 ||
        dsn.find(" dbname=algolens_test_") == std::string::npos ||
        dsn.find("hostaddr") != std::string::npos || dsn.find("service=") != std::string::npos)
        return 2;
    try {
        PostgresDatabase db(dsn);
        if (db.connect().is_error()) return 3;
        if (mode=="manifest_vector") {
            QtModelSeedPublication sample{
                "00000000-0000-4000-8000-000000000001","BOOK","LIVE_TREND",
                "2026-09-22",{},"","vector",{}};
            auto row=[](const std::string& symbol,Quantity quantity,Price basis,
                        const std::string& action,
                        std::optional<std::string> revision,
                        std::optional<std::string> origin) {
                return QtProposalManifestRow{
                    ComponentPositionKey{"BOOK","LIVE_TREND","TREND","2026-09-22",
                                         symbol,"qt_proposal"},
                    quantity,basis,action,std::move(revision),std::move(origin)};
            };
            sample.proposal_components.push_back(row("ES",Quantity(12),Price(101),
                "inserted","00000000-0000-4000-8000-000000000011",
                "00000000-0000-4000-8000-000000000001"));
            sample.proposal_components.push_back(row("NQ",Quantity(0),Price(202),
                "preserved","00000000-0000-4000-8000-000000000022",
                "00000000-0000-4000-8000-000000000002"));
            sample.proposal_components.push_back(row("YM",Quantity::from_raw(525000000),
                Price(303),"preserved",std::nullopt,std::nullopt));
            for (size_t n=1;n<=sample.proposal_components.size();++n) {
                sample.proposal_components.resize(n);
                auto document=qt_proposal_manifest_document(sample);
                auto digest=qt_proposal_manifest_digest(sample);
                if (document.is_error() || digest.is_error()) return 47;
                std::cout << "MANIFEST_VECTOR_" << n << '=' << document.value().dump()
                          << ' ' << digest.value() << '\n';
                if (n<3) sample.proposal_components.push_back(
                    n==1 ? row("NQ",Quantity(0),Price(202),"preserved",
                        "00000000-0000-4000-8000-000000000022",
                        "00000000-0000-4000-8000-000000000002") :
                        row("YM",Quantity::from_raw(525000000),Price(303),
                            "preserved",std::nullopt,std::nullopt));
            }
            return 0;
        }
        if (mode=="invalid_seed_date" || mode=="invalid_seed_book") {
            auto result=db.seed_qt_proposal_positions_from_system("LIVE_TREND","TREND",
                mode=="invalid_seed_book" ? " " : "BOOK",
                mode=="invalid_seed_date" ? "2026-02-30" : "2026-09-22");
            if (!result.is_error()) return 25;
            std::cout << "PROPOSAL_ERROR=1\n";
            return 0;
        }
        if (mode=="invalid_reader") {
            auto day=std::chrono::system_clock::from_time_t(1790035200);
            if (db.load_qt_proposal_positions_by_date("LIVE_TREND",{},"BOOK",day).is_ok() ||
                db.load_qt_proposal_positions_by_date("LIVE_TREND",{"TREND"}," ",day).is_ok()) return 26;
            std::cout << "PROPOSAL_ERROR=1\n";
            return 0;
        }
        if (mode.starts_with("handoff_")) {
            // The same pending-publication seam used by the runner, without a live app.
            AppConfig cfg; cfg.portfolio_id="BOOK";
            cfg.strategies_config={{"TREND",{{"enabled_live",true},
                {"default_allocation",0.5},{"type","TrendFollowingStrategy"}}},
                {"OTHER",{{"enabled_live",true},{"default_allocation",0.5},
                {"type","TrendFollowingStrategy"}}}};
            auto snapshot=build_runtime_trading_snapshot(cfg);
            if (snapshot.is_error()) return 42;
            const bool next_day=mode=="handoff_next_day";
            const auto day=std::chrono::system_clock::from_time_t(
                next_day ? 1790121600 : 1790035200);
            const std::string source_day=next_day ? "2026-09-23" : "2026-09-22";
            const bool controlled=mode!="handoff_legacy" && !next_day;
            auto started=db.begin_live_publication("LIVE_TREND","BOOK",day,
                snapshot.value(),controlled,"proposal-handoff-test");
            if (started.is_error() || started.value()) return 43;
            std::vector<Position> trend{
                Position("ES",Quantity(12),Price(101),Decimal(1),Decimal(2),day),
                Position("NQ",Quantity(0),Price(202),Decimal(3),Decimal(4),day)};
            if (mode=="handoff_repeat" || mode=="handoff_repeat_again")
                trend.emplace_back("YM",Quantity(7),Price(303),Decimal(5),Decimal(6),day);
            if (mode=="handoff_repeat_again")
                trend.emplace_back("ZN",Quantity(0),Price(404),Decimal(0),Decimal(0),day);
            if (db.store_positions(trend,"LIVE_TREND","TREND","BOOK",
                    "trading.positions").is_error() ||
                db.store_positions({Position("ES",Quantity(22),Price(111),
                    Decimal(7),Decimal(8),day)},"LIVE_TREND","OTHER","BOOK",
                    "trading.positions").is_error()) return 44;
            ExecutionReport fill;
            fill.exec_id="fresh";fill.order_id="DAILY_ES_20260922";fill.symbol="ES";
            fill.side=Side::BUY;fill.filled_quantity=2;fill.fill_price=100;fill.fill_time=day;
            fill.commissions_fees=0;fill.implicit_price_impact=0;
            fill.slippage_market_impact=0;fill.total_transaction_costs=0;fill.is_partial=false;
            if (db.store_risk_limits("LIVE_TREND","BOOK",{{"max_gross_leverage",4.0}}).is_error() ||
                db.delete_live_results("LIVE_TREND",day,"BOOK").is_error() ||
                db.store_live_results_complete("LIVE_TREND",day,{{"total_pnl",42}},{},{},"BOOK").is_error() ||
                db.store_executions({fill},"LIVE_TREND","TREND","BOOK").is_error() ||
                db.store_trading_equity_curve("LIVE_TREND",day,4200,"BOOK").is_error() ||
                db.store_live_run_metadata(day,"LIVE_TREND","BOOK",{{"TREND",0.5},{"OTHER",0.5}},{},{}).is_error()) return 46;
            const auto seeded=seed_qt_proposal_positions(db,"LIVE_TREND",
                {"TREND","OTHER"},"BOOK",day);
            if (seeded.is_error()) {
                db.abandon_live_publication();
                std::cout << "HANDOFF_HELPER_ERROR=1\nHANDOFF_PUBLISHED=0\n";
                return 0;
            }
            // A separate session must see neither newly queued MODEL rows nor drafts.
            pqxx::connection reader(dsn);
            pqxx::read_transaction read(reader);
            const auto visible=read.exec(
                "SELECT count(*) FROM trading.positions WHERE strategy_id='LIVE_TREND' "
                "AND portfolio_id='BOOK' AND date="+read.quote(source_day)+" "
                "AND strategy_name='TREND' AND symbol="
                +read.quote(mode=="handoff_repeat_again" ? "ZN" :
                            mode=="handoff_repeat" ? "YM" : "NQ")+
                " AND portfolio_type IN ('system','qt_proposal')");
            const auto count=visible[0][0].as<int>();
            std::cout << "HANDOFF_PRECOMMIT_VISIBLE=" << count << '\n';
            if (count!=0) return 45;
            if (
                db.seed_qt_positions_from_system("LIVE_TREND","TREND","BOOK",source_day).is_error() ||
                db.seed_qt_positions_from_system("LIVE_TREND","OTHER","BOOK",source_day).is_error() ||
                db.store_live_run_inputs("LIVE_TREND","BOOK",day,
                    {{"trade_ngin_sha","proposal-handoff-test"},{"config_snapshot",snapshot.value()},
                     {"universe",{"ES","NQ"}},{"data_window",{}},{"engine_flags",{}}}).is_error()) return 46;
            const bool published=db.publish_live_publication().is_ok();
            std::cout << "HANDOFF_PUBLISHED=" << published << '\n';
            return 0;
        }
        if (mode.starts_with("pending_")) {
            AppConfig cfg; cfg.portfolio_id="BOOK";
            cfg.strategies_config={{"TREND",{{"enabled_live",true},
                {"default_allocation",1.0},{"type","TrendFollowingStrategy"}}}};
            if (mode=="pending_two_components" || mode=="pending_two_components_snapshot") {
                cfg.strategies_config["TREND"]["default_allocation"]=0.5;
                cfg.strategies_config["OTHER"]=cfg.strategies_config["TREND"];
            }
            auto snapshot=build_runtime_trading_snapshot(cfg);
            if (snapshot.is_error()) return 10;
            if (mode=="pending_snapshot" || mode=="pending_two_components_snapshot") {
                std::cout << "PROPOSAL_SNAPSHOT=" << snapshot.value().dump() << '\n';
                return 0;
            }
            const auto day=std::chrono::system_clock::from_time_t(1790035200);
            auto started=db.begin_live_publication("LIVE_TREND","BOOK",day,snapshot.value(),true,"proposal-test");
            if (started.is_error() || started.value()) return 11;
            auto batch=[&](double quantity) {
                return db.store_positions({Position("ES",Quantity(quantity),Price(100),
                    Decimal(0),Decimal(0),day)},"LIVE_TREND","TREND","BOOK","trading.positions");
            };
            if (mode=="pending_empty_source" && db.store_positions({},"LIVE_TREND","TREND",
                "BOOK","trading.positions").is_error()) return 38;
            if (mode!="pending_missing_source" && mode!="pending_empty_source" && batch(12).is_error()) return 12;
            if ((mode=="pending_before" || mode=="pending_no_proposal") && batch(14).is_error()) return 13;
            if (mode!="pending_no_proposal") {
                const auto seed_day=mode=="pending_wrong_date" ? "2026-09-21" : "2026-09-22";
                const auto proposal=db.seed_qt_proposal_positions_from_system(
                    "LIVE_TREND",mode=="pending_wrong_member" ? "OTHER" : "TREND",
                    mode=="pending_wrong_book" ? "OTHER" : "BOOK",seed_day);
                const bool expected_error=mode=="pending_missing_source" || mode=="pending_wrong_date" ||
                    mode=="pending_wrong_member" || mode=="pending_wrong_book" ||
                    mode=="pending_missing_015" || mode=="pending_empty_source" ||
                    mode=="pending_conditional_fence";
                if (proposal.is_error()!=expected_error ||
                    (!expected_error && proposal.value()!=0)) return 14;
                if (mode=="pending_repeat" && db.seed_qt_proposal_positions_from_system(
                    "LIVE_TREND","TREND","BOOK","2026-09-22").is_error()) return 15;
            }
            if (mode=="pending_after_diff" && batch(14).is_ok()) return 16;
            if (mode=="pending_after_same" && batch(12).is_ok()) return 17;
            if (mode=="pending_empty_after" && db.store_positions({},"LIVE_TREND","TREND",
                "BOOK","trading.positions").is_error()) return 39;
            if (mode=="pending_two_components") {
                if (db.store_positions({Position("ES",Quantity(22),Price(100),Decimal(0),Decimal(0),day)},
                        "LIVE_TREND","OTHER","BOOK","trading.positions").is_error() ||
                    db.store_positions({Position("ES",Quantity(24),Price(100),Decimal(0),Decimal(0),day)},
                        "LIVE_TREND","OTHER","BOOK","trading.positions").is_error()) return 40;
            }
            if (mode=="pending_abandon") {
                db.abandon_live_publication();
                auto restarted=db.begin_live_publication("LIVE_TREND","BOOK",day,
                    snapshot.value(),true,"proposal-test");
                if (restarted.is_error() || restarted.value() || batch(12).is_error() ||
                    batch(14).is_error()) return 41;
            }
            if (mode=="pending_generic_position") {
                if (db.store_positions({},"LIVE_TREND","TREND","BOOK","trading.positions",
                    "qt_proposal").is_ok()) return 18;
            }
            if (mode=="pending_legacy_read") {
                if (db.load_positions_by_date("LIVE_TREND","TREND","BOOK",day,
                    "trading.positions","qt_proposal").is_ok()) return 19;
            }
            if (mode=="pending_report_read") {
                if (db.load_report_positions_by_date("LIVE_TREND",{"TREND"},"BOOK",day,
                    "qt_proposal").is_ok()) return 27;
            }
            if (mode=="pending_prior_results") {
                if (db.get_previous_live_aggregates("LIVE_TREND","BOOK",day,
                    "trading.live_results","qt_proposal").is_ok()) return 28;
            }
            ExecutionReport fill;
            fill.exec_id="fresh";fill.order_id="DAILY_ES_20260922";fill.symbol="ES";
            fill.side=Side::BUY;fill.filled_quantity=2;fill.fill_price=100;fill.fill_time=day;
            fill.commissions_fees=0;fill.implicit_price_impact=0;
            fill.slippage_market_impact=0;fill.total_transaction_costs=0;fill.is_partial=false;
            if (db.store_risk_limits("LIVE_TREND","BOOK",{{"max_gross_leverage",4.0}}).is_error() ||
                db.delete_live_results("LIVE_TREND",day,"BOOK").is_error() ||
                db.store_live_results_complete("LIVE_TREND",day,{{"total_pnl",42}},{},{},"BOOK").is_error() ||
                db.store_executions({fill},"LIVE_TREND","TREND","BOOK").is_error() ||
                db.store_trading_equity_curve("LIVE_TREND",day,4200,"BOOK").is_error() ||
                db.store_live_run_metadata(day,"LIVE_TREND","BOOK",{{"TREND",1.0}},{},{}).is_error() ||
                (mode!="pending_no_qt" && db.seed_qt_positions_from_system(
                    "LIVE_TREND","TREND","BOOK","2026-09-22").is_error()) ||
                (mode=="pending_two_components" && db.seed_qt_positions_from_system(
                    "LIVE_TREND","OTHER","BOOK","2026-09-22").is_error()) ||
                db.store_live_run_inputs("LIVE_TREND","BOOK",day,
                    {{"trade_ngin_sha","proposal-test"},{"config_snapshot",snapshot.value()},
                     {"universe",{"ES"}},{"data_window",{}},{"engine_flags",{}}}).is_error()) return 20;
            if (mode=="pending_invalid_result") {
                if (db.store_live_results_complete("LIVE_TREND",day,{{"total_pnl",777}},{},{},
                    "BOOK","trading.live_results","qt_proposal").is_ok()) return 21;
            }
            if (mode=="pending_invalid_numeric") {
                if (db.store_live_results("LIVE_TREND",day,1,2,777,4,5,6000,7,8,9,10,11,12,
                    13,14,15,16,17,18,19,20,21,22,{},"trading.live_results","BOOK",
                    "qt_proposal").is_ok()) return 29;
            }
            if (mode=="pending_invalid_execution") {
                if (db.store_executions({fill},"LIVE_TREND","TREND","BOOK",
                    "trading.executions","qt_proposal").is_ok()) return 30;
            }
            if (mode=="pending_invalid_empty_execution") {
                if (db.store_executions({},"LIVE_TREND","TREND","", "trading.executions",
                    "qt_proposal").is_ok()) return 22;
            }
            if (mode=="pending_invalid_legacy_cleanup") {
                if (db.delete_stale_executions({},day,"TREND","trading.executions",
                    "qt_proposal").is_ok()) return 23;
            }
            if (mode=="pending_invalid_scoped_cleanup") {
                if (db.delete_stale_executions_scoped({},day,"LIVE_TREND","TREND","",
                    "trading.executions","qt_proposal").is_ok()) return 31;
            }
            if (mode=="pending_invalid_single_equity") {
                if (db.store_trading_equity_curve("LIVE_TREND",day,777,"BOOK",
                    "trading.equity_curve","qt_proposal").is_ok()) return 32;
            }
            if (mode=="pending_invalid_batch_equity") {
                if (db.store_trading_equity_curve_batch("LIVE_TREND",{},"",
                    "trading.equity_curve","qt_proposal").is_ok()) return 33;
            }
            if (mode=="pending_invalid_delete_result") {
                if (db.delete_live_results("LIVE_TREND",day,"BOOK",
                    "trading.live_results","qt_proposal").is_ok()) return 34;
            }
            if (mode=="pending_invalid_update_result") {
                if (db.update_live_results("LIVE_TREND",day,{},"BOOK",
                    "trading.live_results","qt_proposal").is_ok()) return 35;
            }
            if (mode=="pending_invalid_delete_equity") {
                if (db.delete_live_equity_curve("LIVE_TREND",day,"BOOK",
                    "trading.equity_curve","qt_proposal").is_ok()) return 36;
            }
            if (mode=="pending_invalid_update_equity") {
                if (db.update_live_equity_curve("LIVE_TREND",day,777,"BOOK",
                    "trading.equity_curve","qt_proposal").is_ok()) return 37;
            }
            if (mode=="pending_capability_revoked" || mode=="pending_capability_conditional" ||
                mode=="pending_retired" ||
                mode=="pending_revision_aba" || mode=="pending_intent_revoked") {
                pqxx::connection other(dsn);
                pqxx::work change(other);
                if (mode=="pending_capability_revoked")
                    change.exec("ALTER TABLE trading.positions DISABLE TRIGGER runtime_publication_fence");
                else if (mode=="pending_capability_conditional")
                    change.exec("DROP TRIGGER runtime_publication_fence ON trading.positions;"
                                "CREATE TRIGGER runtime_publication_fence BEFORE INSERT OR UPDATE OR DELETE "
                                "ON trading.positions FOR EACH ROW WHEN (false) "
                                "EXECUTE FUNCTION trading.fence_runtime_publication_row()");
                else if (mode=="pending_retired")
                    change.exec("UPDATE trading.strategy_registry SET lifecycle='retired' WHERE id='trend'");
                else if (mode=="pending_revision_aba")
                    change.exec("UPDATE trading.strategy_registry SET lifecycle='retired' WHERE id='trend';"
                                "UPDATE trading.strategy_registry SET lifecycle='live' WHERE id='trend'");
                else change.exec("UPDATE trading.runtime_intents SET status='superseded' WHERE status='approved'");
                change.commit();
            }
            const bool published=db.publish_live_publication().is_ok();
            std::cout << "PROPOSAL_PUBLISHED=" << published << '\n';
            return 0;
        }
        if (mode == "seed" || mode == "seed_missing_schema" || mode == "seed_refused" ||
            mode == "seed_other_component" || mode == "seed_other_book" ||
            mode == "seed_other_engine" || mode == "seed_other_day") {
            auto seeded = db.seed_qt_proposal_positions_from_system(
                mode=="seed_other_engine" ? "LIVE_OTHER" : "LIVE_TREND",
                mode=="seed_other_component" ? "OTHER" : "TREND",
                mode=="seed_other_book" ? "BOOK2" : "BOOK",
                mode=="seed_other_day" ? "2026-09-23" : "2026-09-22");
            if (mode == "seed_missing_schema" || mode == "seed_refused") {
                if (!seeded.is_error()) return 4;
                std::cout << "PROPOSAL_ERROR=1\n";
                return 0;
            }
            if (seeded.is_error()) return 5;
            std::cout << "PROPOSAL_SEEDED=" << seeded.value() << '\n';
            return 0;
        }
        if (mode == "read" || mode == "read_missing_schema") {
            auto day = std::chrono::system_clock::from_time_t(1790035200);
            auto loaded = db.load_qt_proposal_positions_by_date(
                "LIVE_TREND",{"TREND","OTHER"},"BOOK",day);
            if (mode == "read_missing_schema") {
                if (!loaded.is_error()) return 6;
                std::cout << "PROPOSAL_ERROR=1\n";
                return 0;
            }
            if (loaded.is_error()) return 7;
            nlohmann::json values = nlohmann::json::object();
            for (const auto& [component,positions] : loaded.value())
                for (const auto& [symbol,pos] : positions)
                    values[component][symbol] = {
                        {"quantity",pos.quantity.as_double()},
                        {"price",pos.average_price.as_double()},
                        {"unrealized",pos.unrealized_pnl.as_double()},
                        {"realized",pos.realized_pnl.as_double()}};
            std::cout << "PROPOSAL_ROWS=" << values.dump() << '\n';
            return 0;
        }
        if (mode == "decimal_report_system" || mode == "decimal_report_qt" ||
            mode == "decimal_proposal") {
            const auto day = std::chrono::system_clock::from_time_t(1790035200);
            auto loaded = mode == "decimal_proposal"
                ? db.load_qt_proposal_positions_by_date("LIVE_TREND", {"TREND"}, "BOOK", day)
                : db.load_report_positions_by_date("LIVE_TREND", {"TREND"}, "BOOK", day,
                    mode == "decimal_report_qt" ? "qt" : "system");
            if (loaded.is_error()) {
                std::cout << "DECIMAL_ERROR=1\n";
                return 0;
            }
            nlohmann::json values = nlohmann::json::object();
            for (const auto& [component, positions] : loaded.value())
                for (const auto& [symbol, pos] : positions)
                    values[component][symbol] = {
                        {"quantity", pos.quantity.raw_value()},
                        {"price", pos.average_price.raw_value()},
                        {"unrealized", pos.unrealized_pnl.raw_value()},
                        {"realized", pos.realized_pnl.raw_value()}};
            std::cout << "DECIMAL_ROWS=" << values.dump() << '\n';
            return 0;
        }
        return 8;
    } catch (...) { return 9; }
}
