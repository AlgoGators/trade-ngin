#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/portfolio/qt_evaluation.hpp"
#include "trade_ngin/git_version.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace trade_ngin {
namespace {
using Json = nlohmann::json;

void fail() { throw std::invalid_argument("invalid_qt_evaluation_request"); }

std::set<std::string> words(std::string_view names) {
    std::set<std::string> out;
    size_t begin = 0;
    while (begin < names.size()) {
        auto end = names.find(' ', begin);
        if (end == std::string_view::npos) end = names.size();
        out.emplace(names.substr(begin, end - begin));
        begin = end + 1;
    }
    return out;
}

void fields(const Json& value, std::string_view required) {
    if (!value.is_object() || value.size() != words(required).size()) fail();
    auto expected = words(required);
    for (auto it = value.begin(); it != value.end(); ++it)
        if (expected.erase(it.key()) != 1) fail();
    if (!expected.empty()) fail();
}

void nonempty(const std::string& value) {
    if (value.empty() || value.size() > 4096 ||
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c <= ' '; })) fail();
}

std::string string_at(const Json& object, const char* name) {
    const auto& value = object.at(name);
    if (!value.is_string()) fail();
    auto text = value.get<std::string>();
    nonempty(text);
    return text;
}

bool bool_at(const Json& object, const char* name) {
    const auto& value = object.at(name);
    if (!value.is_boolean()) fail();
    return value.get<bool>();
}

int integer_at(const Json& object, const char* name) {
    const auto& value = object.at(name);
    if (!value.is_number_integer()) fail();
    if (value.is_number_unsigned() && value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int>::max())) fail();
    const auto number = value.get<int64_t>();
    if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) fail();
    return static_cast<int>(number);
}

Quantity exact_at(const Json& object, const char* name) {
    auto parsed = parse_qt_quantity_exact(string_at(object, name));
    if (parsed.is_error()) fail();
    return parsed.value();
}

double diagnostic_at(const Json& object, const char* name) {
    const auto text = string_at(object, name);
    if (text.find_first_not_of("-+0123456789.eE") != std::string::npos) fail();
    double result = 0;
    auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), result);
    if (error != std::errc{} || end != text.data()+text.size() || !std::isfinite(result)) fail();
    return result;
}

int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
    return static_cast<int64_t>(era) * 146097 + doe - 719468;
}

int read_two(std::string_view text, size_t index) {
    if (text[index] < '0' || text[index] > '9' || text[index+1] < '0' || text[index+1] > '9') fail();
    return (text[index]-'0')*10 + (text[index+1]-'0');
}

Timestamp timestamp_at(const Json& object, const char* name) {
    const auto text = string_at(object, name);
    if (text.size() < 20 || text[4]!='-' || text[7]!='-' || text[10]!='T' ||
        text[13]!=':' || text[16]!=':' || text.back()!='Z') fail();
    int year = 0;
    for (size_t i=0; i<4; ++i) {
        if (text[i]<'0'||text[i]>'9') fail();
        year = year*10 + text[i]-'0';
    }
    const int month = read_two(text,5), day = read_two(text,8);
    const int hour = read_two(text,11), minute = read_two(text,14), second = read_two(text,17);
    constexpr int month_days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (year<1||month<1||month>12||hour>23||minute>59||second>59) fail();
    const bool leap = year%4==0 && (year%100!=0||year%400==0);
    if (day<1||day>month_days[month]+(month==2&&leap?1:0)) fail();
    int64_t nanosecond = 0;
    if (text.size() > 20) {
        if (text[19]!='.' || text.size()>30) fail();
        const size_t digits = text.size()-21;
        if (digits<1||digits>9) fail();
        for (size_t i=20; i<text.size()-1; ++i) {
            if (text[i]<'0'||text[i]>'9') fail();
            nanosecond = nanosecond*10 + text[i]-'0';
        }
        for (size_t i=digits; i<9; ++i) nanosecond *= 10;
    }
    const int64_t seconds = days_from_civil(year,month,day)*86400 + hour*3600 + minute*60 + second;
    if (seconds > INT64_MAX/1000000000LL || seconds < INT64_MIN/1000000000LL) fail();
    const int64_t base = seconds*1000000000LL;
    if (base > INT64_MAX-nanosecond) fail();
    const std::chrono::nanoseconds raw(base + nanosecond);
    const auto native = std::chrono::duration_cast<Timestamp::duration>(raw);
    if (std::chrono::duration_cast<std::chrono::nanoseconds>(native) != raw) fail();
    return Timestamp(native);
}

ComponentPositionKey key(const Json& value) {
    fields(value,"portfolio_id strategy_id strategy_name date symbol portfolio_type");
    ComponentPositionKey out;
    out.portfolio_id=string_at(value,"portfolio_id");
    out.strategy_id=string_at(value,"strategy_id");
    out.strategy_name=string_at(value,"strategy_name");
    out.date=string_at(value,"date");
    out.symbol=string_at(value,"symbol");
    out.portfolio_type=string_at(value,"portfolio_type");
    return out;
}

InstrumentIdentity instrument(const Json& value) {
    fields(value,"instrument_type symbol");
    InstrumentIdentity out;
    const auto type=string_at(value,"instrument_type");
    if (type=="FUTURE") out.type=AssetType::FUTURE;
    else if (type=="EQUITY") out.type=AssetType::EQUITY;
    else if (type=="OPTION") out.type=AssetType::OPTION;
    else if (type=="FOREX") out.type=AssetType::FOREX;
    else if (type=="CRYPTO") out.type=AssetType::CRYPTO;
    else fail();
    out.symbol=string_at(value,"symbol");
    return out;
}

Position position(const Json& value) {
    fields(value,"symbol quantity_exact average_price_exact unrealized_pnl_exact realized_pnl_exact last_update");
    return Position(string_at(value,"symbol"), exact_at(value,"quantity_exact"),
                    exact_at(value,"average_price_exact"),exact_at(value,"unrealized_pnl_exact"),
                    exact_at(value,"realized_pnl_exact"),timestamp_at(value,"last_update"));
}

ComponentBookContext context(const Json& value) {
    fields(value,"portfolio_id date portfolio_type revision slots");
    ComponentBookContext out;
    out.portfolio_id=string_at(value,"portfolio_id");
    out.date=string_at(value,"date");
    out.portfolio_type=string_at(value,"portfolio_type");
    out.revision=string_at(value,"revision");
    const auto& slots=value.at("slots");
    if (!slots.is_array()||slots.size()>4096) fail();
    for (const auto& item:slots) {
        fields(item,"key instrument editable previous");
        ComponentBookSlot slot;
        slot.key=key(item.at("key"));
        slot.instrument=instrument(item.at("instrument"));
        slot.editable=bool_at(item,"editable");
        if (!item.at("previous").is_null()) slot.previous=position(item.at("previous"));
        out.slots.push_back(std::move(slot));
    }
    return out;
}

ComponentBookProposal proposal(const Json& value) {
    fields(value,"expected_portfolio_id expected_date expected_portfolio_type expected_revision quantities");
    ComponentBookProposal out;
    out.expected_portfolio_id=string_at(value,"expected_portfolio_id");
    out.expected_date=string_at(value,"expected_date");
    out.expected_portfolio_type=string_at(value,"expected_portfolio_type");
    out.expected_revision=string_at(value,"expected_revision");
    const auto& quantities=value.at("quantities");
    if (!quantities.is_array()||quantities.size()>4096) fail();
    for (const auto& item:quantities) {
        fields(item,"key quantity_exact");
        out.quantities.push_back({key(item.at("key")),exact_at(item,"quantity_exact")});
    }
    return out;
}

ComponentRiskInputs risk_inputs(const Json& value) {
    fields(value,"expected_portfolio_id expected_date expected_portfolio_type expected_revision market_snapshot_id valuation_time capital_currency valuations expected_observation_times closes");
    ComponentRiskInputs out;
    out.expected_portfolio_id=string_at(value,"expected_portfolio_id");
    out.expected_date=string_at(value,"expected_date");
    out.expected_portfolio_type=string_at(value,"expected_portfolio_type");
    out.expected_revision=string_at(value,"expected_revision");
    out.market_snapshot_id=string_at(value,"market_snapshot_id");
    out.valuation_time=timestamp_at(value,"valuation_time");
    out.capital_currency=string_at(value,"capital_currency");
    const auto& valuations=value.at("valuations");
    const auto& times=value.at("expected_observation_times");
    const auto& closes=value.at("closes");
    if (!valuations.is_array()||valuations.size()>4096||
        !times.is_array()||times.size()>4096||
        !closes.is_array()||closes.size()>4096) fail();
    for (const auto& item:valuations) {
        fields(item,"instrument mark_as_of mark price_multiplier quote_currency");
        out.valuations.push_back({instrument(item.at("instrument")),timestamp_at(item,"mark_as_of"),
                                  diagnostic_at(item,"mark"),diagnostic_at(item,"price_multiplier"),
                                  string_at(item,"quote_currency")});
    }
    for (const auto& item:times) {
        Json wrap={{"at",item}};
        out.expected_observation_times.push_back(timestamp_at(wrap,"at"));
    }
    for (const auto& item:closes) {
        fields(item,"instrument timestamp close");
        out.closes.push_back({instrument(item.at("instrument")),timestamp_at(item,"timestamp"),
                              diagnostic_at(item,"close")});
    }
    return out;
}

RiskConfig risk_config(const Json& value) {
    fields(value,"var_limit jump_risk_limit max_correlation corr_shock_threshold jump_shock_threshold max_gross_leverage max_net_leverage confidence_level lookback_period capital_exact version");
    RiskConfig out;
    out.var_limit=diagnostic_at(value,"var_limit");
    out.jump_risk_limit=diagnostic_at(value,"jump_risk_limit");
    out.max_correlation=diagnostic_at(value,"max_correlation");
    out.corr_shock_threshold=diagnostic_at(value,"corr_shock_threshold");
    out.jump_shock_threshold=diagnostic_at(value,"jump_shock_threshold");
    out.max_gross_leverage=diagnostic_at(value,"max_gross_leverage");
    out.max_net_leverage=diagnostic_at(value,"max_net_leverage");
    out.confidence_level=diagnostic_at(value,"confidence_level");
    out.lookback_period=integer_at(value,"lookback_period");
    out.capital=exact_at(value,"capital_exact");
    out.version=string_at(value,"version");
    return out;
}

InstrumentQuantityRules quantity_rules(const Json& value) {
    if (!value.is_array()||value.size()>4096) fail();
    InstrumentQuantityRules out;
    for (const auto& item:value) {
        fields(item,"instrument increment_exact mode minimum_exact maximum_exact");
        QuantityRule rule;
        rule.increment=exact_at(item,"increment_exact");
        if (string_at(item,"mode")!="reject_off_increment") fail();
        rule.mode=QuantityRoundingMode::reject_off_increment;
        if (!item.at("minimum_exact").is_null()) rule.minimum=exact_at(item,"minimum_exact");
        if (!item.at("maximum_exact").is_null()) rule.maximum=exact_at(item,"maximum_exact");
        if (!out.emplace(instrument(item.at("instrument")),std::move(rule)).second) fail();
    }
    return out;
}

std::vector<QtComponentCostInput> cost_inputs(const Json& value) {
    if (!value.is_array()||value.size()>4096) fail();
    std::vector<QtComponentCostInput> out;
    std::set<ComponentPositionKey> seen;
    for (const auto& item:value) {
        fields(item,"key instrument calculation_increment_exact cash_cost_per_increment_exact approved_model_id source_id currency");
        QtComponentCostInput input;
        input.key=key(item.at("key"));
        input.instrument=instrument(item.at("instrument"));
        input.calculation_increment=exact_at(item,"calculation_increment_exact");
        input.cash_cost_per_increment=exact_at(item,"cash_cost_per_increment_exact");
        input.approved_model_id=string_at(item,"approved_model_id");
        input.source_id=string_at(item,"source_id");
        input.currency=string_at(item,"currency");
        if (!seen.insert(input.key).second) fail();
        out.push_back(std::move(input));
    }
    return out;
}

ComponentOptimizerInputs optimizer_inputs(const Json& value) {
    fields(value,"expected_portfolio_id expected_date expected_portfolio_type expected_revision market_snapshot_id valuation_time capital_currency instruments expected_observation_times closes");
    ComponentOptimizerInputs out;
    out.expected_portfolio_id=string_at(value,"expected_portfolio_id");
    out.expected_date=string_at(value,"expected_date");
    out.expected_portfolio_type=string_at(value,"expected_portfolio_type");
    out.expected_revision=string_at(value,"expected_revision");
    out.market_snapshot_id=string_at(value,"market_snapshot_id");
    out.valuation_time=timestamp_at(value,"valuation_time");
    out.capital_currency=string_at(value,"capital_currency");
    const auto& instruments=value.at("instruments");
    const auto& times=value.at("expected_observation_times");
    const auto& closes=value.at("closes");
    if (!instruments.is_array()||instruments.size()>4096||
        !times.is_array()||times.size()>4096||
        !closes.is_array()||closes.size()>4096) fail();
    for (const auto& item:instruments) {
        fields(item,"instrument mark_as_of mark price_multiplier quote_currency calculation_increment_exact cash_cost_per_increment cost_currency increment_source_id cost_source_id");
        out.instruments.push_back({instrument(item.at("instrument")),timestamp_at(item,"mark_as_of"),
            diagnostic_at(item,"mark"),diagnostic_at(item,"price_multiplier"),
            string_at(item,"quote_currency"),exact_at(item,"calculation_increment_exact"),
            diagnostic_at(item,"cash_cost_per_increment"),string_at(item,"cost_currency"),
            string_at(item,"increment_source_id"),string_at(item,"cost_source_id")});
    }
    for (const auto& item:times) {
        Json wrap={{"at",item}};
        out.expected_observation_times.push_back(timestamp_at(wrap,"at"));
    }
    for (const auto& item:closes) {
        fields(item,"instrument timestamp close");
        out.closes.push_back({instrument(item.at("instrument")),timestamp_at(item,"timestamp"),diagnostic_at(item,"close")});
    }
    return out;
}

DynamicOptConfig optimizer_config(const Json& value) {
    fields(value,"tau capital cost_penalty_scalar asymmetric_risk_buffer max_iterations convergence_threshold use_buffering buffer_size_factor version");
    DynamicOptConfig out;
    out.tau=diagnostic_at(value,"tau");
    out.capital=diagnostic_at(value,"capital");
    out.cost_penalty_scalar=diagnostic_at(value,"cost_penalty_scalar");
    out.asymmetric_risk_buffer=diagnostic_at(value,"asymmetric_risk_buffer");
    out.max_iterations=integer_at(value,"max_iterations");
    out.convergence_threshold=diagnostic_at(value,"convergence_threshold");
    out.use_buffering=bool_at(value,"use_buffering");
    out.buffer_size_factor=diagnostic_at(value,"buffer_size_factor");
    out.version=string_at(value,"version");
    return out;
}

Json strict_parse(std::string_view utf8) {
    if (utf8.size()>8*1024*1024) fail();
    std::vector<std::set<std::string>> keys;
    auto callback=[&keys](int,Json::parse_event_t event,Json& parsed) {
        if (event==Json::parse_event_t::object_start) keys.emplace_back();
        else if (event==Json::parse_event_t::object_end) keys.pop_back();
        else if (event==Json::parse_event_t::key &&
                 (keys.empty()||!keys.back().insert(parsed.get<std::string>()).second)) fail();
        return true;
    };
    return Json::parse(utf8.begin(),utf8.end(),callback);
}

}  // namespace

Result<QtEvaluationRequest> parse_qt_evaluation_request(std::string_view utf8) {
    try {
        auto value=strict_parse(utf8);
        if (!value.is_object()) fail();
        const auto operation=string_at(value,"operation");
        if (operation=="selected_book")
            fields(value,"schema operation evaluator_build context_fingerprint risk_config_source_id context proposal risk_inputs risk_config quantity_rules component_cost_inputs");
        else if (operation=="draft_diagnostic") {
            const auto& policy=value.at("optimizer_policy");
            fields(policy,"enabled config_source_id");
            if (bool_at(policy,"enabled"))
                fields(value,"schema operation evaluator_build context_fingerprint risk_config_source_id context proposal risk_inputs risk_config quantity_rules component_cost_inputs optimizer_policy optimizer_inputs optimizer_config");
            else
                fields(value,"schema operation evaluator_build context_fingerprint risk_config_source_id context proposal risk_inputs risk_config quantity_rules component_cost_inputs optimizer_policy");
        } else fail();
        const auto schema=string_at(value,"schema");
        if (schema!="qt-eval/v1" && schema!="qt-eval-empty-owner/v2") fail();
        QtEvaluationRequest out;
        out.input_mode=schema=="qt-eval-empty-owner/v2" ?
            QtEvaluationInputMode::EmptyOwnerV2 : QtEvaluationInputMode::LegacyV1;
        out.operation=operation=="selected_book" ? QtEvaluationOperation::SelectedBook : QtEvaluationOperation::DraftDiagnostic;
        out.evaluator_build=string_at(value,"evaluator_build");
        if (out.evaluator_build != TRADE_NGIN_GIT_SHA) fail();
        out.context_fingerprint=string_at(value,"context_fingerprint");
        if (out.context_fingerprint.size()!=64 ||
            out.context_fingerprint.find_first_not_of("0123456789abcdef")!=std::string::npos) fail();
        out.risk_config_source_id=string_at(value,"risk_config_source_id");
        out.context=context(value.at("context"));
        out.proposal=proposal(value.at("proposal"));
        out.risk_inputs=risk_inputs(value.at("risk_inputs"));
        if (out.risk_inputs.expected_portfolio_id!=out.context.portfolio_id ||
            out.risk_inputs.expected_date!=out.context.date ||
            out.risk_inputs.expected_portfolio_type!=out.context.portfolio_type ||
            out.risk_inputs.expected_revision!=out.context.revision) fail();
        out.risk_config=risk_config(value.at("risk_config"));
        out.quantity_rules=quantity_rules(value.at("quantity_rules"));
        out.component_cost_inputs=cost_inputs(value.at("component_cost_inputs"));
        if (out.operation==QtEvaluationOperation::DraftDiagnostic) {
            const auto& policy=value.at("optimizer_policy");
            out.optimizer_enabled=bool_at(policy,"enabled");
            out.optimizer_config_source_id=string_at(policy,"config_source_id");
            if (out.optimizer_enabled) {
                out.optimizer_inputs=optimizer_inputs(value.at("optimizer_inputs"));
                out.optimizer_config=optimizer_config(value.at("optimizer_config"));
                if (out.optimizer_inputs->expected_portfolio_id!=out.context.portfolio_id ||
                    out.optimizer_inputs->expected_date!=out.context.date ||
                    out.optimizer_inputs->expected_portfolio_type!=out.context.portfolio_type ||
                    out.optimizer_inputs->expected_revision!=out.context.revision) fail();
            }
        }
        if (out.input_mode==QtEvaluationInputMode::EmptyOwnerV2 &&
            (!out.context.slots.empty() || !out.proposal.quantities.empty() ||
             !out.risk_inputs.valuations.empty() || !out.risk_inputs.closes.empty() ||
             !out.risk_inputs.expected_observation_times.empty() || !out.quantity_rules.empty() ||
             !out.component_cost_inputs.empty() || out.optimizer_enabled ||
             out.optimizer_inputs || out.optimizer_config)) fail();
        auto overlay=overlay_component_book(out.context,out.proposal);
        if (overlay.is_error()) fail();
        return out;
    } catch (const std::exception& e) {
        return make_error<QtEvaluationRequest>(ErrorCode::INVALID_ARGUMENT,e.what(),"qt_eval_wire");
    }
}

Result<std::string> canonical_qt_eval_bytes(const nlohmann::json& value) {
    try {
        auto parsed=parse_qt_evaluation_request(value.dump());
        if (parsed.is_error())
            return make_error<std::string>(parsed.error()->code(),parsed.error()->what(),"qt_eval_wire");
        Json normalized=value;
        auto sort_keys=[](Json& array) {
            std::sort(array.begin(),array.end(),[](const Json& a,const Json& b) {
                return key(a.at("key")) < key(b.at("key"));
            });
        };
        sort_keys(normalized["context"]["slots"]);
        sort_keys(normalized["proposal"]["quantities"]);
        sort_keys(normalized["component_cost_inputs"]);
        auto bytes=normalized.dump(-1,' ',false,Json::error_handler_t::strict);
        if (bytes.size()>8*1024*1024) fail();
        return bytes;
    } catch (const std::exception& e) {
        return make_error<std::string>(ErrorCode::INVALID_ARGUMENT,e.what(),"qt_eval_wire");
    }
}

}  // namespace trade_ngin
