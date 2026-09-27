#include "trade_ngin/apps/equity_strategy_consumption.hpp"
#include <cmath>
#include <set>
#include <type_traits>
namespace trade_ngin {
namespace {
bool identity(const std::string& value) {
    if(value.empty() || value.size()>64)return false;
    for(char c:value)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-'||c=='/'))return false;
    return true;
}
template<class T> void observed(nlohmann::json& output,const char* key,const std::optional<T>& value) {
    if(!value)return;
    if constexpr(std::is_floating_point_v<T>)if(!std::isfinite(*value))throw std::invalid_argument("nonfinite_observed_read");
    output[key]=*value;
}
}
Result<nlohmann::json> project_equity_strategy_consumption(const StrategyConsumptionTrace& trace) {
    if(trace.profile!=StrategyConsumptionProfile::MeanReversion)
        return make_error<nlohmann::json>(ErrorCode::INVALID_ARGUMENT,"equity_consumer_required");
    if(trace.mean_reversion.capacity_exceeded || trace.mean_reversion.symbols.size()>256)
        return make_error<nlohmann::json>(ErrorCode::INVALID_DATA,"equity_consumption_capacity_exceeded");
    try {
        nlohmann::json symbols=nlohmann::json::object();
        for(const auto& [symbol,read]:trace.mean_reversion.symbols) {
            if(!identity(symbol))throw std::invalid_argument("invalid_observed_symbol");
            if(read.position_limit.present!=read.position_limit.value.has_value() ||
               (!read.position_limit_reached && (read.position_limit.present || read.position_limit.value)) ||
               (!read.sizing_reached && (read.capital_allocation || read.position_size || read.risk_target ||
                    read.allow_fractional_shares || read.fractional_min_price || read.fractional_eligible || read.position_limit_reached)) ||
               (!read.fractional_adv_reached && (read.fractional_min_adv || read.volume_sample_count || read.average_daily_volume)) ||
               (read.fractional_adv_reached && (!read.volume_sample_count || !read.average_daily_volume || !read.fractional_min_adv)) ||
               (read.allow_fractional_shares==false && read.fractional_min_price) ||
               (read.use_stop_loss==false && read.stop_loss_pct))
                throw std::invalid_argument("contradictory_observed_branch");
            nlohmann::json values=nlohmann::json::object();
            observed(values,"lookback_period",read.lookback_period);observed(values,"vol_lookback",read.vol_lookback);
            observed(values,"maximum_price_history",read.maximum_price_history);observed(values,"maximum_volatility_history",read.maximum_volatility_history);
            observed(values,"entry_threshold",read.entry_threshold);observed(values,"exit_threshold",read.exit_threshold);
            observed(values,"use_stop_loss",read.use_stop_loss);observed(values,"stop_loss_pct",read.stop_loss_pct);
            observed(values,"capital_allocation",read.capital_allocation);observed(values,"position_size",read.position_size);
            observed(values,"risk_target",read.risk_target);observed(values,"allow_fractional_shares",read.allow_fractional_shares);
            observed(values,"fractional_min_price",read.fractional_min_price);observed(values,"fractional_min_adv",read.fractional_min_adv);
            if(read.position_limit_reached) {
                values["position_limit_present"]=read.position_limit.present;
                observed(values,"position_limit",read.position_limit.value);
            }
            nlohmann::json state=nlohmann::json::object();
            observed(state,"volume_sample_count",read.volume_sample_count);observed(state,"average_daily_volume",read.average_daily_volume);
            observed(state,"fractional_eligible",read.fractional_eligible);observed(state,"short_allowed",read.short_allowed);
            symbols[symbol]={{"reads",values},{"observed_state",state}};
        }
        return Result<nlohmann::json>(nlohmann::json{
            {"schema_version","qt-equity-strategy-consumption/v1"},{"available",true},
            {"profile","mean_reversion"},{"scope","strategy_invocation"},
            {"full_run_certification",false},{"symbols",symbols}});
    } catch(const std::exception& error) {
        return make_error<nlohmann::json>(ErrorCode::INVALID_DATA,error.what());
    }
}
}
