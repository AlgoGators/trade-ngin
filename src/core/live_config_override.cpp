#include "trade_ngin/core/live_config_override.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include <algorithm>
#include <cctype>
#include <optional>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <vector>
namespace trade_ngin {
namespace {
using Json = nlohmann::json;
struct Refusal { const char* code; };
void require(bool condition, const char* code = "live_config_invalid_effective") {
    if (!condition) throw Refusal{code};
}
void finite_tree(const Json& node) {
    if (node.is_number_float()) require(std::isfinite(node.get<double>()), "live_config_nonfinite");
    if (node.is_structured()) for (const auto& child : node) finite_tree(child);
}
bool secret_tree(const Json& node) {
    if (node.is_object()) for (const auto& item : node.items()) {
        auto key=item.key();
        std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return std::tolower(c);});
        if (key=="database" || key=="email" || key.find("password")!=std::string::npos ||
            key.find("secret")!=std::string::npos || key.find("token")!=std::string::npos ||
            key.find("credential")!=std::string::npos || key.rfind("smtp",0)==0 || secret_tree(item.value())) return true;
    }
    if (node.is_array()) for (const auto& item : node) if(secret_tree(item)) return true;
    return false;
}
std::string escape(const std::string& key) {
    std::string out;
    for(char c:key) { if(c=='~') out+="~0"; else if(c=='/') out+="~1"; else out+=c; }
    return out;
}
enum class Type { Number, Integer, Boolean, IntegerPairs, NumberPairs };
using Policy = std::map<std::string,Type>;
void leaves(Policy& policy,const Json& node,const std::string& root,
            const std::vector<std::pair<std::string,Type>>& keys) {
    if(!node.is_object()) return;
    for(const auto& [key,type]:keys) if(node.contains(key)) policy.emplace(root+escape(key),type);
}
const std::vector<std::pair<std::string,Type>> risk_fields={
    {"var_limit",Type::Number},{"jump_risk_limit",Type::Number},{"max_correlation",Type::Number},
    {"max_gross_leverage",Type::Number},{"max_net_leverage",Type::Number},
    {"confidence_level",Type::Number},{"lookback_period",Type::Integer}};
void modules(Policy& policy,const Json& list,const std::string& root) {
    for(size_t i=0;i<list.size();++i) {
        const auto& module=list.at(i); const auto prefix=root+std::to_string(i)+"/";
        const auto type=module.at("type").get<std::string>();
        if(type=="carver") { leaves(policy,module,prefix,risk_fields);
            leaves(policy,module,prefix,{{"min_gate_dates",Type::Integer}}); }
        if(type=="constant_scale") leaves(policy,module,prefix,{{"scale",Type::Number}});
        // none, warning/refusal predicates and activation flags are immutable.
    }
}
Policy editable(const Json& snapshot, bool live_profile_only = true) {
    Policy policy;
    bool live_trend = false;
    for (const auto& entry : snapshot.at("strategies").items()) {
        if (!entry.value().is_object() || !entry.value().value("enabled_live",false)) continue;
        const auto type = entry.value().value("type", std::string("TrendFollowingStrategy"));
        if (type == "TrendFollowingStrategy" || type == "TrendFollowingFastStrategy" ||
            type == "TrendFollowingSlowStrategy") live_trend = true;
    }
    leaves(policy,snapshot,"/",{{"covariance_history_prices",Type::Integer}});
    if (!live_profile_only || live_trend)
        leaves(policy,snapshot,"/",{{"use_optimization",Type::Boolean}});
    leaves(policy,snapshot.at("execution"),"/execution/",{{"position_limit_live",Type::Number}});
    if (!live_profile_only || live_trend)
        leaves(policy,snapshot.at("optimization"),"/optimization/",{
        {"tau",Type::Number},{"cost_penalty_scalar",Type::Number},{"max_iterations",Type::Integer},
        {"convergence_threshold",Type::Number},{"use_buffering",Type::Boolean},{"buffer_size_factor",Type::Number}});
    if (!live_profile_only || live_trend)
        leaves(policy,snapshot.at("strategy_defaults"),"/strategy_defaults/",{
        {"fdm",Type::NumberPairs},{"carver_buffer_floor",Type::Number},{"carver_buffer_position_factor",Type::Number}});
    const auto& risk=snapshot.at("risk");
    leaves(policy,risk,"/risk/",{{"max_drawdown",Type::Number},{"max_leverage",Type::Number}});
    leaves(policy,risk.at("risk_reporting"),"/risk/risk_reporting/",risk_fields);
    modules(policy,risk.at("modules"),"/risk/modules/");
    for(const auto& sleeve:snapshot.at("sleeve_risk_modules").items())
        modules(policy,sleeve.value(),"/sleeve_risk_modules/"+escape(sleeve.key())+"/");
    for(const auto& entry:snapshot.at("strategies").items()) {
        if(!entry.value().is_object()) continue;
        const auto& def=entry.value();
        if (live_profile_only && !def.value("enabled_live",false)) continue;
        const auto type=def.value("type",std::string("TrendFollowingStrategy"));
        if(!def.contains("config")) continue;
        const auto& params=def.at("config"); const auto root="/strategies/"+escape(entry.key())+"/config/";
        if(type=="TrendFollowingStrategy" || type=="TrendFollowingFastStrategy" || type=="TrendFollowingSlowStrategy")
            leaves(policy,params,root,{{"weight",Type::Number},{"risk_target",Type::Number},
                {"idm",Type::Number},{"max_symbol_concentration",Type::Number},{"use_position_buffering",Type::Boolean},
                {"carver_buffer_floor",Type::Number},{"carver_buffer_position_factor",Type::Number},
                {"ema_windows",Type::IntegerPairs},{"vol_lookback_short",Type::Integer},{"vol_lookback_long",Type::Integer}});
        if(type=="MeanReversionStrategy") leaves(policy,params,root,{
            {"lookback_period",Type::Integer},{"entry_threshold",Type::Number},{"exit_threshold",Type::Number},
            {"risk_target",Type::Number},{"position_size",Type::Number},{"vol_lookback",Type::Integer},
            {"stop_loss_pct",Type::Number},{"allow_fractional_shares",Type::Boolean},
            {"fractional_min_price",Type::Number},{"fractional_min_adv",Type::Number}});
    }
    return policy;
}
bool integer(const Json& value) {
    if(!value.is_number_integer()) return false;
    if(value.is_number_unsigned()) return value.get<uint64_t>()<=static_cast<uint64_t>(std::numeric_limits<int>::max());
    return value.get<int64_t>()>=std::numeric_limits<int>::min() && value.get<int64_t>()<=std::numeric_limits<int>::max();
}
void check_type(const Json& value,Type type) {
    if(type==Type::Number) require(value.is_number(),"live_config_invalid_type");
    else if(type==Type::Integer) require(integer(value),"live_config_invalid_type");
    else if(type==Type::Boolean) require(value.is_boolean(),"live_config_invalid_type");
    else {
        require(value.is_array() && !value.empty() && value.size()<=1024,"live_config_invalid_type");
        for(const auto& pair:value) {
            require(pair.is_array() && pair.size()==2 && integer(pair.at(0)) &&
                (type==Type::IntegerPairs?integer(pair.at(1)):pair.at(1).is_number()),"live_config_invalid_type");
            require(pair.at(0).get<int>()>0);
            if(type==Type::IntegerPairs) require(pair.at(1).get<int>()>pair.at(0).get<int>());
            else require(pair.at(1).get<double>()>0);
        }
    }
    finite_tree(value);
}
void number_bound(const Json& params,const char* key,double minimum,double maximum,bool strict=true) {
    if(!params.contains(key)) return;
    require(params.at(key).is_number()); const auto v=params.at(key).get<double>();
    require((strict?v>minimum:v>=minimum) && v<=maximum);
}
void integer_bound(const Json& params,const char* key,int minimum) {
    if(!params.contains(key)) return;
    require(integer(params.at(key)) && params.at(key).get<int>()>=minimum);
}
void validate_consumers(const AppConfig& c) {
    // Match controlled selection: explicit positive weights, no silent redistribution.
    double allocation_sum = 0.0;
    int profile = 0; // 1 = futures trend family; 2 = equity mean reversion.
    for (const auto& entry : c.strategies_config.items()) {
        const auto& definition = entry.value();
        require(definition.is_object());
        const bool enabled = definition.value("enabled_live", false);
        if (!enabled) continue;
        const auto type = definition.value("type", std::string("TrendFollowingStrategy"));
        const int current = type == "MeanReversionStrategy" ? 2 :
            (type == "TrendFollowingStrategy" || type == "TrendFollowingFastStrategy" ||
             type == "TrendFollowingSlowStrategy") ? 1 : 0;
        require(current != 0 && (profile == 0 || profile == current));
        profile = current;
        require(definition.contains("default_allocation") && definition.at("default_allocation").is_number());
        const auto weight = definition.at("default_allocation").get<double>();
        require(std::isfinite(weight) && weight > 0.0);
        allocation_sum += weight;
    }
    require(profile != 0 && std::isfinite(allocation_sum) && std::abs(allocation_sum - 1.0) <= 1e-9);
    require(c.initial_capital>0 && c.reserve_capital_pct>=0 && c.reserve_capital_pct<1);
    require(c.execution.position_limit_live>0);
    const auto opt=c.opt_config.to_json();
    number_bound(opt,"tau",0,std::numeric_limits<double>::max());
    number_bound(opt,"cost_penalty_scalar",0,std::numeric_limits<double>::max(),false);
    number_bound(opt,"buffer_size_factor",0,std::numeric_limits<double>::max(),false);
    number_bound(opt,"convergence_threshold",0,std::numeric_limits<double>::max());
    integer_bound(opt,"max_iterations",1);
    require(c.covariance_history_prices>=2 && c.covariance_history_prices<=static_cast<size_t>(std::numeric_limits<int>::max()));
    for(const auto& [count,mult]:c.strategy_defaults.fdm) require(count>0 && mult>0);
    require(!c.strategy_defaults.fdm.empty() && c.strategy_defaults.carver_buffer_floor>=0 && c.strategy_defaults.carver_buffer_position_factor>=0);
    std::optional<bool> fractional;
    const auto full_config = c.to_json();
    const auto known_inputs = editable(full_config, false);
    for(const auto& entry:c.strategies_config.items()) {
        const auto& def=entry.value(); if(!def.is_object()) continue;
        const auto type=def.value("type",std::string("TrendFollowingStrategy"));
        const bool trend=type=="TrendFollowingStrategy" || type=="TrendFollowingFastStrategy" || type=="TrendFollowingSlowStrategy";
        const bool equity=type=="MeanReversionStrategy";
        if(!trend && !equity) continue;
        if(equity && def.value("enabled_live",false)) require(!c.use_optimization);
        if(!def.contains("config")) continue;
        const auto& p=def.at("config"); require(p.is_object());
        number_bound(p,"risk_target",0,1);
        if(trend) {
            number_bound(p,"weight",0,std::numeric_limits<double>::max());
            number_bound(p,"idm",0,std::numeric_limits<double>::max());
            number_bound(p,"max_symbol_concentration",0,1);
            number_bound(p,"carver_buffer_floor",0,std::numeric_limits<double>::max(),false);
            number_bound(p,"carver_buffer_position_factor",0,std::numeric_limits<double>::max(),false);
            integer_bound(p,"vol_lookback_short",1); integer_bound(p,"vol_lookback_long",2);
            if(p.contains("vol_lookback_short") && p.contains("vol_lookback_long"))
                require(p.at("vol_lookback_long").get<int>()>p.at("vol_lookback_short").get<int>());
            if(p.contains("ema_windows")) check_type(p.at("ema_windows"),Type::IntegerPairs);
        } else {
            integer_bound(p,"lookback_period",2); integer_bound(p,"vol_lookback",2);
            number_bound(p,"entry_threshold",0,std::numeric_limits<double>::max());
            number_bound(p,"exit_threshold",0,std::numeric_limits<double>::max(),false);
            number_bound(p,"position_size",0,1); number_bound(p,"stop_loss_pct",0,1);
            number_bound(p,"fractional_min_price",0,std::numeric_limits<double>::max());
            number_bound(p,"fractional_min_adv",0,std::numeric_limits<double>::max(),false);
            if(def.value("enabled_live",false)) {
                const auto allows=p.value("allow_fractional_shares",true);
                if(fractional) require(*fractional==allows);
                fractional=allows;
            }
        }
        // Check exact types for all known configured inputs, including immutable activation.
        const auto root="/strategies/"+escape(entry.key())+"/config/";
        for(const auto& [path,t]:known_inputs) if(path.rfind(root,0)==0)
            check_type(full_config.at(Json::json_pointer(path)),t);
        if(p.contains("use_stop_loss")) require(p.at("use_stop_loss").is_boolean());
    }
}
}

Result<nlohmann::json> build_runtime_trading_snapshot(const AppConfig& c) {
    try {
        Json snapshot={{"snapshot_version",2},{"portfolio_id",c.portfolio_id},
            {"initial_capital",c.initial_capital},{"reserve_capital_pct",c.reserve_capital_pct},
            {"benchmark_mode",c.benchmark_mode},{"execution",c.execution.to_json()},
            {"optimization",c.opt_config.to_json()},{"risk",c.risk_schema.to_json()},
            {"sleeve_risk_modules",c.risk_schema.sleeves_to_json()},{"use_optimization",c.use_optimization},
            {"covariance_history_prices",c.covariance_history_prices},{"max_drawdown",c.max_drawdown},
            {"max_leverage",c.max_leverage},{"backtest",c.backtest.to_json()},{"live",c.live.to_json()},
            {"strategy_defaults",c.strategy_defaults.to_json()},{"strategies",c.strategies_config}};
        finite_tree(snapshot); require(!secret_tree(snapshot),"runtime_snapshot_contains_secret_key");
        return snapshot;
    } catch(const Refusal& e) { return make_error<Json>(ErrorCode::INVALID_ARGUMENT,e.code); }
      catch(const std::exception&) { return make_error<Json>(ErrorCode::INVALID_ARGUMENT,"live_config_invalid_snapshot"); }
}
Result<AppConfig> parse_runtime_trading_snapshot(const Json& snapshot) {
    try {
        require(snapshot.is_object() && integer(snapshot.at("snapshot_version")) &&
                snapshot.at("snapshot_version")==2,"live_config_invalid_base");
        finite_tree(snapshot); require(!secret_tree(snapshot),"live_config_invalid_base");
        // Validate wire types before typed extraction can coerce a whole-valued float.
        for (const auto& [path,type] : editable(snapshot, false))
            check_type(snapshot.at(Json::json_pointer(path)),type);
        require(integer(snapshot.at("backtest").at("lookback_years")));
        const auto& live = snapshot.at("live");
        for (const char* key : {"historical_days", "data_staleness_tolerance_days",
                               "execution_price_max_staleness_days"})
            if (live.contains(key)) require(integer(live.at(key)));
        auto parsed=ConfigLoader::parse_trading_config(snapshot);
        require(parsed.is_ok(),"live_config_invalid_base");
        validate_consumers(parsed.value());
        auto canonical=build_runtime_trading_snapshot(parsed.value());
        require(canonical.is_ok() && canonical.value()==snapshot,"live_config_invalid_base");
        return parsed;
    } catch(const Refusal&) { return make_error<AppConfig>(ErrorCode::INVALID_ARGUMENT,"live_config_invalid_base"); }
      catch(const std::exception&) { return make_error<AppConfig>(ErrorCode::INVALID_ARGUMENT,"live_config_invalid_base"); }
}
Result<std::string> live_config_snapshot_sha256(const AppConfig& config) {
    auto snapshot = build_runtime_trading_snapshot(config);
    if (snapshot.is_error()) return make_error<std::string>(snapshot.error()->code(),
                                                           snapshot.error()->what());
    return qt_sha256_hex(snapshot.value().dump());
}
Result<std::string> live_config_snapshot_sha256(const Json& snapshot) {
    auto parsed = parse_runtime_trading_snapshot(snapshot);
    if (parsed.is_error()) return make_error<std::string>(ErrorCode::INVALID_ARGUMENT,
                                                         "live_config_invalid_base");
    auto canonical = build_runtime_trading_snapshot(parsed.value());
    if (canonical.is_error()) return make_error<std::string>(ErrorCode::INVALID_ARGUMENT,
                                                            "live_config_invalid_base");
    return qt_sha256_hex(canonical.value().dump());
}
Result<AppConfig> apply_live_config_override(const AppConfig& base,const Json& changes) {
    try {
        require(changes.is_object() && !changes.empty(),"live_config_noop");
        auto snapshot=build_runtime_trading_snapshot(base);
        require(snapshot.is_ok(),"live_config_invalid_base");
        auto baseline=parse_runtime_trading_snapshot(snapshot.value());
        require(baseline.is_ok(),"live_config_invalid_base");
        auto effective=snapshot.value(); const auto policy=editable(effective); bool changed=false;
        for(const auto& edit:changes.items()) {
            const auto found=policy.find(edit.key());
            require(found!=policy.end(),"live_config_path_denied");
            const auto pointer=Json::json_pointer(edit.key());
            require(effective.contains(pointer),"live_config_path_denied");
            check_type(edit.value(),found->second);
            if(effective.at(pointer)!=edit.value()) changed=true;
            effective[pointer]=edit.value();
        }
        require(changed,"live_config_noop");
        // C1 coupling is explicit, rather than silently synchronizing reporting or gates.
        const auto& list=snapshot.value().at("risk").at("modules");
        for(size_t i=0;i<list.size();++i) if(list.at(i).at("type")=="carver") {
            for(const auto& [key,type]:risk_fields) {
                (void)type;
                const auto gate="/risk/modules/"+std::to_string(i)+"/"+key;
                const auto reporting="/risk/risk_reporting/"+key;
                if(changes.contains(gate) || changes.contains(reporting))
                    require(changes.contains(gate) && changes.contains(reporting) &&
                        changes.at(gate)==changes.at(reporting),"live_config_coupled_risk_required");
            }
        }
        auto parsed=ConfigLoader::parse_trading_config(effective);
        require(parsed.is_ok()); validate_consumers(parsed.value());
        // Preserve private connection/delivery settings without passing them to the parser.
        auto result = parsed.value();
        result.database=base.database; result.email=base.email;
        return result;
    } catch(const Refusal& e) { return make_error<AppConfig>(ErrorCode::INVALID_ARGUMENT,e.code); }
      catch(const std::exception&) { return make_error<AppConfig>(ErrorCode::INVALID_ARGUMENT,"live_config_invalid_effective"); }
}
Result<Json> validate_live_config_request(const Json& request) {
    try {
        require(request.is_object() && request.size()==3 && request.at("schema")=="live-config-validation/v1",
                "live_config_invalid_request");
        finite_tree(request);
        auto base=parse_runtime_trading_snapshot(request.at("base_snapshot"));
        require(base.is_ok(),"live_config_invalid_base");
        auto effective=apply_live_config_override(base.value(),request.at("changes"));
        if(effective.is_error()) return make_error<Json>(ErrorCode::INVALID_ARGUMENT,effective.error()->what());
        auto base_snapshot=build_runtime_trading_snapshot(base.value());
        auto effective_snapshot=build_runtime_trading_snapshot(effective.value());
        require(base_snapshot.is_ok() && effective_snapshot.is_ok());
        auto base_hash=qt_sha256_hex(base_snapshot.value().dump());
        auto effective_hash=qt_sha256_hex(effective_snapshot.value().dump());
        require(base_hash.is_ok() && effective_hash.is_ok(),"live_config_hash_unavailable");
        Json paths=Json::array();
        for(const auto& edit:request.at("changes").items()) {
            const auto pointer=Json::json_pointer(edit.key());
            if(base_snapshot.value().at(pointer)!=effective_snapshot.value().at(pointer)) paths.push_back(edit.key());
        }
        return Json{{"schema","live-config-validation/v1"},{"base_sha256",base_hash.value()},
            {"effective_sha256",effective_hash.value()},{"effective_snapshot",effective_snapshot.value()},
            {"changed_paths",paths}};
    } catch(const Refusal& e) { return make_error<Json>(ErrorCode::INVALID_ARGUMENT,e.code); }
      catch(const std::exception&) { return make_error<Json>(ErrorCode::INVALID_ARGUMENT,"live_config_invalid_request"); }
}
Result<Json> parse_live_config_request(std::string_view bytes) {
    try {
        require(!bytes.empty() && bytes.size()<=live_config_max_request_bytes,"live_config_request_size");
        std::vector<std::set<std::string>> keys;
        auto callback=[&](int depth,Json::parse_event_t event,Json& parsed) {
            require(depth<=64,"live_config_request_depth");
            if(event==Json::parse_event_t::object_start) keys.emplace_back();
            if(event==Json::parse_event_t::key) require(keys.back().insert(parsed.get<std::string>()).second,"live_config_duplicate_key");
            if(event==Json::parse_event_t::object_end) keys.pop_back();
            return true;
        };
        auto result=Json::parse(bytes.begin(),bytes.end(),callback);
        finite_tree(result); return result;
    } catch(const Refusal& e) { return make_error<Json>(ErrorCode::INVALID_ARGUMENT,e.code); }
      catch(const std::exception&) { return make_error<Json>(ErrorCode::INVALID_ARGUMENT,"live_config_invalid_json"); }
}
}
