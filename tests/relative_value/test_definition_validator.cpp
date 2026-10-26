// tests/relative_value/test_definition_validator.cpp
//
// Phase 2 of #101: every rule of section 6.1 has a test that rejects a bad file, and the first-set
// definition files validate. A valid baseline definition is checked first, so a rejection below
// cannot come from some other fault in the fixture.

#include <gtest/gtest.h>

#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#include "trade_ngin/relative_value/analysis_registry.hpp"
#include "trade_ngin/relative_value/definition_validator.hpp"
#include "trade_ngin/relative_value/trade_definition.hpp"

using namespace trade_ngin::relative_value;
using nlohmann::json;

namespace {

const AnalysisRegistry& registry() {
    static const AnalysisRegistry r = AnalysisRegistry::with_builtins();
    return r;
}

// A registry of the roots the first set uses, with a contract size for each.
ValidationContext base_context() {
    ValidationContext ctx;
    ctx.registry = &registry();
    static const std::set<std::string> roots{"GC", "SI", "HG", "ZN", "ZT", "ZF", "MNQ", "MES",
                                             "CL", "RB", "HO", "ZS", "ZM", "ZL"};
    ctx.contract_lookup = [](const std::string& symbol) -> std::optional<ContractInfo> {
        const auto root = symbol.substr(0, symbol.find(".v."));
        if (!roots.count(root)) return std::nullopt;
        return ContractInfo{"100"};
    };
    ctx.series_exists = [](const std::string& table, const std::string& key) {
        return table == "macro_data.yield_curve" && (key == "treasury_2y" || key == "treasury_10y");
    };
    ctx.first_data_date = [](const TradeDefinition&) { return std::optional<std::string>("2019-01-02"); };
    return ctx;
}

// A valid definition: a gold/silver pair on a fixed package.
json baseline() {
    return json::parse(R"({
      "id": "t1",
      "description": "baseline",
      "legs": [{"symbol": "GC.v.0", "asset_class": "futures"}, {"symbol": "SI.v.0", "asset_class": "futures"}],
      "package": {"mode": "fixed", "contracts": {"GC.v.0": 5, "SI.v.0": -7}},
      "inputs": [{"name": "gc", "source": "leg", "symbol": "GC.v.0"},
                 {"name": "si", "source": "leg", "symbol": "SI.v.0"}],
      "series": {"rule": "ratio", "numerator": "gc", "denominator": "si", "log": true},
      "signals": [{"name": "z60", "type": "zscore", "window": 60},
                  {"name": "trend", "type": "ewmac", "fast": 16, "slow": 64}],
      "diagnostics": ["adf_250"],
      "trade": {"blend": {"z60": 0.5, "trend": 0.5}, "risk_target": 0.2, "capital": 500000},
      "history_start": "2020-01-02"
    })");
}

struct Checked {
    std::vector<ValidationIssue> parse_issues;
    std::vector<ValidationIssue> issues;
    std::string all() const {
        std::string out;
        for (const auto& i : parse_issues) out += "  parse " + i.str() + "\n";
        for (const auto& i : issues) out += "  " + i.str() + "\n";
        return out;
    }
};

Checked check(const json& body, ValidationContext ctx = base_context(),
              const std::vector<const TradeDefinition*>& others = {}) {
    Checked c;
    auto parsed = parse_trade_definition(body);
    c.parse_issues = parsed.issues;
    if (!parsed.definition) return c;
    ctx.others = others;
    c.issues = validate_trade_definition(*parsed.definition, ctx);
    return c;
}

// True when some issue's path contains `path` and its message contains `text`.
bool has(const Checked& c, const std::string& path, const std::string& text) {
    for (const auto* list : {&c.parse_issues, &c.issues}) {
        for (const auto& i : *list) {
            if (i.path.find(path) != std::string::npos && i.message.find(text) != std::string::npos) return true;
        }
    }
    return false;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Case {
    std::string name;
    std::function<void(json&)> mutate;
    std::string path;
    std::string text;
};

}  // namespace

TEST(DefinitionValidator, TheBaselineDefinitionIsValid) {
    const auto c = check(baseline());
    EXPECT_TRUE(c.parse_issues.empty()) << c.all();
    EXPECT_TRUE(c.issues.empty()) << c.all();
}

// Each case changes the valid baseline in one way and must be rejected with a message that names
// the place and the problem.
TEST(DefinitionValidator, EachRuleOfSection61RejectsItsBadFile) {
    const std::vector<Case> cases = {
        // ---- id
        {"id with a double dash", [](json& b) { b["id"] = "a--b"; }, "id", "no \"--\""},
        {"id with a space", [](json& b) { b["id"] = "has space"; }, "id", "letters, digits"},
        {"id of 51 characters", [](json& b) { b["id"] = std::string(51, 'a'); }, "id", "1 to 50"},
        // ---- legs
        {"leg not in the registry", [](json& b) { b["legs"][1]["symbol"] = "XX.v.0"; b["inputs"][1]["symbol"] = "XX.v.0";
                                                   b["package"]["contracts"] = {{"GC.v.0", 5}, {"XX.v.0", -7}}; },
         "legs[1].symbol", "not in the instrument registry"},
        {"mixed asset classes", [](json& b) { b["legs"][1]["asset_class"] = "equity"; }, "legs", "one asset class"},
        {"a leg listed twice", [](json& b) { b["legs"][1]["symbol"] = "GC.v.0"; }, "legs[1].symbol", "listed twice"},
        {"an unknown asset class", [](json& b) { b["legs"][0]["asset_class"] = "crypto"; b["legs"][1]["asset_class"] = "crypto"; },
         "asset_class", "futures"},
        // ---- inputs
        {"a duplicate input name", [](json& b) { b["inputs"][1]["name"] = "gc"; }, "inputs[1].name", "declared twice"},
        {"an input name with a dot", [](json& b) { b["inputs"][0]["name"] = "g.c"; }, "inputs[0].name", "letters, digits"},
        {"a leg input that is not a leg", [](json& b) { b["inputs"][0]["symbol"] = "HG.v.0"; }, "inputs[0].symbol", "not one of the legs"},
        {"a series input that does not exist", [](json& b) {
             b["inputs"].push_back({{"name", "y"}, {"source", "series"}, {"table", "macro_data.yield_curve"}, {"key", "nope"}}); },
         "inputs[2]", "does not exist"},
        {"a trade input that names no definition", [](json& b) {
             b["inputs"].push_back({{"name", "t"}, {"source", "trade"}, {"trade_id", "ghost"}}); },
         "inputs[2].trade_id", "no definition with id 'ghost'"},
        {"a trade input that names itself", [](json& b) {
             b["inputs"].push_back({{"name", "t"}, {"source", "trade"}, {"trade_id", "t1"}}); },
         "inputs[2].trade_id", "its own series"},
        {"a series that names an undeclared input", [](json& b) { b["series"]["numerator"] = "nope"; },
         "series.numerator", "not declared"},
        {"a signal that names an undeclared input", [](json& b) { b["signals"][0]["input"] = "nope"; },
         "signals[0].input", "not declared"},
        // ---- series rules
        {"an unknown series rule", [](json& b) { b["series"]["rule"] = "magic"; }, "series.rule", "unknown series rule"},
        {"ratio with identical numerator and denominator", [](json& b) { b["series"]["denominator"] = "gc"; },
         "series.denominator", "must differ"},
        {"ratio missing its denominator", [](json& b) { b["series"].erase("denominator"); }, "series.denominator", "is required"},
        {"an unknown series parameter", [](json& b) { b["series"]["colour"] = "red"; }, "series.colour", "unknown parameter"},
        {"estimated with no x", [](json& b) {
             b["series"] = {{"rule", "estimated"}, {"y", "gc"}, {"x", json::array()}, {"window", 60}}; },
         "series.x", "non-empty"},
        {"estimated with an undeclared x", [](json& b) {
             b["series"] = {{"rule", "estimated"}, {"y", "gc"}, {"x", {"zz"}}, {"window", 60}}; },
         "series.x[0]", "not declared"},
        {"estimated kalman with two x", [](json& b) {
             b["series"] = {{"rule", "estimated"}, {"y", "gc"}, {"x", {"si", "gc"}}, {"method", "kalman"}, {"window", 60}}; },
         "series.x", "exactly one"},
        {"estimated ols with kalman noise", [](json& b) {
             b["series"] = {{"rule", "estimated"}, {"y", "gc"}, {"x", {"si"}}, {"window", 60}, {"process_noise", 0.1}}; },
         "series.process_noise", "Kalman"},
        {"estimated with bad beta_bounds", [](json& b) {
             b["series"] = {{"rule", "estimated"}, {"y", "gc"}, {"x", {"si"}}, {"window", 60},
                            {"beta_bounds", {{"min", 3.0}, {"max", 1.0}}}}; },
         "series.beta_bounds", "min must be less than max"},
        {"estimated with an unknown method", [](json& b) {
             b["series"] = {{"rule", "estimated"}, {"y", "gc"}, {"x", {"si"}}, {"window", 60}, {"method", "ridge"}}; },
         "series.method", "must be one of"},
        {"estimated with an unknown transform", [](json& b) {
             b["series"] = {{"rule", "estimated"}, {"y", "gc"}, {"x", {"si"}}, {"window", 60}, {"transform", "sqrt"}}; },
         "series.transform", "must be one of"},
        {"a zero window", [](json& b) {
             b["series"] = {{"rule", "estimated"}, {"y", "gc"}, {"x", {"si"}}, {"window", 0}}; },
         "series.window", "at least 2"},
        {"forward with near not before far", [](json& b) {
             b["series"] = {{"rule", "forward"}, {"near", {{"input", "gc"}, {"years", 5}}}, {"far", {{"input", "si"}, {"years", 3}}}}; },
         "series", "0 < near.years < far.years"},
        {"forward with a missing maturity", [](json& b) {
             b["series"] = {{"rule", "forward"}, {"near", {{"input", "gc"}}}, {"far", {{"input", "si"}, {"years", 3}}}}; },
         "series.near", "years"},
        {"forward with an undeclared input", [](json& b) {
             b["series"] = {{"rule", "forward"}, {"near", {{"input", "zz"}, {"years", 3}}}, {"far", {{"input", "si"}, {"years", 5}}}}; },
         "series.near.input", "not declared"},
        {"fixed with no terms", [](json& b) { b["series"] = {{"rule", "fixed"}, {"terms", json::array()}}; }, "series.terms", "non-empty"},
        {"fixed with a zero coefficient", [](json& b) {
             b["series"] = {{"rule", "fixed"}, {"terms", {{{"input", "gc"}, {"coef", 0}}}}}; },
         "series.terms[0].coef", "zero"},
        {"fixed with an undeclared input", [](json& b) {
             b["series"] = {{"rule", "fixed"}, {"terms", {{{"input", "zz"}, {"coef", 1}}}}}; },
         "series.terms[0].input", "not declared"},
        // ---- neutral
        {"neutral with risk pca", [](json& b) {
             b["series"] = {{"rule", "neutral"}, {"risk", "pca"}, {"shape", {1, -1}}}; },
         "series.risk", "must be one of: duration, beta, notional"},
        {"neutral with an unknown fallback", [](json& b) {
             b["series"] = {{"rule", "neutral"}, {"risk", "notional"}, {"shape", {1, -1}}, {"fallback", "mean"}}; },
         "series.fallback", "must be one of"},
        {"neutral shape with the wrong number of entries", [](json& b) {
             b["series"] = {{"rule", "neutral"}, {"risk", "notional"}, {"shape", {1, -2, 1}}}; },
         "series.shape", "one entry per leg"},
        {"neutral shape with a zero", [](json& b) {
             b["series"] = {{"rule", "neutral"}, {"risk", "notional"}, {"shape", {1, 0}}}; },
         "series.shape", "non-zero"},
        {"neutral beta without a benchmark", [](json& b) {
             b["series"] = {{"rule", "neutral"}, {"risk", "beta"}, {"shape", {1, -1}}}; },
         "series.risk_inputs", "benchmark"},
        {"neutral beta with an undeclared benchmark", [](json& b) {
             b["series"] = {{"rule", "neutral"}, {"risk", "beta"}, {"shape", {1, -1}}, {"risk_inputs", {{"benchmark", "zz"}}}}; },
         "series.risk_inputs.benchmark", "not declared"},
        {"neutral duration with neither DV01 inputs nor a fallback", [](json& b) {
             b["series"] = {{"rule", "neutral"}, {"risk", "duration"}, {"shape", {1, -1}}}; },
         "series", "DV01"},
        {"neutral duration with a risk input that is not a leg", [](json& b) {
             b["series"] = {{"rule", "neutral"}, {"risk", "duration"}, {"shape", {1, -1}},
                            {"risk_inputs", {{"HG.v.0", "gc"}}}}; },
         "series.risk_inputs.HG.v.0", "leg symbol"},
        // ---- signals
        {"an unknown signal type", [](json& b) { b["signals"][0]["type"] = "crystal_ball"; }, "signals[0].type", "unknown signal type"},
        {"a signal missing a required parameter", [](json& b) { b["signals"][0].erase("window"); }, "signals[0].window", "is required"},
        {"a non-integer window", [](json& b) { b["signals"][0]["window"] = 60.5; }, "signals[0].window", "integer"},
        {"a window of one", [](json& b) { b["signals"][0]["window"] = 1; }, "signals[0].window", "at least 2"},
        {"a bad direction", [](json& b) { b["signals"][0]["direction"] = "sideways"; }, "signals[0].direction", "revert, momentum"},
        {"z_at_10 of zero", [](json& b) { b["signals"][0]["z_at_10"] = 0; }, "signals[0].z_at_10", "greater than 0"},
        {"an unknown signal parameter", [](json& b) { b["signals"][0]["speed"] = 3; }, "signals[0].speed", "unknown parameter"},
        {"ewmac with fast not below slow", [](json& b) { b["signals"][1]["fast"] = 64; }, "signals[1]", "fast < slow"},
        {"a sign other than 1 or -1", [](json& b) { b["signals"][0]["sign"] = 2; }, "signals[0].sign", "1 or -1"},
        {"a signal name with a dot", [](json& b) { b["signals"][0]["name"] = "z.60"; b["trade"]["blend"] = {{"z.60", 0.5}, {"trend", 0.5}}; },
         "signals[0].name", "letters, digits"},
        {"a signal name of 31 characters", [](json& b) { b["signals"][0]["name"] = std::string(31, 'z');
                                                          b["trade"]["blend"] = {{std::string(31, 'z'), 0.5}, {"trend", 0.5}}; },
         "signals[0].name", "1 to 30"},
        {"a duplicate signal name", [](json& b) { b["signals"][1]["name"] = "z60"; b["trade"]["blend"] = {{"z60", 1.0}}; },
         "signals[1].name", "used twice"},
        {"value_vs_anchor with both anchors", [](json& b) {
             b["signals"][0] = {{"name", "v"}, {"type", "value_vs_anchor"}, {"scale", 1.0},
                                {"anchor", {{"constant", 1.0}, {"input", "gc"}}}};
             b["trade"]["blend"] = {{"v", 0.5}, {"trend", 0.5}}; },
         "signals[0].anchor", "constant"},
        {"value_vs_anchor with an undeclared anchor input", [](json& b) {
             b["signals"][0] = {{"name", "v"}, {"type", "value_vs_anchor"}, {"scale", 1.0}, {"anchor", {{"input", "zz"}}}};
             b["trade"]["blend"] = {{"v", 0.5}, {"trend", 0.5}}; },
         "signals[0].anchor.input", "not declared"},
        {"value_vs_anchor with a zero scale", [](json& b) {
             b["signals"][0] = {{"name", "v"}, {"type", "value_vs_anchor"}, {"scale", 0}, {"anchor", {{"constant", 1.0}}}};
             b["trade"]["blend"] = {{"v", 0.5}, {"trend", 0.5}}; },
         "signals[0].scale", "greater than 0"},
        {"a non-price signal on an undeclared input", [](json& b) {
             b["signals"][0] = {{"name", "f"}, {"type", "forward_gap"}, {"near_forward", "gc"}, {"far_forward", "zz"}, {"window", 60}};
             b["trade"]["blend"] = {{"f", 0.5}, {"trend", 0.5}}; },
         "signals[0].far_forward", "not declared"},
        // ---- info_ratio_signal
        {"info_ratio_signal whose base has a name", [](json& b) {
             b["signals"][0] = {{"name", "i"}, {"type", "info_ratio_signal"}, {"leg", "GC.v.0"},
                                {"base", {{"type", "momentum"}, {"name", "x"}, {"n", 63}, {"window", 252}}}};
             b["trade"]["blend"] = {{"i", 0.5}, {"trend", 0.5}}; },
         "signals[0].base.name", "no name"},
        {"info_ratio_signal whose base has a sign", [](json& b) {
             b["signals"][0] = {{"name", "i"}, {"type", "info_ratio_signal"}, {"leg", "GC.v.0"},
                                {"base", {{"type", "momentum"}, {"sign", -1}, {"n", 63}, {"window", 252}}}};
             b["trade"]["blend"] = {{"i", 0.5}, {"trend", 0.5}}; },
         "signals[0].base.sign", "outer sign"},
        {"info_ratio_signal whose base is itself", [](json& b) {
             b["signals"][0] = {{"name", "i"}, {"type", "info_ratio_signal"}, {"leg", "GC.v.0"},
                                {"base", {{"type", "info_ratio_signal"}}}};
             b["trade"]["blend"] = {{"i", 0.5}, {"trend", 0.5}}; },
         "signals[0].base.type", "cannot itself"},
        {"info_ratio_signal whose base has bad parameters", [](json& b) {
             b["signals"][0] = {{"name", "i"}, {"type", "info_ratio_signal"}, {"leg", "GC.v.0"},
                                {"base", {{"type", "momentum"}, {"n", 0}, {"window", 252}}}};
             b["trade"]["blend"] = {{"i", 0.5}, {"trend", 0.5}}; },
         "signals[0].base.n", "at least 1"},
        {"info_ratio_signal on something that is not a leg", [](json& b) {
             b["signals"][0] = {{"name", "i"}, {"type", "info_ratio_signal"}, {"leg", "ZN.v.0"},
                                {"base", {{"type", "momentum"}, {"n", 63}, {"window", 252}}}};
             b["trade"]["blend"] = {{"i", 0.5}, {"trend", 0.5}}; },
         "signals[0].leg", "not one of the legs"},
        // ---- forecast blocks
        {"an unknown forecast method", [](json& b) { b["signals"][0]["forecast"] = {{"method", "psychic"}}; },
         "signals[0].forecast.method", "unknown forecast method"},
        {"a scalar forecast with a bad window", [](json& b) { b["signals"][1]["forecast"] = {{"method", "scalar"}, {"window", "forever"}}; },
         "signals[1].forecast.window", "expanding"},
        {"a fixed_scalar forecast without its value", [](json& b) { b["signals"][1]["forecast"] = {{"method", "fixed_scalar"}}; },
         "signals[1].forecast.value", "is required"},
        {"a linear forecast without k", [](json& b) { b["signals"][1]["forecast"] = {{"method", "linear"}}; },
         "signals[1].forecast.k", "is required"},
        // ---- diagnostics
        {"a diagnostic without a window", [](json& b) { b["diagnostics"] = {"adf"}; }, "diagnostics[0]", "<name>_<window>"},
        {"a diagnostic with a zero window", [](json& b) { b["diagnostics"] = {"adf_0"}; }, "diagnostics[0]", "positive integer"},
        {"a diagnostic with a text window", [](json& b) { b["diagnostics"] = {"adf_long"}; }, "diagnostics[0]", "<name>_<window>"},
        {"an unknown diagnostic", [](json& b) { b["diagnostics"] = {"magic_10"}; }, "diagnostics[0]", "unknown diagnostic"},
        {"a diagnostic that reuses a signal name", [](json& b) { b["signals"][0]["name"] = "adf_250";
                                                                  b["trade"]["blend"] = {{"adf_250", 0.5}, {"trend", 0.5}}; },
         "diagnostics[0]", "also used"},
        {"a diagnostic listed twice", [](json& b) { b["diagnostics"] = {"adf_250", "adf_250"}; }, "diagnostics[1]", "listed twice"},
        // ---- key width
        {"a key longer than the signals column", [](json& b) {
             b["inputs"][0]["name"] = std::string(40, 'g');
             b["series"] = {{"rule", "fixed"}, {"terms", {{{"input", std::string(40, 'g')}, {"coef", 1}}}}}; },
         "", "analysis.signals holds 50"},
        // ---- legs, package, trade go together
        {"legs without a package", [](json& b) { b.erase("package"); }, "package", "needs a package"},
        {"legs without a trade block", [](json& b) { b.erase("trade"); }, "trade", "needs a trade block"},
        {"an analysis-only definition with a package", [](json& b) { b["legs"] = json::array(); b["inputs"] = {
             {{"name", "gc"}, {"source", "instrument"}, {"symbol", "GC.v.0"}, {"asset_class", "futures"}},
             {{"name", "si"}, {"source", "instrument"}, {"symbol", "SI.v.0"}, {"asset_class", "futures"}}}; b.erase("trade"); },
         "package", "analysis-only"},
        {"an analysis-only definition with a trade block", [](json& b) { b["legs"] = json::array(); b["inputs"] = {
             {{"name", "gc"}, {"source", "instrument"}, {"symbol", "GC.v.0"}, {"asset_class", "futures"}},
             {{"name", "si"}, {"source", "instrument"}, {"symbol", "SI.v.0"}, {"asset_class", "futures"}}}; b.erase("package"); },
         "trade", "analysis-only"},
        // ---- package
        {"a fixed package with a zero count", [](json& b) { b["package"]["contracts"]["GC.v.0"] = 0; },
         "package.contracts.GC.v.0", "non-zero"},
        {"a fixed package with a fractional count", [](json& b) { b["package"]["contracts"]["GC.v.0"] = 1.5; },
         "package.contracts.GC.v.0", "integer"},
        {"a fixed package missing a leg", [](json& b) { b["package"]["contracts"].erase("SI.v.0"); },
         "package.contracts", "exactly the leg symbols"},
        {"a fixed package with an extra symbol", [](json& b) { b["package"]["contracts"]["HG.v.0"] = 1; },
         "package.contracts", "exactly the leg symbols"},
        {"a derived package with a zero scale", [](json& b) { b["package"] = {{"mode", "derived"}, {"scale", 0},
                                                                              {"weights", {{"risk", "notional"}, {"shape", {1, -1}}}}}; },
         "package.scale", "positive"},
        {"a derived package on a rule with no weights and no weights block", [](json& b) { b["package"] = {{"mode", "derived"}, {"scale", 5}}; },
         "package", "needs weights"},
        {"a derived package whose weights block has the wrong shape length", [](json& b) {
             b["package"] = {{"mode", "derived"}, {"scale", 5}, {"weights", {{"risk", "notional"}, {"shape", {1, -1, 1}}}}}; },
         "package.weights.shape", "one entry per leg"},
        {"a derived package with a negative rebalance threshold", [](json& b) {
             b["package"] = {{"mode", "derived"}, {"scale", 5}, {"rebalance_threshold", -0.1},
                             {"weights", {{"risk", "notional"}, {"shape", {1, -1}}}}}; },
         "package.rebalance_threshold", "positive"},
        // ---- trade
        {"a zero risk target", [](json& b) { b["trade"]["risk_target"] = 0; }, "trade.risk_target", "positive"},
        {"a negative capital", [](json& b) { b["trade"]["capital"] = -1; }, "trade.capital", "positive"},
        {"a blend that names no signal", [](json& b) { b["trade"]["blend"] = {{"z60", 0.5}, {"ghost", 0.5}}; },
         "trade.blend.ghost", "not a signal"},
        {"a blend whose weights do not sum to 1", [](json& b) { b["trade"]["blend"] = {{"z60", 0.5}, {"trend", 0.4}}; },
         "trade.blend", "sum to 1"},
        {"a blend that is out by 1e-6", [](json& b) { b["trade"]["blend"] = {{"z60", 0.5}, {"trend", 0.5 + 1e-6}}; },
         "trade.blend", "sum to 1"},
        {"a blend that is out by 1e-7 below 1", [](json& b) { b["trade"]["blend"] = {{"z60", 0.5}, {"trend", 0.5 - 1e-7}}; },
         "trade.blend", "sum to 1"},
        {"a blend with a negative weight", [](json& b) { b["trade"]["blend"] = {{"z60", 1.5}, {"trend", -0.5}}; },
         "trade.blend.trend", "non-negative"},
        {"a blend on a signal with no forecast mapping", [](json& b) { b["signals"][0]["forecast"] = {{"method", "none"}}; },
         "trade.blend.z60", "mapping"},
        // ---- history_start
        {"a history_start that is not a date", [](json& b) { b["history_start"] = "soon"; }, "history_start", "valid date"},
        {"a history_start on a day that does not exist", [](json& b) { b["history_start"] = "2021-02-30"; }, "history_start", "valid date"},
        {"a history_start before there is data", [](json& b) { b["history_start"] = "2015-01-02"; }, "history_start", "before the first date"},
    };

    int rejected = 0;
    for (const auto& c : cases) {
        json body = baseline();
        c.mutate(body);
        const auto result = check(body);
        EXPECT_TRUE(has(result, c.path, c.text))
            << "case: " << c.name << "\nexpected an issue at '" << c.path << "' containing '" << c.text
            << "'; got:\n" << result.all();
        ++rejected;
    }
    EXPECT_GE(rejected, 90);
}

TEST(DefinitionValidator, AcceptedVariationsAreNotRejected) {
    // The other side of the rules above: values on the right side of each boundary pass.
    struct Ok { std::string name; std::function<void(json&)> mutate; };
    const std::vector<Ok> ok = {
        {"an id of exactly 50 characters", [](json& b) { b["id"] = std::string(50, 'a'); }},
        {"an id with single dashes and underscores", [](json& b) { b["id"] = "a-b_c-1"; }},
        {"a signal name of exactly 30 characters", [](json& b) {
             b["signals"][0]["name"] = std::string(30, 'z');
             b["trade"]["blend"] = {{std::string(30, 'z'), 0.5}, {"trend", 0.5}}; }},
        {"a blend that sums to 1 within 1e-9", [](json& b) { b["trade"]["blend"] = {{"z60", 0.1}, {"trend", 0.9 + 1e-12}}; }},
        {"a history_start equal to the first data date", [](json& b) { b["history_start"] = "2019-01-02"; }},
        {"a history_start on a leap day", [](json& b) { b["history_start"] = "2020-02-29"; }},
        {"no history_start at all", [](json& b) { b.erase("history_start"); }},
        {"a window of exactly 2", [](json& b) { b["signals"][0]["window"] = 2; }},
        {"a derived package with its own weights block", [](json& b) {
             b["package"] = {{"mode", "derived"}, {"scale", 5}, {"weights", {{"risk", "notional"}, {"shape", {1, -1}}}}}; }},
        {"an ewmac with a custom forecast scalar window", [](json& b) {
             b["signals"][1]["forecast"] = {{"method", "scalar"}, {"window", 500}, {"min_history", 250}}; }},
        {"a signal with a sign of -1", [](json& b) { b["signals"][0]["sign"] = -1; }},
        {"a signal with forecast none that is not in the blend", [](json& b) {
             b["signals"][0]["forecast"] = {{"method", "none"}}; b["trade"]["blend"] = {{"trend", 1.0}}; }},
        {"a null strategy", [](json& b) { b["strategy"] = nullptr; }},
    };
    for (const auto& c : ok) {
        json body = baseline();
        c.mutate(body);
        const auto result = check(body);
        EXPECT_TRUE(result.parse_issues.empty() && result.issues.empty()) << "case: " << c.name << "\n" << result.all();
    }
}

TEST(DefinitionValidator, WithoutLookupsTheDatabaseChecksAreSkipped) {
    // A caller with no database supplies no lookups; the checks that need one are skipped, the
    // structural ones still run.
    ValidationContext ctx;
    ctx.registry = &registry();
    json body = baseline();
    body["legs"][0]["symbol"] = "NOPE.v.0";
    body["inputs"][0]["symbol"] = "NOPE.v.0";
    body["package"]["contracts"] = {{"NOPE.v.0", 5}, {"SI.v.0", -7}};
    body["history_start"] = "1999-01-01";
    EXPECT_TRUE(check(body, ctx).issues.empty()) << check(body, ctx).all();
    body["trade"]["capital"] = -1;
    EXPECT_FALSE(check(body, ctx).issues.empty());
}

TEST(DefinitionValidator, AMetadataRowWithNoContractSizeIsRejectedNotSilentlyTakenAsOne) {
    auto ctx = base_context();
    ctx.contract_lookup = [](const std::string& symbol) -> std::optional<ContractInfo> {
        if (symbol == "SI.v.0") return ContractInfo{"  "};  // a row whose size is blank
        return ContractInfo{"100"};
    };
    const auto c = check(baseline(), ctx);
    EXPECT_TRUE(has(c, "legs[1].symbol", "no Contract Size")) << c.all();
}

TEST(DefinitionValidator, DuplicateIdsAcrossASetAreRejected) {
    auto a = parse_trade_definition(baseline());
    auto b = parse_trade_definition(baseline());
    ASSERT_TRUE(a.definition && b.definition);
    const auto issues = validate_definition_set({*a.definition, *b.definition}, base_context());
    bool found = false;
    for (const auto& i : issues) found = found || i.message.find("already used") != std::string::npos;
    EXPECT_TRUE(found);
}

TEST(DefinitionValidator, TradeInputsMustFormNoCycle) {
    auto make = [](const std::string& id, const std::string& uses) {
        json b = json::parse(R"({"id": "x", "legs": [], "inputs": [], "series": {"rule": "fixed", "terms": [{"input": "in", "coef": 1}]}})");
        b["id"] = id;
        b["inputs"] = {{{"name", "in"}, {"source", "trade"}, {"trade_id", uses}}};
        auto p = parse_trade_definition(b);
        EXPECT_TRUE(p.definition) << id;
        return *p.definition;
    };
    // a -> b -> c -> a
    const auto issues = validate_definition_set({make("a", "b"), make("b", "c"), make("c", "a")}, base_context());
    bool cycle = false;
    for (const auto& i : issues) cycle = cycle || i.message.find("cycle") != std::string::npos;
    EXPECT_TRUE(cycle);
    // a chain with no cycle is fine: a -> b -> c
    json leaf = json::parse(R"({"id": "c", "legs": [], "inputs": [{"name": "z", "source": "series", "table": "macro_data.yield_curve", "key": "treasury_2y"}],
                               "series": {"rule": "fixed", "terms": [{"input": "z", "coef": 1}]}})");
    auto leaf_def = parse_trade_definition(leaf);
    ASSERT_TRUE(leaf_def.definition);
    const auto chain = validate_definition_set({make("a", "b"), make("b", "c"), *leaf_def.definition}, base_context());
    for (const auto& i : chain) EXPECT_EQ(i.message.find("cycle"), std::string::npos) << i.str();
    EXPECT_TRUE(chain.empty()) << chain.size() << " issue(s), first: " << (chain.empty() ? "" : chain[0].str());
}

TEST(DefinitionValidator, TheFirstSetFilesAndTheFlyValidateAsASet) {
    const std::string dir = std::string(TRADE_NGIN_SOURCE_DIR) + "/config_template/trades/";
    const std::vector<std::string> files = {"crack_321", "gold_silver", "copper_gold_zn", "yield_spread_10y_2y",
                                            "curve_2s10s", "nasdaq_vs_sp", "fly_zt_zf_zn"};
    std::vector<TradeDefinition> defs;
    for (const auto& f : files) {
        const std::string text = read_file(dir + f + ".json");
        ASSERT_FALSE(text.empty()) << "missing " << dir << f << ".json";
        auto parsed = parse_trade_definition_text(text);
        ASSERT_TRUE(parsed.definition) << f << ": " << (parsed.issues.empty() ? "" : parsed.issues[0].str());
        EXPECT_EQ(parsed.definition->id, f) << "the file name is the definition id";
        defs.push_back(*parsed.definition);
    }
    auto ctx = base_context();
    ctx.first_data_date = [](const TradeDefinition&) { return std::optional<std::string>("2010-01-01"); };
    const auto issues = validate_definition_set(defs, ctx);
    for (const auto& i : issues) ADD_FAILURE() << i.str();
    EXPECT_TRUE(issues.empty());

    // The yield spread is analysis only; the 2s10s trade reads it as a trade input.
    EXPECT_TRUE(defs[3].analysis_only());
    EXPECT_FALSE(defs[0].analysis_only());
}

TEST(DefinitionValidator, TheSoyCrushExampleFromTheIssueValidates) {
    const std::string text = read_file(std::string(TRADE_NGIN_SOURCE_DIR) + "/tests/relative_value/fixtures/soy_board_crush.json");
    ASSERT_FALSE(text.empty());
    auto parsed = parse_trade_definition_text(text);
    ASSERT_TRUE(parsed.definition);
    auto ctx = base_context();
    ctx.first_data_date = [](const TradeDefinition&) { return std::optional<std::string>("2010-01-01"); };
    const auto issues = validate_trade_definition(*parsed.definition, ctx);
    for (const auto& i : issues) ADD_FAILURE() << i.str();
}

TEST(DefinitionValidator, AnAnalysisAddedToTheRegistryIsPickedUpWithNoOtherChange) {
    AnalysisRegistry reg = AnalysisRegistry::with_builtins();
    AnalysisDescriptor d;
    d.name = "dummy";
    d.kind = AnalysisKind::Signal;
    d.params.params = {ParamSpec{"depth", ParamType::Int, true, nullptr, 1.0, false, std::nullopt, {}, ""}};
    d.default_forecast = "linear";
    d.outputs = {"extra"};
    ASSERT_TRUE(reg.add(d).is_ok());
    EXPECT_TRUE(reg.add(d).is_error()) << "registering the same name twice is refused";

    auto ctx = base_context();
    ctx.registry = &reg;
    json body = baseline();
    body["signals"].push_back({{"name", "dm"}, {"type", "dummy"}, {"depth", 3}});
    auto good = check(body, ctx);
    EXPECT_TRUE(good.issues.empty()) << good.all();

    // its parameter schema is enforced
    body["signals"][2]["depth"] = 0;
    EXPECT_TRUE(has(check(body, ctx), "signals[2].depth", "at least 1"));
    body["signals"][2].erase("depth");
    EXPECT_TRUE(has(check(body, ctx), "signals[2].depth", "is required"));

    // its keys are counted
    json named = baseline();
    named["signals"].push_back({{"name", "dm"}, {"type", "dummy"}, {"depth", 3}});
    auto p = parse_trade_definition(named);
    ASSERT_TRUE(p.definition);
    const auto keys = signal_keys(*p.definition, reg);
    EXPECT_NE(std::find(keys.begin(), keys.end(), "dm.extra"), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "dm.forecast"), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "dm.k"), keys.end());
}

TEST(DefinitionValidator, TheKeysADefinitionWritesFollowSection92) {
    auto p = parse_trade_definition(baseline());
    ASSERT_TRUE(p.definition);
    const auto keys = signal_keys(*p.definition, registry());
    auto has_key = [&](const std::string& k) { return std::find(keys.begin(), keys.end(), k) != keys.end(); };
    for (const char* k : {"series", "series.flag.roll_day", "z60", "z60.forecast", "z60.k",
                          "trend", "trend.forecast", "trend.scalar", "adf_250", "adf_250.pvalue",
                          "forecast.blended", "GC.v.0.recommended", "GC.v.0.held"}) {
        EXPECT_TRUE(has_key(k)) << "missing key " << k;
    }
    EXPECT_FALSE(has_key("z60.scalar")) << "a linear forecast stores k, not a scalar";
    EXPECT_FALSE(has_key("series.weight.GC.v.0")) << "a ratio series has no weights";

    // a fixed series has one weight per input; a neutral series one per leg
    auto fixed = parse_trade_definition_text(R"({"id":"f","legs":[],"inputs":[{"name":"a","source":"series","table":"t","key":"k"}],
                                                 "series":{"rule":"fixed","terms":[{"input":"a","coef":2}]}})");
    ASSERT_TRUE(fixed.definition);
    const auto fk = signal_keys(*fixed.definition, registry());
    EXPECT_NE(std::find(fk.begin(), fk.end(), "series.weight.a"), fk.end());
    auto neutral = parse_trade_definition_text(R"({"id":"n","legs":[{"symbol":"ZT.v.0","asset_class":"futures"},{"symbol":"ZN.v.0","asset_class":"futures"}],
                                                   "package":{"mode":"derived","scale":10},"inputs":[],
                                                   "series":{"rule":"neutral","risk":"duration","shape":[1,-1],"fallback":"vol"},
                                                   "trade":{"blend":{},"risk_target":0.2,"capital":1}})");
    ASSERT_TRUE(neutral.definition);
    const auto nk = signal_keys(*neutral.definition, registry());
    EXPECT_NE(std::find(nk.begin(), nk.end(), "series.weight.ZT.v.0"), nk.end());
    EXPECT_NE(std::find(nk.begin(), nk.end(), "series.flag.vol_fallback"), nk.end());
}
