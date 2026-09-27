// Public owned config -> actual native runtime snapshot only.
// No database connection, strategy construction, runner, email or delivery.
#include <filesystem>
#include <iostream>
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/strategy/equity_strategy_builder.hpp"
using namespace trade_ngin;
int main(int argc,char** argv) {try {
    if(argc!=2)return 64;
    const std::filesystem::path config=argv[1];
    if(!config.is_absolute() || config.filename()!="config" || std::filesystem::is_symlink(config) ||
       std::filesystem::canonical(config)!=config ||
       !config.parent_path().filename().string().starts_with("equity-model-run-"))return 65;
    // The explicit owned fixture root is checked before this standalone probe.
    // Never accept a config under production/system directories.
    const auto root=config.parent_path().parent_path();
    if(root.filename()!="actual-mr-run-staging" ||
       root.parent_path().filename()!="equity-model-entry-staging" ||
       root.parent_path().parent_path().filename()!="2026-09-26-hemdutt-issue-completion")return 65;
    LoggerConfig logging;logging.destination=LogDestination::NONE;
    logging.min_level=LogLevel::ERR;Logger::instance().initialize(logging);
    auto loaded=ConfigLoader::load(config,"equity_mr");if(loaded.is_error())return 66;
    auto actual=loaded.value();actual.live.record_equity_policy_snapshot();
    if(actual.portfolio_id!="EQUITY_MR_PORTFOLIO")return 67;
    const auto selected=apps::collect_enabled_equity_strategies(actual.strategies_config,"enabled_live");
    if(selected.is_error() || selected.value().size()!=1 || selected.value().front().type!="MeanReversionStrategy")return 68;
    actual.strategies_config={{"EQUITY_MEAN_REVERSION",selected.value().front().def}};
    actual.strategies_config["EQUITY_MEAN_REVERSION"]["default_allocation"]=1.0;
    const auto snapshot=build_runtime_trading_snapshot(actual);if(snapshot.is_error())return 69;
    std::cout<<"EQ_MODEL_RUNTIME_SNAPSHOT="<<snapshot.value().dump()<<'\n';return 0;
}catch(const std::exception&){std::cerr<<"EQ_PUBLIC_CONFIG_SETUP_REFUSED\n";return 70;}}
