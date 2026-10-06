// Owned disposable PostgreSQL only; no engine, financial writes, broker or email.
#include <cstdlib>
#include <iostream>
#include <iterator>
#include "trade_ngin/git_version.hpp"
#include "trade_ngin/core/live_config_override.hpp"
#include "trade_ngin/data/live_config_selection.hpp"
#include "trade_ngin/data/postgres_database.hpp"
using namespace trade_ngin;
using Json=nlohmann::json;
AppConfig fixture() {
    AppConfig c;
    c.portfolio_id="BOOK";
    c.max_drawdown=c.risk_schema.max_drawdown=.3;
    c.max_leverage=c.risk_schema.max_leverage=2.0;
    c.execution.position_limit_live=12.75;
    CarverModuleConfig gate;
    gate.var_limit=.25; gate.jump_risk_limit=.05; gate.max_correlation=.85;
    gate.max_gross_leverage=4; gate.max_net_leverage=2; gate.confidence_level=.99;
    gate.lookback_period=252; gate.missing_symbol_policy_reason="fixture";
    c.risk_schema.portfolio.push_back({"carver","carver",gate});
    c.risk_schema.reporting={"carver","all_bars",.25,.05,.85,4,2,.99,252};
    c.risk_config=c.risk_schema.reporting.to_risk_config();
    c.strategies_config={{"TREND",{{"type","TrendFollowingStrategy"},{"enabled_live",true},
        {"enabled_backtest",true},{"default_allocation",1.0},{"config",{
        {"weight",.03},{"risk_target",.2},{"idm",2.5},{"max_symbol_concentration",.15},
        {"use_position_buffering",true},{"carver_buffer_floor",.5},{"carver_buffer_position_factor",.1},
        {"ema_windows",Json::array({{8,32}})},{"vol_lookback_short",32},{"vol_lookback_long",252}}}}}};
    c.database.password="private-test";c.email.password="private-email";
    return c;
}
int main(int argc,char** argv) {
    if(argc!=2)return 10;
    const std::string mode=argv[1];
    auto c=fixture();
    if(mode=="validate_baseline") {
        auto v=validate_live_config_baseline_request({{"schema","live-config-baseline-validation/v1"},
            {"base_snapshot",build_runtime_trading_snapshot(c).value()}});
        if(v.is_error())return 11;
        std::cout<<v.value().dump()<<'\n';return 0;
    }
    if(mode=="validate_other")c.portfolio_id="OTHER";
    if(mode.starts_with("validate")) {
        auto v=validate_live_config_request({{"schema","live-config-validation/v1"},
            {"base_snapshot",build_runtime_trading_snapshot(c).value()},
            {"changes",{{"/optimization/cost_penalty_scalar",12.75}}}});
        if(v.is_error()) {std::cout<<Json{{"error",v.error()->what()}}.dump()<<'\n';return 11;}
        std::cout<<v.value().dump()<<'\n';return 0;
    }
    const char* raw=std::getenv("ALGOLENS_TEST_DB");
    if(!raw || !std::string(raw).starts_with("host=/tmp/algolens-repair-pg-") ||
        std::string(raw).find("dbname=algolens_test_")==std::string::npos)return 12;
    PostgresDatabase db(raw);if(db.connect().is_error())return 13;
    if(mode=="select_changed_base")c.opt_config.tau=1.1;
    if(mode=="legacy")c.strategies_config["TREND"]["default_allocation"]=.5;
    if(mode=="investor")c.portfolio_id="INVESTOR";
    if(mode=="equity_multi") {
        c.portfolio_id="EQUITYBOOK";
        c.strategies_config=Json::object();
        for(const auto* name:{"BETA","ALPHA"})
            c.strategies_config[name]={{"type","MeanReversionStrategy"},{"enabled_live",true},
                {"enabled_backtest",true},{"default_allocation",.5},{"config",Json::object()}};
    }
    const std::string build=mode=="select_other_build"?"other-build":TRADE_NGIN_GIT_SHA;
    auto selected=select_live_configuration(db,c,build);
    if(selected.is_error()) {
        std::cout<<Json{{"error",selected.error()->what()}}.dump()<<'\n';return 2;
    }
    auto receipt=selected.value().receipt;
    if(mode=="admit_receipt") {
        receipt=Json::parse(std::string(std::istreambuf_iterator<char>(std::cin),{}));
    }
    if(mode.starts_with("admit") || mode=="investor") {
        auto begin=db.begin_live_publication("LIVE_TREND",c.portfolio_id,
            std::chrono::sys_days{std::chrono::year{2026}/10/6},
            build_runtime_trading_snapshot(selected.value().config).value(),mode=="admit_controlled",build,
            PublicationEvidenceRequirement::LegacyNotCollected,nullptr,PublicationPriorRequirement::None,receipt);
        if(begin.is_error()) {
            std::cout<<Json{{"error",begin.error()->what()}}.dump()<<'\n';return 2;
        }
        db.abandon_live_publication();
    }
    std::cout<<Json{{"receipt",receipt},{"cost_penalty_scalar",selected.value().config.opt_config.cost_penalty_scalar},
        {"private_preserved",selected.value().config.database.password=="private-test" &&
                            selected.value().config.email.password=="private-email"}}.dump()<<'\n';
}
