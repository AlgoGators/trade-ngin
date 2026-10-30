// src/relative_value/builtin_analyses.cpp
//
// The descriptors of the built-in series rules (section 7.1), signals (8.1, 8.2), diagnostics (8.3)
// and forecast methods (8.4): their names, parameters, bounds, defaults, outputs and flags. The
// compute side of each analysis lives with the signal module and the series builder.

#include <cmath>
#include <stdexcept>

#include "trade_ngin/relative_value/analysis_registry.hpp"

namespace trade_ngin::relative_value {

using nlohmann::json;
using Issues = std::vector<ValidationIssue>;

namespace {

ParamSpec req(std::string name, ParamType type) {
    ParamSpec p;
    p.name = std::move(name);
    p.type = type;
    p.required = true;
    return p;
}

ParamSpec opt(std::string name, ParamType type, json def = nullptr) {
    ParamSpec p;
    p.name = std::move(name);
    p.type = type;
    p.default_value = std::move(def);
    return p;
}

ParamSpec at_least(ParamSpec p, double lo) {
    p.min = lo;
    return p;
}

ParamSpec above(ParamSpec p, double lo) {
    p.min = lo;
    p.min_exclusive = true;
    return p;
}

ParamSpec one_of(ParamSpec p, std::vector<std::string> allowed) {
    p.allowed = std::move(allowed);
    return p;
}

ParamSpec direction() {
    return one_of(opt("direction", ParamType::Enum, "revert"), {"revert", "momentum"});
}

ParamSpec z_at_10() { return above(opt("z_at_10", ParamType::Number, 1.5), 0.0); }

ParamSpec window(int lo = 2) { return at_least(req("window", ParamType::Int), lo); }

void register_builtin(AnalysisRegistry& r, AnalysisDescriptor d) {
    // The built-in names are distinct by construction; a clash is a programming error.
    auto result = r.add(std::move(d));
    if (result.is_error()) throw std::logic_error(result.error()->what());
}

AnalysisDescriptor signal(std::string name, std::vector<ParamSpec> params, std::string forecast,
                          std::string description) {
    AnalysisDescriptor d;
    d.name = std::move(name);
    d.kind = AnalysisKind::Signal;
    d.params.params = std::move(params);
    d.default_forecast = std::move(forecast);
    d.description = std::move(description);
    return d;
}

bool is_name(const json& v) { return v.is_string() && !v.get<std::string>().empty(); }

}  // namespace

AnalysisRegistry AnalysisRegistry::with_builtins() {
    AnalysisRegistry r;

    // ---------------------------------------------------------------- series rules (7.1)
    {
        AnalysisDescriptor d;
        d.name = "fixed";
        d.kind = AnalysisKind::SeriesRule;
        d.description = "sum of coef x input value";
        d.params.params = {req("terms", ParamType::Any), opt("units", ParamType::String)};
        d.check = [](const json& p, const std::string& path, Issues& issues) {
            const auto it = p.find("terms");
            if (it == p.end()) return;
            if (!it->is_array() || it->empty()) {
                issues.push_back({path + ".terms", "must be a non-empty list of {input, coef}"});
                return;
            }
            for (size_t i = 0; i < it->size(); ++i) {
                const auto& t = (*it)[i];
                const std::string at = path + ".terms[" + std::to_string(i) + "]";
                if (!t.is_object() || !t.contains("input") || !is_name(t["input"]) ||
                    !t.contains("coef") || !t["coef"].is_number()) {
                    issues.push_back({at, "must be {\"input\": <input name>, \"coef\": <number>}"});
                } else if (t["coef"].get<double>() == 0.0) {
                    issues.push_back({at + ".coef", "must not be zero"});
                }
            }
        };
        d.input_refs = [](const json& p) {
            std::vector<std::pair<std::string, std::string>> refs;
            const auto it = p.find("terms");
            if (it == p.end() || !it->is_array()) return refs;
            for (size_t i = 0; i < it->size(); ++i) {
                const auto& t = (*it)[i];
                if (t.is_object() && t.contains("input") && is_name(t["input"])) {
                    refs.emplace_back("terms[" + std::to_string(i) + "].input",
                                      t["input"].get<std::string>());
                }
            }
            return refs;
        };
        register_builtin(r, std::move(d));
    }
    {
        AnalysisDescriptor d;
        d.name = "ratio";
        d.kind = AnalysisKind::SeriesRule;
        d.description = "num / den, or ln(num) - ln(den) when log is true";
        d.params.params = {req("numerator", ParamType::InputRef),
                           req("denominator", ParamType::InputRef), opt("log", ParamType::Bool, false)};
        d.check = [](const json& p, const std::string& path, Issues& issues) {
            if (p.contains("numerator") && p.contains("denominator") &&
                p["numerator"] == p["denominator"]) {
                issues.push_back({path + ".denominator", "must differ from the numerator"});
            }
        };
        register_builtin(r, std::move(d));
    }
    {
        AnalysisDescriptor d;
        d.name = "estimated";
        d.kind = AnalysisKind::SeriesRule;
        d.description = "y - sum(beta_k,t x_k), betas fitted on data up to day t (OLS or Kalman)";
        d.params.params = {
            req("y", ParamType::InputRef),
            req("x", ParamType::Any),
            one_of(opt("method", ParamType::Enum, "ols"), {"ols", "kalman"}),
            window(2),
            one_of(opt("transform", ParamType::Enum, "level"), {"level", "log"}),
            opt("beta_bounds", ParamType::Any),
            above(opt("process_noise", ParamType::Number), 0.0),
            above(opt("measurement_noise", ParamType::Number), 0.0)};
        d.flags = {"beta_breach"};
        d.check = [](const json& p, const std::string& path, Issues& issues) {
            size_t nx = 0;
            if (p.contains("x")) {
                const auto& x = p["x"];
                if (!x.is_array() || x.empty()) {
                    issues.push_back({path + ".x", "must be a non-empty list of input names"});
                } else {
                    nx = x.size();
                    for (size_t i = 0; i < x.size(); ++i) {
                        if (!is_name(x[i])) {
                            issues.push_back({path + ".x[" + std::to_string(i) + "]",
                                              "must be the name of a declared input"});
                        }
                    }
                }
            }
            const bool kalman = p.value("method", std::string("ols")) == "kalman";
            if (kalman && nx > 1) {
                issues.push_back({path + ".x", "the Kalman method takes exactly one x input"});
            }
            if (!kalman) {
                for (const char* k : {"process_noise", "measurement_noise"}) {
                    if (p.contains(k)) {
                        issues.push_back({path + "." + k, "applies to the Kalman method only"});
                    }
                }
            }
            if (p.contains("beta_bounds")) {
                const auto& b = p["beta_bounds"];
                if (!b.is_object() || !b.contains("min") || !b.contains("max") ||
                    !b["min"].is_number() || !b["max"].is_number()) {
                    issues.push_back({path + ".beta_bounds", "must be {\"min\": <number>, \"max\": <number>}"});
                } else if (b["min"].get<double>() >= b["max"].get<double>()) {
                    issues.push_back({path + ".beta_bounds", "min must be less than max"});
                }
            }
        };
        d.input_refs = [](const json& p) {
            std::vector<std::pair<std::string, std::string>> refs;
            const auto it = p.find("x");
            if (it == p.end() || !it->is_array()) return refs;
            for (size_t i = 0; i < it->size(); ++i) {
                if (is_name((*it)[i])) {
                    refs.emplace_back("x[" + std::to_string(i) + "]", (*it)[i].get<std::string>());
                }
            }
            return refs;
        };
        register_builtin(r, std::move(d));
    }
    {
        AnalysisDescriptor d;
        d.name = "neutral";
        d.kind = AnalysisKind::SeriesRule;
        d.description =
            "contract weights w_i = shape_i / risk_i; the series is the dollar value of one weight unit";
        d.params.params = {
            req("shape", ParamType::Any),
            one_of(req("risk", ParamType::Enum), {"duration", "beta", "notional"}),
            opt("risk_inputs", ParamType::Any),
            at_least(opt("beta_window", ParamType::Int, 250), 2),
            one_of(opt("fallback", ParamType::Enum), {"vol"})};
        d.flags = {"vol_fallback"};
        d.check = [](const json& p, const std::string& path, Issues& issues) {
            if (p.contains("shape")) {
                const auto& s = p["shape"];
                bool ok = s.is_array() && s.size() >= 2;
                if (ok) {
                    for (const auto& v : s) ok = ok && v.is_number() && v.get<double>() != 0.0;
                }
                if (!ok) {
                    issues.push_back({path + ".shape", "must be a list of at least two non-zero numbers"});
                }
            }
            if (p.contains("risk_inputs") && !p["risk_inputs"].is_object()) {
                issues.push_back({path + ".risk_inputs", "must be an object"});
            }
        };
        d.input_refs = [](const json& p) {
            std::vector<std::pair<std::string, std::string>> refs;
            const auto it = p.find("risk_inputs");
            if (it == p.end() || !it->is_object()) return refs;
            for (auto e = it->begin(); e != it->end(); ++e) {
                if (is_name(e.value())) refs.emplace_back("risk_inputs." + e.key(), e.value().get<std::string>());
            }
            return refs;
        };
        register_builtin(r, std::move(d));
    }
    {
        AnalysisDescriptor d;
        d.name = "forward";
        d.kind = AnalysisKind::SeriesRule;
        d.description = "the forward rate between two spot-yield maturities";
        d.params.params = {req("near", ParamType::Any), req("far", ParamType::Any),
                           one_of(opt("yield_type", ParamType::Enum, "zero"), {"zero", "par"})};
        d.check = [](const json& p, const std::string& path, Issues& issues) {
            double years[2] = {0, 0};
            bool ok[2] = {false, false};
            const char* names[2] = {"near", "far"};
            for (int i = 0; i < 2; ++i) {
                if (!p.contains(names[i])) continue;
                const auto& o = p[names[i]];
                if (!o.is_object() || !o.contains("input") || !is_name(o["input"]) ||
                    !o.contains("years") || !o["years"].is_number() || o["years"].get<double>() <= 0.0) {
                    issues.push_back({path + "." + names[i],
                                      "must be {\"input\": <input name>, \"years\": <positive number>}"});
                } else {
                    ok[i] = true;
                    years[i] = o["years"].get<double>();
                }
            }
            if (ok[0] && ok[1] && !(years[0] < years[1])) {
                issues.push_back({path, "needs 0 < near.years < far.years"});
            }
        };
        d.input_refs = [](const json& p) {
            std::vector<std::pair<std::string, std::string>> refs;
            for (const char* k : {"near", "far"}) {
                const auto it = p.find(k);
                if (it != p.end() && it->is_object() && it->contains("input") && is_name((*it)["input"])) {
                    refs.emplace_back(std::string(k) + ".input", (*it)["input"].get<std::string>());
                }
            }
            return refs;
        };
        register_builtin(r, std::move(d));
    }

    // ------------------------------------------------------------- signals on prices (8.1)
    register_builtin(r, signal("zscore", {window(2), direction(), z_at_10()}, "linear",
                  "(s_t - mean) / sample std over the last window values"));
    {
        auto d = signal("ewmac", {at_least(req("fast", ParamType::Int), 1),
                                  at_least(req("slow", ParamType::Int), 1)},
                        "scalar", "EMA_fast - EMA_slow over the difference-based volatility");
        d.check = [](const json& p, const std::string& path, Issues& issues) {
            if (p.contains("fast") && p.contains("slow") && p["fast"].is_number_integer() &&
                p["slow"].is_number_integer() && p["fast"].get<long>() >= p["slow"].get<long>()) {
                issues.push_back({path, "needs fast < slow"});
            }
        };
        register_builtin(r, std::move(d));
    }
    register_builtin(r, signal("momentum", {at_least(req("n", ParamType::Int), 1), window(2), z_at_10()}, "linear",
                  "z-score of the n-day change against the last window n-day changes"));
    register_builtin(r, signal("percentile", {window(2), direction()}, "percentile",
                  "fraction of the last window values below s_t, ties split evenly"));
    register_builtin(r, signal("seasonal",
                  {at_least(req("window_days", ParamType::Int), 1), at_least(req("years", ParamType::Int), 1),
                   direction(), z_at_10()},
                  "linear", "the series against its average over the same calendar window in past years"));
    {
        auto d = signal("value_vs_anchor", {req("anchor", ParamType::Any), above(req("scale", ParamType::Number), 0.0)},
                        "linear", "(anchor - s_t) / scale");
        d.check = [](const json& p, const std::string& path, Issues& issues) {
            if (!p.contains("anchor")) return;
            const auto& a = p["anchor"];
            const bool has_c = a.is_object() && a.contains("constant");
            const bool has_i = a.is_object() && a.contains("input");
            const bool one = has_c != has_i && a.size() == 1;
            if (!one || (has_c && !a["constant"].is_number()) || (has_i && !is_name(a["input"]))) {
                issues.push_back({path + ".anchor",
                                  "must be {\"constant\": <number>} or {\"input\": <input name>}"});
            }
        };
        d.input_refs = [](const json& p) {
            std::vector<std::pair<std::string, std::string>> refs;
            const auto it = p.find("anchor");
            if (it != p.end() && it->is_object() && it->contains("input") && is_name((*it)["input"])) {
                refs.emplace_back("anchor.input", (*it)["input"].get<std::string>());
            }
            return refs;
        };
        register_builtin(r, std::move(d));
    }
    {
        // `base` is a signal object (checked by the validator, which knows the other signal types);
        // `leg` is a leg symbol (checked against the definition's legs).
        auto d = signal("info_ratio_signal", {req("base", ParamType::Any), req("leg", ParamType::String)},
                        "base", "a base signal on this series applied as a forecast on a different leg");
        register_builtin(r, std::move(d));
    }

    // -------------------------------------------------------- signals on other inputs (8.2)
    register_builtin(r, signal("forward_gap",
                  {req("near_forward", ParamType::InputRef), req("far_forward", ParamType::InputRef),
                   window(2), direction(), z_at_10()},
                  "linear", "the gap between two adjacent forward rates against its own history"));
    register_builtin(r, signal("equity_risk_premium",
                  {req("earnings_yield", ParamType::InputRef), req("rate", ParamType::InputRef), window(2),
                   direction(), z_at_10()},
                  "linear", "forward earnings yield less the 10-year yield, z-scored"));
    register_builtin(r, signal("implied_growth_gap",
                  {req("earnings_yield_a", ParamType::InputRef), req("earnings_yield_b", ParamType::InputRef),
                   req("rate", ParamType::InputRef), req("risk_premium", ParamType::Number), window(2),
                   direction(), z_at_10()},
                  "linear", "implied growth g = r + RP - E/P for each leg, A minus B, z-scored"));
    register_builtin(r, signal("relative_pe",
                  {req("pe_a", ParamType::InputRef), req("pe_b", ParamType::InputRef), window(2), z_at_10()},
                  "linear", "ln(P/E of A) - ln(P/E of B) against its history, z-scored"));

    // --------------------------------------------------------------- diagnostics (8.3)
    auto diagnostic = [&](const std::string& name, std::vector<std::string> outputs,
                          std::vector<std::string> flags, const std::string& description) {
        AnalysisDescriptor d;
        d.name = name;
        d.kind = AnalysisKind::Diagnostic;
        d.outputs = std::move(outputs);
        d.flags = std::move(flags);
        d.description = description;
        register_builtin(r, std::move(d));
    };
    diagnostic("adf", {"pvalue"}, {}, "augmented Dickey-Fuller on the series over the window");
    diagnostic("kpss", {"pvalue"}, {}, "KPSS on the series over the window");
    diagnostic("coint", {"pvalue", "hedge_ratio", "rank"}, {},
               "Engle-Granger for two inputs, Johansen for three or more");
    diagnostic("hurst", {"r2"}, {}, "Hurst exponent over the window");
    diagnostic("vr", {"pvalue"}, {}, "variance ratio over the window");
    diagnostic("half_life", {}, {"no_reversion"}, "mean-reversion half-life in days");
    {
        AnalysisDescriptor d;
        d.name = "factors";
        d.kind = AnalysisKind::Diagnostic;
        d.description = "OLS of the trade's daily package PnL on daily changes in CL, ZN, MES and VIX";
        d.expand_outputs = [](const json&) {
            std::vector<std::string> out{"r2"};
            for (const char* f : {"CL", "ZN", "MES", "VIX"}) {
                for (const char* o : {"beta", "tstat"}) out.push_back(std::string(f) + "." + o);
            }
            return out;
        };
        register_builtin(r, std::move(d));
    }

    // ------------------------------------------------------------ forecast methods (8.4)
    auto method = [&](const std::string& name, std::vector<ParamSpec> params, const std::string& description) {
        AnalysisDescriptor d;
        d.name = name;
        d.kind = AnalysisKind::ForecastMethod;
        d.params.params = std::move(params);
        d.description = description;
        return d;
    };
    register_builtin(r, method("linear", {req("k", ParamType::Number), above(opt("cap", ParamType::Number, 20), 0.0)},
                  "raw x k, capped at +/-cap"));
    {
        auto d = method("scalar",
                        {opt("window", ParamType::Any, "expanding"),
                         at_least(opt("min_history", ParamType::Int, 250), 1),
                         above(opt("target", ParamType::Number, 10), 0.0),
                         above(opt("cap", ParamType::Number, 20), 0.0)},
                        "raw x (target / mean(abs(raw))), capped");
        d.check = [](const json& p, const std::string& path, Issues& issues) {
            const auto it = p.find("window");
            if (it == p.end()) return;
            const bool expanding = it->is_string() && it->get<std::string>() == "expanding";
            const bool days = it->is_number_integer() && it->get<long>() >= 1;
            if (!expanding && !days) {
                issues.push_back({path + ".window", "must be \"expanding\" or a positive number of days"});
            }
        };
        register_builtin(r, std::move(d));
    }
    register_builtin(r, method("fixed_scalar", {req("value", ParamType::Number), above(opt("cap", ParamType::Number, 20), 0.0)},
                  "raw x value, capped"));
    register_builtin(r, method("percentile", {window(2), direction()},
                  "(0.5 - p) x 40 for revert, (p - 0.5) x 40 for momentum"));
    register_builtin(r, method("none", {}, "no forecast; the analysis is stored only"));

    return r;
}

}  // namespace trade_ngin::relative_value
