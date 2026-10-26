// tests/relative_value/test_trade_definition.cpp
//
// The parser (structure only), the parameter schema, the registry and the contract lookup.

#include <gtest/gtest.h>

#include <arrow/api.h>

#include "trade_ngin/relative_value/analysis_registry.hpp"
#include "trade_ngin/relative_value/contract_lookup.hpp"
#include "trade_ngin/relative_value/param_schema.hpp"
#include "trade_ngin/relative_value/trade_definition.hpp"

using namespace trade_ngin::relative_value;
using nlohmann::json;

namespace {

json minimal() {
    return json::parse(R"({
      "id": "m1",
      "legs": [],
      "inputs": [{"name": "a", "source": "series", "table": "t", "key": "k"}],
      "series": {"rule": "fixed", "terms": [{"input": "a", "coef": 1}]}
    })");
}

bool mentions(const ParseOutcome& o, const std::string& path, const std::string& text) {
    for (const auto& i : o.issues) {
        if (i.path.find(path) != std::string::npos && i.message.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ---- parser ----------------------------------------------------------------------------------

TEST(TradeDefinitionParser, AMinimalAnalysisOnlyDefinitionParses) {
    const auto o = parse_trade_definition(minimal());
    ASSERT_TRUE(o.definition) << (o.issues.empty() ? "" : o.issues[0].str());
    EXPECT_TRUE(o.definition->analysis_only());
    EXPECT_EQ(o.definition->id, "m1");
    EXPECT_EQ(o.definition->series.rule, "fixed");
    EXPECT_TRUE(o.definition->series.params.contains("terms"));
    EXPECT_FALSE(o.definition->series.params.contains("rule"));
}

TEST(TradeDefinitionParser, StructuralMistakesAreReportedTogether) {
    json b = minimal();
    b["colour"] = "red";
    b["legs"] = "none";
    b["inputs"][0]["source"] = "magic";
    b["series"].erase("rule");
    const auto o = parse_trade_definition(b);
    EXPECT_FALSE(o.definition);
    EXPECT_TRUE(mentions(o, "colour", "unknown key"));
    EXPECT_TRUE(mentions(o, "legs", "must be a list"));
    EXPECT_TRUE(mentions(o, "inputs[0].source", "one of: leg, instrument, series, trade"));
    EXPECT_TRUE(mentions(o, "series.rule", "is required"));
}

TEST(TradeDefinitionParser, EachRequiredPieceIsRequired) {
    for (const char* key : {"id", "legs", "inputs", "series"}) {
        json b = minimal();
        b.erase(key);
        const auto o = parse_trade_definition(b);
        EXPECT_FALSE(o.definition) << key;
        EXPECT_TRUE(mentions(o, key, "is required")) << key;
    }
}

TEST(TradeDefinitionParser, InputsCarryOnlyTheKeysOfTheirSource) {
    json b = minimal();
    b["inputs"] = {{{"name", "a"}, {"source", "leg"}, {"symbol", "GC.v.0"}, {"table", "oops"}}};
    EXPECT_TRUE(mentions(parse_trade_definition(b), "inputs[0].table", "unknown key"));
    b["inputs"] = {{{"name", "a"}, {"source", "instrument"}, {"symbol", "GC.v.0"}}};
    EXPECT_TRUE(mentions(parse_trade_definition(b), "inputs[0].asset_class", "is required"));
    b["inputs"] = {{{"name", "a"}, {"source", "trade"}}};
    EXPECT_TRUE(mentions(parse_trade_definition(b), "inputs[0].trade_id", "is required"));
}

TEST(TradeDefinitionParser, PackagesAndTradeBlocksAreChecked) {
    json b = minimal();
    b["package"] = {{"mode", "sideways"}};
    EXPECT_TRUE(mentions(parse_trade_definition(b), "package.mode", "fixed"));
    b["package"] = {{"mode", "fixed"}};
    EXPECT_TRUE(mentions(parse_trade_definition(b), "package.contracts", "is required"));
    b["package"] = {{"mode", "derived"}};
    EXPECT_TRUE(mentions(parse_trade_definition(b), "package.scale", "is required"));
    b.erase("package");
    b["trade"] = {{"risk_target", 0.2}};
    const auto o = parse_trade_definition(b);
    EXPECT_TRUE(mentions(o, "trade.blend", "is required"));
    EXPECT_TRUE(mentions(o, "trade.capital", "is required"));
}

TEST(TradeDefinitionParser, SignalsKeepTheirParametersApartFromTheCommonKeys) {
    json b = minimal();
    b["signals"] = {{{"name", "s"}, {"type", "zscore"}, {"window", 60}, {"sign", -1}, {"input", "a"},
                     {"forecast", {{"method", "linear"}, {"k", 2.0}}}}};
    const auto o = parse_trade_definition(b);
    ASSERT_TRUE(o.definition);
    const auto& s = o.definition->signals[0];
    EXPECT_EQ(s.sign, -1);
    EXPECT_EQ(*s.input, "a");
    EXPECT_EQ(s.forecast->method, "linear");
    EXPECT_DOUBLE_EQ(s.forecast->params["k"].get<double>(), 2.0);
    EXPECT_EQ(s.params, json::parse(R"({"window": 60})"));
}

TEST(TradeDefinitionParser, TextThatIsNotJsonIsReportedNotThrown) {
    const auto o = parse_trade_definition_text("{not json");
    EXPECT_FALSE(o.definition);
    ASSERT_FALSE(o.issues.empty());
    EXPECT_NE(o.issues[0].message.find("not valid JSON"), std::string::npos);
    EXPECT_TRUE(parse_trade_definition(json::array()).issues[0].message.find("JSON object") != std::string::npos);
}

TEST(TradeDefinitionParser, TheCanonicalTextIsTheSameForTheSameBodyInAnyKeyOrder) {
    const auto a = parse_trade_definition_text(R"({"id":"x","legs":[],"inputs":[],"series":{"rule":"fixed","terms":[]}})");
    const auto b = parse_trade_definition_text(R"({"series":{"terms":[],"rule":"fixed"},"inputs":[],"legs":[],"id":"x"})");
    ASSERT_TRUE(a.definition && b.definition);
    EXPECT_EQ(canonical_text(*a.definition), canonical_text(*b.definition));
}

// ---- parameter schema -----------------------------------------------------------------------

TEST(ParamSchema, RequiredTypesBoundsEnumsAndUnknownKeys) {
    ParamSchema s;
    s.params = {ParamSpec{"n", ParamType::Int, true, nullptr, 2.0, false, 10.0, {}, ""},
                ParamSpec{"x", ParamType::Number, false, 1.5, 0.0, true, std::nullopt, {}, ""},
                ParamSpec{"mode", ParamType::Enum, false, "a", std::nullopt, false, std::nullopt, {"a", "b"}, ""},
                ParamSpec{"flag", ParamType::Bool, false, nullptr, std::nullopt, false, std::nullopt, {}, ""},
                ParamSpec{"ref", ParamType::InputRef, false, nullptr, std::nullopt, false, std::nullopt, {}, ""}};

    EXPECT_TRUE(s.check(json{{"n", 5}}, "p").empty());
    EXPECT_FALSE(s.check(json::object(), "p").empty()) << "n is required";
    EXPECT_FALSE(s.check(json{{"n", 1}}, "p").empty()) << "below the minimum";
    EXPECT_TRUE(s.check(json{{"n", 2}}, "p").empty()) << "the minimum is inclusive";
    EXPECT_TRUE(s.check(json{{"n", 10}}, "p").empty()) << "the maximum is inclusive";
    EXPECT_FALSE(s.check(json{{"n", 11}}, "p").empty());
    EXPECT_FALSE(s.check(json{{"n", 5.5}}, "p").empty()) << "an integer is required";
    EXPECT_FALSE(s.check(json{{"n", true}}, "p").empty()) << "a bool is not an integer";
    EXPECT_FALSE(s.check(json{{"n", 5}, {"x", 0.0}}, "p").empty()) << "x must be above 0";
    EXPECT_TRUE(s.check(json{{"n", 5}, {"x", 1e-9}}, "p").empty());
    EXPECT_FALSE(s.check(json{{"n", 5}, {"mode", "c"}}, "p").empty());
    EXPECT_FALSE(s.check(json{{"n", 5}, {"mode", 3}}, "p").empty());
    EXPECT_FALSE(s.check(json{{"n", 5}, {"flag", "yes"}}, "p").empty());
    EXPECT_FALSE(s.check(json{{"n", 5}, {"ref", ""}}, "p").empty());
    EXPECT_FALSE(s.check(json{{"n", 5}, {"extra", 1}}, "p").empty()) << "unknown key";
    EXPECT_FALSE(s.check(json::array(), "p").empty()) << "not an object";

    const auto issues = s.check(json{{"n", 11}}, "sig");
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].path, "sig.n");
    EXPECT_NE(issues[0].message.find("at most 10"), std::string::npos);
}

TEST(ParamSchema, DefaultsFillOnlyTheAbsentOptionalParameters) {
    ParamSchema s;
    s.params = {ParamSpec{"n", ParamType::Int, true, nullptr, std::nullopt, false, std::nullopt, {}, ""},
                ParamSpec{"x", ParamType::Number, false, 1.5, std::nullopt, false, std::nullopt, {}, ""},
                ParamSpec{"y", ParamType::Number, false, nullptr, std::nullopt, false, std::nullopt, {}, ""}};
    EXPECT_EQ(s.with_defaults(json{{"n", 3}}), json::parse(R"({"n": 3, "x": 1.5})"));
    EXPECT_EQ(s.with_defaults(json{{"n", 3}, {"x", 9.0}}), json::parse(R"({"n": 3, "x": 9.0})"));
}

// ---- registry -------------------------------------------------------------------------------

TEST(AnalysisRegistry, TheBuiltInsCoverEveryNameInTheIssue) {
    const auto r = AnalysisRegistry::with_builtins();
    for (const char* n : {"fixed", "ratio", "estimated", "neutral", "forward"}) {
        EXPECT_NE(r.find(AnalysisKind::SeriesRule, n), nullptr) << n;
    }
    for (const char* n : {"zscore", "ewmac", "momentum", "percentile", "seasonal", "value_vs_anchor",
                          "info_ratio_signal", "forward_gap", "equity_risk_premium", "implied_growth_gap",
                          "relative_pe"}) {
        EXPECT_NE(r.find(AnalysisKind::Signal, n), nullptr) << n;
    }
    for (const char* n : {"adf", "kpss", "coint", "hurst", "vr", "half_life", "factors"}) {
        EXPECT_NE(r.find(AnalysisKind::Diagnostic, n), nullptr) << n;
    }
    for (const char* n : {"linear", "scalar", "fixed_scalar", "percentile", "none"}) {
        EXPECT_NE(r.find(AnalysisKind::ForecastMethod, n), nullptr) << n;
    }
    EXPECT_EQ(r.find(AnalysisKind::Signal, "fixed"), nullptr) << "kinds are separate namespaces";
    EXPECT_EQ(r.names(AnalysisKind::SeriesRule).size(), 5u);
    EXPECT_EQ(r.names(AnalysisKind::Signal).size(), 11u);
    EXPECT_EQ(r.names(AnalysisKind::Diagnostic).size(), 7u);
    EXPECT_EQ(r.names(AnalysisKind::ForecastMethod).size(), 5u);
}

TEST(AnalysisRegistry, ADiagnosticStringSplitsIntoNameAndWindow) {
    std::string name;
    int window = 0;
    ASSERT_TRUE(split_diagnostic("half_life_120", name, window));
    EXPECT_EQ(name, "half_life");
    EXPECT_EQ(window, 120);
    ASSERT_TRUE(split_diagnostic("adf_250", name, window));
    EXPECT_EQ(name, "adf");
    for (const char* bad : {"adf", "adf_", "_250", "adf_0", "adf_-5", "adf_2x", "adf_9999999", ""}) {
        EXPECT_FALSE(split_diagnostic(bad, name, window)) << bad;
    }
}

TEST(AnalysisRegistry, ASecondRegistrationOfTheSameNameIsRefusedButAnotherKindIsNot) {
    AnalysisRegistry r;
    AnalysisDescriptor d;
    d.name = "thing";
    d.kind = AnalysisKind::Signal;
    EXPECT_TRUE(r.add(d).is_ok());
    EXPECT_TRUE(r.add(d).is_error());
    d.kind = AnalysisKind::Diagnostic;
    EXPECT_TRUE(r.add(d).is_ok());
    AnalysisDescriptor nameless;
    EXPECT_TRUE(r.add(nameless).is_error());
}

// ---- contract lookup ------------------------------------------------------------------------

namespace {
std::shared_ptr<arrow::Table> metadata(const std::vector<std::pair<std::string, std::optional<std::string>>>& rows) {
    arrow::StringBuilder syms, sizes;
    for (const auto& [s, z] : rows) {
        EXPECT_TRUE(syms.Append(s).ok());
        if (z) {
            EXPECT_TRUE(sizes.Append(*z).ok());
        } else {
            EXPECT_TRUE(sizes.AppendNull().ok());
        }
    }
    std::shared_ptr<arrow::Array> a, b;
    EXPECT_TRUE(syms.Finish(&a).ok());
    EXPECT_TRUE(sizes.Finish(&b).ok());
    return arrow::Table::Make(arrow::schema({arrow::field("Databento Symbol", arrow::utf8()),
                                             arrow::field("Contract Size", arrow::utf8())}),
                              {a, b});
}
}  // namespace

TEST(ContractLookup, ARootIsFoundFromItsContinuousSymbolAndAMissingRowIsNotASize) {
    const auto lookup = make_contract_lookup(metadata({{"ZS", "5000"}, {"ZM", ""}, {"ZL", std::nullopt}}));
    EXPECT_EQ(contract_root("ZS.v.0"), "ZS");
    EXPECT_EQ(contract_root("AAPL"), "AAPL");

    const auto zs = lookup("ZS.v.0");
    ASSERT_TRUE(zs);
    EXPECT_EQ(zs->contract_size, "5000");
    ASSERT_TRUE(lookup("ZM.v.0"));
    EXPECT_TRUE(lookup("ZM.v.0")->contract_size.empty()) << "a row with an empty size";
    ASSERT_TRUE(lookup("ZL.v.0"));
    EXPECT_TRUE(lookup("ZL.v.0")->contract_size.empty()) << "a row with a null size";
    EXPECT_FALSE(lookup("XX.v.0")) << "no row at all";
}

TEST(ContractLookup, ATableWithoutTheColumnsGivesNoRowsAndANullTableIsSafe) {
    arrow::StringBuilder b;
    ASSERT_TRUE(b.Append("x").ok());
    std::shared_ptr<arrow::Array> a;
    ASSERT_TRUE(b.Finish(&a).ok());
    const auto odd = arrow::Table::Make(arrow::schema({arrow::field("other", arrow::utf8())}), {a});
    EXPECT_FALSE(make_contract_lookup(odd)("ZS.v.0"));
    EXPECT_FALSE(make_contract_lookup(std::shared_ptr<arrow::Table>())("ZS.v.0"));
}
