// src/relative_value/definition_validator.cpp
#include "trade_ngin/relative_value/definition_validator.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace trade_ngin::relative_value {

using nlohmann::json;
using Issues = std::vector<ValidationIssue>;

namespace {

bool is_word(const std::string& s, bool allow_dash) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (!(std::isalnum(c) || c == '_' || (allow_dash && c == '-'))) return false;
    }
    return true;
}

bool valid_id(const std::string& id) {
    return id.size() >= 1 && id.size() <= 50 && is_word(id, true) && id.find("--") == std::string::npos;
}

bool valid_iso_date(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9}) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    const int y = std::stoi(s.substr(0, 4));
    const int m = std::stoi(s.substr(5, 2));
    const int d = std::stoi(s.substr(8, 2));
    if (m < 1 || m > 12 || d < 1) return false;
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return d <= days[m - 1] + (m == 2 && leap ? 1 : 0);
}

std::string known(const AnalysisRegistry& reg, AnalysisKind kind) {
    std::string out;
    for (const auto& n : reg.names(kind)) out += (out.empty() ? "" : ", ") + n;
    return out;
}

// The forecast mapping a signal ends up with: its own block, else its type's default (an
// info_ratio_signal takes its base's).
std::string effective_mapping(const SignalSpec& s, const AnalysisRegistry& reg) {
    if (s.forecast) return s.forecast->method;
    const auto* d = reg.find(AnalysisKind::Signal, s.type);
    if (!d) return "none";
    if (d->default_forecast == "base") {
        if (s.params.contains("base") && s.params["base"].is_object() && s.params["base"].contains("type") &&
            s.params["base"]["type"].is_string()) {
            const auto* b = reg.find(AnalysisKind::Signal, s.params["base"]["type"].get<std::string>());
            if (b && b->default_forecast != "base") return b->default_forecast;
        }
        return "none";
    }
    return d->default_forecast;
}

// A neutral-style block: the series rule `neutral` and a package's own `weights` use the same
// parameters, so they are checked the same way.
void check_neutral_block(const json& block, const std::string& path, const TradeDefinition& def,
                         const std::set<std::string>& input_names, const AnalysisRegistry& reg,
                         Issues& issues) {
    const auto* d = reg.find(AnalysisKind::SeriesRule, "neutral");
    if (!d) return;
    auto found = d->params.check(block, path);
    issues.insert(issues.end(), found.begin(), found.end());
    if (d->check) d->check(block, path, issues);

    if (block.contains("shape") && block["shape"].is_array() && block["shape"].size() != def.legs.size()) {
        issues.push_back({path + ".shape", "needs one entry per leg: " + std::to_string(def.legs.size()) +
                                               " legs, " + std::to_string(block["shape"].size()) + " entries"});
    }

    const std::string risk = block.value("risk", std::string());
    const json inputs = block.contains("risk_inputs") && block["risk_inputs"].is_object() ? block["risk_inputs"]
                                                                                          : json::object();
    std::set<std::string> leg_symbols;
    for (const auto& l : def.legs) leg_symbols.insert(l.symbol);

    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
        const std::string at = path + ".risk_inputs." + it.key();
        const bool key_ok = risk == "beta" ? it.key() == "benchmark" : leg_symbols.count(it.key()) > 0;
        if (!key_ok) {
            issues.push_back({at, risk == "beta" ? "only \"benchmark\" is accepted for risk \"beta\""
                                                 : "must be a leg symbol"});
        }
        if (it.value().is_string() && !input_names.count(it.value().get<std::string>())) {
            issues.push_back({at, "refers to input '" + it.value().get<std::string>() +
                                      "', which is not declared"});
        }
    }
    if (risk == "beta" && !inputs.contains("benchmark")) {
        issues.push_back({path + ".risk_inputs", "risk \"beta\" needs a \"benchmark\" input"});
    }
    if (risk == "duration" && inputs.size() != leg_symbols.size() && !block.contains("fallback")) {
        issues.push_back({path, "risk \"duration\" needs a DV01 input for every leg in risk_inputs, or "
                                "\"fallback\": \"vol\""});
    }
}

void check_refs(const AnalysisDescriptor& d, const json& params, const std::string& path,
                const std::set<std::string>& input_names, Issues& issues) {
    for (const auto& spec : d.params.params) {
        if (spec.type != ParamType::InputRef) continue;
        auto it = params.find(spec.name);
        if (it != params.end() && it->is_string() && !input_names.count(it->get<std::string>())) {
            issues.push_back({path + "." + spec.name, "refers to input '" + it->get<std::string>() +
                                                          "', which is not declared"});
        }
    }
    if (d.input_refs) {
        for (const auto& [suffix, name] : d.input_refs(params)) {
            if (!input_names.count(name)) {
                issues.push_back({path + "." + suffix, "refers to input '" + name + "', which is not declared"});
            }
        }
    }
}

// Check `params` against a registered analysis: the schema, the cross-parameter check, and the
// input references.
void check_analysis(const AnalysisDescriptor& d, const json& params, const std::string& path,
                    const std::set<std::string>& input_names, Issues& issues) {
    auto found = d.params.check(params, path);
    issues.insert(issues.end(), found.begin(), found.end());
    if (d.check) d.check(params, path, issues);
    check_refs(d, params, path, input_names, issues);
}

}  // namespace

std::vector<std::string> signal_keys(const TradeDefinition& def, const AnalysisRegistry& reg) {
    std::vector<std::string> keys{"series"};

    // weights: per input for `fixed`, per leg otherwise
    if (def.series.rule == "fixed" && def.series.params.contains("terms") && def.series.params["terms"].is_array()) {
        for (const auto& t : def.series.params["terms"]) {
            if (t.is_object() && t.contains("input") && t["input"].is_string()) {
                keys.push_back("series.weight." + t["input"].get<std::string>());
            }
        }
    } else if (def.series.rule == "estimated" || def.series.rule == "neutral" ||
               (def.package && def.package->weights.is_object())) {
        // contract weights per leg (a ratio or forward series has none)
        for (const auto& l : def.legs) keys.push_back("series.weight." + l.symbol);
    }
    if (const auto* rule = reg.find(AnalysisKind::SeriesRule, def.series.rule)) {
        for (const auto& f : rule->flags) keys.push_back("series.flag." + f);
    }
    keys.push_back("series.flag.roll_day");

    for (const auto& s : def.signals) {
        keys.push_back(s.name);
        const std::string mapping = effective_mapping(s, reg);
        if (mapping != "none") {
            keys.push_back(s.name + ".forecast");
            if (mapping == "scalar") keys.push_back(s.name + ".scalar");
            if (mapping == "linear") keys.push_back(s.name + ".k");
        }
        if (const auto* d = reg.find(AnalysisKind::Signal, s.type)) {
            for (const auto& o : d->output_names(s.params)) keys.push_back(s.name + "." + o);
            for (const auto& f : d->flags) keys.push_back(s.name + ".flag." + f);
        }
    }
    for (const auto& text : def.diagnostics) {
        keys.push_back(text);
        std::string name;
        int window = 0;
        if (split_diagnostic(text, name, window)) {
            if (const auto* d = reg.find(AnalysisKind::Diagnostic, name)) {
                for (const auto& o : d->output_names(json::object())) keys.push_back(text + "." + o);
                for (const auto& f : d->flags) keys.push_back(text + ".flag." + f);
            }
        }
    }
    if (def.trade) keys.push_back("forecast.blended");
    for (const auto& l : def.legs) {
        keys.push_back(l.symbol + ".recommended");
        keys.push_back(l.symbol + ".held");
    }
    return keys;
}

Issues validate_trade_definition(const TradeDefinition& def, const ValidationContext& ctx) {
    Issues issues;
    const AnalysisRegistry local = ctx.registry ? AnalysisRegistry() : AnalysisRegistry::with_builtins();
    const AnalysisRegistry& reg = ctx.registry ? *ctx.registry : local;
    auto bad = [&](const std::string& path, const std::string& message) { issues.push_back({path, message}); };

    // ---- id
    if (!valid_id(def.id)) {
        bad("id", "must be 1 to 50 characters of letters, digits, _ and - with no \"--\", got '" + def.id + "'");
    }
    for (const auto* other : ctx.others) {
        if (other && other->id == def.id) bad("id", "'" + def.id + "' is already used by another definition");
    }

    // ---- legs
    std::set<std::string> leg_symbols;
    std::set<std::string> asset_classes;
    for (size_t i = 0; i < def.legs.size(); ++i) {
        const auto& l = def.legs[i];
        const std::string path = "legs[" + std::to_string(i) + "]";
        if (!leg_symbols.insert(l.symbol).second) bad(path + ".symbol", "'" + l.symbol + "' is listed twice");
        if (l.asset_class != "futures" && l.asset_class != "equity") {
            bad(path + ".asset_class", "must be \"futures\" or \"equity\", got '" + l.asset_class + "'");
        }
        asset_classes.insert(l.asset_class);
        if (ctx.contract_lookup) {
            const auto info = ctx.contract_lookup(l.symbol);
            if (!info) {
                bad(path + ".symbol", "'" + l.symbol + "' is not in the instrument registry (no contract metadata row)");
            } else if (info->contract_size.find_first_not_of(" \t") == std::string::npos) {
                bad(path + ".symbol", "'" + l.symbol + "' has no Contract Size in its metadata row; the engine "
                                      "would size it with 1.0");
            }
        }
    }
    if (asset_classes.size() > 1) {
        bad("legs", "all legs must be one asset class; mixed futures-and-equity trades are not supported yet");
    }

    // ---- inputs
    std::set<std::string> input_names;
    for (size_t i = 0; i < def.inputs.size(); ++i) {
        const auto& in = def.inputs[i];
        const std::string path = "inputs[" + std::to_string(i) + "]";
        if (!is_word(in.name, false)) bad(path + ".name", "must be letters, digits and _ only, got '" + in.name + "'");
        if (!input_names.insert(in.name).second) bad(path + ".name", "'" + in.name + "' is declared twice");
        if (in.source == "leg" && !leg_symbols.count(in.symbol)) {
            bad(path + ".symbol", "'" + in.symbol + "' is not one of the legs");
        } else if (in.source == "instrument") {
            if (in.asset_class != "futures" && in.asset_class != "equity") {
                bad(path + ".asset_class", "must be \"futures\" or \"equity\"");
            }
            if (ctx.contract_lookup && in.asset_class == "futures" && !ctx.contract_lookup(in.symbol)) {
                bad(path + ".symbol", "'" + in.symbol + "' is not in the instrument registry");
            }
        } else if (in.source == "series") {
            if (ctx.series_exists && !ctx.series_exists(in.table, in.key)) {
                bad(path, "series " + in.table + "." + in.key + " does not exist in the database");
            }
        } else if (in.source == "trade") {
            if (in.trade_id == def.id) {
                bad(path + ".trade_id", "a definition cannot use its own series as an input");
            } else {
                bool found = false;
                for (const auto* o : ctx.others) found = found || (o && o->id == in.trade_id);
                if (!found) bad(path + ".trade_id", "no definition with id '" + in.trade_id + "'");
            }
        }
    }

    // ---- legs, package and trade go together
    const bool has_legs = !def.legs.empty();
    if (has_legs) {
        if (!def.package) bad("package", "a definition with legs needs a package");
        if (!def.trade) bad("trade", "a definition with legs needs a trade block");
    } else {
        if (def.package) bad("package", "an analysis-only definition (\"legs\": []) has no package");
        if (def.trade) bad("trade", "an analysis-only definition (\"legs\": []) has no trade block");
    }

    // ---- series
    const AnalysisDescriptor* rule = reg.find(AnalysisKind::SeriesRule, def.series.rule);
    if (!rule) {
        bad("series.rule", "unknown series rule '" + def.series.rule + "' (registered: " +
                               known(reg, AnalysisKind::SeriesRule) + ")");
    } else {
        check_analysis(*rule, def.series.params, "series", input_names, issues);
        if (def.series.rule == "neutral") {
            check_neutral_block(def.series.params, "series", def, input_names, reg, issues);
        }
    }

    // ---- package
    if (def.package) {
        const auto& p = *def.package;
        if (p.mode == "fixed") {
            std::set<std::string> keys;
            for (const auto& [sym, n] : p.contracts) {
                keys.insert(sym);
                if (n == 0) bad("package.contracts." + sym, "must be a non-zero integer");
            }
            if (keys != leg_symbols) {
                bad("package.contracts", "keys must be exactly the leg symbols");
            }
        } else if (p.mode == "derived") {
            if (!(p.scale > 0.0)) bad("package.scale", "must be positive");
            if (!(p.rebalance_threshold > 0.0)) bad("package.rebalance_threshold", "must be positive");
            const bool rule_has_weights = def.series.rule == "estimated" || def.series.rule == "neutral";
            if (p.weights.is_object()) {
                check_neutral_block(p.weights, "package.weights", def, input_names, reg, issues);
            } else if (!rule_has_weights) {
                bad("package", "a derived package needs weights: the series rule '" + def.series.rule +
                                   "' has none, so add a \"weights\" block {risk, shape, risk_inputs}");
            }
        }
    }

    // ---- signals and diagnostics
    std::set<std::string> names;
    std::map<std::string, const SignalSpec*> by_name;
    for (size_t i = 0; i < def.signals.size(); ++i) {
        const auto& s = def.signals[i];
        const std::string path = "signals[" + std::to_string(i) + "]";
        if (!is_word(s.name, false) || s.name.size() > kSignalNameWidth) {
            bad(path + ".name", "must be 1 to " + std::to_string(kSignalNameWidth) +
                                    " letters, digits and _ ('.' separates key parts), got '" + s.name + "'");
        }
        if (!names.insert(s.name).second) bad(path + ".name", "'" + s.name + "' is used twice");
        by_name[s.name] = &s;

        const auto* d = reg.find(AnalysisKind::Signal, s.type);
        if (!d) {
            bad(path + ".type", "unknown signal type '" + s.type + "' (registered: " +
                                    known(reg, AnalysisKind::Signal) + ")");
        } else {
            check_analysis(*d, s.params, path, input_names, issues);
        }
        if (s.input && !input_names.count(*s.input)) {
            bad(path + ".input", "refers to input '" + *s.input + "', which is not declared");
        }
        if (s.forecast) {
            const auto* fm = reg.find(AnalysisKind::ForecastMethod, s.forecast->method);
            if (!fm) {
                bad(path + ".forecast.method", "unknown forecast method '" + s.forecast->method +
                                                   "' (registered: " + known(reg, AnalysisKind::ForecastMethod) + ")");
            } else {
                check_analysis(*fm, s.forecast->params, path + ".forecast", input_names, issues);
            }
        }

        if (s.type == "info_ratio_signal" && s.params.contains("base")) {
            const auto& base = s.params["base"];
            const std::string bpath = path + ".base";
            if (!base.is_object() || !base.contains("type") || !base["type"].is_string()) {
                bad(bpath, "must be a signal object with a \"type\"");
            } else {
                const std::string btype = base["type"].get<std::string>();
                const auto* bd = reg.find(AnalysisKind::Signal, btype);
                if (!bd) {
                    bad(bpath + ".type", "unknown signal type '" + btype + "'");
                } else if (btype == "info_ratio_signal") {
                    bad(bpath + ".type", "a base signal cannot itself be an info_ratio_signal");
                } else {
                    if (base.contains("name")) bad(bpath + ".name", "a base signal takes no name");
                    if (base.contains("sign")) bad(bpath + ".sign", "a sign inside the base is rejected; use the outer sign");
                    json params = json::object();
                    for (auto it = base.begin(); it != base.end(); ++it) {
                        if (it.key() != "type" && it.key() != "name" && it.key() != "sign") params[it.key()] = it.value();
                    }
                    check_analysis(*bd, params, bpath, input_names, issues);
                }
            }
            if (s.params.contains("leg") && s.params["leg"].is_string() &&
                !leg_symbols.count(s.params["leg"].get<std::string>())) {
                bad(path + ".leg", "'" + s.params["leg"].get<std::string>() + "' is not one of the legs");
            }
        }
    }

    for (size_t i = 0; i < def.diagnostics.size(); ++i) {
        const auto& text = def.diagnostics[i];
        const std::string path = "diagnostics[" + std::to_string(i) + "]";
        std::string name;
        int window = 0;
        if (!split_diagnostic(text, name, window)) {
            bad(path, "'" + text + "' must be <name>_<window>, like adf_250, with a positive integer window");
        } else if (!reg.find(AnalysisKind::Diagnostic, name)) {
            bad(path, "unknown diagnostic '" + name + "' (registered: " + known(reg, AnalysisKind::Diagnostic) + ")");
        }
        if (!is_word(text, false) || text.size() > kSignalNameWidth) {
            bad(path, "must be 1 to " + std::to_string(kSignalNameWidth) + " letters, digits and _, got '" + text + "'");
        }
        if (!names.insert(text).second) bad(path, "'" + text + "' is also used as a signal name or listed twice");
    }

    // ---- every key fits the signal column
    for (const auto& key : signal_keys(def, reg)) {
        if (key.size() > kSignalKeyWidth) {
            bad("", "the key '" + key + "' is " + std::to_string(key.size()) + " characters; analysis.signals holds " +
                        std::to_string(kSignalKeyWidth) + ". Shorten a signal name, input name or symbol");
        }
    }

    // ---- trade
    if (def.trade) {
        const auto& t = *def.trade;
        if (!(t.risk_target > 0.0)) bad("trade.risk_target", "must be positive");
        if (!(t.capital > 0.0)) bad("trade.capital", "must be positive");
        double sum = 0.0;
        for (const auto& [name, w] : t.blend) {
            sum += w;
            const std::string path = "trade.blend." + name;
            if (w < 0.0) bad(path, "weights must be non-negative");
            auto it = by_name.find(name);
            if (it == by_name.end()) {
                bad(path, "'" + name + "' is not a signal of this definition");
            } else if (effective_mapping(*it->second, reg) == "none") {
                bad(path, "signal '" + name + "' has no forecast mapping (its mapping is \"none\")");
            }
        }
        if (std::abs(sum - 1.0) > 1e-9) bad("trade.blend", "weights must sum to 1, got " + std::to_string(sum));
    }

    // ---- history_start
    if (def.history_start) {
        if (!valid_iso_date(*def.history_start)) {
            bad("history_start", "must be a valid date like 2015-01-02, got '" + *def.history_start + "'");
        } else if (ctx.first_data_date) {
            const auto first = ctx.first_data_date(def);
            if (first && *def.history_start < *first) {
                bad("history_start", *def.history_start + " is before the first date on which every leg and input has data (" +
                                         *first + ")");
            }
        }
    }

    return issues;
}

Issues validate_definition_set(const std::vector<TradeDefinition>& defs, const ValidationContext& ctx) {
    Issues all;
    for (size_t i = 0; i < defs.size(); ++i) {
        ValidationContext c = ctx;
        c.others.clear();
        for (size_t j = 0; j < defs.size(); ++j) {
            if (j != i) c.others.push_back(&defs[j]);
        }
        for (auto issue : validate_trade_definition(defs[i], c)) {
            issue.path = defs[i].id + (issue.path.empty() ? "" : "." + issue.path);
            all.push_back(std::move(issue));
        }
    }

    // cycles among `trade` inputs: depth-first search over id -> ids
    std::map<std::string, std::vector<std::string>> edges;
    for (const auto& d : defs) {
        for (const auto& in : d.inputs) {
            if (in.source == "trade") edges[d.id].push_back(in.trade_id);
        }
    }
    std::map<std::string, int> state;  // 0 new, 1 on the path, 2 done
    std::set<std::string> reported;
    std::function<void(const std::string&, std::vector<std::string>&)> visit =
        [&](const std::string& id, std::vector<std::string>& path) {
            state[id] = 1;
            path.push_back(id);
            for (const auto& next : edges[id]) {
                if (state[next] == 1) {
                    std::string cycle;
                    bool in = false;
                    for (const auto& p : path) {
                        in = in || p == next;
                        if (in) cycle += p + " -> ";
                    }
                    cycle += next;
                    if (reported.insert(cycle).second) {
                        all.push_back({id, "trade inputs form a cycle: " + cycle});
                    }
                } else if (state[next] == 0) {
                    visit(next, path);
                }
            }
            path.pop_back();
            state[id] = 2;
        };
    for (const auto& d : defs) {
        if (state[d.id] == 0) {
            std::vector<std::string> path;
            visit(d.id, path);
        }
    }
    return all;
}

}  // namespace trade_ngin::relative_value
