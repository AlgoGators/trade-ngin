#include "trade_ngin/apps/consumption_projection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "trade_ngin/apps/run_consumption.hpp"

namespace trade_ngin {
namespace {
using Json = nlohmann::json;
const Json& catalog();
constexpr std::array<const char*, 8> stages = {"setup", "market_input", "cost_history",
    "preparation", "primary", "execution", "diagnostics", "control_flow"};

const char* unavailable_text(ConsumptionUnavailableReason reason) {
    switch (reason) {
        case ConsumptionUnavailableReason::InstrumentationMissing: return "instrumentation_missing";
        case ConsumptionUnavailableReason::UnsupportedConsumer: return "unsupported_consumer";
        case ConsumptionUnavailableReason::CapacityExceeded: return "capacity_exceeded";
        case ConsumptionUnavailableReason::InvalidObservedValue: return "invalid_observed_value";
        case ConsumptionUnavailableReason::InvalidObservedIdentity: return "invalid_observed_identity";
    }
    return "invalid_observed_value";
}
struct Fault { ConsumptionUnavailableReason reason; };
[[noreturn]] void fail(ConsumptionUnavailableReason reason) { throw Fault{reason}; }

const Json& consumer_def(std::string_view name) {
    for (const auto& def : catalog().at("consumers")) if (def.at("name") == name) return def;
    fail(ConsumptionUnavailableReason::InvalidObservedValue);
}
const Json& field_def(std::string_view consumer, std::string_view field) {
    for (const auto& def : catalog().at("fields"))
        if (def.at("consumer") == consumer && def.at("field") == field) return def;
    fail(ConsumptionUnavailableReason::InvalidObservedValue);
}
bool has(const Json& values, std::string_view value) {
    return std::any_of(values.begin(), values.end(), [value](const Json& x) { return x == value; });
}
size_t stage_index(std::string_view stage) {
    for (size_t i = 0; i < stages.size(); ++i) if (stage == stages[i]) return i;
    fail(ConsumptionUnavailableReason::InvalidObservedValue);
}
int rank(std::string_view reason) {
    if (reason == "nonfatal_error") return 4;
    if (reason == "incomplete_call") return 3;
    if (reason == "unsupported_consumer") return 2;
    if (reason == "instrumentation_missing") return 1;
    return 0;
}
const char* outcome(RunCallOutcome x) {
    switch (x) {
        case RunCallOutcome::ReturnedOk: return "returned_ok";
        case RunCallOutcome::ReturnedError: return "returned_error";
        case RunCallOutcome::Threw: return "threw";
        case RunCallOutcome::InProgress: return "incomplete";
        case RunCallOutcome::NotCalled: break;
    }
    fail(ConsumptionUnavailableReason::InvalidObservedValue);
}
const char* outcome(PortfolioCallOutcome x) {
    switch (x) {
        case PortfolioCallOutcome::ReturnedOk: return "returned_ok";
        case PortfolioCallOutcome::ReturnedError: return "returned_error";
        case PortfolioCallOutcome::Threw: return "threw";
        case PortfolioCallOutcome::InProgress: return "incomplete";
        case PortfolioCallOutcome::NotCalled: break;
    }
    fail(ConsumptionUnavailableReason::InvalidObservedValue);
}
template <typename T> bool entered(T x) { return x != T::NotCalled; }
template <typename T> const char* spell(T x, std::initializer_list<std::pair<T, const char*>> choices) {
    for (const auto& [value, text] : choices) if (x == value) return text;
    fail(ConsumptionUnavailableReason::InvalidObservedValue);
}

class Builder {
public:
    Json nodes = Json::array();
    std::array<std::string, 8> causes{};
    size_t reads_total = 0, pairs_total = 0;
    std::unordered_set<std::string> strategies, symbols;
    std::vector<size_t> depths;

    void cause(std::string_view stage, std::string_view reason) {
        auto& current = causes.at(stage_index(stage));
        if (rank(reason) > rank(current)) current = reason;
    }
    void strategy(std::string_view identity) {
        if (!valid_run_identity(identity, false)) fail(ConsumptionUnavailableReason::InvalidObservedIdentity);
        if (!strategies.contains(std::string(identity))) {
            if (strategies.size() >= 32) fail(ConsumptionUnavailableReason::CapacityExceeded);
            strategies.emplace(identity);
        }
    }
    void symbol(std::string_view identity) {
        if (!valid_run_identity(identity, true)) fail(ConsumptionUnavailableReason::InvalidObservedIdentity);
        if (!symbols.contains(std::string(identity))) {
            if (symbols.size() >= 1024) fail(ConsumptionUnavailableReason::CapacityExceeded);
            symbols.emplace(identity);
        }
    }
    template <typename T> void count(const T& collection) {
        if (collection.size() > 2048) fail(ConsumptionUnavailableReason::CapacityExceeded);
    }

    size_t node(std::string_view consumer, std::optional<size_t> parent = {},
                std::optional<const char*> call_outcome = {}, Json meta = Json::object(),
                std::optional<std::string_view> strategy_id = {},
                std::optional<std::string_view> symbol_id = {}, std::optional<size_t> index = {}) {
        const auto& def = consumer_def(consumer);
        const bool is_call = def.at("kind") == "call";
        if (is_call != call_outcome.has_value()) fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (nodes.size() >= 4096) fail(ConsumptionUnavailableReason::CapacityExceeded);
        std::string parent_name = parent ? nodes.at(*parent).at("consumer").get<std::string>() : "ROOT";
        if (!has(def.at("parents"), parent_name)) fail(ConsumptionUnavailableReason::InvalidObservedValue);
        size_t depth = parent ? depths.at(*parent) + 1 : 1;
        if (depth > 6) fail(ConsumptionUnavailableReason::CapacityExceeded);
        const auto& dims = def.at("dimensions");
        if (has(dims, "strategy") != strategy_id.has_value() ||
            has(dims, "symbol") != symbol_id.has_value() || has(dims, "index") != index.has_value())
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (strategy_id) strategy(*strategy_id);
        if (symbol_id) symbol(*symbol_id);
        if (index && *index > 4095) fail(ConsumptionUnavailableReason::CapacityExceeded);
        for (auto it = meta.begin(); it != meta.end(); ++it) {
            bool found = false;
            for (const auto& member : catalog().at("metadata")) {
                if (member.at("consumer") == consumer && member.at("key") == it.key()) {
                    found = true;
                    auto type = member.at("type").get<std::string>();
                    if (type == "bool" && !it.value().is_boolean()) fail(ConsumptionUnavailableReason::InvalidObservedValue);
                    if (type.rfind("enum:", 0) == 0 &&
                        (!it.value().is_string() || ("," + type.substr(5) + ",").find("," + it.value().get<std::string>() + ",") == std::string::npos))
                        fail(ConsumptionUnavailableReason::InvalidObservedValue);
                }
            }
            if (!found) fail(ConsumptionUnavailableReason::InvalidObservedValue);
        }
        for (const auto& member : catalog().at("metadata"))
            if (member.at("consumer") == consumer && member.at("required") == true &&
                !meta.contains(member.at("key").get<std::string>()))
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
        Json result = {{"id", nodes.size()}, {"parent", parent ? Json(*parent) : Json(nullptr)},
            {"kind", is_call ? "call" : "scope"}, {"consumer", consumer},
            {"reads", Json::array()}, {"meta", std::move(meta)}};
        if (call_outcome) result["outcome"] = *call_outcome;
        if (strategy_id) result["strategy"] = *strategy_id;
        if (symbol_id) result["symbol"] = *symbol_id;
        if (index) result["index"] = *index;
        if (is_call) {
            std::string stage = def.at("stage") == "inherited" ?
                consumer_def(parent_name).at("stage").get<std::string>() : def.at("stage").get<std::string>();
            if (std::string_view(*call_outcome) == "returned_error" || std::string_view(*call_outcome) == "threw")
                cause(stage, "nonfatal_error");
            else if (std::string_view(*call_outcome) == "incomplete") cause(stage, "incomplete_call");
        }
        depths.push_back(depth);
        nodes.push_back(std::move(result));
        return nodes.size() - 1;
    }

    void read(size_t id, std::string_view field, Json value, std::string_view origin,
              std::optional<std::string_view> symbol_id = {}) {
        std::string consumer = nodes.at(id).at("consumer");
        const auto& def = field_def(consumer, field);
        if (!has(def.at("origins"), origin)) fail(ConsumptionUnavailableReason::InvalidObservedValue);
        std::string type = def.at("type");
        if (type == "bool" && !value.is_boolean()) fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (type == "number" && (!value.is_number() || !std::isfinite(value.get<double>()) || value.dump().size() > 25))
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (type == "int32" && (!value.is_number_integer() || value.get<int64_t>() < INT32_MIN || value.get<int64_t>() > INT32_MAX))
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (type == "uint53" && (!value.is_number_unsigned() || value.get<uint64_t>() > 9007199254740991ULL))
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (type == "fixed_decimal8" && (!value.is_string() || value.get<std::string>().size() > 21))
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (type == "int32_pairs" || type == "int32_number_pairs") {
            if (!value.is_array()) fail(ConsumptionUnavailableReason::InvalidObservedValue);
            if (value.size() > 128 || pairs_total + value.size() > 8192) fail(ConsumptionUnavailableReason::CapacityExceeded);
            pairs_total += value.size();
        }
        if (type == "benchmark_mode" && value != "live" && value != "deferred")
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (symbol_id) {
            if (field.find(".symbol.") == std::string_view::npos &&
                field.find("strategy.position_limits.symbol.") != 0)
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
            symbol(*symbol_id);
        }
        auto& rows = nodes.at(id).at("reads");
        if (rows.size() >= 4096 || reads_total >= 16384) fail(ConsumptionUnavailableReason::CapacityExceeded);
        for (const auto& row : rows)
            if (row.at("field") == field && (!symbol_id || (row.contains("symbol") && row.at("symbol") == *symbol_id)))
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
        Json row = {{"field", field}, {"value_type", type}, {"value", std::move(value)}, {"origin", origin}};
        if (symbol_id) row["symbol"] = *symbol_id;
        rows.push_back(std::move(row));
        ++reads_total;
    }
};

template <typename T> void optional_read(Builder& b, size_t node, std::string_view field,
                                          const std::optional<T>& value, std::string_view origin) {
    if (value) b.read(node, field, *value, origin);
}
void uint_read(Builder& b, size_t node, std::string_view field,
               const std::optional<size_t>& value, std::string_view origin) {
    if (!value) return;
    if (*value > 9007199254740991ULL) fail(ConsumptionUnavailableReason::InvalidObservedValue);
    b.read(node, field, static_cast<uint64_t>(*value), origin);
}
void decimal_read(Builder& b, size_t node, std::string_view field,
                  const std::optional<Decimal>& value, std::string_view origin) {
    if (value) b.read(node, field, value->to_string(), origin);
}
template <typename Pair> void pair_read(Builder& b, size_t node, std::string_view field,
                                        const std::optional<std::vector<Pair>>& value, std::string_view origin) {
    if (!value) return;
    if (value->size() > 128 || b.pairs_total + value->size() > 8192) fail(ConsumptionUnavailableReason::CapacityExceeded);
    Json pairs = Json::array();
    for (const auto& [first, second] : *value) {
        if constexpr (std::is_floating_point_v<decltype(second)>)
            if (!std::isfinite(second)) fail(ConsumptionUnavailableReason::InvalidObservedValue);
        pairs.push_back(Json::array({first, second}));
    }
    b.read(node, field, std::move(pairs), origin);
}
template <typename Map, typename Callback> void sorted_entries(Builder& b, const Map& map, bool symbols,
                                                                Callback&& callback) {
    b.count(map);
    std::vector<const typename Map::value_type*> ordered;
    ordered.reserve(map.size());
    for (const auto& entry : map) {
        if (symbols) b.symbol(entry.first); else b.strategy(entry.first);
        ordered.push_back(&entry);
    }
    std::sort(ordered.begin(), ordered.end(), [](auto* a, auto* c) { return a->first < c->first; });
    for (const auto* entry : ordered) callback(*entry);
}

Json cost_meta(const transaction_cost::CostChargeObservation& x) {
    Json meta = Json::object();
    if (x.input_source) meta["input_source"] = spell(*x.input_source, {
        {transaction_cost::CostInputSource::internally_tracked, "internally_tracked"},
        {transaction_cost::CostInputSource::explicit_values, "explicit_values"}});
    if (x.asset_lookup.path) meta["asset_lookup"] = spell(*x.asset_lookup.path, {
        {transaction_cost::AssetLookupPath::exact_symbol, "exact_symbol"},
        {transaction_cost::AssetLookupPath::pre_dot_root, "pre_dot_root"},
        {transaction_cost::AssetLookupPath::fallback, "fallback"}});
    return meta;
}
void cost_reads(Builder& b, size_t id, const transaction_cost::CostChargeObservation& x) {
    constexpr auto R = "runtime_effective";
    optional_read(b, id, "cost.spread.baseline_spread_ticks", x.spread.baseline_spread_ticks, R);
    optional_read(b, id, "cost.spread.min_spread_ticks", x.spread.min_spread_ticks, R);
    optional_read(b, id, "cost.spread.max_spread_ticks", x.spread.max_spread_ticks, R);
    optional_read(b, id, "cost.spread.spread_cost_multiplier", x.spread.spread_cost_multiplier, R);
    optional_read(b, id, "cost.spread.tick_size", x.spread.tick_size, R);
    optional_read(b, id, "cost.volatility.lambda", x.volatility.lambda, R);
    optional_read(b, id, "cost.volatility.min_multiplier", x.volatility.min_multiplier, R);
    optional_read(b, id, "cost.volatility.max_multiplier", x.volatility.max_multiplier, R);
    optional_read(b, id, "cost.impact.min_adv", x.impact.min_adv, R);
    optional_read(b, id, "cost.impact.min_participation", x.impact.min_participation, R);
    optional_read(b, id, "cost.impact.max_participation", x.impact.max_participation, R);
    optional_read(b, id, "cost.impact.max_impact_bps", x.impact.max_impact_bps, R);
    optional_read(b, id, "cost.charge.explicit_fee_per_contract", x.explicit_fee_per_contract, R);
    optional_read(b, id, "cost.charge.point_value", x.point_value, R);
    optional_read(b, id, "cost.spread.tick_constrained", x.spread.tick_constrained, R);
    optional_read(b, id, "cost.charge.commission_per_unit", x.commission_per_unit, R);
    optional_read(b, id, "cost.charge.max_commission_pct", x.max_commission_pct, R);
    optional_read(b, id, "cost.charge.max_commission_per_order", x.max_commission_per_order, R);
    optional_read(b, id, "cost.charge.min_commission_per_order", x.min_commission_per_order, R);
    optional_read(b, id, "cost.charge.apply_regulatory_fees", x.apply_regulatory_fees, R);
    optional_read(b, id, "cost.charge.sec_fee_per_million", x.sec_fee_per_million, R);
    optional_read(b, id, "cost.charge.finra_taf_per_share", x.finra_taf_per_share, R);
    optional_read(b, id, "cost.charge.finra_taf_cap_per_trade", x.finra_taf_cap_per_trade, R);
    optional_read(b, id, "cost.charge.max_total_implicit_bps", x.max_total_implicit_bps, R);
}
bool cost_has_public_read(const transaction_cost::CostChargeObservation& x) {
    return x.spread.baseline_spread_ticks || x.spread.min_spread_ticks || x.spread.max_spread_ticks ||
        x.spread.spread_cost_multiplier || x.spread.tick_size || x.volatility.lambda ||
        x.volatility.min_multiplier || x.volatility.max_multiplier || x.impact.min_adv ||
        x.impact.min_participation || x.impact.max_participation || x.impact.max_impact_bps ||
        x.explicit_fee_per_contract || x.point_value || x.spread.tick_constrained ||
        x.commission_per_unit || x.max_commission_pct || x.max_commission_per_order ||
        x.min_commission_per_order || x.apply_regulatory_fees || x.sec_fee_per_million ||
        x.finra_taf_per_share || x.finra_taf_cap_per_trade || x.max_total_implicit_bps;
}
void risk_reads(Builder& b, size_t id, const RiskConfigConsumption& x, bool external = false) {
    constexpr auto R = "runtime_effective";
    const char* origin = external ? R : "app_config_effective";
    optional_read(b, id, "risk.var_limit", x.var_limit, origin);
    optional_read(b, id, "risk.jump_risk_limit", x.jump_risk_limit, origin);
    optional_read(b, id, "risk.max_correlation", x.max_correlation, origin);
    optional_read(b, id, "risk.max_gross_leverage", x.max_gross_leverage, origin);
    optional_read(b, id, "risk.max_net_leverage", x.max_net_leverage, origin);
    optional_read(b, id, "risk.confidence_level", x.confidence_level, origin);
    decimal_read(b, id, "risk.capital", x.capital, external ? R : "derived");
}
const char* profile_text(StrategyConsumptionProfile x) {
    return spell(x, {{StrategyConsumptionProfile::Unsupported, "unsupported"},
        {StrategyConsumptionProfile::Base, "base"}, {StrategyConsumptionProfile::Standard, "standard"},
        {StrategyConsumptionProfile::Fast, "fast"}, {StrategyConsumptionProfile::Slow, "slow"}});
}
void symbol_limit_reads(Builder& b, size_t id,
    const std::unordered_map<std::string, StrategySymbolLimitRead>& values,
    std::string_view prefix) {
    sorted_entries(b, values, true, [&](const auto& entry) {
        if (!entry.second.present && entry.second.value)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        const std::string present = std::string(prefix) + ".present";
        const std::string value = std::string(prefix) + ".value";
        b.read(id, present, entry.second.present, "runtime_effective", entry.first);
        if (entry.second.value) b.read(id, value, *entry.second.value, "runtime_effective", entry.first);
    });
}
void strategy_reads(Builder& b, size_t parent, const StrategyConsumptionTrace& x) {
    constexpr auto C = "constructor_effective";
    constexpr auto R = "runtime_effective";
    constexpr auto D = "derived";
    const bool standard_data = x.history.max_history_size || x.history.ema_windows ||
        x.volatility.vol_lookback_short || x.volatility.max_history_size ||
        x.forecast.ema_windows || x.forecast.vol_lookback_short || x.forecast.fdm ||
        x.regime.vol_lookback_long || x.sizing.capital_allocation || x.sizing.max_leverage ||
        x.sizing.idm || x.sizing.risk_target || x.sizing.fx_rate ||
        x.sizing.max_symbol_concentration || !x.sizing.symbol_limits.empty() ||
        x.buffering.use_position_buffering || x.buffering.weight || x.buffering.capital_allocation ||
        x.buffering.idm || x.buffering.risk_target || x.buffering.fx_rate ||
        x.buffering.carver_buffer_floor || x.buffering.carver_buffer_position_factor ||
        !x.buffering.symbol_limits.empty();
    const bool base_data = x.base_risk.supported || x.base_risk.capital_allocation ||
        x.base_risk.risk_max_leverage || x.base_risk.fallback_config_max_leverage ||
        x.base_risk.risk_max_drawdown || !x.base_risk.trading_multipliers.empty() ||
        x.position_limits.supported || !x.position_limits.symbols.empty();
    if (x.profile == StrategyConsumptionProfile::Unsupported) {
        if (standard_data || base_data) fail(ConsumptionUnavailableReason::InvalidObservedValue);
        b.cause(b.nodes.at(parent).at("consumer") == "strategy.preparation" ? "preparation" : "primary",
            "unsupported_consumer");
        return;
    }
    const bool standard = x.profile != StrategyConsumptionProfile::Base;
    if (!standard && standard_data) fail(ConsumptionUnavailableReason::InvalidObservedValue);
    if (standard && (x.history.max_history_size || x.history.ema_windows)) {
        auto id = b.node("strategy.history", parent);
        uint_read(b, id, "strategy.history.max_history_size", x.history.max_history_size, C);
        pair_read(b, id, "strategy.history.ema_windows", x.history.ema_windows, C);
    }
    if (standard && (x.volatility.vol_lookback_short || x.volatility.max_history_size)) {
        auto id = b.node("strategy.volatility", parent);
        optional_read(b, id, "strategy.volatility.vol_lookback_short", x.volatility.vol_lookback_short, C);
        uint_read(b, id, "strategy.volatility.max_history_size", x.volatility.max_history_size, C);
    }
    if (standard && (x.forecast.ema_windows || x.forecast.vol_lookback_short || x.forecast.fdm)) {
        auto id = b.node("strategy.forecast", parent);
        pair_read(b, id, "strategy.forecast.ema_windows", x.forecast.ema_windows, C);
        optional_read(b, id, "strategy.forecast.vol_lookback_short", x.forecast.vol_lookback_short, C);
        pair_read(b, id, "strategy.forecast.fdm", x.forecast.fdm, C);
    }
    if (standard && x.regime.vol_lookback_long) {
        auto id = b.node("strategy.regime", parent);
        optional_read(b, id, "strategy.regime.vol_lookback_long", x.regime.vol_lookback_long, C);
    }
    const auto& s = x.sizing;
    if (standard && (s.capital_allocation || s.max_leverage || s.idm || s.risk_target ||
                     s.fx_rate || s.max_symbol_concentration || !s.symbol_limits.empty())) {
        auto id = b.node("strategy.sizing", parent);
        optional_read(b, id, "strategy.sizing.capital_allocation", s.capital_allocation, D);
        optional_read(b, id, "strategy.sizing.max_leverage", s.max_leverage, C);
        optional_read(b, id, "strategy.sizing.idm", s.idm, C);
        optional_read(b, id, "strategy.sizing.risk_target", s.risk_target, C);
        optional_read(b, id, "strategy.sizing.fx_rate", s.fx_rate, C);
        optional_read(b, id, "strategy.sizing.max_symbol_concentration", s.max_symbol_concentration, C);
        symbol_limit_reads(b, id, s.symbol_limits, "strategy.sizing.symbol_limit.symbol");
    }
    const auto& f = x.buffering;
    if (standard && (f.use_position_buffering || f.weight || f.capital_allocation || f.idm ||
        f.risk_target || f.fx_rate || f.carver_buffer_floor || f.carver_buffer_position_factor ||
        !f.symbol_limits.empty())) {
        auto id = b.node("strategy.buffering", parent);
        optional_read(b, id, "strategy.buffering.use_position_buffering", f.use_position_buffering, C);
        optional_read(b, id, "strategy.buffering.weight", f.weight, C);
        optional_read(b, id, "strategy.buffering.capital_allocation", f.capital_allocation, D);
        optional_read(b, id, "strategy.buffering.idm", f.idm, C);
        optional_read(b, id, "strategy.buffering.risk_target", f.risk_target, C);
        optional_read(b, id, "strategy.buffering.fx_rate", f.fx_rate, C);
        optional_read(b, id, "strategy.buffering.carver_buffer_floor", f.carver_buffer_floor, C);
        optional_read(b, id, "strategy.buffering.carver_buffer_position_factor", f.carver_buffer_position_factor, C);
        symbol_limit_reads(b, id, f.symbol_limits, "strategy.buffering.symbol_limit.symbol");
    }
    const auto& r = x.base_risk;
    if (!r.supported && (r.capital_allocation || r.risk_max_leverage || r.fallback_config_max_leverage ||
        r.risk_max_drawdown || !r.trading_multipliers.empty()))
        fail(ConsumptionUnavailableReason::InvalidObservedValue);
    if (r.supported || r.capital_allocation || r.risk_max_leverage || r.fallback_config_max_leverage ||
        r.risk_max_drawdown || !r.trading_multipliers.empty()) {
        auto id = b.node("strategy.base_risk", parent, {}, {{"supported", r.supported}});
        optional_read(b, id, "strategy.base_risk.capital_allocation", r.capital_allocation, R);
        decimal_read(b, id, "strategy.base_risk.risk_max_leverage", r.risk_max_leverage, R);
        optional_read(b, id, "strategy.base_risk.fallback_config_max_leverage", r.fallback_config_max_leverage, R);
        decimal_read(b, id, "strategy.base_risk.risk_max_drawdown", r.risk_max_drawdown, R);
        sorted_entries(b, r.trading_multipliers, true, [&](const auto& entry) {
            if (!entry.second.present && entry.second.value) fail(ConsumptionUnavailableReason::InvalidObservedValue);
            b.read(id, "strategy.base_risk.trading_multiplier.symbol.present", entry.second.present, R, entry.first);
            if (entry.second.value)
                b.read(id, "strategy.base_risk.trading_multiplier.symbol.value", *entry.second.value, R, entry.first);
        });
    }
    if (!x.position_limits.supported && !x.position_limits.symbols.empty())
        fail(ConsumptionUnavailableReason::InvalidObservedValue);
    if (x.position_limits.supported || !x.position_limits.symbols.empty()) {
        auto id = b.node("strategy.position_limits", parent, {}, {{"supported", x.position_limits.supported}});
        symbol_limit_reads(b, id, x.position_limits.symbols, "strategy.position_limits.symbol");
    }
}

const char* setup_stage(SetupStage state) {
    return spell(state, {{SetupStage::NotReached, "not_reached"},
        {SetupStage::Attempted, "attempted"}, {SetupStage::Succeeded, "succeeded"},
        {SetupStage::Failed, "failed"}});
}
void selection_entry(Builder& b, size_t parent, const SelectionRead& x, bool controlled) {
    Json meta = {{"enabled_live_read", x.enabled_live_read}, {"allocation_read", x.allocation_read}};
    if (x.enabled_live_present) meta["enabled_live_present"] = *x.enabled_live_present;
    if (x.enabled_live_defaulted) meta["enabled_live_defaulted"] = *x.enabled_live_defaulted;
    if (x.allocation_defaulted) meta["allocation_defaulted"] = *x.allocation_defaulted;
    if ((!x.enabled_live_read && (x.enabled_live_value || x.enabled_live_defaulted)) ||
        (!x.allocation_read && (x.allocation_value || x.allocation_defaulted)) ||
        (x.enabled_live_value && !x.enabled_live_present.has_value()) ||
        (x.enabled_live_defaulted && !x.enabled_live_value) ||
        (x.allocation_defaulted && !x.allocation_value) ||
        (controlled && x.enabled_live_value && !*x.enabled_live_present &&
            x.enabled_live_defaulted != true) ||
        (controlled && x.enabled_live_defaulted && x.enabled_live_present &&
            *x.enabled_live_defaulted == *x.enabled_live_present) ||
        (controlled && x.allocation_defaulted == true) ||
        (!controlled && x.enabled_live_defaulted.has_value()) ||
        (!controlled && x.enabled_live_present == false &&
            (x.enabled_live_read || x.enabled_live_value.has_value())))
        fail(ConsumptionUnavailableReason::InvalidObservedValue);
    auto id = b.node("setup.selection_entry", parent, {}, std::move(meta), x.name);
    if (x.enabled_live_value) b.read(id, "setup.selection.enabled_live", *x.enabled_live_value,
        x.enabled_live_defaulted.value_or(false) ? "code_default" : "configured_strategy_leaf");
    if (x.allocation_value) b.read(id, "setup.selection.default_allocation", *x.allocation_value,
        x.allocation_defaulted.value_or(false) ? "code_default" : "configured_strategy_leaf");
    optional_read(b, id, "setup.selection.effective_allocation", x.effective_allocation, "derived");
}
void selector(Builder& b, const RunConsumption& run) {
    if (!run.selection || !entered(run.selection->outcome)) return;
    const auto& x = run.selection->payload;
    b.count(x.controlled_validation);
    b.count(x.ordinary_selection);
    auto id = b.node("setup.selector", {}, outcome(run.selection->outcome),
        {{"mode", run.controlled_selection ? "controlled" : "ordinary"}});
    if (!run.controlled_selection && (!x.controlled_validation.empty() || x.controlled_sum))
        fail(ConsumptionUnavailableReason::InvalidObservedValue);
    if (run.controlled_selection && (!x.controlled_validation.empty() || x.controlled_sum)) {
        auto phase = b.node("setup.controlled_validation", id);
        optional_read(b, phase, "setup.selection.sum", x.controlled_sum, "derived");
        std::set<std::string> names;
        for (const auto& entry : x.controlled_validation) {
            b.strategy(entry.name);
            if (!names.insert(entry.name).second) fail(ConsumptionUnavailableReason::InvalidObservedValue);
            selection_entry(b, phase, entry, true);
        }
    }
    if (!x.ordinary_selection.empty() || x.ordinary_sum || x.ordinary_normalized) {
        Json meta = Json::object();
        if (x.ordinary_normalized) meta["normalized"] = *x.ordinary_normalized;
        auto phase = b.node("setup.ordinary_selection", id, {}, std::move(meta));
        optional_read(b, phase, "setup.selection.sum", x.ordinary_sum, "derived");
        std::set<std::string> names;
        for (const auto& entry : x.ordinary_selection) {
            b.strategy(entry.name);
            if (!names.insert(entry.name).second) fail(ConsumptionUnavailableReason::InvalidObservedValue);
            selection_entry(b, phase, entry, false);
        }
    }
}
void factory(Builder& b, const RunConsumption& run) {
    if (!run.primary_factory || !entered(run.primary_factory->outcome)) return;
    b.count(run.primary_factory->payload.entries);
    auto id = b.node("setup.factory", {}, outcome(run.primary_factory->outcome));
    std::set<std::string> names;
    for (const auto& x : run.primary_factory->payload.entries) {
        b.strategy(x.name);
        if (!names.insert(x.name).second) fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (x.initialize != SetupStage::NotReached && x.construction != SetupStage::Succeeded)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (x.start != SetupStage::NotReached && x.initialize != SetupStage::Succeeded)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        Json meta = {{"construction", setup_stage(x.construction)},
            {"initialize", setup_stage(x.initialize)}, {"start", setup_stage(x.start)}};
        if (x.type_defaulted) meta["type_defaulted"] = *x.type_defaulted;
        if (x.profile) meta["profile"] = spell(*x.profile, {
            {FactoryProfile::Standard, "standard"}, {FactoryProfile::Fast, "fast"},
            {FactoryProfile::Slow, "slow"}, {FactoryProfile::Unsupported, "unsupported"}});
        auto child = b.node("setup.factory_entry", id, {}, std::move(meta), x.name);
        optional_read(b, child, "setup.factory.effective_allocation", x.effective_allocation, "selected_allocation");
        optional_read(b, child, "setup.factory.initial_capital", x.initial_capital_argument, "runtime_effective");
        optional_read(b, child, "setup.factory.allocated_capital", x.allocated_capital, "derived");
        if (x.construction == SetupStage::Failed || x.initialize == SetupStage::Failed || x.start == SetupStage::Failed)
            b.cause("setup", "nonfatal_error");
        else if (x.construction == SetupStage::Attempted || x.initialize == SetupStage::Attempted ||
                 x.start == SetupStage::Attempted) b.cause("setup", "incomplete_call");
        if (x.profile == FactoryProfile::Unsupported) b.cause("setup", "unsupported_consumer");
    }
}
void registration(Builder& b, const RunNamedInvocation<PortfolioRegistrationTrace>& x) {
    if (!entered(x.call.outcome)) return;
    const auto& r = x.call.payload;
    if (entered(r.outcome) && outcome(r.outcome) != std::string_view(outcome(x.call.outcome)))
        fail(ConsumptionUnavailableReason::InvalidObservedValue);
    Json meta = Json::object();
    if (r.total_within_limit) meta["total_within_limit"] = *r.total_within_limit;
    auto id = b.node("portfolio.registration", {}, outcome(x.call.outcome), std::move(meta), x.identity);
    optional_read(b, id, "portfolio.registration.initial_allocation", r.initial_allocation, "selected_allocation");
    optional_read(b, id, "portfolio.registration.min_allocation", r.min_allocation, "runtime_effective");
    optional_read(b, id, "portfolio.registration.max_allocation", r.max_allocation, "runtime_effective");
    optional_read(b, id, "portfolio.registration.total_allocation", r.total_allocation, "derived");
    optional_read(b, id, "portfolio.registration.requested_optimization", r.requested_optimization, "runtime_effective");
    optional_read(b, id, "portfolio.registration.portfolio_optimization", r.portfolio_optimization, "runtime_effective");
    optional_read(b, id, "portfolio.registration.requested_risk", r.requested_risk, "runtime_effective");
    optional_read(b, id, "portfolio.registration.portfolio_risk", r.portfolio_risk, "runtime_effective");
    optional_read(b, id, "portfolio.registration.stored_allocation", r.stored_allocation, "selected_allocation");
    optional_read(b, id, "portfolio.registration.stored_optimization", r.stored_optimization, "derived");
    optional_read(b, id, "portfolio.registration.stored_risk", r.stored_risk, "derived");
    if ((r.stored_allocation && r.initial_allocation && *r.stored_allocation != *r.initial_allocation) ||
        (r.total_within_limit && r.total_allocation && *r.total_within_limit != !(*r.total_allocation > 1.0)))
        fail(ConsumptionUnavailableReason::InvalidObservedValue);
    if ((r.requested_optimization == false && r.portfolio_optimization) ||
        (r.requested_risk == false && r.portfolio_risk))
        fail(ConsumptionUnavailableReason::InvalidObservedValue);
    if (r.stored_optimization && (!r.requested_optimization ||
        (*r.requested_optimization && !r.portfolio_optimization) ||
        *r.stored_optimization != (*r.requested_optimization && *r.portfolio_optimization)))
        fail(ConsumptionUnavailableReason::InvalidObservedValue);
    if (r.stored_risk && (!r.requested_risk ||
        (*r.requested_risk && !r.portfolio_risk) ||
        *r.stored_risk != (*r.requested_risk && *r.portfolio_risk)))
        fail(ConsumptionUnavailableReason::InvalidObservedValue);
}

const char* optimization_skip(PortfolioHelperSkip x) {
    return spell(x, {{PortfolioHelperSkip::None, "none"},
        {PortfolioHelperSkip::NoEligibleSymbols, "no_eligible_symbols"},
        {PortfolioHelperSkip::InsufficientHistory, "insufficient_history"},
        {PortfolioHelperSkip::AbsentOptimizer, "absent_optimizer"}});
}
const char* risk_skip(PortfolioHelperSkip x) {
    return spell(x, {{PortfolioHelperSkip::None, "none"},
        {PortfolioHelperSkip::AbsentRiskManager, "absent_risk_manager"},
        {PortfolioHelperSkip::NoPositions, "no_positions"}});
}
void optimization(Builder& b, size_t parent, const PortfolioPassConsumption& pass) {
    if (!entered(pass.optimization_helper)) return;
    const auto& x = pass.optimization;
    auto id = b.node("portfolio.optimization", parent, outcome(pass.optimization_helper),
        {{"skip", optimization_skip(x.skip)}});
    decimal_read(b, id, "portfolio.optimization.total_capital", x.total_capital, "derived");
    constexpr std::array<const char*, 3> scopes = {"portfolio.symbol_collection",
        "portfolio.numeric_aggregation", "portfolio.redistribution"};
    for (size_t i = 0; i < x.strategies.size(); ++i) {
        const auto& map = x.strategies[i];
        if (map.empty()) continue;
        b.count(map);
        auto scope = b.node(scopes[i], id);
        sorted_entries(b, map, false, [&](const auto& entry) {
            if (!entry.second.enabled && entry.second.allocation)
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
            auto child = b.node("portfolio.optimization_strategy", scope, {}, Json::object(), entry.first);
            b.read(child, "portfolio.optimization.strategy.enabled", entry.second.enabled, "derived");
            optional_read(b, child, "portfolio.optimization.strategy.allocation", entry.second.allocation,
                "selected_allocation");
        });
    }
    b.count(x.estimates);
    for (size_t i = 0; i < x.estimates.size(); ++i) {
        const auto& estimate = x.estimates[i];
        if (estimate.symbol_index != i) fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (!entered(estimate.charge_call) &&
            (cost_has_public_read(estimate.charge) || !cost_meta(estimate.charge).empty()))
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        auto scope = b.node("portfolio.estimate", id, {}, Json::object(), {}, estimate.symbol,
            estimate.symbol_index);
        if (entered(estimate.charge_call)) {
            auto charge = b.node("cost.estimate", scope, outcome(estimate.charge_call), cost_meta(estimate.charge));
            cost_reads(b, charge, estimate.charge);
        }
    }
    if (entered(x.optimizer_call)) {
        if (x.skip != PortfolioHelperSkip::None) fail(ConsumptionUnavailableReason::InvalidObservedValue);
        const auto& opt = x.optimizer;
        Json meta = {{"buffer_branch", spell(opt.buffer_branch, {
            {OptimizationBufferBranch::NotReached, "not_reached"},
            {OptimizationBufferBranch::Disabled, "disabled"},
            {OptimizationBufferBranch::ReturnedPrior, "returned_prior"},
            {OptimizationBufferBranch::Applied, "applied"},
            {OptimizationBufferBranch::Failed, "failed"}})}};
        auto child = b.node("optimizer.primary", id, outcome(x.optimizer_call), std::move(meta));
        const auto& c = opt.consumed_config;
        constexpr auto R = "runtime_effective";
        optional_read(b, child, "optimizer.cost_penalty_scalar", c.cost_penalty_scalar, R);
        optional_read(b, child, "optimizer.max_iterations", c.max_iterations, R);
        optional_read(b, child, "optimizer.convergence_threshold", c.convergence_threshold, R);
        optional_read(b, child, "optimizer.use_buffering", c.use_buffering, R);
        optional_read(b, child, "optimizer.tau", c.tau, R);
        optional_read(b, child, "optimizer.buffer_size_factor", c.buffer_size_factor, R);
        if (opt.buffer_branch == OptimizationBufferBranch::Failed) b.cause("primary", "nonfatal_error");
    } else if (x.skip == PortfolioHelperSkip::None &&
               pass.optimization_helper == PortfolioCallOutcome::ReturnedOk) {
        b.cause("primary", "instrumentation_missing");
    }
}
void risk_helper(Builder& b, size_t parent, const PortfolioPassConsumption& pass) {
    if (!entered(pass.risk_helper)) return;
    const auto& x = pass.risk;
    Json meta = {{"skip", risk_skip(x.skip)}};
    if (x.source) meta["manager_source"] = spell(*x.source, {
        {PortfolioRiskManagerSource::Absent, "absent"},
        {PortfolioRiskManagerSource::Internal, "internal"},
        {PortfolioRiskManagerSource::External, "external"}});
    auto id = b.node("portfolio.risk", parent, outcome(pass.risk_helper), std::move(meta));
    optional_read(b, id, "portfolio.risk.lookback_period", x.lookback_period, "runtime_effective");
    if (entered(x.risk_call)) {
        if (x.skip != PortfolioHelperSkip::None || !x.source || *x.source == PortfolioRiskManagerSource::Absent)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        auto child = b.node("risk.primary", id, outcome(x.risk_call));
        risk_reads(b, child, x.risk, *x.source == PortfolioRiskManagerSource::External);
    } else if (x.skip == PortfolioHelperSkip::None &&
               pass.risk_helper == PortfolioCallOutcome::ReturnedOk) {
        b.cause("primary", "instrumentation_missing");
    }
}
void primary(Builder& b, const RunConsumption& run) {
    if (!run.primary || !entered(run.primary->outcome)) return;
    const auto& x = run.primary->payload;
    if (x.pass_count > 5) fail(ConsumptionUnavailableReason::CapacityExceeded);
    b.count(x.strategies); b.count(x.strategy_charges); b.count(x.compatibility_charges);
    if (x.skip_execution_generation.value_or(false) &&
        (!x.strategy_charges.empty() || !x.compatibility_charges.empty()))
        fail(ConsumptionUnavailableReason::InvalidObservedValue);
    Json meta = Json::object();
    if (x.skip_execution_generation) meta["skip_execution_generation"] = *x.skip_execution_generation;
    auto id = b.node("portfolio.primary", {}, outcome(run.primary->outcome), std::move(meta));
    if (entered(x.outcome)) {
        if (x.outcome == PortfolioCallOutcome::ReturnedError || x.outcome == PortfolioCallOutcome::Threw)
            b.cause("primary", "nonfatal_error");
        if (x.outcome == PortfolioCallOutcome::InProgress) b.cause("primary", "incomplete_call");
    }
    for (const auto& strategy : x.strategies) {
        if (!entered(strategy.outcome)) continue;
        auto child = b.node("strategy.primary", id, outcome(strategy.outcome),
            {{"profile", profile_text(strategy.strategy.profile)}}, strategy.strategy_id);
        strategy_reads(b, child, strategy.strategy);
    }
    for (size_t i = 0; i < x.pass_count; ++i) {
        const auto& pass = x.passes[i];
        auto scope = b.node("portfolio.pass", id, {}, Json::object(), {}, {}, i);
        optional_read(b, scope, "portfolio.pass.use_optimization", pass.use_optimization, "runtime_effective");
        optional_read(b, scope, "portfolio.pass.use_risk_management", pass.use_risk_management, "runtime_effective");
        if (pass.use_optimization == false && entered(pass.optimization_helper))
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (pass.use_risk_management == false && entered(pass.risk_helper))
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (pass.use_optimization == true && !entered(pass.optimization_helper) &&
            run.primary->outcome == RunCallOutcome::ReturnedOk)
            b.cause("primary", "instrumentation_missing");
        if (pass.use_risk_management == true && !entered(pass.risk_helper) &&
            run.primary->outcome == RunCallOutcome::ReturnedOk)
            b.cause("primary", "instrumentation_missing");
        optimization(b, scope, pass);
        risk_helper(b, scope, pass);
    }
    for (const auto& charge : x.strategy_charges) {
        if (charge.purpose != PortfolioChargePurpose::PerStrategy)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (!entered(charge.charge_call)) {
            if (cost_has_public_read(charge.charge) || !cost_meta(charge.charge).empty())
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
            continue;
        }
        auto child = b.node("cost.strategy_execution", id, outcome(charge.charge_call),
            cost_meta(charge.charge), charge.strategy_id, charge.symbol);
        cost_reads(b, child, charge.charge);
    }
    for (const auto& charge : x.compatibility_charges) {
        if (charge.purpose != PortfolioChargePurpose::Compatibility || !charge.strategy_id.empty())
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (!entered(charge.charge_call)) {
            if (cost_has_public_read(charge.charge) || !cost_meta(charge.charge).empty())
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
            continue;
        }
        auto child = b.node("cost.compatibility_execution", id, outcome(charge.charge_call),
            cost_meta(charge.charge), {}, charge.symbol);
        cost_reads(b, child, charge.charge);
    }
}

void history(Builder& b, const RunConsumption& run) {
    b.count(run.history_updates);
    for (const auto& entry : run.history_updates) {
        if (!entered(entry.call.outcome)) continue;
        const auto& x = entry.call.payload;
        Json meta = {{"cost_model_reached", x.cost_model_reached}};
        if (x.previous_close_source) meta["previous_close_source"] = spell(*x.previous_close_source, {
            {PreviousCloseSource::initial_current_close, "initial_current_close"},
            {PreviousCloseSource::stored_previous_close, "stored_previous_close"}});
        if (!x.cost_model_reached && (x.previous_close_source ||
            x.market_data.volume.adv_lookback_days || x.market_data.log_returns.lookback_days))
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        auto id = b.node("execution.history_update", {}, outcome(entry.call.outcome),
            std::move(meta), {}, entry.identity);
        if (x.cost_model_reached && (x.market_data.volume.adv_lookback_days ||
                                     x.market_data.log_returns.lookback_days)) {
            auto scope = b.node("cost.history", id);
            uint_read(b, scope, "cost.impact_history.adv_lookback_days",
                x.market_data.volume.adv_lookback_days, "runtime_effective");
            uint_read(b, scope, "cost.spread_history.lookback_days",
                x.market_data.log_returns.lookback_days, "runtime_effective");
        }
    }
}
void execution(Builder& b, const RunConsumption& run) {
    b.count(run.execution_batches);
    for (const auto& entry : run.execution_batches) {
        if (!entered(entry.call.outcome)) continue;
        const auto& x = entry.call.payload;
        b.count(x.attempts);
        const char* state = spell(x.state, {
            {DailyExecutionState::entered, "entered"},
            {DailyExecutionState::rejected_stream, "rejected_stream"},
            {DailyExecutionState::returned, "returned"},
            {DailyExecutionState::invalid_argument, "invalid_argument"}});
        const char* batch_outcome = x.state == DailyExecutionState::entered
            ? (entry.call.outcome == RunCallOutcome::Threw ? "threw" : "incomplete")
            : outcome(entry.call.outcome);
        if (x.state == DailyExecutionState::returned && entry.call.outcome != RunCallOutcome::ReturnedOk)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if ((x.state == DailyExecutionState::rejected_stream || x.state == DailyExecutionState::invalid_argument) &&
            entry.call.outcome == RunCallOutcome::ReturnedOk)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        auto id = b.node("execution.batch", {}, batch_outcome, {{"state", state}}, entry.identity);
        for (size_t i = 0; i < x.attempts.size(); ++i) {
            const auto& attempt = x.attempts[i];
            if (attempt.sequence != i) fail(ConsumptionUnavailableReason::InvalidObservedValue);
            if (attempt.returned && attempt.execution.state != ExecutionCallState::returned)
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
            if (i + 1 < x.attempts.size() && !attempt.returned)
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
            const char* branch = spell(attempt.branch, {
                {DailyPositionBranch::current_position, "current_position"},
                {DailyPositionBranch::removed_position, "removed_position"}});
            const char* price_source = spell(attempt.price_source, {
                {ExecutionPriceSource::market_prices, "market_prices"},
                {ExecutionPriceSource::current_average_price, "current_average_price"},
                {ExecutionPriceSource::previous_average_price, "previous_average_price"}});
            if ((attempt.branch == DailyPositionBranch::current_position &&
                 attempt.price_source == ExecutionPriceSource::previous_average_price) ||
                (attempt.branch == DailyPositionBranch::removed_position &&
                 attempt.price_source == ExecutionPriceSource::current_average_price))
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
            const char* attempt_state = spell(attempt.execution.state, {
                {ExecutionCallState::entered, "entered"},
                {ExecutionCallState::rejected_stream, "rejected_stream"},
                {ExecutionCallState::rejected_id, "rejected_id"},
                {ExecutionCallState::cost_call_reached, "cost_call_reached"},
                {ExecutionCallState::returned, "returned"}});
            const bool rejected = attempt.execution.state == ExecutionCallState::rejected_stream ||
                                  attempt.execution.state == ExecutionCallState::rejected_id;
            const char* call_outcome = attempt.returned ? "returned_ok" :
                rejected && (entry.call.outcome == RunCallOutcome::Threw ||
                             entry.call.outcome == RunCallOutcome::ReturnedError) ? "threw" : "incomplete";
            auto child = b.node("execution.attempt", id, call_outcome,
                {{"state", attempt_state}, {"branch", branch}, {"price_source", price_source},
                 {"returned", attempt.returned}}, {}, attempt.symbol, attempt.sequence);
            const auto& cost = attempt.execution.cost;
            if ((attempt.execution.state == ExecutionCallState::cost_call_reached ||
                 attempt.execution.state == ExecutionCallState::returned) &&
                (cost_has_public_read(cost) || !cost_meta(cost).empty())) {
                auto scope = b.node("cost.execution", child, {}, cost_meta(cost));
                cost_reads(b, scope, cost);
            } else if (cost_has_public_read(cost) || !cost_meta(cost).empty()) {
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
            }
        }
    }
}
void diagnostics(Builder& b, const RunConsumption& run) {
    b.count(run.pnl_finalizations);
    for (const auto& x : run.pnl_finalizations) {
        if (!entered(x.call.outcome)) continue;
        auto id = b.node("runner.pnl_finalization", {}, outcome(x.call.outcome),
            {{"allocation_lookup", x.allocation_found ? "hit" : "fallback"}}, x.strategy);
        b.read(id, "runner.pnl_finalization.strategy_allocation", x.allocation,
            x.allocation_found ? "selected_allocation" : "code_default");
        b.read(id, "runner.pnl_finalization.initial_capital", x.initial_capital, "runtime_effective");
        b.read(id, "runner.pnl_finalization.strategy_capital", x.strategy_capital, "derived");
    }
    if (run.snapshot_risk && entered(run.snapshot_risk->outcome)) {
        auto id = b.node("risk.diagnostics", {}, outcome(run.snapshot_risk->outcome));
        risk_reads(b, id, run.snapshot_risk->payload);
    }
}

Json finalize(Builder& b, const RunConsumption& run) {
    std::set<std::string> successful_registrations;
    std::set<std::string> entered_registrations;
    for (const auto& registration : run.registrations) {
        if (!entered(registration.call.outcome)) continue;
        b.strategy(registration.identity);
        if (!entered_registrations.insert(registration.identity).second)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (registration.call.outcome == RunCallOutcome::ReturnedOk)
            successful_registrations.insert(registration.identity);
    }
    for (const auto& node : b.nodes) {
        const auto& consumer = node.at("consumer");
        if ((consumer == "strategy.preparation" || consumer == "strategy.primary" ||
             consumer == "portfolio.optimization_strategy" || consumer == "execution.batch" ||
             consumer == "cost.strategy_execution") &&
            !successful_registrations.contains(node.at("strategy").get<std::string>()))
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
    }
    if (run.primary_factory && run.primary_factory->outcome == RunCallOutcome::ReturnedOk) {
        std::set<std::string> factory_names;
        for (const auto& entry : run.primary_factory->payload.entries) {
            b.strategy(entry.name);
            if (entry.start == SetupStage::Succeeded && !factory_names.insert(entry.name).second)
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
        }
        for (const auto& name : entered_registrations)
            if (!factory_names.contains(name))
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (factory_names != successful_registrations)
            b.cause("setup", "instrumentation_missing");
        if (run.preparation && entered(run.preparation->call.outcome) &&
            !run.primary_factory->payload.entries.empty() &&
            run.preparation->identity != run.primary_factory->payload.entries.front().name)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
    }
    if (run.primary && entered(run.primary->outcome)) {
        std::set<std::string> primary_names;
        for (const auto& invocation : run.primary->payload.strategies) {
            if (!entered(invocation.outcome)) continue;
            b.strategy(invocation.strategy_id);
            if (!primary_names.insert(invocation.strategy_id).second)
                fail(ConsumptionUnavailableReason::InvalidObservedValue);
        }
        if (run.primary->outcome == RunCallOutcome::ReturnedOk && run.primary_stage.completed &&
            primary_names != successful_registrations)
            b.cause("primary", "instrumentation_missing");
    }
    std::set<std::string> batch_names;
    for (const auto& batch : run.execution_batches) {
        if (!entered(batch.call.outcome)) continue;
        b.strategy(batch.identity);
        if (!batch_names.insert(batch.identity).second)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
    }
    if (run.execution_loop.completed && batch_names != successful_registrations)
        b.cause("execution", "instrumentation_missing");
    std::set<std::string> pnl_names;
    for (const auto& entry : run.pnl_finalizations) {
        if (!entered(entry.call.outcome)) continue;
        b.strategy(entry.strategy);
        if (!pnl_names.insert(entry.strategy).second)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
    }
    const auto missing = [&](std::string_view stage, bool present) {
        if (!present) b.cause(stage, "instrumentation_missing");
    };
    missing("setup", run.selection && entered(run.selection->outcome));
    missing("setup", run.primary_factory && entered(run.primary_factory->outcome));
    missing("market_input", run.historical_days_reached &&
        run.market_fetch.outcome == RunCallOutcome::ReturnedOk &&
        run.arrow_conversion.outcome == RunCallOutcome::ReturnedOk &&
        run.market_input_completed);
    switch (run.arrow_conversion.outcome) {
        case RunCallOutcome::NotCalled:
        case RunCallOutcome::ReturnedOk: break;
        case RunCallOutcome::ReturnedError:
        case RunCallOutcome::Threw: b.cause("market_input", "nonfatal_error"); break;
        case RunCallOutcome::InProgress: b.cause("market_input", "incomplete_call"); break;
        default: fail(ConsumptionUnavailableReason::InvalidObservedValue);
    }
    missing("cost_history", run.history_loop.reached);
    if (run.history_loop.reached && !run.history_loop.completed) b.cause("cost_history", "incomplete_call");
    if (run.non_trading_decision_reached && run.skip_strategy_processing) {
        if (!run.preparation_skipped || !run.primary_skipped ||
            (run.preparation && entered(run.preparation->call.outcome)) ||
            (run.primary && entered(run.primary->outcome)))
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
    } else {
        if (run.preparation_skipped || run.primary_skipped) fail(ConsumptionUnavailableReason::InvalidObservedValue);
        missing("preparation", run.preparation_stage.reached && run.preparation &&
            entered(run.preparation->call.outcome));
        missing("primary", run.primary_stage.reached && run.primary && entered(run.primary->outcome));
        if (run.preparation_stage.reached && !run.preparation_stage.completed)
            b.cause("preparation", "incomplete_call");
        if (run.primary_stage.reached && !run.primary_stage.completed)
            b.cause("primary", "incomplete_call");
    }
    missing("execution", run.execution_loop.reached);
    if (run.execution_loop.reached && !run.execution_loop.completed) b.cause("execution", "incomplete_call");
    missing("diagnostics", run.diagnostics_reached && run.snapshot_risk &&
        entered(run.snapshot_risk->outcome) && run.pnl_path_decision_reached);
    if (run.diagnostics_reached && !run.diagnostics_completed)
        b.cause("diagnostics", "incomplete_call");
    if (run.pnl_path_eligible && !run.pnl_loop.reached) b.cause("diagnostics", "instrumentation_missing");
    if (run.pnl_loop.reached && !run.pnl_loop.completed) b.cause("diagnostics", "incomplete_call");
    missing("control_flow", run.non_trading_decision_reached && run.benchmark_decision_reached);
    if (run.benchmark_decision_reached) {
        if (run.benchmark_mode != RunBenchmarkMode::Deferred &&
            run.benchmark_mode != RunBenchmarkMode::Live &&
            run.benchmark_mode != RunBenchmarkMode::Unsupported)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (run.benchmark_mode == RunBenchmarkMode::Unsupported) b.cause("control_flow", "unsupported_consumer");
        if (run.benchmark_mode == RunBenchmarkMode::Live &&
            run.benchmark_state == RunBenchmarkState::NotReached)
            b.cause("control_flow", "instrumentation_missing");
        if (run.benchmark_state == RunBenchmarkState::NonfatalError) b.cause("control_flow", "nonfatal_error");
        if (run.benchmark_state == RunBenchmarkState::InProgress) b.cause("control_flow", "incomplete_call");
    }
    Json coverage = Json::object();
    std::string global_reason;
    for (size_t i = 0; i < stages.size(); ++i) {
        std::string reason = b.causes[i];
        if (run.non_trading_decision_reached && run.skip_strategy_processing &&
            (i == stage_index("preparation") || i == stage_index("primary"))) {
            coverage[stages[i]] = {{"status", "skipped"}, {"reason", "non_trading_day"}};
            continue;
        }
        coverage[stages[i]] = {{"status", reason.empty() ? "complete" : "partial"},
                               {"reason", reason.empty() ? "none" : reason}};
        if (rank(reason) > rank(global_reason)) global_reason = reason;
    }
    bool any_call = false;
    for (const auto& node : b.nodes) if (node.at("kind") == "call") { any_call = true; break; }
    if (!any_call) {
        auto reason = global_reason == "unsupported_consumer"
            ? ConsumptionUnavailableReason::UnsupportedConsumer
            : ConsumptionUnavailableReason::InstrumentationMissing;
        Json all = Json::object();
        for (const auto* stage : stages) all[stage] = {{"status", "unavailable"}, {"reason", unavailable_text(reason)}};
        return {{"version", 2}, {"status", "unavailable"}, {"reason", unavailable_text(reason)},
                {"coverage", std::move(all)}, {"nodes", Json::array()}};
    }
    Json result = {{"version", 2}, {"status", global_reason.empty() ? "complete" : "partial"},
        {"reason", global_reason.empty() ? "none" : global_reason}, {"coverage", std::move(coverage)},
        {"nodes", std::move(b.nodes)}};
    if (result.dump().size() > 2097152) fail(ConsumptionUnavailableReason::CapacityExceeded);
    return result;
}

// Byte-for-byte catalog content is embedded to keep production independent of
// the neighboring checkout and of runtime filesystem access.
const Json& catalog() {
    static const Json descriptor = Json::parse(R"CATALOG(
{
  "version": 2,
  "stages": [
    "setup",
    "market_input",
    "cost_history",
    "preparation",
    "primary",
    "execution",
    "diagnostics",
    "control_flow"
  ],
  "statuses": {
    "complete": [
      "none"
    ],
    "skipped": [
      "non_trading_day"
    ],
    "partial": [
      "nonfatal_error",
      "incomplete_call",
      "unsupported_consumer",
      "instrumentation_missing"
    ],
    "unavailable": [
      "instrumentation_missing",
      "unsupported_consumer",
      "capacity_exceeded",
      "invalid_observed_value",
      "invalid_observed_identity"
    ]
  },
  "global_statuses": {
    "complete": [
      "none"
    ],
    "partial": [
      "nonfatal_error",
      "incomplete_call",
      "unsupported_consumer",
      "instrumentation_missing"
    ],
    "unavailable": [
      "instrumentation_missing",
      "unsupported_consumer",
      "capacity_exceeded",
      "invalid_observed_value",
      "invalid_observed_identity"
    ]
  },
  "limits": {
    "bytes": 2097152,
    "nodes": 4096,
    "reads_total": 16384,
    "reads_per_node": 4096,
    "strategies": 32,
    "symbols": 1024,
    "collection_entries": 2048,
    "passes": 5,
    "pairs_per_read": 128,
    "pairs_total": 8192,
    "chain_depth": 6,
    "identity_length": 64,
    "string_length": 96,
    "json_depth": 10,
    "number_token_bytes": 25
  },
  "identity": {
    "strategy": "[A-Za-z0-9][A-Za-z0-9_.:-]{0,63}",
    "symbol": "[A-Za-z0-9][A-Za-z0-9_.:/-]{0,63}"
  },
  "consumers": [
    {
      "name": "setup.selector",
      "kind": "call",
      "stage": "setup",
      "parents": [
        "ROOT"
      ],
      "dimensions": []
    },
    {
      "name": "setup.controlled_validation",
      "kind": "scope",
      "stage": "setup",
      "parents": [
        "setup.selector"
      ],
      "dimensions": []
    },
    {
      "name": "setup.ordinary_selection",
      "kind": "scope",
      "stage": "setup",
      "parents": [
        "setup.selector"
      ],
      "dimensions": []
    },
    {
      "name": "setup.selection_entry",
      "kind": "scope",
      "stage": "setup",
      "parents": [
        "setup.controlled_validation",
        "setup.ordinary_selection"
      ],
      "dimensions": [
        "strategy"
      ]
    },
    {
      "name": "setup.factory",
      "kind": "call",
      "stage": "setup",
      "parents": [
        "ROOT"
      ],
      "dimensions": []
    },
    {
      "name": "setup.factory_entry",
      "kind": "scope",
      "stage": "setup",
      "parents": [
        "setup.factory"
      ],
      "dimensions": [
        "strategy"
      ]
    },
    {
      "name": "portfolio.registration",
      "kind": "call",
      "stage": "setup",
      "parents": [
        "ROOT"
      ],
      "dimensions": [
        "strategy"
      ]
    },
    {
      "name": "runner.market_window",
      "kind": "scope",
      "stage": "market_input",
      "parents": [
        "ROOT"
      ],
      "dimensions": []
    },
    {
      "name": "runner.market_fetch",
      "kind": "call",
      "stage": "market_input",
      "parents": [
        "ROOT"
      ],
      "dimensions": []
    },
    {
      "name": "execution.history_update",
      "kind": "call",
      "stage": "cost_history",
      "parents": [
        "ROOT"
      ],
      "dimensions": [
        "symbol"
      ]
    },
    {
      "name": "cost.history",
      "kind": "scope",
      "stage": "cost_history",
      "parents": [
        "execution.history_update"
      ],
      "dimensions": []
    },
    {
      "name": "strategy.preparation",
      "kind": "call",
      "stage": "preparation",
      "parents": [
        "ROOT"
      ],
      "dimensions": [
        "strategy"
      ]
    },
    {
      "name": "portfolio.primary",
      "kind": "call",
      "stage": "primary",
      "parents": [
        "ROOT"
      ],
      "dimensions": []
    },
    {
      "name": "strategy.primary",
      "kind": "call",
      "stage": "primary",
      "parents": [
        "portfolio.primary"
      ],
      "dimensions": [
        "strategy"
      ]
    },
    {
      "name": "strategy.history",
      "kind": "scope",
      "stage": "inherited",
      "parents": [
        "strategy.preparation",
        "strategy.primary"
      ],
      "dimensions": []
    },
    {
      "name": "strategy.volatility",
      "kind": "scope",
      "stage": "inherited",
      "parents": [
        "strategy.preparation",
        "strategy.primary"
      ],
      "dimensions": []
    },
    {
      "name": "strategy.forecast",
      "kind": "scope",
      "stage": "inherited",
      "parents": [
        "strategy.preparation",
        "strategy.primary"
      ],
      "dimensions": []
    },
    {
      "name": "strategy.regime",
      "kind": "scope",
      "stage": "inherited",
      "parents": [
        "strategy.preparation",
        "strategy.primary"
      ],
      "dimensions": []
    },
    {
      "name": "strategy.sizing",
      "kind": "scope",
      "stage": "inherited",
      "parents": [
        "strategy.preparation",
        "strategy.primary"
      ],
      "dimensions": []
    },
    {
      "name": "strategy.buffering",
      "kind": "scope",
      "stage": "inherited",
      "parents": [
        "strategy.preparation",
        "strategy.primary"
      ],
      "dimensions": []
    },
    {
      "name": "strategy.base_risk",
      "kind": "scope",
      "stage": "inherited",
      "parents": [
        "strategy.preparation",
        "strategy.primary"
      ],
      "dimensions": []
    },
    {
      "name": "strategy.position_limits",
      "kind": "scope",
      "stage": "inherited",
      "parents": [
        "strategy.preparation",
        "strategy.primary"
      ],
      "dimensions": []
    },
    {
      "name": "portfolio.pass",
      "kind": "scope",
      "stage": "primary",
      "parents": [
        "portfolio.primary"
      ],
      "dimensions": [
        "index"
      ]
    },
    {
      "name": "portfolio.optimization",
      "kind": "call",
      "stage": "primary",
      "parents": [
        "portfolio.pass"
      ],
      "dimensions": []
    },
    {
      "name": "portfolio.symbol_collection",
      "kind": "scope",
      "stage": "primary",
      "parents": [
        "portfolio.optimization"
      ],
      "dimensions": []
    },
    {
      "name": "portfolio.numeric_aggregation",
      "kind": "scope",
      "stage": "primary",
      "parents": [
        "portfolio.optimization"
      ],
      "dimensions": []
    },
    {
      "name": "portfolio.redistribution",
      "kind": "scope",
      "stage": "primary",
      "parents": [
        "portfolio.optimization"
      ],
      "dimensions": []
    },
    {
      "name": "portfolio.optimization_strategy",
      "kind": "scope",
      "stage": "primary",
      "parents": [
        "portfolio.symbol_collection",
        "portfolio.numeric_aggregation",
        "portfolio.redistribution"
      ],
      "dimensions": [
        "strategy"
      ]
    },
    {
      "name": "portfolio.estimate",
      "kind": "scope",
      "stage": "primary",
      "parents": [
        "portfolio.optimization"
      ],
      "dimensions": [
        "symbol",
        "index"
      ]
    },
    {
      "name": "cost.estimate",
      "kind": "call",
      "stage": "primary",
      "parents": [
        "portfolio.estimate"
      ],
      "dimensions": []
    },
    {
      "name": "optimizer.primary",
      "kind": "call",
      "stage": "primary",
      "parents": [
        "portfolio.optimization"
      ],
      "dimensions": []
    },
    {
      "name": "portfolio.risk",
      "kind": "call",
      "stage": "primary",
      "parents": [
        "portfolio.pass"
      ],
      "dimensions": []
    },
    {
      "name": "risk.primary",
      "kind": "call",
      "stage": "primary",
      "parents": [
        "portfolio.risk"
      ],
      "dimensions": []
    },
    {
      "name": "cost.strategy_execution",
      "kind": "call",
      "stage": "primary",
      "parents": [
        "portfolio.primary"
      ],
      "dimensions": [
        "strategy",
        "symbol"
      ]
    },
    {
      "name": "cost.compatibility_execution",
      "kind": "call",
      "stage": "primary",
      "parents": [
        "portfolio.primary"
      ],
      "dimensions": [
        "symbol"
      ]
    },
    {
      "name": "execution.batch",
      "kind": "call",
      "stage": "execution",
      "parents": [
        "ROOT"
      ],
      "dimensions": [
        "strategy"
      ]
    },
    {
      "name": "execution.attempt",
      "kind": "call",
      "stage": "execution",
      "parents": [
        "execution.batch"
      ],
      "dimensions": [
        "symbol",
        "index"
      ]
    },
    {
      "name": "cost.execution",
      "kind": "scope",
      "stage": "execution",
      "parents": [
        "execution.attempt"
      ],
      "dimensions": []
    },
    {
      "name": "runner.pnl_finalization",
      "kind": "call",
      "stage": "diagnostics",
      "parents": [
        "ROOT"
      ],
      "dimensions": [
        "strategy"
      ]
    },
    {
      "name": "risk.diagnostics",
      "kind": "call",
      "stage": "diagnostics",
      "parents": [
        "ROOT"
      ],
      "dimensions": []
    },
    {
      "name": "runner.non_trading_day",
      "kind": "scope",
      "stage": "control_flow",
      "parents": [
        "ROOT"
      ],
      "dimensions": []
    },
    {
      "name": "runner.benchmark",
      "kind": "scope",
      "stage": "control_flow",
      "parents": [
        "ROOT"
      ],
      "dimensions": []
    }
  ],
  "metadata": [
    {
      "consumer": "setup.selector",
      "key": "mode",
      "type": "enum:ordinary,controlled",
      "required": true
    },
    {
      "consumer": "setup.ordinary_selection",
      "key": "normalized",
      "type": "bool",
      "required": false
    },
    {
      "consumer": "setup.selection_entry",
      "key": "enabled_live_read",
      "type": "bool",
      "required": true
    },
    {
      "consumer": "setup.selection_entry",
      "key": "allocation_read",
      "type": "bool",
      "required": true
    },
    {
      "consumer": "setup.selection_entry",
      "key": "enabled_live_present",
      "type": "bool",
      "required": false
    },
    {
      "consumer": "setup.selection_entry",
      "key": "enabled_live_defaulted",
      "type": "bool",
      "required": false
    },
    {
      "consumer": "setup.selection_entry",
      "key": "allocation_defaulted",
      "type": "bool",
      "required": false
    },
    {
      "consumer": "setup.factory_entry",
      "key": "type_defaulted",
      "type": "bool",
      "required": false
    },
    {
      "consumer": "setup.factory_entry",
      "key": "profile",
      "type": "enum:standard,fast,slow,unsupported",
      "required": false
    },
    {
      "consumer": "setup.factory_entry",
      "key": "construction",
      "type": "enum:not_reached,attempted,succeeded,failed",
      "required": true
    },
    {
      "consumer": "setup.factory_entry",
      "key": "initialize",
      "type": "enum:not_reached,attempted,succeeded,failed",
      "required": true
    },
    {
      "consumer": "setup.factory_entry",
      "key": "start",
      "type": "enum:not_reached,attempted,succeeded,failed",
      "required": true
    },
    {
      "consumer": "portfolio.registration",
      "key": "total_within_limit",
      "type": "bool",
      "required": false
    },
    {
      "consumer": "execution.history_update",
      "key": "cost_model_reached",
      "type": "bool",
      "required": true
    },
    {
      "consumer": "execution.history_update",
      "key": "previous_close_source",
      "type": "enum:initial_current_close,stored_previous_close",
      "required": false
    },
    {
      "consumer": "strategy.preparation",
      "key": "profile",
      "type": "enum:unsupported,base,standard,fast,slow",
      "required": true
    },
    {
      "consumer": "strategy.primary",
      "key": "profile",
      "type": "enum:unsupported,base,standard,fast,slow",
      "required": true
    },
    {
      "consumer": "strategy.base_risk",
      "key": "supported",
      "type": "bool",
      "required": true
    },
    {
      "consumer": "strategy.position_limits",
      "key": "supported",
      "type": "bool",
      "required": true
    },
    {
      "consumer": "portfolio.primary",
      "key": "skip_execution_generation",
      "type": "bool",
      "required": false
    },
    {
      "consumer": "portfolio.optimization",
      "key": "skip",
      "type": "enum:none,no_eligible_symbols,insufficient_history,absent_optimizer",
      "required": true
    },
    {
      "consumer": "portfolio.risk",
      "key": "skip",
      "type": "enum:none,absent_risk_manager,no_positions",
      "required": true
    },
    {
      "consumer": "portfolio.risk",
      "key": "manager_source",
      "type": "enum:absent,internal,external",
      "required": false
    },
    {
      "consumer": "optimizer.primary",
      "key": "buffer_branch",
      "type": "enum:not_reached,disabled,returned_prior,applied,failed",
      "required": true
    },
    {
      "consumer": "cost.estimate",
      "key": "input_source",
      "type": "enum:internally_tracked,explicit_values",
      "required": false
    },
    {
      "consumer": "cost.estimate",
      "key": "asset_lookup",
      "type": "enum:exact_symbol,pre_dot_root,fallback",
      "required": false
    },
    {
      "consumer": "cost.strategy_execution",
      "key": "input_source",
      "type": "enum:internally_tracked,explicit_values",
      "required": false
    },
    {
      "consumer": "cost.strategy_execution",
      "key": "asset_lookup",
      "type": "enum:exact_symbol,pre_dot_root,fallback",
      "required": false
    },
    {
      "consumer": "cost.compatibility_execution",
      "key": "input_source",
      "type": "enum:internally_tracked,explicit_values",
      "required": false
    },
    {
      "consumer": "cost.compatibility_execution",
      "key": "asset_lookup",
      "type": "enum:exact_symbol,pre_dot_root,fallback",
      "required": false
    },
    {
      "consumer": "cost.execution",
      "key": "input_source",
      "type": "enum:internally_tracked,explicit_values",
      "required": false
    },
    {
      "consumer": "cost.execution",
      "key": "asset_lookup",
      "type": "enum:exact_symbol,pre_dot_root,fallback",
      "required": false
    },
    {
      "consumer": "execution.batch",
      "key": "state",
      "type": "enum:entered,rejected_stream,returned,invalid_argument",
      "required": true
    },
    {
      "consumer": "execution.attempt",
      "key": "state",
      "type": "enum:entered,rejected_stream,rejected_id,cost_call_reached,returned",
      "required": true
    },
    {
      "consumer": "execution.attempt",
      "key": "branch",
      "type": "enum:current_position,removed_position",
      "required": true
    },
    {
      "consumer": "execution.attempt",
      "key": "price_source",
      "type": "enum:market_prices,current_average_price,previous_average_price",
      "required": true
    },
    {
      "consumer": "execution.attempt",
      "key": "returned",
      "type": "bool",
      "required": true
    },
    {
      "consumer": "runner.pnl_finalization",
      "key": "allocation_lookup",
      "type": "enum:hit,fallback",
      "required": false
    },
    {
      "consumer": "runner.non_trading_day",
      "key": "skip_strategy_processing",
      "type": "bool",
      "required": true
    },
    {
      "consumer": "runner.benchmark",
      "key": "branch",
      "type": "enum:not_reached,attempted,succeeded,failed",
      "required": true
    }
  ],
  "fields": [
    {
      "consumer": "setup.selection_entry",
      "field": "setup.selection.enabled_live",
      "type": "bool",
      "origins": [
        "configured_strategy_leaf",
        "code_default"
      ]
    },
    {
      "consumer": "setup.selection_entry",
      "field": "setup.selection.default_allocation",
      "type": "number",
      "origins": [
        "configured_strategy_leaf",
        "code_default"
      ]
    },
    {
      "consumer": "setup.selection_entry",
      "field": "setup.selection.effective_allocation",
      "type": "number",
      "origins": [
        "derived"
      ]
    },
    {
      "consumer": "setup.controlled_validation",
      "field": "setup.selection.sum",
      "type": "number",
      "origins": [
        "derived"
      ]
    },
    {
      "consumer": "setup.ordinary_selection",
      "field": "setup.selection.sum",
      "type": "number",
      "origins": [
        "derived"
      ]
    },
    {
      "consumer": "setup.factory_entry",
      "field": "setup.factory.effective_allocation",
      "type": "number",
      "origins": [
        "selected_allocation"
      ]
    },
    {
      "consumer": "setup.factory_entry",
      "field": "setup.factory.initial_capital",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "setup.factory_entry",
      "field": "setup.factory.allocated_capital",
      "type": "number",
      "origins": [
        "derived"
      ]
    },
    {
      "consumer": "portfolio.registration",
      "field": "portfolio.registration.initial_allocation",
      "type": "number",
      "origins": [
        "selected_allocation"
      ]
    },
    {
      "consumer": "portfolio.registration",
      "field": "portfolio.registration.min_allocation",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "portfolio.registration",
      "field": "portfolio.registration.max_allocation",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "portfolio.registration",
      "field": "portfolio.registration.total_allocation",
      "type": "number",
      "origins": [
        "derived"
      ]
    },
    {
      "consumer": "portfolio.registration",
      "field": "portfolio.registration.requested_optimization",
      "type": "bool",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "portfolio.registration",
      "field": "portfolio.registration.portfolio_optimization",
      "type": "bool",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "portfolio.registration",
      "field": "portfolio.registration.requested_risk",
      "type": "bool",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "portfolio.registration",
      "field": "portfolio.registration.portfolio_risk",
      "type": "bool",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "portfolio.registration",
      "field": "portfolio.registration.stored_allocation",
      "type": "number",
      "origins": [
        "selected_allocation"
      ]
    },
    {
      "consumer": "portfolio.registration",
      "field": "portfolio.registration.stored_optimization",
      "type": "bool",
      "origins": [
        "derived"
      ]
    },
    {
      "consumer": "portfolio.registration",
      "field": "portfolio.registration.stored_risk",
      "type": "bool",
      "origins": [
        "derived"
      ]
    },
    {
      "consumer": "runner.market_window",
      "field": "runner.market_input.historical_days",
      "type": "int32",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "runner.benchmark",
      "field": "runner.benchmark.mode",
      "type": "benchmark_mode",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "runner.pnl_finalization",
      "field": "runner.pnl_finalization.strategy_allocation",
      "type": "number",
      "origins": [
        "selected_allocation",
        "code_default"
      ]
    },
    {
      "consumer": "runner.pnl_finalization",
      "field": "runner.pnl_finalization.initial_capital",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "runner.pnl_finalization",
      "field": "runner.pnl_finalization.strategy_capital",
      "type": "number",
      "origins": [
        "derived"
      ]
    },
    {
      "consumer": "strategy.history",
      "field": "strategy.history.max_history_size",
      "type": "uint53",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.history",
      "field": "strategy.history.ema_windows",
      "type": "int32_pairs",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.volatility",
      "field": "strategy.volatility.vol_lookback_short",
      "type": "int32",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.volatility",
      "field": "strategy.volatility.max_history_size",
      "type": "uint53",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.forecast",
      "field": "strategy.forecast.ema_windows",
      "type": "int32_pairs",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.forecast",
      "field": "strategy.forecast.vol_lookback_short",
      "type": "int32",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.forecast",
      "field": "strategy.forecast.fdm",
      "type": "int32_number_pairs",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.regime",
      "field": "strategy.regime.vol_lookback_long",
      "type": "int32",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.sizing",
      "field": "strategy.sizing.capital_allocation",
      "type": "number",
      "origins": [
        "derived",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.sizing",
      "field": "strategy.sizing.max_leverage",
      "type": "number",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.sizing",
      "field": "strategy.sizing.idm",
      "type": "number",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.sizing",
      "field": "strategy.sizing.risk_target",
      "type": "number",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.sizing",
      "field": "strategy.sizing.fx_rate",
      "type": "number",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.sizing",
      "field": "strategy.sizing.max_symbol_concentration",
      "type": "number",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.sizing",
      "field": "strategy.sizing.symbol_limit.symbol.present",
      "type": "bool",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.sizing",
      "field": "strategy.sizing.symbol_limit.symbol.value",
      "type": "number",
      "origins": [
        "derived",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.buffering",
      "field": "strategy.buffering.use_position_buffering",
      "type": "bool",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.buffering",
      "field": "strategy.buffering.weight",
      "type": "number",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.buffering",
      "field": "strategy.buffering.idm",
      "type": "number",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.buffering",
      "field": "strategy.buffering.risk_target",
      "type": "number",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.buffering",
      "field": "strategy.buffering.fx_rate",
      "type": "number",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.buffering",
      "field": "strategy.buffering.carver_buffer_floor",
      "type": "number",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.buffering",
      "field": "strategy.buffering.carver_buffer_position_factor",
      "type": "number",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.buffering",
      "field": "strategy.buffering.capital_allocation",
      "type": "number",
      "origins": [
        "derived",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.buffering",
      "field": "strategy.buffering.symbol_limit.symbol.present",
      "type": "bool",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.buffering",
      "field": "strategy.buffering.symbol_limit.symbol.value",
      "type": "number",
      "origins": [
        "derived",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.base_risk",
      "field": "strategy.base_risk.capital_allocation",
      "type": "number",
      "origins": [
        "derived",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.base_risk",
      "field": "strategy.base_risk.risk_max_leverage",
      "type": "fixed_decimal8",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.base_risk",
      "field": "strategy.base_risk.risk_max_drawdown",
      "type": "fixed_decimal8",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.base_risk",
      "field": "strategy.base_risk.fallback_config_max_leverage",
      "type": "number",
      "origins": [
        "constructor_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.base_risk",
      "field": "strategy.base_risk.trading_multiplier.symbol.present",
      "type": "bool",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.base_risk",
      "field": "strategy.base_risk.trading_multiplier.symbol.value",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.position_limits",
      "field": "strategy.position_limits.symbol.present",
      "type": "bool",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "strategy.position_limits",
      "field": "strategy.position_limits.symbol.value",
      "type": "number",
      "origins": [
        "derived",
        "runtime_effective"
      ]
    },
    {
      "consumer": "portfolio.pass",
      "field": "portfolio.pass.use_optimization",
      "type": "bool",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "portfolio.pass",
      "field": "portfolio.pass.use_risk_management",
      "type": "bool",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "portfolio.optimization",
      "field": "portfolio.optimization.total_capital",
      "type": "fixed_decimal8",
      "origins": [
        "derived",
        "runtime_effective"
      ]
    },
    {
      "consumer": "portfolio.optimization_strategy",
      "field": "portfolio.optimization.strategy.enabled",
      "type": "bool",
      "origins": [
        "derived"
      ]
    },
    {
      "consumer": "portfolio.optimization_strategy",
      "field": "portfolio.optimization.strategy.allocation",
      "type": "number",
      "origins": [
        "selected_allocation",
        "runtime_effective"
      ]
    },
    {
      "consumer": "portfolio.risk",
      "field": "portfolio.risk.lookback_period",
      "type": "int32",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "optimizer.primary",
      "field": "optimizer.cost_penalty_scalar",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "optimizer.primary",
      "field": "optimizer.convergence_threshold",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "optimizer.primary",
      "field": "optimizer.tau",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "optimizer.primary",
      "field": "optimizer.buffer_size_factor",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "optimizer.primary",
      "field": "optimizer.max_iterations",
      "type": "int32",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "optimizer.primary",
      "field": "optimizer.use_buffering",
      "type": "bool",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.primary",
      "field": "risk.var_limit",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.primary",
      "field": "risk.jump_risk_limit",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.primary",
      "field": "risk.max_correlation",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.primary",
      "field": "risk.max_gross_leverage",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.primary",
      "field": "risk.max_net_leverage",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.primary",
      "field": "risk.confidence_level",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.diagnostics",
      "field": "risk.var_limit",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.diagnostics",
      "field": "risk.jump_risk_limit",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.diagnostics",
      "field": "risk.max_correlation",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.diagnostics",
      "field": "risk.max_gross_leverage",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.diagnostics",
      "field": "risk.max_net_leverage",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.diagnostics",
      "field": "risk.confidence_level",
      "type": "number",
      "origins": [
        "app_config_effective",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.primary",
      "field": "risk.capital",
      "type": "fixed_decimal8",
      "origins": [
        "derived",
        "runtime_effective"
      ]
    },
    {
      "consumer": "risk.diagnostics",
      "field": "risk.capital",
      "type": "fixed_decimal8",
      "origins": [
        "derived",
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.spread.baseline_spread_ticks",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.spread.min_spread_ticks",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.spread.max_spread_ticks",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.spread.spread_cost_multiplier",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.spread.tick_size",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.spread.baseline_spread_ticks",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.spread.min_spread_ticks",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.spread.max_spread_ticks",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.spread.spread_cost_multiplier",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.spread.tick_size",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.spread.baseline_spread_ticks",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.spread.min_spread_ticks",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.spread.max_spread_ticks",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.spread.spread_cost_multiplier",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.spread.tick_size",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.spread.baseline_spread_ticks",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.spread.min_spread_ticks",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.spread.max_spread_ticks",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.spread.spread_cost_multiplier",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.spread.tick_size",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.volatility.lambda",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.volatility.min_multiplier",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.volatility.max_multiplier",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.volatility.lambda",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.volatility.min_multiplier",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.volatility.max_multiplier",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.volatility.lambda",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.volatility.min_multiplier",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.volatility.max_multiplier",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.volatility.lambda",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.volatility.min_multiplier",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.volatility.max_multiplier",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.impact.min_adv",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.impact.min_participation",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.impact.max_participation",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.impact.max_impact_bps",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.impact.min_adv",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.impact.min_participation",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.impact.max_participation",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.impact.max_impact_bps",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.impact.min_adv",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.impact.min_participation",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.impact.max_participation",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.impact.max_impact_bps",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.impact.min_adv",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.impact.min_participation",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.impact.max_participation",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.impact.max_impact_bps",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.charge.explicit_fee_per_contract",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.charge.point_value",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.charge.explicit_fee_per_contract",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.charge.point_value",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.charge.explicit_fee_per_contract",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.charge.point_value",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.charge.explicit_fee_per_contract",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.charge.point_value",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.history",
      "field": "cost.impact_history.adv_lookback_days",
      "type": "uint53",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.history",
      "field": "cost.spread_history.lookback_days",
      "type": "uint53",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.spread.tick_constrained",
      "type": "bool",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.charge.commission_per_unit",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.charge.max_commission_pct",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.charge.max_commission_per_order",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.charge.min_commission_per_order",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.charge.apply_regulatory_fees",
      "type": "bool",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.charge.sec_fee_per_million",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.charge.finra_taf_per_share",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.charge.finra_taf_cap_per_trade",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.estimate",
      "field": "cost.charge.max_total_implicit_bps",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.spread.tick_constrained",
      "type": "bool",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.charge.commission_per_unit",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.charge.max_commission_pct",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.charge.max_commission_per_order",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.charge.min_commission_per_order",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.charge.apply_regulatory_fees",
      "type": "bool",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.charge.sec_fee_per_million",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.charge.finra_taf_per_share",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.charge.finra_taf_cap_per_trade",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.strategy_execution",
      "field": "cost.charge.max_total_implicit_bps",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.spread.tick_constrained",
      "type": "bool",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.charge.commission_per_unit",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.charge.max_commission_pct",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.charge.max_commission_per_order",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.charge.min_commission_per_order",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.charge.apply_regulatory_fees",
      "type": "bool",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.charge.sec_fee_per_million",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.charge.finra_taf_per_share",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.charge.finra_taf_cap_per_trade",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.compatibility_execution",
      "field": "cost.charge.max_total_implicit_bps",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.spread.tick_constrained",
      "type": "bool",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.charge.commission_per_unit",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.charge.max_commission_pct",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.charge.max_commission_per_order",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.charge.min_commission_per_order",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.charge.apply_regulatory_fees",
      "type": "bool",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.charge.sec_fee_per_million",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.charge.finra_taf_per_share",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.charge.finra_taf_cap_per_trade",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    },
    {
      "consumer": "cost.execution",
      "field": "cost.charge.max_total_implicit_bps",
      "type": "number",
      "origins": [
        "runtime_effective"
      ]
    }
  ]
}
)CATALOG");
    return descriptor;
}

Json project(const RunConsumption& run) {
    if (run.failure == RunEvidenceFailure::CapacityExceeded)
        fail(ConsumptionUnavailableReason::CapacityExceeded);
    if (run.failure == RunEvidenceFailure::InvalidObservedIdentity)
        fail(ConsumptionUnavailableReason::InvalidObservedIdentity);
    if (run.failure != RunEvidenceFailure::None)
        fail(ConsumptionUnavailableReason::InvalidObservedValue);
    Builder b;
    selector(b, run);
    factory(b, run);
    b.count(run.registrations);
    for (const auto& entry : run.registrations) registration(b, entry);
    if (run.historical_days_reached) {
        auto id = b.node("runner.market_window");
        b.read(id, "runner.market_input.historical_days", run.historical_days, "runtime_effective");
    }
    if (entered(run.market_fetch.outcome))
        b.node("runner.market_fetch", {}, outcome(run.market_fetch.outcome));
    history(b, run);
    if (run.preparation && entered(run.preparation->call.outcome)) {
        const auto& entry = *run.preparation;
        auto id = b.node("strategy.preparation", {}, outcome(entry.call.outcome),
            {{"profile", profile_text(entry.call.payload.profile)}}, entry.identity);
        strategy_reads(b, id, entry.call.payload);
    }
    primary(b, run);
    execution(b, run);
    diagnostics(b, run);
    if (run.non_trading_decision_reached)
        b.node("runner.non_trading_day", {}, {},
            {{"skip_strategy_processing", run.skip_strategy_processing}});
    if (run.benchmark_decision_reached) {
        if (run.benchmark_mode == RunBenchmarkMode::Live &&
            run.benchmark_state == RunBenchmarkState::Deferred)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        if (run.benchmark_mode == RunBenchmarkMode::Live &&
            run.benchmark_state == RunBenchmarkState::NotReached)
            return finalize(b, run);
        const char* branch = nullptr;
        switch (run.benchmark_state) {
            case RunBenchmarkState::NotReached: branch = "not_reached"; break;
            case RunBenchmarkState::Deferred: branch = "not_reached"; break;
            case RunBenchmarkState::InProgress: branch = "attempted"; break;
            case RunBenchmarkState::Succeeded: branch = "succeeded"; break;
            case RunBenchmarkState::NonfatalError: branch = "failed"; break;
            default: fail(ConsumptionUnavailableReason::InvalidObservedValue);
        }
        if (run.benchmark_mode == RunBenchmarkMode::Deferred &&
            run.benchmark_state != RunBenchmarkState::Deferred)
            fail(ConsumptionUnavailableReason::InvalidObservedValue);
        auto id = b.node("runner.benchmark", {}, {}, {{"branch", branch}});
        if (run.benchmark_mode != RunBenchmarkMode::Unsupported)
            b.read(id, "runner.benchmark.mode",
                run.benchmark_mode == RunBenchmarkMode::Live ? "live" : "deferred",
                "runtime_effective");
    }
    return finalize(b, run);
}
}  // namespace

ConsumptionProjection ConsumptionProjection::unavailable(ConsumptionUnavailableReason reason) {
    Json coverage = Json::object();
    for (const char* stage : stages)
        coverage[stage] = {{"status", "unavailable"}, {"reason", unavailable_text(reason)}};
    return ConsumptionProjection({{"version", 2}, {"status", "unavailable"},
        {"reason", unavailable_text(reason)}, {"coverage", std::move(coverage)}, {"nodes", Json::array()}});
}

ConsumptionProjection project_run_consumption(const RunConsumption& run) {
    try { return ConsumptionProjection(project(run)); }
    catch (const Fault& error) { return ConsumptionProjection::unavailable(error.reason); }
}
const nlohmann::json& consumption_projection_catalog() { return catalog(); }
}  // namespace trade_ngin
