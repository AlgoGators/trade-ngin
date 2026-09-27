// Synthetic local PostgreSQL only; no engine, configuration, network or email.
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"
#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include <cstdlib>
#include <iostream>
using namespace trade_ngin;
int main(int argc, char** argv) {
    const char* raw = std::getenv("ALGOLENS_TEST_DB");
    if (!raw || argc != 2) return 2;
    const std::string dsn(raw);
    std::string mode(argv[1]);
    const bool qt=mode.ends_with("_qt");
    if(qt) mode.resize(mode.size()-3);
    const std::string stream=qt?"qt":"system";
    if (dsn.rfind("host=/tmp/algolens-repair-pg-",0)!=0 ||
        dsn.find(" dbname=algolens_test_")==std::string::npos ||
        dsn.find("hostaddr")!=std::string::npos || dsn.find("service=")!=std::string::npos) return 2;
    try {
        auto shared=std::make_shared<PostgresDatabase>(dsn);
        auto& db=*shared;
        if (db.connect().is_error()) return 3;
        const auto day = std::chrono::system_clock::from_time_t(1790035200);
        Result<void> result;
        if (mode=="delete_results") result=qt?db.delete_live_results("LIVE_TREND",day,"BOOK","trading.live_results",stream):db.delete_live_results("LIVE_TREND",day,"BOOK");
        else if (mode=="update_results") result=qt?db.update_live_results("LIVE_TREND",day,{{"total_pnl",42}},"BOOK","trading.live_results",stream):db.update_live_results("LIVE_TREND",day,{{"total_pnl",42}},"BOOK");
        else if (mode=="delete_equity") result=qt?db.delete_live_equity_curve("LIVE_TREND",day,"BOOK","trading.equity_curve",stream):db.delete_live_equity_curve("LIVE_TREND",day,"BOOK");
        else if (mode=="update_equity") result=qt?db.update_live_equity_curve("LIVE_TREND",day,4200,"BOOK","trading.equity_curve",stream):db.update_live_equity_curve("LIVE_TREND",day,4200,"BOOK");
        else if (mode=="complete") {
            result=db.delete_live_results("LIVE_TREND",day,"BOOK","trading.live_results",stream);
            if(result.is_error()) return 10;
            result=db.store_live_results_complete("LIVE_TREND",day,{{"total_pnl",42}}, {},{},"BOOK","trading.live_results",stream);
        } else if(mode=="numeric") {
            result=db.store_live_results("LIVE_TREND",day,1,2,42,4,5,6000,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,{},"trading.live_results","BOOK",stream);
        } else if(mode=="single_equity") result=db.store_trading_equity_curve("LIVE_TREND",day,4200,"BOOK","trading.equity_curve",stream);
        else if(mode=="batch_equity") result=db.store_trading_equity_curve_batch("LIVE_TREND",{{day,4200}},"BOOK","trading.equity_curve",stream);
        else if(mode=="cleanup") result=db.delete_stale_executions_scoped({"DAILY_ES_20260922"},day,"LIVE_TREND","TREND","BOOK","trading.executions",stream);
        else if (mode=="previous") {
            auto prior=db.get_previous_live_aggregates("LIVE_TREND","BOOK",day,"trading.live_results",stream);
            if(prior.is_error()) return 4;
            std::cout << "PREVIOUS_VALUE=" << std::get<0>(prior.value()) << '\n';
        } else if (mode=="executions" || mode=="rollback" || mode=="manager") {
            ExecutionReport fill;
            fill.exec_id="fresh";fill.order_id="DAILY_ES_20260922";fill.symbol="ES";
            fill.side=Side::BUY;fill.filled_quantity=2;fill.fill_price=100;fill.fill_time=day;
            fill.commissions_fees=0;fill.implicit_price_impact=0;
            fill.slippage_market_impact=0;fill.total_transaction_costs=0;fill.is_partial=false;
            if(mode=="manager") {
                LiveResultsManager manager(shared,true,"LIVE_TREND","BOOK",stream);
                manager.set_positions({Position("ES",Quantity(2),Price(100),Decimal(0),Decimal(0),day)});
                manager.set_executions({fill});manager.set_metrics({{"total_pnl",42}});manager.set_equity(4200);
                result=manager.save_all_results("synthetic",day);
                if(result.is_ok()) result=manager.update_live_results(day,{{"total_pnl",43}});
                if(result.is_ok()) result=manager.update_equity_curve(day,4300);
            } else if(mode=="rollback") {
                auto bad=fill;bad.exec_id="bad";bad.filled_quantity=0;
                result=db.store_executions({fill,bad},"LIVE_TREND","TREND","BOOK","trading.executions",stream);
                if(result.is_ok()) return 11;
                result=Result<void>();
            } else {
                fill.is_partial=true;auto second=fill;second.exec_id="fresh2";second.filled_quantity=3;
                result=qt?db.store_executions({fill,second},"LIVE_TREND","TREND","BOOK","trading.executions",stream):
                    db.store_executions({fill},"LIVE_TREND","TREND","BOOK","trading.executions");
            }
        } else if(mode=="fallback") {
            LiveResultsManager manager(shared,true,"LIVE_TREND","BOOK",stream);
            manager.set_equity(0);result=manager.save_equity_curve(day);
        } else if(mode.starts_with("pending_")) {
            AppConfig cfg;cfg.portfolio_id="BOOK";
            cfg.strategies_config={{"TREND",{{"enabled_live",true},{"default_allocation",1.0},
                {"type","TrendFollowingStrategy"}}}};
            auto snapshot=build_runtime_trading_snapshot(cfg);
            if(snapshot.is_error()) return 12;
            if(mode=="pending_snapshot") {
                std::cout << "PENDING_SNAPSHOT=" << snapshot.value().dump() << '\n';
                return 0;
            }
            auto started=db.begin_live_publication("LIVE_TREND","BOOK",day,snapshot.value(),true,"stream-test");
            if(started.is_error() || started.value()) return 12;
            ExecutionReport fill;
            fill.exec_id="fresh";fill.order_id="DAILY_ES_20260922";fill.symbol="ES";
            fill.side=Side::BUY;fill.filled_quantity=2;fill.fill_price=100;fill.fill_time=day;
            fill.commissions_fees=0;fill.implicit_price_impact=0;
            fill.slippage_market_impact=0;fill.total_transaction_costs=0;fill.is_partial=false;
            // Identical complete system payload for the positive control and every
            // forbidden operation. Existing same-day rows must not mask a no-op.
            if(db.store_positions({Position("ES",Quantity(12),Price(100),Decimal(0),Decimal(0),day)},
                    "LIVE_TREND","TREND","BOOK","trading.positions").is_error() ||
               db.store_risk_limits("LIVE_TREND","BOOK",{{"max_gross_leverage",4.0}}).is_error() ||
               db.delete_live_results("LIVE_TREND",day,"BOOK").is_error() ||
               db.store_live_results_complete("LIVE_TREND",day,{{"total_pnl",42}},{},{},"BOOK").is_error() ||
               db.store_executions({fill},"LIVE_TREND","TREND","BOOK").is_error() ||
               db.store_trading_equity_curve("LIVE_TREND",day,4200,"BOOK").is_error() ||
               db.store_live_run_metadata(day,"LIVE_TREND","BOOK",{{"TREND",1.0}},{},{}).is_error() ||
               db.seed_qt_positions_from_system("LIVE_TREND","TREND","BOOK","2026-09-22").is_error() ||
               db.store_live_run_inputs("LIVE_TREND","BOOK",day,
                    {{"trade_ngin_sha","stream-test"},{"config_snapshot",snapshot.value()},
                     {"universe",{"ES"}},{"data_window",{}},{"engine_flags",{}}}).is_error()) return 18;
            Result<void> forbidden;
            if(mode=="pending_complete") forbidden=db.store_live_results_complete("LIVE_TREND",day,{{"total_pnl",777}},{},{},"BOOK","trading.live_results","qt");
            else if(mode=="pending_numeric") forbidden=db.store_live_results("LIVE_TREND",day,1,2,777,4,5,6000,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,{},"trading.live_results","BOOK","qt");
            else if(mode=="pending_executions") forbidden=db.store_executions({fill},"LIVE_TREND","TREND","BOOK","trading.executions","qt");
            else if(mode=="pending_cleanup") forbidden=db.delete_stale_executions_scoped({fill.order_id},day,"LIVE_TREND","TREND","BOOK","trading.executions","qt");
            else if(mode=="pending_single_equity") forbidden=db.store_trading_equity_curve("LIVE_TREND",day,7777,"BOOK","trading.equity_curve","qt");
            else if(mode=="pending_batch_equity") forbidden=db.store_trading_equity_curve_batch("LIVE_TREND",{{day,7777}},"BOOK","trading.equity_curve","qt");
            else if(mode=="pending_delete_results") forbidden=db.delete_live_results("LIVE_TREND",day,"BOOK","trading.live_results","qt");
            else if(mode=="pending_update_results") forbidden=db.update_live_results("LIVE_TREND",day,{{"total_pnl",777}},"BOOK","trading.live_results","qt");
            else if(mode=="pending_delete_equity") forbidden=db.delete_live_equity_curve("LIVE_TREND",day,"BOOK","trading.equity_curve","qt");
            else if(mode=="pending_update_equity") forbidden=db.update_live_equity_curve("LIVE_TREND",day,7777,"BOOK","trading.equity_curve","qt");
            else if(mode!="pending_control") return 19;
            if(mode!="pending_control" && forbidden.is_ok()) return 13;
            // Still publish after the expected method error: the regression is
            // that ignoring that error must never acknowledge a system run.
            const bool published=db.publish_live_publication().is_ok();
            std::cout << "PENDING_PUBLISHED=" << published << '\n';
        } else if(mode=="invalid") {
            if(db.store_live_results_complete("LIVE_TREND",day,{{"total_pnl",42}},{},{},"","trading.live_results","qt").is_ok() ||
               db.store_executions({},"LIVE_TREND","TREND","BOOK","trading.executions","unknown").is_ok() ||
               db.delete_stale_executions({"x"},day,"TREND").is_ok()) return 14;
        } else if(mode=="missing_history") {
            LiveResultsManager manager(shared,true,"LIVE_TREND","BOOK",stream);manager.set_equity(0);
            if(manager.save_equity_curve(day).is_ok() ||
               db.get_previous_live_aggregates("LIVE_TREND","BOOK",day,"trading.live_results",stream).is_ok()) return 16;
        } else if(mode=="loader_qt_only") {
            LiveDataLoader loader(shared);
            if(loader.load_previous_portfolio_value("LIVE_TREND","BOOK",day).value()!=0 ||
               loader.load_portfolio_value("LIVE_TREND","BOOK",day).is_ok() ||
               loader.load_live_results("LIVE_TREND","BOOK",day).is_ok() ||
               loader.load_previous_day_data("LIVE_TREND","BOOK",day).value().exists ||
               loader.has_live_results("LIVE_TREND","BOOK",day).value() ||
               loader.get_live_results_count("LIVE_TREND","BOOK").value()!=0 ||
               !loader.load_daily_returns_history("LIVE_TREND","BOOK",day).value().empty() ||
               !loader.load_daily_pnl_history("LIVE_TREND","BOOK",day).value().empty() ||
               !loader.load_equity_curve_history("LIVE_TREND","BOOK",day).value().empty() ||
               loader.load_total_trades_count("LIVE_TREND","BOOK",day).value()!=0 ||
               loader.load_daily_transaction_costs("LIVE_TREND","BOOK",day).value()!=0 ||
               loader.load_margin_metrics("LIVE_TREND","BOOK",day).value().valid ||
               !loader.load_daily_metrics_for_email("LIVE_TREND","BOOK",day).value().empty()) return 17;
        } else if(mode=="loader") {
            LiveDataLoader loader(shared);
            auto prior=loader.load_previous_portfolio_value("LIVE_TREND","BOOK",day);
            auto current=loader.load_portfolio_value("LIVE_TREND","BOOK",day);
            auto previous=loader.load_previous_day_data("LIVE_TREND","BOOK",day);
            auto rows=loader.load_live_results("LIVE_TREND","BOOK",day);
            auto exists=loader.has_live_results("LIVE_TREND","BOOK",day-std::chrono::hours(24));
            auto count=loader.get_live_results_count("LIVE_TREND","BOOK");
            auto returns=loader.load_daily_returns_history("LIVE_TREND","BOOK",day);
            auto pnls=loader.load_daily_pnl_history("LIVE_TREND","BOOK",day);
            auto equities=loader.load_equity_curve_history("LIVE_TREND","BOOK",day);
            auto executions=loader.load_total_trades_count("LIVE_TREND","BOOK",day);
            auto costs=loader.load_daily_transaction_costs("LIVE_TREND","BOOK",day);
            auto margin=loader.load_margin_metrics("LIVE_TREND","BOOK",day);
            auto email_metrics=loader.load_daily_metrics_for_email("LIVE_TREND","BOOK",day);
            if(prior.is_error()||current.is_error()||previous.is_error()||rows.is_error()||exists.is_error()||count.is_error()||
               returns.is_error()||pnls.is_error()||equities.is_error()||executions.is_error()||costs.is_error()||margin.is_error()||email_metrics.is_error()) return 15;
            nlohmann::json output={{"prior",prior.value()},{"current",current.value()},{"previous",previous.value().portfolio_value},
                {"row_pnl",rows.value().daily_pnl},{"exists",exists.value()},{"count",count.value()},
                {"returns",returns.value()},{"pnls",pnls.value()},{"equities",equities.value()},
                {"executions",executions.value()},{"costs",costs.value()},{"margin",margin.value().gross_leverage},
                {"email_metrics",email_metrics.value()}};
            std::cout<<"LOADER="<<output.dump()<<'\n';
        } else return 5;
        if(result.is_error()) {std::cerr << result.error()->what() << '\n'; return 6;}
        std::cout << "STREAM_PROBE_OK=" << mode << '\n';
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 7;}
}
