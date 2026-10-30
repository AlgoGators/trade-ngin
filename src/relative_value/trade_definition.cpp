// src/relative_value/trade_definition.cpp
#include "trade_ngin/relative_value/trade_definition.hpp"

#include <set>

namespace trade_ngin::relative_value {

using nlohmann::json;

namespace {

struct Reader {
    std::vector<ValidationIssue>& issues;

    void bad(const std::string& path, const std::string& message) { issues.push_back({path, message}); }

    // Reject keys outside `allowed`.
    void only(const json& o, const std::string& path, const std::set<std::string>& allowed) {
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (!allowed.count(it.key())) {
                std::string list;
                for (const auto& a : allowed) list += (list.empty() ? "" : ", ") + a;
                bad(path.empty() ? it.key() : path + "." + it.key(), "unknown key (allowed: " + list + ")");
            }
        }
    }

    bool str(const json& o, const std::string& key, const std::string& path, std::string& out,
             bool required) {
        auto it = o.find(key);
        const std::string where = path.empty() ? key : path + "." + key;
        if (it == o.end() || it->is_null()) {
            if (required) bad(where, "is required");
            return false;
        }
        if (!it->is_string() || it->get<std::string>().empty()) {
            bad(where, "must be a non-empty string");
            return false;
        }
        out = it->get<std::string>();
        return true;
    }

    bool number(const json& o, const std::string& key, const std::string& path, double& out,
                bool required) {
        auto it = o.find(key);
        const std::string where = path.empty() ? key : path + "." + key;
        if (it == o.end()) {
            if (required) bad(where, "is required");
            return false;
        }
        if (it->is_boolean() || !it->is_number()) {
            bad(where, "must be a number");
            return false;
        }
        out = it->get<double>();
        return true;
    }
};

}  // namespace

ParseOutcome parse_trade_definition(const json& body) {
    ParseOutcome out;
    Reader r{out.issues};

    if (!body.is_object()) {
        r.bad("", "a trade definition must be a JSON object");
        return out;
    }
    r.only(body, "", {"id", "description", "legs", "package", "inputs", "series", "signals",
                      "diagnostics", "trade", "history_start", "strategy"});

    TradeDefinition d;
    d.body = body;
    r.str(body, "id", "", d.id, true);
    r.str(body, "description", "", d.description, false);

    // legs
    if (!body.contains("legs") || !body["legs"].is_array()) {
        r.bad("legs", "is required and must be a list (use [] for an analysis-only definition)");
    } else {
        for (size_t i = 0; i < body["legs"].size(); ++i) {
            const auto& l = body["legs"][i];
            const std::string path = "legs[" + std::to_string(i) + "]";
            if (!l.is_object()) {
                r.bad(path, "must be an object {symbol, asset_class}");
                continue;
            }
            r.only(l, path, {"symbol", "asset_class"});
            Leg leg;
            const bool a = r.str(l, "symbol", path, leg.symbol, true);
            const bool b = r.str(l, "asset_class", path, leg.asset_class, true);
            if (a && b) d.legs.push_back(std::move(leg));
        }
    }

    // inputs
    if (!body.contains("inputs") || !body["inputs"].is_array()) {
        r.bad("inputs", "is required and must be a list");
    } else {
        for (size_t i = 0; i < body["inputs"].size(); ++i) {
            const auto& in = body["inputs"][i];
            const std::string path = "inputs[" + std::to_string(i) + "]";
            if (!in.is_object()) {
                r.bad(path, "must be an object");
                continue;
            }
            InputSpec s;
            if (!r.str(in, "name", path, s.name, true) || !r.str(in, "source", path, s.source, true)) continue;
            if (s.source == "leg") {
                r.only(in, path, {"name", "source", "symbol"});
                r.str(in, "symbol", path, s.symbol, true);
            } else if (s.source == "instrument") {
                r.only(in, path, {"name", "source", "symbol", "asset_class"});
                r.str(in, "symbol", path, s.symbol, true);
                r.str(in, "asset_class", path, s.asset_class, true);
            } else if (s.source == "series") {
                r.only(in, path, {"name", "source", "table", "key"});
                r.str(in, "table", path, s.table, true);
                r.str(in, "key", path, s.key, true);
            } else if (s.source == "trade") {
                r.only(in, path, {"name", "source", "trade_id"});
                r.str(in, "trade_id", path, s.trade_id, true);
            } else {
                r.bad(path + ".source", "must be one of: leg, instrument, series, trade");
                continue;
            }
            d.inputs.push_back(std::move(s));
        }
    }

    // series
    if (!body.contains("series") || !body["series"].is_object()) {
        r.bad("series", "is required and must be an object with a \"rule\"");
    } else {
        const auto& s = body["series"];
        if (r.str(s, "rule", "series", d.series.rule, true)) {
            d.series.params = json::object();
            for (auto it = s.begin(); it != s.end(); ++it) {
                if (it.key() != "rule") d.series.params[it.key()] = it.value();
            }
        }
    }

    // package
    if (body.contains("package") && !body["package"].is_null()) {
        const auto& p = body["package"];
        if (!p.is_object()) {
            r.bad("package", "must be an object");
        } else {
            PackageSpec ps;
            if (r.str(p, "mode", "package", ps.mode, true)) {
                if (ps.mode == "fixed") {
                    r.only(p, "package", {"mode", "contracts"});
                    if (!p.contains("contracts") || !p["contracts"].is_object()) {
                        r.bad("package.contracts", "is required and must map each leg symbol to a signed integer");
                    } else {
                        for (auto it = p["contracts"].begin(); it != p["contracts"].end(); ++it) {
                            if (it.value().is_boolean() || !it.value().is_number_integer()) {
                                r.bad("package.contracts." + it.key(), "must be an integer");
                            } else {
                                ps.contracts[it.key()] = it.value().get<long>();
                            }
                        }
                    }
                } else if (ps.mode == "derived") {
                    r.only(p, "package", {"mode", "scale", "rebalance_threshold", "weights"});
                    r.number(p, "scale", "package", ps.scale, true);
                    r.number(p, "rebalance_threshold", "package", ps.rebalance_threshold, false);
                    if (p.contains("weights")) {
                        if (!p["weights"].is_object()) {
                            r.bad("package.weights", "must be an object {risk, shape, risk_inputs}");
                        } else {
                            ps.weights = p["weights"];
                        }
                    }
                } else {
                    r.bad("package.mode", "must be \"fixed\" or \"derived\"");
                }
            }
            d.package = std::move(ps);
        }
    }

    // signals
    if (body.contains("signals")) {
        if (!body["signals"].is_array()) {
            r.bad("signals", "must be a list");
        } else {
            for (size_t i = 0; i < body["signals"].size(); ++i) {
                const auto& s = body["signals"][i];
                const std::string path = "signals[" + std::to_string(i) + "]";
                if (!s.is_object()) {
                    r.bad(path, "must be an object");
                    continue;
                }
                SignalSpec sp;
                const bool a = r.str(s, "name", path, sp.name, true);
                const bool b = r.str(s, "type", path, sp.type, true);
                if (s.contains("sign")) {
                    if (!s["sign"].is_number_integer() || (s["sign"].get<long>() != 1 && s["sign"].get<long>() != -1)) {
                        r.bad(path + ".sign", "must be 1 or -1");
                    } else {
                        sp.sign = static_cast<int>(s["sign"].get<long>());
                    }
                }
                std::string input;
                if (r.str(s, "input", path, input, false)) sp.input = input;
                if (s.contains("forecast")) {
                    if (!s["forecast"].is_object()) {
                        r.bad(path + ".forecast", "must be an object with a \"method\"");
                    } else {
                        ForecastSpec f;
                        if (r.str(s["forecast"], "method", path + ".forecast", f.method, true)) {
                            f.params = json::object();
                            for (auto it = s["forecast"].begin(); it != s["forecast"].end(); ++it) {
                                if (it.key() != "method") f.params[it.key()] = it.value();
                            }
                            sp.forecast = std::move(f);
                        }
                    }
                }
                sp.params = json::object();
                for (auto it = s.begin(); it != s.end(); ++it) {
                    const auto& k = it.key();
                    if (k != "name" && k != "type" && k != "sign" && k != "input" && k != "forecast") {
                        sp.params[k] = it.value();
                    }
                }
                if (a && b) d.signals.push_back(std::move(sp));
            }
        }
    }

    // diagnostics
    if (body.contains("diagnostics")) {
        if (!body["diagnostics"].is_array()) {
            r.bad("diagnostics", "must be a list of strings like \"adf_250\"");
        } else {
            for (size_t i = 0; i < body["diagnostics"].size(); ++i) {
                const auto& e = body["diagnostics"][i];
                if (!e.is_string() || e.get<std::string>().empty()) {
                    r.bad("diagnostics[" + std::to_string(i) + "]", "must be a string like \"adf_250\"");
                } else {
                    d.diagnostics.push_back(e.get<std::string>());
                }
            }
        }
    }

    // trade
    if (body.contains("trade") && !body["trade"].is_null()) {
        const auto& t = body["trade"];
        if (!t.is_object()) {
            r.bad("trade", "must be an object {blend, risk_target, capital}");
        } else {
            r.only(t, "trade", {"blend", "risk_target", "capital"});
            TradeBlock tb;
            if (!t.contains("blend") || !t["blend"].is_object()) {
                r.bad("trade.blend", "is required and must map signal names to weights");
            } else {
                for (auto it = t["blend"].begin(); it != t["blend"].end(); ++it) {
                    if (it.value().is_boolean() || !it.value().is_number()) {
                        r.bad("trade.blend." + it.key(), "must be a number");
                    } else {
                        tb.blend[it.key()] = it.value().get<double>();
                    }
                }
            }
            r.number(t, "risk_target", "trade", tb.risk_target, true);
            r.number(t, "capital", "trade", tb.capital, true);
            d.trade = std::move(tb);
        }
    }

    std::string text;
    if (r.str(body, "history_start", "", text, false)) d.history_start = text;
    if (r.str(body, "strategy", "", text, false)) d.strategy = text;

    if (out.issues.empty()) out.definition = std::move(d);
    return out;
}

ParseOutcome parse_trade_definition_text(const std::string& text) {
    json body;
    try {
        body = json::parse(text);
    } catch (const json::parse_error& e) {
        ParseOutcome out;
        out.issues.push_back({"", std::string("not valid JSON: ") + e.what()});
        return out;
    }
    return parse_trade_definition(body);
}

std::string canonical_text(const TradeDefinition& definition) { return definition.body.dump(); }

}  // namespace trade_ngin::relative_value
