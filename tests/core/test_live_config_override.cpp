// Mutations caught: dropping schema-2 risk, coercing fractions, broadening policy,
// silently changing coupled fields, trusting incomplete/contradictory baselines.
#include <gtest/gtest.h>
#include <limits>
#include "trade_ngin/core/live_config_override.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
using namespace trade_ngin;
using Json = nlohmann::json;
namespace {
AppConfig base_config() {
    AppConfig c;
    c.portfolio_id = "CONSERVATIVE_PORTFOLIO";
    c.max_drawdown = c.risk_schema.max_drawdown = 0.3;
    c.max_leverage = c.risk_schema.max_leverage = 2.0;
    CarverModuleConfig gate;
    gate.var_limit=.25; gate.jump_risk_limit=.05; gate.max_correlation=.85;
    gate.max_gross_leverage=4; gate.max_net_leverage=2; gate.confidence_level=.99;
    gate.lookback_period=252; gate.missing_symbol_policy_reason="fixture";
    c.risk_schema.portfolio.push_back({"carver","carver",gate});
    c.risk_schema.reporting={"carver","all_bars",.25,.05,.85,4,2,.99,252};
    c.risk_config=c.risk_schema.reporting.to_risk_config();
    c.execution.position_limit_live=12.75;
    c.strategies_config={{"TREND", {{"type","TrendFollowingStrategy"},{"enabled_live",true},
        {"enabled_backtest",true},{"default_allocation",1.0},{"config",{
        {"weight",.03},{"risk_target",.2},{"idm",2.5},{"max_symbol_concentration",.15},
        {"use_position_buffering",true},{"carver_buffer_floor",.5},{"carver_buffer_position_factor",.1},
        {"ema_windows",Json::array({{8,32}})},{"vol_lookback_short",32},{"vol_lookback_long",252}}}}}};
    return c;
}
Json request(const AppConfig& c, const Json& changes) {
    return {{"schema","live-config-validation/v1"},{"base_snapshot",build_runtime_trading_snapshot(c).value()},
            {"changes",changes}};
}
}
TEST(LiveConfigOverride, SnapshotPreservesCompleteRiskAndFractionalLimits) {
    const auto s=build_runtime_trading_snapshot(base_config()); ASSERT_TRUE(s.is_ok());
    ASSERT_EQ(s.value().at("snapshot_version"),2);
    EXPECT_EQ(s.value()["risk"]["schema"],2);
    EXPECT_EQ(s.value()["risk"]["modules"][0]["max_net_leverage"],2.0);
    EXPECT_EQ(s.value()["max_drawdown"],.3);
    EXPECT_EQ(s.value()["execution"]["position_limit_live"],12.75);
    EXPECT_TRUE(s.value().contains("sleeve_risk_modules"));
    EXPECT_TRUE(s.value().contains("use_optimization"));
    EXPECT_EQ(s.value()["covariance_history_prices"],756);
}
TEST(LiveConfigOverride, UnrelatedEditPreservesLimitsCredentialsAndFractions) {
    auto c=base_config(); c.database.password="private"; c.email.password="private-email";
    auto changed=apply_live_config_override(c,{{"/optimization/cost_penalty_scalar",12.75}});
    ASSERT_TRUE(changed.is_ok()); EXPECT_EQ(changed.value().opt_config.cost_penalty_scalar,12.75);
    EXPECT_EQ(changed.value().max_drawdown,.3); EXPECT_EQ(changed.value().max_leverage,2.0);
    EXPECT_EQ(changed.value().execution.position_limit_live,12.75);
    EXPECT_EQ(changed.value().database.password,"private");
    EXPECT_EQ(changed.value().email.password,"private-email");
}
TEST(LiveConfigOverride, RejectsProtectedUnknownStructuralMalformedAndNoopChanges) {
    auto c=base_config();
    for(const auto& p:{"/portfolio_id","/initial_capital","/database/password","/email/password",
        "/strategies/TREND/enabled_live","/strategies/TREND/default_allocation","/strategies/TREND/type",
        "/strategies/TREND/symbols","/strategies/TREND/config/fx_rate","/risk/modules",
        "/risk/modules/0/type","/risk/modules/0/missing_symbol_policy","/risk/risk_reporting/window",
        "/live/historical_days","/execution/commission_rate","/optimization/asymmetric_risk_buffer",
        "/new","/risk~2modules","optimization/tau","/risk/modules/00/var_limit"}) {
        EXPECT_TRUE(apply_live_config_override(c,{{p,3}}).is_error()) << p;
    }
    for(const auto& value:{Json(),Json::array({1}),Json::object(),Json("sensitive-value")})
        EXPECT_TRUE(apply_live_config_override(c,{{"/optimization/tau",value}}).is_error());
    EXPECT_TRUE(apply_live_config_override(c,Json::object()).is_error());
    EXPECT_TRUE(apply_live_config_override(c,{{"/optimization/tau",1.0}}).is_error());
    EXPECT_TRUE(apply_live_config_override(c,{{"/optimization/tau",std::numeric_limits<double>::infinity()}}).is_error());
    EXPECT_TRUE(apply_live_config_override(c,{{"/optimization/max_iterations",1.5}}).is_error());
}
TEST(LiveConfigOverride, RequiresExplicitEqualReportingAndPortfolioCarverChanges) {
    auto c=base_config();
    EXPECT_TRUE(apply_live_config_override(c,{{"/risk/modules/0/var_limit",.2}}).is_error());
    EXPECT_TRUE(apply_live_config_override(c,{{"/risk/modules/0/var_limit",.2},
        {"/risk/risk_reporting/var_limit",.3}}).is_error());
    auto ok=apply_live_config_override(c,{{"/risk/modules/0/var_limit",.2},
        {"/risk/risk_reporting/var_limit",.2}});
    ASSERT_TRUE(ok.is_ok()); EXPECT_EQ(ok.value().risk_config.var_limit,.2);
    EXPECT_TRUE(apply_live_config_override(c,{{"/risk/modules/0/max_net_leverage",5},
        {"/risk/risk_reporting/max_net_leverage",5}}).is_error());
}
TEST(LiveConfigOverride, ProtocolRejectsLegacyIncompleteContradictoryAndUnknownRequests) {
    auto r=request(base_config(),{{"/optimization/tau",1.2}});
    for(const auto& key:{"schema","base_snapshot","changes"}) {
        auto bad=r; bad.erase(key); EXPECT_TRUE(validate_live_config_request(bad).is_error());
    }
    auto bad=r; bad["actor"]="not-authority"; EXPECT_TRUE(validate_live_config_request(bad).is_error());
    bad=r; bad["base_snapshot"]["snapshot_version"]=1; EXPECT_TRUE(validate_live_config_request(bad).is_error());
    bad=r; bad["base_snapshot"].erase("sleeve_risk_modules"); EXPECT_TRUE(validate_live_config_request(bad).is_error());
    bad=r; bad["base_snapshot"]["max_drawdown"]=.4; EXPECT_TRUE(validate_live_config_request(bad).is_error());
    bad=r; bad["base_snapshot"]["optimization"]["capital"]=1; EXPECT_TRUE(validate_live_config_request(bad).is_error());
}
TEST(LiveConfigOverride, ProtocolEmitsSortedChangesAndNativeCanonicalHashes) {
    auto r=request(base_config(),{{"/optimization/tau",1.2},{"/execution/position_limit_live",13.25}});
    auto out=validate_live_config_request(r); ASSERT_TRUE(out.is_ok());
    EXPECT_EQ(out.value()["schema"],"live-config-validation/v1");
    EXPECT_EQ(out.value()["changed_paths"],Json::array({"/execution/position_limit_live","/optimization/tau"}));
    EXPECT_EQ(out.value()["base_sha256"].get<std::string>().size(),64u);
    EXPECT_NE(out.value()["base_sha256"],out.value()["effective_sha256"]);
    EXPECT_EQ(out.value()["effective_snapshot"]["execution"]["position_limit_live"],13.25);
}
TEST(LiveConfigOverride, RejectsDuplicateKeysNonfiniteAndDeepWireInput) {
    EXPECT_TRUE(parse_live_config_request("{\"changes\":{},\"changes\":{}}").is_error());
    EXPECT_TRUE(parse_live_config_request("{\"x\":{\"a\":1,\"a\":2}}").is_error());
    EXPECT_TRUE(parse_live_config_request("{\"x\":1e999}").is_error());
    EXPECT_TRUE(parse_live_config_request("{\"x\":NaN}").is_error());
    EXPECT_TRUE(parse_live_config_request(std::string(1024*1024+1,' ')).is_error());
    EXPECT_TRUE(parse_live_config_request(std::string(80,'[')+"0"+std::string(80,']')).is_error());
}
TEST(LiveConfigOverride, AcceptsEveryWiredFuturesLeafWithoutAddingMissingPaths) {
    const std::vector<std::pair<std::string,Json>> edits={
        {"/execution/position_limit_live",13.25},{"/optimization/tau",1.2},
        {"/optimization/cost_penalty_scalar",12.75},{"/optimization/max_iterations",120},
        {"/optimization/convergence_threshold",.0001},{"/optimization/use_buffering",false},
        {"/optimization/buffer_size_factor",.06},{"/use_optimization",true},{"/covariance_history_prices",800},
        {"/strategies/TREND/config/weight",.05},{"/strategies/TREND/config/risk_target",.25},
        {"/strategies/TREND/config/idm",3.0},{"/strategies/TREND/config/max_symbol_concentration",.2},
        {"/strategies/TREND/config/use_position_buffering",false},
        {"/strategies/TREND/config/carver_buffer_floor",.8},
        {"/strategies/TREND/config/carver_buffer_position_factor",.2},
        {"/strategies/TREND/config/ema_windows",Json::array({{16,64}})},
        {"/strategies/TREND/config/vol_lookback_short",40},{"/strategies/TREND/config/vol_lookback_long",256},
        {"/risk/modules/0/min_gate_dates",22},{"/risk/max_drawdown",.25},{"/risk/max_leverage",2.5}};
    for(const auto& [path,value]:edits) EXPECT_TRUE(apply_live_config_override(base_config(),{{path,value}}).is_ok()) << path;
    const std::vector<std::pair<std::string,Json>> risk={
        {"var_limit",.2},{"jump_risk_limit",.06},{"max_correlation",.8},
        {"max_gross_leverage",3},{"max_net_leverage",1.8},{"confidence_level",.98},{"lookback_period",260}};
    for(const auto& [key,value]:risk) EXPECT_TRUE(apply_live_config_override(base_config(),{
        {"/risk/modules/0/"+key,value},{"/risk/risk_reporting/"+key,value}}).is_ok()) << key;
    auto missing=base_config(); missing.strategies_config["TREND"]["config"].erase("weight");
    EXPECT_TRUE(apply_live_config_override(missing,{{"/strategies/TREND/config/weight",.1}}).is_error());
    EXPECT_TRUE(apply_live_config_override(base_config(),{{"/strategies/TREND/config/ema_windows",Json::array({{64,8}})}}).is_error());
}
TEST(LiveConfigOverride, AcceptsWiredEquityParametersAndKeepsRiskAssignment) {
    auto c=base_config(); c.use_optimization=false;
    c.strategies_config={{"MR",{{"type","MeanReversionStrategy"},{"enabled_live",true},{"default_allocation",1.0},
        {"config",{{"lookback_period",20},{"entry_threshold",2.0},{"exit_threshold",.5},{"risk_target",.15},
        {"position_size",.1},{"vol_lookback",20},{"use_stop_loss",true},{"stop_loss_pct",.05},
        {"allow_fractional_shares",true},{"fractional_min_price",1.0},{"fractional_min_adv",10000.0}}}}}};
    const std::vector<std::pair<std::string,Json>> edits={{"lookback_period",25},{"entry_threshold",2.5},
        {"exit_threshold",.6},{"risk_target",.2},{"position_size",.2},{"vol_lookback",30},
        {"stop_loss_pct",.04},{"allow_fractional_shares",false},{"fractional_min_price",2.0},{"fractional_min_adv",20000.0}};
    for(const auto& [key,value]:edits) EXPECT_TRUE(apply_live_config_override(c,{{"/strategies/MR/config/"+key,value}}).is_ok()) << key;
    EXPECT_TRUE(apply_live_config_override(c,{{"/strategies/MR/config/use_stop_loss",false}}).is_error());
    EXPECT_TRUE(apply_live_config_override(c,{{"/use_optimization",true}}).is_error());
    EXPECT_TRUE(apply_live_config_override(c,{{"/strategies/MR/config/entry_threshold",-1}}).is_error());
    EXPECT_TRUE(apply_live_config_override(c,{{"/strategies/MR/config/lookback_period",1}}).is_error());
}
TEST(LiveConfigOverride, SleeveNumericalTuningPreservesAssignmentAndPredicates) {
    auto c=base_config();
    c.risk_schema.sleeves["TREND"]={{"scale","constant_scale",ConstantScaleModuleConfig{.8,false}}};
    auto changed=apply_live_config_override(c,{{"/sleeve_risk_modules/TREND/0/scale",.7}});
    ASSERT_TRUE(changed.is_ok()); EXPECT_EQ(changed.value().risk_schema.sleeves.at("TREND")[0].id,"scale");
    EXPECT_TRUE(apply_live_config_override(c,{{"/sleeve_risk_modules/TREND/0/every_lap",true}}).is_error());
    EXPECT_TRUE(apply_live_config_override(c,{{"/sleeve_risk_modules/TREND",Json::array()}}).is_error());
}
TEST(LiveConfigOverride, PureParserRoundtripKeepsIdentityWithoutCredentialPlaceholders) {
    auto c=base_config(); c.database.password="private";
    auto s=build_runtime_trading_snapshot(c); ASSERT_TRUE(s.is_ok());
    auto parsed=parse_runtime_trading_snapshot(s.value()); ASSERT_TRUE(parsed.is_ok());
    EXPECT_TRUE(parsed.value().database.host.empty()); EXPECT_TRUE(parsed.value().database.password.empty());
    EXPECT_TRUE(parsed.value().email.password.empty());
    EXPECT_EQ(parsed.value().portfolio_id,"CONSERVATIVE_PORTFOLIO");
    EXPECT_EQ(parsed.value().max_drawdown,.3); EXPECT_EQ(parsed.value().max_leverage,2);
}
TEST(LiveConfigOverride, NoOverrideCanonicalHashValidatesBaseline) {
    const auto snapshot=build_runtime_trading_snapshot(base_config()); ASSERT_TRUE(snapshot.is_ok());
    const auto hash=live_config_snapshot_sha256(snapshot.value());
    ASSERT_TRUE(hash.is_ok()); EXPECT_EQ(hash.value().size(),64u);
    auto invalid=snapshot.value(); invalid["optimization"]["capital"]=1;
    EXPECT_TRUE(live_config_snapshot_sha256(invalid).is_error());
}
TEST(LiveConfigOverride, BaselineRejectsIntegerCoercionAndOutOfRangeConsumerInputs) {
    const auto s=build_runtime_trading_snapshot(base_config()); ASSERT_TRUE(s.is_ok());
    for(const auto& path:{"/snapshot_version","/optimization/max_iterations","/covariance_history_prices",
        "/backtest/lookback_years","/live/historical_days"}) {
        auto bad=s.value(); auto pointer=Json::json_pointer(path);
        bad[pointer]=bad.at(pointer).get<double>();
        EXPECT_TRUE(parse_runtime_trading_snapshot(bad).is_error()) << path;
    }
    for(const auto& [path,value]:std::vector<std::pair<std::string,Json>>{
        {"/optimization/max_iterations",-1},{"/optimization/tau",0},{"/execution/position_limit_live",0},
        {"/strategies/TREND/config/vol_lookback_long",1},{"/strategies/TREND/config/risk_target",1.1}}) {
        auto bad=s.value(); bad[Json::json_pointer(path)]=value;
        EXPECT_TRUE(parse_runtime_trading_snapshot(bad).is_error()) << path;
    }
}
TEST(LiveConfigOverride, DeniesInputsWithoutAnEligibleLiveProfileConsumer) {
    auto c=base_config(); c.use_optimization=false;
    c.strategies_config={{"MR",{{"type","MeanReversionStrategy"},{"enabled_live",true},
        {"default_allocation",1.0},{"config",{{"risk_target",.15},{"entry_threshold",2.0}}}}}};
    for(const auto& [path,value]:std::vector<std::pair<std::string,Json>>{
        {"/optimization/tau",2.0},{"/optimization/cost_penalty_scalar",12.75},
        {"/optimization/max_iterations",120},{"/optimization/convergence_threshold",.001},
        {"/optimization/use_buffering",false},{"/optimization/buffer_size_factor",.1},
        {"/use_optimization",true},{"/strategy_defaults/fdm",Json::array({{1,1.1}})},
        {"/strategy_defaults/carver_buffer_floor",1.0},{"/strategy_defaults/carver_buffer_position_factor",.2}})
        EXPECT_TRUE(apply_live_config_override(c,{{path,value}}).is_error()) << path;
    c=base_config(); c.strategies_config["OFF"]=c.strategies_config["TREND"];
    c.strategies_config["OFF"]["enabled_live"]=false;
    EXPECT_TRUE(apply_live_config_override(c,{{"/strategies/OFF/config/weight",.2}}).is_error());
    auto s=build_runtime_trading_snapshot(c); ASSERT_TRUE(s.is_ok());
    EXPECT_TRUE(parse_runtime_trading_snapshot(s.value()).is_ok());
}
TEST(LiveConfigOverride, GovernedBaselineRejectsUnknownMixedEmptyOrUnallocatedEnabledSets) {
    auto c=base_config();
    c.strategies_config["TREND"]["type"]="UnknownStrategy";
    EXPECT_TRUE(parse_runtime_trading_snapshot(build_runtime_trading_snapshot(c).value()).is_error());
    c=base_config(); c.strategies_config["MR"]={{"type","MeanReversionStrategy"},
        {"enabled_live",true},{"default_allocation",.5},{"config",{{"risk_target",.15}}}};
    c.strategies_config["TREND"]["default_allocation"]=.5;
    EXPECT_TRUE(parse_runtime_trading_snapshot(build_runtime_trading_snapshot(c).value()).is_error());
    c=base_config(); c.strategies_config["TREND"]["enabled_live"]=false;
    EXPECT_TRUE(parse_runtime_trading_snapshot(build_runtime_trading_snapshot(c).value()).is_error());
    c=base_config();
    for(const auto& allocation:{Json(),Json("1"),Json(-.5),Json(.7)}) {
        c.strategies_config["TREND"]["default_allocation"]=allocation;
        EXPECT_TRUE(parse_runtime_trading_snapshot(build_runtime_trading_snapshot(c).value()).is_error());
    }
    c=base_config(); c.strategies_config["TREND"].erase("default_allocation");
    EXPECT_TRUE(parse_runtime_trading_snapshot(build_runtime_trading_snapshot(c).value()).is_error());
    // Legacy file-only serialization keeps its original allocation for its own selector.
    c=base_config(); c.strategies_config["TREND"]["default_allocation"]=.7;
    const auto legacy=build_runtime_trading_snapshot(c); ASSERT_TRUE(legacy.is_ok());
    EXPECT_EQ(legacy.value()["strategies"]["TREND"]["default_allocation"],.7);
}
TEST(LiveConfigOverride, NativeBaseHashKeepsLegacyAllocationNormalizationAvailable) {
    auto c=base_config(); c.strategies_config["TREND"]["default_allocation"]=.7;
    const auto hash=live_config_snapshot_sha256(c);
    ASSERT_TRUE(hash.is_ok()); EXPECT_EQ(hash.value().size(),64u);
    EXPECT_TRUE(parse_runtime_trading_snapshot(build_runtime_trading_snapshot(c).value()).is_error());
}

TEST(LiveConfigOverride, DefaultEligibilityMatchesActualFactoryConsumers) {
    for (const auto* type : {"TrendFollowingStrategy", "TrendFollowingFastStrategy", "TrendFollowingSlowStrategy"}) {
        auto c=base_config(); c.strategies_config["TREND"]["type"]=type;
        auto resolve=[&](const AppConfig& config) {
            return resolve_factory_trend_config(type, config.strategies_config.at("TREND"), config.strategy_defaults, std::nullopt);
        };
        auto factory=resolve(c);
        std::visit([&](const auto& actual) {
            using T=std::decay_t<decltype(actual)>;
            if constexpr (!std::is_same_v<T,std::monostate>) {
                EXPECT_EQ(actual.carver_buffer_floor,.5); EXPECT_EQ(actual.carver_buffer_position_factor,.1);
                auto defaults=c.strategy_defaults; defaults.fdm={{1,9.0}};
                const auto changed=resolve_factory_trend_config(type,c.strategies_config.at("TREND"),defaults,std::nullopt);
                EXPECT_EQ(std::get<T>(changed).fdm,actual.fdm); // Existing nonempty struct table shadows FDM.
            }
        },factory);
        EXPECT_TRUE(apply_live_config_override(c,{{"/strategy_defaults/fdm",Json::array({{1,9.0}})}}).is_error());
        for (const auto* key : {"carver_buffer_floor","carver_buffer_position_factor"}) {
            const std::string path=std::string("/strategy_defaults/")+key;
            EXPECT_TRUE(apply_live_config_override(c,{{path,.8}}).is_error()); // Both supplied: defaults shadowed.
            auto fallback=c; fallback.strategies_config["TREND"]["config"].erase(key);
            auto changed=apply_live_config_override(fallback,{{path,.8}}); ASSERT_TRUE(changed.is_ok()) << type << key;
            std::visit([&](const auto& actual) {
                using T=std::decay_t<decltype(actual)>;
                if constexpr (!std::is_same_v<T,std::monostate>) {
                    const auto before=std::get<T>(resolve(fallback));
                    if (std::string(key)=="carver_buffer_floor") {
                        EXPECT_EQ(before.carver_buffer_floor,.5); EXPECT_EQ(actual.carver_buffer_floor,.8);
                    } else { EXPECT_EQ(before.carver_buffer_position_factor,0); EXPECT_EQ(actual.carver_buffer_position_factor,.8); }
                }
            },resolve(changed.value()));
            auto absent=c; absent.strategies_config["TREND"].erase("config");
            EXPECT_TRUE(apply_live_config_override(absent,{{path,.8}}).is_error()); // No config branch uses struct values.
        }
        // A disabled fallback cannot authorize a shadowed live default.
        c.strategies_config["OFF"]=c.strategies_config["TREND"];
        c.strategies_config["OFF"]["enabled_live"]=false;
        c.strategies_config["OFF"]["config"].erase("carver_buffer_floor");
        EXPECT_TRUE(apply_live_config_override(c,{{"/strategy_defaults/carver_buffer_floor",.8}}).is_error());
    }
}
TEST(LiveConfigOverride, GovernedSleevesMustBelongToSelectedLiveStrategies) {
    auto c=base_config(); c.strategies_config["OFF"]=c.strategies_config["TREND"];
    c.strategies_config["OFF"]["enabled_live"]=false;
    c.risk_schema.sleeves["OFF"]={{"off_scale","constant_scale",ConstantScaleModuleConfig{.8,false}}};
    const auto snapshot=build_runtime_trading_snapshot(c); ASSERT_TRUE(snapshot.is_ok()); // Legacy builder stays permissive.
    EXPECT_TRUE(live_config_snapshot_sha256(c).is_ok());
    EXPECT_TRUE(parse_runtime_trading_snapshot(snapshot.value()).is_error());
    EXPECT_TRUE(apply_live_config_override(c,{{"/sleeve_risk_modules/OFF/0/scale",.7}}).is_error());
    EXPECT_TRUE(apply_live_config_override(c,{{"/optimization/tau",1.2}}).is_error());
}
TEST(LiveConfigOverride, OmittedVolatilityDefaultsAreResolvedBeforeOverflowChecks) {
    for (const auto* type : {"TrendFollowingStrategy", "TrendFollowingFastStrategy", "TrendFollowingSlowStrategy"}) {
        auto c=base_config(); auto& def=c.strategies_config["TREND"]; def["type"]=type;
        def["config"].erase("vol_lookback_long");
        const auto resolve=[&](const AppConfig& config) {
            return resolve_factory_trend_config(type,config.strategies_config.at("TREND"),config.strategy_defaults,std::nullopt);
        };
        std::visit([](auto actual) {
            using T=std::decay_t<decltype(actual)>;
            if constexpr (!std::is_same_v<T,std::monostate>) { EXPECT_EQ(actual.vol_lookback_long,252); }
        },resolve(c));
        EXPECT_TRUE(apply_live_config_override(c,{{"/strategies/TREND/config/vol_lookback_short",std::numeric_limits<int>::max()}}).is_error());
        EXPECT_TRUE(apply_live_config_override(c,{{"/strategies/TREND/config/vol_lookback_short",std::numeric_limits<int>::max()/4+1}}).is_error());
        EXPECT_TRUE(apply_live_config_override(c,{{"/strategies/TREND/config/vol_lookback_short",std::numeric_limits<int>::max()/4}}).is_ok());
        def["config"]["vol_lookback_short"]=std::numeric_limits<int>::max();
        EXPECT_TRUE(parse_runtime_trading_snapshot(build_runtime_trading_snapshot(c).value()).is_error());
        def["config"]["vol_lookback_short"]=32;
        auto safe=apply_live_config_override(c,{{"/strategies/TREND/config/vol_lookback_short",500}}); ASSERT_TRUE(safe.is_ok());
        std::visit([](auto actual) {
            using T=std::decay_t<decltype(actual)>;
            if constexpr (!std::is_same_v<T,std::monostate>) {
                EXPECT_EQ(actual.vol_lookback_short,500); EXPECT_EQ(actual.vol_lookback_long,252);
                normalize_constructor_trend_config(actual); EXPECT_EQ(actual.vol_lookback_long,2000);
                EXPECT_EQ(actual.max_history_size,2000u);
            }
        },resolve(safe.value()));
        // The other omitted key also resolves through the factory, not supplied JSON alone.
        def["config"].erase("vol_lookback_short"); def["config"]["vol_lookback_long"]=256;
        auto long_edit=apply_live_config_override(c,{{"/strategies/TREND/config/vol_lookback_long",300}});
        ASSERT_TRUE(long_edit.is_ok());
        std::visit([&](auto actual) {
            using T=std::decay_t<decltype(actual)>;
            if constexpr (!std::is_same_v<T,std::monostate>) {
                EXPECT_EQ(actual.vol_lookback_short,std::string(type)=="TrendFollowingFastStrategy"?16:std::string(type)=="TrendFollowingSlowStrategy"?64:32);
                EXPECT_EQ(actual.vol_lookback_long,300); normalize_constructor_trend_config(actual);
                EXPECT_EQ(actual.vol_lookback_long,300);
            }
        },resolve(long_edit.value()));
    }
}
TEST(LiveConfigOverride, AttributedNonCarverPortfolioSurvivesSnapshotAndTuning) {
    auto c=base_config(); auto risk=c.risk_schema.to_json();
    risk["modules"]=Json::array({{{"id","scale"},{"type","constant_scale"},{"scale",.8},{"every_lap",false}},
        {{"id","warn"},{"type","warn"},{"condition",{{"kind","lap_at_least"},{"threshold",3}}},{"reason","fixture"}}});
    risk["_ruled_by"]="unit test"; risk["_ruled_on"]="2026-10-06";
    auto parsed=parse_risk_schema(risk,Json::object(),c.strategies_config,c.portfolio_id); ASSERT_TRUE(parsed.is_ok());
    c.risk_schema=parsed.value();
    auto snapshot=build_runtime_trading_snapshot(c); ASSERT_TRUE(snapshot.is_ok());
    EXPECT_EQ(snapshot.value()["risk"].value("_ruled_by",std::string()),"unit test");
    EXPECT_EQ(snapshot.value()["risk"].value("_ruled_on",std::string()),"2026-10-06");
    auto roundtrip=parse_runtime_trading_snapshot(snapshot.value()); ASSERT_TRUE(roundtrip.is_ok());
    auto tuned=apply_live_config_override(roundtrip.value(),{{"/risk/modules/0/scale",.7}}); ASSERT_TRUE(tuned.is_ok());
    auto effective=build_runtime_trading_snapshot(tuned.value()); ASSERT_TRUE(effective.is_ok());
    EXPECT_EQ(effective.value()["risk"]["modules"][0]["scale"],.7);
    EXPECT_EQ(effective.value()["risk"]["_ruled_by"],"unit test"); EXPECT_EQ(effective.value()["risk"]["_ruled_on"],"2026-10-06");
    EXPECT_EQ(effective.value()["risk"]["modules"][1],snapshot.value()["risk"]["modules"][1]);
    EXPECT_TRUE(apply_live_config_override(tuned.value(),{{"/risk/_ruled_by","somebody else"}}).is_error());
}
