// src/risk/risk_module_config.cpp
//
// The schema-2 parser. Every rule below is an ERROR: there is no code path here on
// which a value the operator did not write ends up gating a book.
//
// Check order (first error wins):
//   T0                      a schema-1 file, named as such
//   S1, S5(top), R9         the top level of risk.json
//   S6, S4, S5, S3, R1-R8   every module object, portfolio scope then sleeve scope
//   A1, A2                  the features that are parsed but do not run yet
//   S5(ids), S8, S9         the rules that need every module in hand
//   C1                      risk_reporting, and its agreement with the gate
#include "trade_ngin/risk/overlay.hpp"
#include "trade_ngin/risk/risk_module_config.hpp"

#include <algorithm>
#include <cctype>

#include "trade_ngin/risk/carver_risk_module.hpp"

namespace trade_ngin {

namespace {

// The message is the whole error: TradeError::what() returns it alone.
Result<void> fail(const std::string& message) {
    return make_error<void>(ErrorCode::INVALID_DATA, message, "ConfigLoader");
}

// Values are printed the way JSON spells them, so `got 1.0` is a number and
// `got "weeks"` is a string and the message says which one the file holds.
std::string val(const nlohmann::json& j) {
    return j.dump();
}

const char* const kFlatSchema1Keys[] = {"var_limit",         "jump_risk_limit",
                                        "max_correlation",   "max_gross_leverage",
                                        "max_net_leverage",  "confidence_level",
                                        "lookback_period"};

const char* const kCarverKeys[] = {"id",
                                   "type",
                                   "var_limit",
                                   "jump_risk_limit",
                                   "max_correlation",
                                   "max_gross_leverage",
                                   "max_net_leverage",
                                   "confidence_level",
                                   "lookback_period",
                                   "lookback_unit",
                                   "min_gate_dates",
                                   "missing_symbol_policy"};

// The old gate's three limits, retired on a module that carries the overlay's limits and on the
// risk_reporting block of its book (LOOP_SPEC section 7.7).
const char* const kRetiredGateKeys[] = {"var_limit", "jump_risk_limit", "max_correlation"};

const char* const kReportingKeys[] = {"type",
                                      "window",
                                      "var_limit",
                                      "jump_risk_limit",
                                      "max_correlation",
                                      "max_gross_leverage",
                                      "max_net_leverage",
                                      "confidence_level",
                                      "lookback_period"};

bool is_comment_key(const std::string& key) {
    return !key.empty() && key[0] == '_';
}

bool is_ymd(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    const int month = std::stoi(s.substr(5, 2));
    const int day = std::stoi(s.substr(8, 2));
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

/// R1-R6, shared by a carver module and by risk_reporting (defined below).
Result<void> parse_gating_fields(const std::string& prefix, const nlohmann::json& m,
                                 const std::string& path, double* var_limit,
                                 double* jump_risk_limit, double* max_correlation,
                                 double* max_gross_leverage, double* max_net_leverage,
                                 double* confidence_level, int* lookback_period,
                                 bool overlay = false);

/// One module object's parse. `path` is the message's subject ("risk.modules[0]",
/// "sleeve_risk_modules.TREND_FOLLOWING_FAST[1]").
class ModuleParser {
public:
    ModuleParser(std::string prefix, std::string path, bool sleeve_scope)
        : prefix_(std::move(prefix)), path_(std::move(path)), sleeve_scope_(sleeve_scope) {}

    Result<RiskModuleConfig> parse(const nlohmann::json& m) const {
        if (!m.is_object()) {
            return make_error<RiskModuleConfig>(
                ErrorCode::INVALID_DATA,
                prefix_ + path_ + " must be a module object", "ConfigLoader");
        }
        // S6 first, so an `enabled` key gets its own words instead of S5's generic one.
        if (m.contains("enabled")) {
            return err(path_ +
                       ".enabled was removed in schema 2 (HD Q8): a module the book does not run "
                       "is a module the book does not list");
        }
        // S4
        if (!m.contains("type")) return err(path_ + ".type is required");
        if (!m.at("type").is_string()) {
            return err(path_ + ".type \"" + val(m.at("type")) +
                       "\" is not a known risk module type (carver, none, constant_scale, warn, "
                       "refuse)");
        }
        const std::string type = m.at("type").get<std::string>();
        if (type != "carver" && type != "none" && type != "constant_scale" && type != "warn" &&
            type != "refuse") {
            return err(path_ + ".type \"" + type +
                       "\" is not a known risk module type (carver, none, constant_scale, warn, "
                       "refuse)");
        }

        RiskModuleConfig out;
        out.type = type;
        for (const auto& item : m.items()) {
            if (is_comment_key(item.key())) out.comments[item.key()] = item.value();
        }

        if (type == "none") {
            // S3: a decision, not an omission, so it carries who ruled it and when.
            if (sleeve_scope_) {
                return err(path_ +
                           " is type \"none\", which is only valid at portfolio scope; omit the "
                           "sleeve's entry instead");
            }
            auto r = require_keys(m, {"id", "type", "_reason", "_ruled_by", "_ruled_on"}, type);
            if (r.is_error()) return forward(r);
            NoneModuleConfig none;
            for (const char* key : {"_reason", "_ruled_by", "_ruled_on"}) {
                if (!m.at(key).is_string() || m.at(key).get<std::string>().empty()) {
                    return err(path_ + " is type \"none\" and needs a non-empty " +
                               std::string(key));
                }
            }
            none.reason = m.at("_reason").get<std::string>();
            none.ruled_by = m.at("_ruled_by").get<std::string>();
            none.ruled_on = m.at("_ruled_on").get<std::string>();
            if (!is_ymd(none.ruled_on)) {
                return err(path_ + "._ruled_on must be YYYY-MM-DD, got \"" + none.ruled_on + "\"");
            }
            out.params = none;
        } else if (type == "carver") {
            // C-2: make_risk_module hands every module the PORTFOLIO's capital, and the Carver
            // module ignores RiskContext::capital by design (risk_module.hpp:72) and keeps the
            // RiskConfig::capital its gate divides by. A carver on a 30 % sleeve would therefore
            // measure that sleeve's leverage against 100 % of the book's money, so
            // max_gross_leverage 4.0 would be 13.3x of the sleeve's own. Portfolio scope only
            // until it honours ctx.capital.
            if (sleeve_scope_) {
                return err(path_ +
                           " is type \"carver\", which is only valid at portfolio scope: the "
                           "Carver gate divides by the portfolio's capital, so at sleeve scope "
                           "its leverage limits would be read against the whole book's money");
            }
            // LOOP_SPEC section 7.7: a module that carries the overlay's limits is the overlay.
            // It requires the per-name cap and the trim cap, and the old gate's three limits are
            // retired on it: a file that still names one is refused, never read.
            const bool some_limit = m.contains("R_max") || m.contains("R_jump_max") ||
                                    m.contains("R_shock_max");
            const bool overlay = m.contains("R_max") && m.contains("R_jump_max") &&
                                 m.contains("R_shock_max");
            if (some_limit && !overlay) {
                return err(path_ + " names some of R_max, R_jump_max and R_shock_max: the "
                                   "overlay's three risk limits (ratios to tau) come together");
            }
            std::vector<std::string> required(std::begin(kCarverKeys), std::end(kCarverKeys));
            if (overlay) {
                for (const char* retired : kRetiredGateKeys) {
                    if (m.contains(retired)) {
                        return err(path_ + "." + retired +
                                   " is retired on a module that carries the overlay's limits "
                                   "(R_max, R_jump_max, R_shock_max): remove the key");
                    }
                    required.erase(std::remove(required.begin(), required.end(), retired),
                                   required.end());
                }
                required.push_back("per_name_cap");
                required.push_back("trim_max");
            }
            auto r = require_keys(m, required, type, {"R_max", "R_jump_max", "R_shock_max"});
            if (r.is_error()) return forward(r);
            CarverModuleConfig c;
            if (overlay) {
                const auto& cap = m.at("per_name_cap");
                if (!cap.is_number() || !(cap.get<double>() > 0.0)) {
                    return err(path_ + ".per_name_cap must be a positive number (the per-name cap "
                                       "L on the sizing capital), got " + val(cap));
                }
                c.per_name_cap = cap.get<double>();
                const auto& trim = m.at("trim_max");
                if (!trim.is_number_integer() || trim.get<int>() < 0) {
                    return err(path_ + ".trim_max must be a whole number of contracts, 0 or more, "
                                       "got " + val(trim));
                }
                c.trim_max = trim.get<int>();
            }
            // LOOP_SPEC sections 4 and 12: the overlay's three risk limits, ratios to tau. All
            // three or none; each a positive number.
            {
                const bool any = m.contains("R_max") || m.contains("R_jump_max") ||
                                 m.contains("R_shock_max");
                const bool all = m.contains("R_max") && m.contains("R_jump_max") &&
                                 m.contains("R_shock_max");
                if (any && !all) {
                    return err(path_ + " names some of R_max, R_jump_max and R_shock_max: the "
                                       "overlay's three risk limits (ratios to tau) come together");
                }
                if (all) {
                    double* into[] = {&c.r_max, &c.r_jump_max, &c.r_shock_max};
                    const char* keys[] = {"R_max", "R_jump_max", "R_shock_max"};
                    for (int k = 0; k < 3; ++k) {
                        const auto& v = m.at(keys[k]);
                        if (!v.is_number() || !(v.get<double>() > 0.0)) {
                            return err(path_ + "." + keys[k] +
                                       " must be a positive number (a ratio to tau), got " + val(v));
                        }
                        *into[k] = v.get<double>();
                    }
                }
            }
            auto ranges = parse_gating_fields(prefix_, m, path_, &c.var_limit,
                                              &c.jump_risk_limit, &c.max_correlation,
                                              &c.max_gross_leverage, &c.max_net_leverage,
                                              &c.confidence_level, &c.lookback_period, overlay);
            if (ranges.is_error()) return forward(ranges);

            // R7. The window is keyed on the bar timestamp and capped at `lookback_period`
            // distinct DATES (CarverRiskModule::on_bars, since T-6b commit 9); nothing reads this
            // key to choose anything else. "bars" used to be the unit and still sits in every
            // production risk.json migrated before that commit, where it now says one thing
            // while the code does another -- so it is a load error that names the fix, not a
            // synonym (T-6b INTERIM ADVERSARIAL B-2).
            const auto& unit = m.at("lookback_unit");
            if (unit.is_string() && unit.get<std::string>() == "bars") {
                return err(path_ +
                           ".lookback_unit is \"bars\", but the Carver window is capped at "
                           "lookback_period distinct DATES and nothing reads a bar count: write "
                           "\"dates\" (python3 scripts/migrate_risk_json.py <config_dir> "
                           "--in-place upgrades the file)");
            }
            if (!unit.is_string() || unit.get<std::string>() != "dates") {
                return err(path_ + ".lookback_unit must be \"dates\", got " + val(unit));
            }
            c.lookback_unit = unit.get<std::string>();
            const auto& gate = m.at("min_gate_dates");
            if (!gate.is_number_integer() || gate.get<int>() < 3) {
                return err(path_ +
                           ".min_gate_dates must be an integer >= 3 (two dates give a NaN "
                           "covariance), got " +
                           val(gate));
            }
            c.min_gate_dates = gate.get<int>();
            if (c.lookback_period < c.min_gate_dates) {
                return err(path_ + ".lookback_period (" + std::to_string(c.lookback_period) +
                           " dates) is shorter than min_gate_dates (" +
                           std::to_string(c.min_gate_dates) + " dates)");
            }
            // R12 (T-6b INTERIM ADVERSARIAL B-5). F5 engages only when the window holds at least
            // kF5MinGateDates COMPLETE dates; below that the gate reads the unfiltered,
            // zero-filled window. `lookback_period` counts DISTINCT dates, of which F5 keeps only
            // the complete ones -- on the shipped futures books 252 distinct dates leave about
            // 187-200 -- so a period below the floor can NEVER engage F5. Necessary, not
            // sufficient: a period at or just above the floor can still fall short on complete
            // dates, which the per-run F5 fallback WARN reports.
            if (static_cast<size_t>(c.lookback_period) < CarverRiskModule::kF5MinGateDates) {
                return err(path_ + ".lookback_period (" + std::to_string(c.lookback_period) +
                           " dates) is below the sparse-date filter's floor of " +
                           std::to_string(CarverRiskModule::kF5MinGateDates) +
                           " complete dates, so the filter could never engage and the gate would "
                           "always read the zero-filled window");
            }

            // R8
            const auto& policy = m.at("missing_symbol_policy");
            if (!policy.is_string() ||
                (policy.get<std::string>() != "ignore" && policy.get<std::string>() != "warn" &&
                 policy.get<std::string>() != "refuse")) {
                return err(path_ +
                           ".missing_symbol_policy must be \"ignore\", \"warn\" or \"refuse\", "
                           "got " +
                           val(policy));
            }
            c.missing_symbol_policy = policy.get<std::string>();
            if (c.missing_symbol_policy == "ignore") {
                const bool has_reason = m.contains("_missing_symbol_policy_reason") &&
                                        m.at("_missing_symbol_policy_reason").is_string() &&
                                        !m.at("_missing_symbol_policy_reason")
                                             .get<std::string>()
                                             .empty();
                if (!has_reason) {
                    return err(path_ +
                               ".missing_symbol_policy \"ignore\" is the fail-open behaviour and "
                               "needs a non-empty _missing_symbol_policy_reason");
                }
                c.missing_symbol_policy_reason =
                    m.at("_missing_symbol_policy_reason").get<std::string>();
            }

            // A1 is implemented as of T-6b commit 9 (the date-keyed window; R7 above refuses the
            // old "bars"). A2 is still refused, with the commit that will delete the refusal.
            if (c.missing_symbol_policy != "ignore") {
                return err(path_ + ".missing_symbol_policy \"" + c.missing_symbol_policy +
                           "\" is not implemented yet; only \"ignore\" (with its reason) is "
                           "accepted");
            }
            out.params = c;
        } else if (type == "constant_scale") {
            auto r = require_keys(m, {"id", "type", "scale"}, type, {"every_lap"});
            if (r.is_error()) return forward(r);
            ConstantScaleModuleConfig cs;
            const auto& scale = m.at("scale");
            if (!scale.is_number() || !(scale.get<double>() > 0.0 && scale.get<double>() <= 1.0)) {
                return err(path_ + ".scale must be in (0, 1], got " + val(scale));
            }
            cs.scale = scale.get<double>();
            if (m.contains("every_lap")) {
                if (!m.at("every_lap").is_boolean()) {
                    return err(path_ + ".every_lap must be true or false, got " +
                               val(m.at("every_lap")));
                }
                cs.every_lap = m.at("every_lap").get<bool>();
            }
            out.params = cs;
        } else {  // warn, refuse
            auto r = require_keys(m, {"id", "type", "condition", "reason"}, type);
            if (r.is_error()) return forward(r);
            ConditionModuleConfig cc;
            auto cond = parse_condition(m.at("condition"));
            if (cond.is_error()) {
                return make_error<RiskModuleConfig>(ErrorCode::INVALID_DATA,
                                                    cond.error()->what(), "ConfigLoader");
            }
            cc.condition = cond.value();
            if (!m.at("reason").is_string() || m.at("reason").get<std::string>().empty()) {
                return err(path_ + ".reason must be a non-empty string (it is what the log says "
                                   "when the module fires), got " +
                           val(m.at("reason")));
            }
            cc.reason = m.at("reason").get<std::string>();
            out.params = cc;
        }

        // id last, so a module with a broken body is reported by its body.
        if (!m.at("id").is_string() || m.at("id").get<std::string>().empty()) {
            return err(path_ + ".id must be a non-empty string, got " + val(m.at("id")));
        }
        out.id = m.at("id").get<std::string>();
        return Result<RiskModuleConfig>(std::move(out));
    }

private:
    Result<RiskModuleConfig> err(const std::string& tail) const {
        return make_error<RiskModuleConfig>(ErrorCode::INVALID_DATA, prefix_ + tail,
                                            "ConfigLoader");
    }
    static Result<RiskModuleConfig> forward(const Result<void>& r) {
        return make_error<RiskModuleConfig>(ErrorCode::INVALID_DATA, r.error()->what(),
                                            "ConfigLoader");
    }

    /// S5: exactly the type's keys, plus `_` comments. Missing and unknown are
    /// different messages because they are different mistakes.
    Result<void> require_keys(const nlohmann::json& m, const std::vector<std::string>& required,
                              const std::string& type,
                              const std::vector<std::string>& optional = {}) const {
        for (const auto& item : m.items()) {
            const std::string& key = item.key();
            if (is_comment_key(key)) continue;
            if (std::find(required.begin(), required.end(), key) != required.end()) continue;
            if (std::find(optional.begin(), optional.end(), key) != optional.end()) continue;
            return fail(prefix_ + path_ + "." + key + " is not a key of a \"" + type + "\" module");
        }
        for (const auto& key : required) {
            if (!m.contains(key)) {
                return fail(prefix_ + path_ + "." + key + " is required for a \"" + type +
                            "\" module (schema 2 has no defaults)");
            }
        }
        return Result<void>();
    }

    Result<RiskCondition> parse_condition(const nlohmann::json& c) const {
        const std::string cpath = path_ + ".condition";
        auto cerr = [&](const std::string& tail) {
            return make_error<RiskCondition>(ErrorCode::INVALID_DATA, prefix_ + tail,
                                             "ConfigLoader");
        };
        if (!c.is_object()) return cerr(cpath + " must be an object {kind, threshold}");
        for (const auto& item : c.items()) {
            if (is_comment_key(item.key())) continue;
            if (item.key() != "kind" && item.key() != "threshold") {
                return cerr(cpath + "." + item.key() + " is not a key of a risk condition");
            }
        }
        if (!c.contains("kind")) return cerr(cpath + ".kind is required");
        if (!c.at("kind").is_string()) {
            return cerr(cpath + ".kind must be \"always\", \"never\", \"lap_at_least\", "
                                "\"nonzero_positions_above\" or \"max_abs_quantity_above\", got " +
                        val(c.at("kind")));
        }
        const std::string kind = c.at("kind").get<std::string>();
        RiskCondition out;
        bool needs_threshold = true;
        if (kind == "always") {
            out.kind = RiskCondition::Kind::ALWAYS;
            needs_threshold = false;
        } else if (kind == "never") {
            out.kind = RiskCondition::Kind::NEVER;
            needs_threshold = false;
        } else if (kind == "lap_at_least") {
            out.kind = RiskCondition::Kind::LAP_AT_LEAST;
        } else if (kind == "nonzero_positions_above") {
            out.kind = RiskCondition::Kind::NONZERO_POSITIONS_ABOVE;
        } else if (kind == "max_abs_quantity_above") {
            out.kind = RiskCondition::Kind::MAX_ABS_QUANTITY_ABOVE;
        } else {
            return cerr(cpath + ".kind must be \"always\", \"never\", \"lap_at_least\", "
                                "\"nonzero_positions_above\" or \"max_abs_quantity_above\", got \"" +
                        kind + "\"");
        }
        if (needs_threshold) {
            if (!c.contains("threshold")) {
                return cerr(cpath + ".threshold is required for a \"" + kind + "\" condition");
            }
            if (!c.at("threshold").is_number()) {
                return cerr(cpath + ".threshold must be a number, got " + val(c.at("threshold")));
            }
            out.threshold = c.at("threshold").get<double>();
        } else if (c.contains("threshold")) {
            return cerr(cpath + ".threshold is meaningless for a \"" + kind +
                        "\" condition; remove it");
        }
        return Result<RiskCondition>(out);
    }

public:
private:
    std::string prefix_;
    std::string path_;
    bool sleeve_scope_;
};

/// R1-R6, shared by a carver module and by risk_reporting.
Result<void> parse_gating_fields(const std::string& prefix, const nlohmann::json& m,
                                 const std::string& path, double* var_limit,
                                 double* jump_risk_limit, double* max_correlation,
                                 double* max_gross_leverage, double* max_net_leverage,
                                 double* confidence_level, int* lookback_period, bool overlay) {
    const std::string p = prefix + path + ".";

    // The overlay's book: the three limits of the old gate are not in the file, and the
    // RiskConfig built from this block keeps its own defaults for them (nothing reads them: the
    // overlay's limits are R_max, R_jump_max and R_shock_max).
    if (overlay) {
        const RiskConfig defaults;
        *max_correlation = defaults.max_correlation;
        *var_limit = defaults.var_limit;
        *jump_risk_limit = defaults.jump_risk_limit;
    }
    // R1: at 1.0 the correlation term can never bind, so a book that writes it has
    // switched the term off without saying so.
    if (!overlay) {
        const auto& corr = m.at("max_correlation");
        if (!corr.is_number() || !(corr.get<double>() > 0.0 && corr.get<double>() < 1.0)) {
            return fail(p + "max_correlation must be in (0, 1), got " + val(corr) +
                        "; at 1.0 the correlation term can never bind (risk_manager.cpp:551 "
                        "clamps rho)");
        }
        *max_correlation = corr.get<double>();
    }

    // R2, R3
    if (!overlay) {
        const auto& var = m.at("var_limit");
        if (!var.is_number() || !(var.get<double>() > 0.0 && var.get<double>() <= 1.0)) {
            return fail(p + "var_limit must be in (0, 1], got " + val(var));
        }
        *var_limit = var.get<double>();
        const auto& jump = m.at("jump_risk_limit");
        if (!jump.is_number() || !(jump.get<double>() > 0.0 && jump.get<double>() <= 1.0)) {
            return fail(p + "jump_risk_limit must be in (0, 1], got " + val(jump));
        }
        *jump_risk_limit = jump.get<double>();
    }

    // R4: the ceiling HD asked for, and the ordering the gate assumes.
    const auto& net = m.at("max_net_leverage");
    const auto& gross = m.at("max_gross_leverage");
    const bool numbers = net.is_number() && gross.is_number();
    if (!numbers || !(net.get<double>() > 0.0 && net.get<double>() <= gross.get<double>() &&
                      gross.get<double>() <= 10.0)) {
        return fail(p +
                    "leverage limits must satisfy 0 < max_net_leverage <= max_gross_leverage <= "
                    "10, got net " +
                    val(net) + " gross " + val(gross));
    }
    *max_net_leverage = net.get<double>();
    *max_gross_leverage = gross.get<double>();

    // R5
    const auto& conf = m.at("confidence_level");
    if (!conf.is_number() || !(conf.get<double>() > 0.0 && conf.get<double>() < 1.0)) {
        return fail(p + "confidence_level must be in (0, 1), got " + val(conf));
    }
    *confidence_level = conf.get<double>();

    // R6
    const auto& look = m.at("lookback_period");
    if (!look.is_number_integer() || look.get<int>() <= 0) {
        return fail(p + "lookback_period must be a positive integer, got " + val(look));
    }
    *lookback_period = look.get<int>();
    return Result<void>();
}

/// The predicate config_loader.cpp's G-03 check uses: a strategy counts when the book
/// runs it on either side.
bool strategy_enabled(const nlohmann::json& def) {
    if (!def.is_object()) return false;
    const auto flag = [&](const char* key) {
        return def.contains(key) && def.at(key).is_boolean() && def.at(key).get<bool>();
    };
    return flag("enabled_backtest") || flag("enabled_live");
}

}  // namespace

RiskConfig CarverModuleConfig::to_risk_config() const {
    RiskConfig c;
    c.var_limit = var_limit;
    c.jump_risk_limit = jump_risk_limit;
    c.max_correlation = max_correlation;
    c.max_gross_leverage = max_gross_leverage;
    c.max_net_leverage = max_net_leverage;
    c.confidence_level = confidence_level;
    c.lookback_period = lookback_period;
    return c;
}

RiskConfig RiskReportingConfig::to_risk_config() const {
    RiskConfig c;
    c.var_limit = var_limit;
    c.jump_risk_limit = jump_risk_limit;
    c.max_correlation = max_correlation;
    c.max_gross_leverage = max_gross_leverage;
    c.max_net_leverage = max_net_leverage;
    c.confidence_level = confidence_level;
    c.lookback_period = lookback_period;
    return c;
}

nlohmann::json RiskReportingConfig::to_json() const {
    if (overlay_book) {
        return nlohmann::json{{"type", type},
                              {"window", window},
                              {"max_gross_leverage", max_gross_leverage},
                              {"max_net_leverage", max_net_leverage},
                              {"confidence_level", confidence_level},
                              {"lookback_period", lookback_period}};
    }
    return nlohmann::json{{"type", type},
                          {"window", window},
                          {"var_limit", var_limit},
                          {"jump_risk_limit", jump_risk_limit},
                          {"max_correlation", max_correlation},
                          {"max_gross_leverage", max_gross_leverage},
                          {"max_net_leverage", max_net_leverage},
                          {"confidence_level", confidence_level},
                          {"lookback_period", lookback_period}};
}

std::set<RiskTerm> RiskModuleConfig::terms() const {
    if (type == "carver") return {RiskTerm::COMPOSITION, RiskTerm::MAGNITUDE};
    if (type == "constant_scale") return {RiskTerm::MAGNITUDE};
    if (type == "warn" || type == "refuse") return {RiskTerm::CUSTOM};
    return {};  // none
}

bool RiskModuleConfig::can_refuse() const {
    if (type == "refuse") return true;
    if (type == "carver") {
        const auto* c = std::get_if<CarverModuleConfig>(&params);
        return c != nullptr && c->missing_symbol_policy == "refuse";
    }
    return false;
}

nlohmann::json RiskModuleConfig::to_json() const {
    nlohmann::json j = comments;
    j["id"] = id;
    j["type"] = type;
    if (const auto* c = std::get_if<CarverModuleConfig>(&params)) {
        if (!c->overlay_limits()) {
            j["var_limit"] = c->var_limit;
            j["jump_risk_limit"] = c->jump_risk_limit;
            j["max_correlation"] = c->max_correlation;
        }
        j["max_gross_leverage"] = c->max_gross_leverage;
        j["max_net_leverage"] = c->max_net_leverage;
        j["confidence_level"] = c->confidence_level;
        j["lookback_period"] = c->lookback_period;
        j["lookback_unit"] = c->lookback_unit;
        j["min_gate_dates"] = c->min_gate_dates;
        if (c->overlay_limits()) {
            j["R_max"] = c->r_max;
            j["R_jump_max"] = c->r_jump_max;
            j["R_shock_max"] = c->r_shock_max;
            j["per_name_cap"] = c->per_name_cap;
            j["trim_max"] = c->trim_max;
        }
        j["missing_symbol_policy"] = c->missing_symbol_policy;
        if (!c->missing_symbol_policy_reason.empty()) {
            j["_missing_symbol_policy_reason"] = c->missing_symbol_policy_reason;
        }
    } else if (const auto* n = std::get_if<NoneModuleConfig>(&params)) {
        j["_reason"] = n->reason;
        j["_ruled_by"] = n->ruled_by;
        j["_ruled_on"] = n->ruled_on;
    } else if (const auto* cs = std::get_if<ConstantScaleModuleConfig>(&params)) {
        j["scale"] = cs->scale;
        j["every_lap"] = cs->every_lap;
    } else if (const auto* cc = std::get_if<ConditionModuleConfig>(&params)) {
        nlohmann::json cond{{"kind", risk_condition_kind_name(cc->condition.kind)}};
        if (cc->condition.kind != RiskCondition::Kind::ALWAYS &&
            cc->condition.kind != RiskCondition::Kind::NEVER) {
            cond["threshold"] = cc->condition.threshold;
        }
        j["condition"] = std::move(cond);
        j["reason"] = cc->reason;
    }
    return j;
}

nlohmann::json RiskSchema::to_json() const {
    nlohmann::json modules = nlohmann::json::array();
    for (const auto& m : portfolio) modules.push_back(m.to_json());
    nlohmann::json j{{"schema", schema},
                     {"modules", std::move(modules)},
                     {"risk_reporting", reporting.to_json()},
                     {"max_drawdown", max_drawdown}};
    // Absent on the overlay's book, where the key is retired.
    if (max_leverage > 0.0) j["max_leverage"] = max_leverage;
    return j;
}

nlohmann::json RiskSchema::sleeves_to_json() const {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [sid, modules] : sleeves) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& m : modules) arr.push_back(m.to_json());
        j[sid] = std::move(arr);
    }
    return j;
}

bool RiskSchema::is_none() const {
    return sleeves.empty() && portfolio.size() == 1 && portfolio.front().type == "none";
}

Result<void> check_not_schema1(const nlohmann::json& risk, const std::string& portfolio_id) {
    if (!risk.is_object() || risk.contains("schema")) return Result<void>();
    for (const char* key : kFlatSchema1Keys) {
        if (risk.contains(key)) {
            return fail("risk config for " + portfolio_id +
                        ": risk.json is schema 1 (flat gating keys, no \"schema\"/\"modules\"); "
                        "migrate it with: python3 scripts/migrate_risk_json.py <config dir> "
                        "--in-place");
        }
    }
    return Result<void>();
}

Result<RiskModuleConfig> make_none_module(const std::string& reason, const std::string& ruled_by,
                                          const std::string& ruled_on) {
    nlohmann::json m{{"id", "none"},
                     {"type", "none"},
                     {"_reason", reason},
                     {"_ruled_by", ruled_by},
                     {"_ruled_on", ruled_on}};
    ModuleParser parser("risk config for <in code>: ", "risk.modules[0]", /*sleeve_scope=*/false);
    return parser.parse(m);
}

Result<RiskModulePtr> make_risk_module(const RiskModuleConfig& config, Decimal capital) {
    if (config.type == "none") {
        return Result<RiskModulePtr>(RiskModulePtr{});
    }
    try {
        if (const auto* c = std::get_if<CarverModuleConfig>(&config.params)) {
            RiskConfig rc = c->to_risk_config();
            rc.capital = capital;
            auto module = std::make_shared<CarverRiskModule>(config.id, rc, c->min_gate_dates);
            if (c->overlay_limits()) {
                overlay::LimitRatios ratios;
                ratios.risk = c->r_max;
                ratios.jump = c->r_jump_max;
                ratios.shock = c->r_shock_max;
                ratios.gross = c->max_gross_leverage;
                ratios.net = c->max_net_leverage;
                module->set_overlay_limits(ratios);
            }
            return Result<RiskModulePtr>(module);
        }
        if (const auto* cs = std::get_if<ConstantScaleModuleConfig>(&config.params)) {
            return Result<RiskModulePtr>(
                std::make_shared<ConstantScaleRiskModule>(config.id, cs->scale, cs->every_lap));
        }
        if (const auto* cc = std::get_if<ConditionModuleConfig>(&config.params)) {
            if (config.type == "warn") {
                return Result<RiskModulePtr>(
                    std::make_shared<WarnRiskModule>(config.id, cc->condition, cc->reason));
            }
            return Result<RiskModulePtr>(std::make_shared<RefuseOnConditionRiskModule>(
                config.id, cc->condition, cc->reason));
        }
    } catch (const std::exception& e) {
        return make_error<RiskModulePtr>(ErrorCode::INVALID_DATA,
                                         "Failed to build risk module \"" + config.id +
                                             "\": " + e.what(),
                                         "ConfigLoader");
    }
    return make_error<RiskModulePtr>(ErrorCode::INVALID_DATA,
                                     "Unknown risk module type \"" + config.type + "\"",
                                     "ConfigLoader");
}

Result<RiskSchema> parse_risk_schema(const nlohmann::json& risk,
                                     const nlohmann::json& sleeve_block,
                                     const nlohmann::json& strategies,
                                     const std::string& portfolio_id) {
    const std::string prefix = "risk config for " + portfolio_id + ": ";
    auto err = [&](const std::string& tail) {
        return make_error<RiskSchema>(ErrorCode::INVALID_DATA, prefix + tail, "ConfigLoader");
    };
    auto forward = [&](const auto& r) {
        return make_error<RiskSchema>(ErrorCode::INVALID_DATA, r.error()->what(), "ConfigLoader");
    };

    // T0
    auto schema1 = check_not_schema1(risk, portfolio_id);
    if (schema1.is_error()) return forward(schema1);

    if (!risk.is_object()) return err("risk.json must be a JSON object");

    // S1
    if (!risk.contains("schema") || !risk.at("schema").is_number_integer() ||
        risk.at("schema").get<int>() != 2) {
        return err("risk.schema is required and must be 2");
    }
    if (!risk.contains("modules")) {
        return err("risk.modules is required; name the module(s) this portfolio runs, or a single "
                   "{\"type\": \"none\"} carrying _reason, _ruled_by and _ruled_on");
    }

    // S5 at the top level of risk.json
    for (const auto& item : risk.items()) {
        const std::string& key = item.key();
        if (is_comment_key(key)) continue;
        if (key == "schema" || key == "modules" || key == "risk_reporting" ||
            key == "max_drawdown" || key == "max_leverage") {
            continue;
        }
        return err("risk." + key + " is not a top-level key of a schema-2 risk.json");
    }

    RiskSchema out;
    out.schema = 2;

    // R9: required, so the 0.4 / 4.0 struct fallback can no longer size a book nobody
    // configured (max_leverage is what trend_following.cpp:1216 sizes with).
    if (!risk.contains("max_drawdown") || !risk.at("max_drawdown").is_number() ||
        !(risk.at("max_drawdown").get<double>() > 0.0 &&
          risk.at("max_drawdown").get<double>() <= 1.0)) {
        return err("risk.max_drawdown is required and must be in (0, 1]");
    }
    out.max_drawdown = risk.at("max_drawdown").get<double>();
    // max_leverage: required on every book but the overlay's, where it is retired (LOOP_SPEC
    // section 7.7; decided below, once the modules are known). A value that is present is a
    // positive number on either.
    if (risk.contains("max_leverage")) {
        if (!risk.at("max_leverage").is_number() ||
            !(risk.at("max_leverage").get<double>() > 0.0)) {
            return err("risk.max_leverage is required and must be > 0 (it sizes the book: "
                       "trend_following.cpp:1216)");
        }
        out.max_leverage = risk.at("max_leverage").get<double>();
    }

    // S2 + the portfolio scope's modules
    const auto& modules = risk.at("modules");
    if (!modules.is_array()) return err("risk.modules must be an array of module objects");
    if (modules.empty()) {
        return err("risk.modules is empty; an omission and a decision must not share an encoding "
                   "(use {\"type\": \"none\"} with _reason, _ruled_by, _ruled_on)");
    }
    for (size_t i = 0; i < modules.size(); ++i) {
        ModuleParser parser(prefix, "risk.modules[" + std::to_string(i) + "]",
                            /*sleeve_scope=*/false);
        auto m = parser.parse(modules.at(i));
        if (m.is_error()) return forward(m);
        out.portfolio.push_back(m.value());
    }
    // S3: `none` is the whole answer for its scope or it is not an answer.
    for (size_t i = 0; i < out.portfolio.size(); ++i) {
        if (out.portfolio[i].type == "none" && out.portfolio.size() > 1) {
            return err("risk.modules[" + std::to_string(i) +
                       "] is type \"none\" and must be the only module in its scope");
        }
    }

    // The sleeve scope, from portfolio.json
    if (!sleeve_block.is_null()) {
        if (!sleeve_block.is_object()) {
            return err("sleeve_risk_modules must be an object keyed by strategy id");
        }
        for (const auto& item : sleeve_block.items()) {
            const std::string& sid = item.key();
            if (is_comment_key(sid)) continue;
            if (!strategies.is_object() || !strategies.contains(sid) ||
                !strategies.at(sid).is_object()) {
                return err("sleeve_risk_modules." + sid +
                           " does not name a strategy of this portfolio");
            }
            const auto& arr = item.value();
            const std::string path = "sleeve_risk_modules." + sid;
            if (!arr.is_array()) return err(path + " must be an array of module objects");
            if (arr.empty()) {
                return err(path +
                           " is empty; an omission and a decision must not share an encoding "
                           "(use {\"type\": \"none\"} with _reason, _ruled_by, _ruled_on)");
            }
            std::vector<RiskModuleConfig> parsed;
            for (size_t i = 0; i < arr.size(); ++i) {
                ModuleParser parser(prefix, path + "[" + std::to_string(i) + "]",
                                    /*sleeve_scope=*/true);
                auto m = parser.parse(arr.at(i));
                if (m.is_error()) return forward(m);
                parsed.push_back(m.value());
            }
            out.sleeves.emplace(sid, std::move(parsed));
        }
    }

    // S5: one id names one module across the whole book, so a decision row can be read
    // back to the module that made it.
    {
        std::map<std::string, std::string> seen;  // id -> path
        auto note = [&](const std::string& id, const std::string& path) -> Result<void> {
            auto it = seen.find(id);
            if (it != seen.end()) {
                return fail(prefix + "module id \"" + id + "\" is used twice (" + it->second +
                            ", " + path + ")");
            }
            seen.emplace(id, path);
            return Result<void>();
        };
        for (size_t i = 0; i < out.portfolio.size(); ++i) {
            auto r = note(out.portfolio[i].id, "risk.modules[" + std::to_string(i) + "]");
            if (r.is_error()) return forward(r);
        }
        for (const auto& [sid, mods] : out.sleeves) {
            for (size_t i = 0; i < mods.size(); ++i) {
                auto r = note(mods[i].id,
                              "sleeve_risk_modules." + sid + "[" + std::to_string(i) + "]");
                if (r.is_error()) return forward(r);
            }
        }
    }

    // S8: a composition term (correlation / VaR / jump) is scale-invariant, so running
    // it twice along one chain measures the same thing twice and cuts twice for it.
    {
        std::vector<std::pair<std::string, std::string>> portfolio_comp;  // path, id
        for (size_t i = 0; i < out.portfolio.size(); ++i) {
            if (out.portfolio[i].terms().count(RiskTerm::COMPOSITION)) {
                portfolio_comp.emplace_back("risk.modules[" + std::to_string(i) + "]",
                                            out.portfolio[i].id);
            }
        }
        if (portfolio_comp.size() > 1) {
            return err("composition terms (correlation/VaR/jump) are owned by " +
                       portfolio_comp[0].first + " and again by " + portfolio_comp[1].first +
                       "; a composition term may be active once along a sleeve->portfolio chain");
        }
        for (const auto& [sid, mods] : out.sleeves) {
            std::vector<std::string> chain;
            for (size_t i = 0; i < mods.size(); ++i) {
                if (mods[i].terms().count(RiskTerm::COMPOSITION)) {
                    chain.push_back("sleeve_risk_modules." + sid + "[" + std::to_string(i) + "]");
                }
            }
            if (!portfolio_comp.empty() && !chain.empty()) {
                return err("composition terms (correlation/VaR/jump) are owned by " +
                           portfolio_comp[0].first + " and again by " + chain[0] +
                           "; a composition term may be active once along a sleeve->portfolio "
                           "chain");
            }
            if (chain.size() > 1) {
                return err("composition terms (correlation/VaR/jump) are owned by " + chain[0] +
                           " and again by " + chain[1] +
                           "; a composition term may be active once along a sleeve->portfolio "
                           "chain");
            }
        }
    }

    // S9: a REFUSE pins every sleeve to a stored T-1, and nothing seeds those before
    // T-BASE, so on a book with more than one sleeve the refusal cannot be honoured.
    {
        int sleeve_count = 0;
        if (strategies.is_object()) {
            for (const auto& item : strategies.items()) {
                if (strategy_enabled(item.value())) ++sleeve_count;
            }
        }
        if (sleeve_count > 1) {
            auto refuse_error = [&](const std::string& path) {
                return err(path + " can REFUSE, and a book with " + std::to_string(sleeve_count) +
                           " sleeves cannot pin every sleeve to a stored T-1 until T-BASE "
                           "(T-RISK-ARCH_ADVERSARIAL B1)");
            };
            for (size_t i = 0; i < out.portfolio.size(); ++i) {
                if (out.portfolio[i].can_refuse()) {
                    return refuse_error("risk.modules[" + std::to_string(i) + "]");
                }
            }
            for (const auto& [sid, mods] : out.sleeves) {
                for (size_t i = 0; i < mods.size(); ++i) {
                    if (mods[i].can_refuse()) {
                        return refuse_error("sleeve_risk_modules." + sid + "[" +
                                            std::to_string(i) + "]");
                    }
                }
            }
        }
    }

    // R10 (T-6a ADVERSARIAL C-1), the `none` attribution generalised. S3 ties attribution to
    // the literal type "none", so a book whose only module is a `warn`, or a `constant_scale`
    // of 1.0, is just as ungated and carries nobody's name -- and the constructor still prints
    // "Risk manager initialized successfully". A book with no `carver` anywhere in its
    // portfolio chain therefore states who ruled that and when, at the top of risk.json. The
    // lone-`none` book is exempt: its own module already carries all three fields.
    {
        bool has_carver = false;
        for (const auto& m : out.portfolio) {
            if (m.type == "carver") has_carver = true;
        }
        for (const auto& [sid, mods] : out.sleeves) {
            (void)sid;
            for (const auto& m : mods) {
                if (m.type == "carver") has_carver = true;
            }
        }
        const bool lone_none = out.portfolio.size() == 1 && out.portfolio.front().type == "none";
        if (!has_carver && !lone_none) {
            for (const char* key : {"_ruled_by", "_ruled_on"}) {
                if (!risk.contains(key) || !risk.at(key).is_string() ||
                    risk.at(key).get<std::string>().empty()) {
                    return err(std::string("risk.") + key +
                               " is required on a book no module of which is a \"carver\": "
                               "nothing here can cut this book, and that has to be somebody's "
                               "decision rather than an omission");
                }
            }
            if (!is_ymd(risk.at("_ruled_on").get<std::string>())) {
                return err("risk._ruled_on must be YYYY-MM-DD, got " + val(risk.at("_ruled_on")));
            }
        }
    }

    // R10's other half: a warn or refuse whose condition is `never` cannot fire, so it is
    // furniture. Listing it is allowed -- a test book does it -- but it says why.
    {
        auto check_never = [&](const RiskModuleConfig& m,
                               const std::string& path) -> Result<void> {
            const auto* c = std::get_if<ConditionModuleConfig>(&m.params);
            if (c == nullptr || c->condition.kind != RiskCondition::Kind::NEVER) {
                return Result<void>();
            }
            const bool has = m.comments.contains("_never_reason") &&
                             m.comments.at("_never_reason").is_string() &&
                             !m.comments.at("_never_reason").get<std::string>().empty();
            if (!has) {
                return fail(prefix + path +
                            " has condition kind \"never\", so it can never fire; say why it is "
                            "listed in a non-empty _never_reason");
            }
            return Result<void>();
        };
        for (size_t i = 0; i < out.portfolio.size(); ++i) {
            auto r = check_never(out.portfolio[i], "risk.modules[" + std::to_string(i) + "]");
            if (r.is_error()) return forward(r);
        }
        for (const auto& [sid, mods] : out.sleeves) {
            for (size_t i = 0; i < mods.size(); ++i) {
                auto r = check_never(mods[i],
                                     "sleeve_risk_modules." + sid + "[" + std::to_string(i) + "]");
                if (r.is_error()) return forward(r);
            }
        }
    }

    // C1: the reporter. Required on every book, because one config directory serves
    // both the live runner (which snapshots the book with it) and the backtest runner.
    if (!risk.contains("risk_reporting")) {
        return err("risk.risk_reporting is required (the live runners' reporter and the equity "
                   "start-up guard read it through AppConfig::risk_config)");
    }
    const auto& rep = risk.at("risk_reporting");
    if (!rep.is_object()) return err("risk.risk_reporting must be an object");
    for (const auto& item : rep.items()) {
        const std::string& key = item.key();
        if (is_comment_key(key)) continue;
        if (std::find(std::begin(kReportingKeys), std::end(kReportingKeys), key) ==
            std::end(kReportingKeys)) {
            return err("risk.risk_reporting." + key +
                       " is not a key of a \"carver\" risk reporter");
        }
    }
    // The book of the overlay: its reporter's block drops the old gate's three limits with it.
    bool overlay_book = false;
    for (const auto& module : out.portfolio) {
        const auto* c = std::get_if<CarverModuleConfig>(&module.params);
        overlay_book = overlay_book || (c != nullptr && c->overlay_limits());
    }
    if (overlay_book && risk.contains("max_leverage")) {
        return err("risk.max_leverage is retired on a book whose carver module carries the "
                   "overlay's limits (R_max, R_jump_max, R_shock_max): remove the key");
    }
    if (!overlay_book && !risk.contains("max_leverage")) {
        return err("risk.max_leverage is required and must be > 0 (it sizes the book: "
                   "trend_following.cpp:1216)");
    }
    for (const char* key : kReportingKeys) {
        const bool retired = overlay_book &&
                             std::find_if(std::begin(kRetiredGateKeys), std::end(kRetiredGateKeys),
                                          [&](const char* k) { return std::string(k) == key; }) !=
                                 std::end(kRetiredGateKeys);
        if (retired) {
            if (rep.contains(key)) {
                return err("risk.risk_reporting." + std::string(key) +
                           " is retired on a book whose carver module carries the overlay's "
                           "limits (R_max, R_jump_max, R_shock_max): remove the key");
            }
            continue;
        }
        if (!rep.contains(key)) {
            return err("risk.risk_reporting." + std::string(key) +
                       " is required for a \"carver\" risk reporter (schema 2 has no defaults)");
        }
    }
    if (!rep.at("type").is_string() || rep.at("type").get<std::string>() != "carver" ||
        !rep.at("window").is_string() || rep.at("window").get<std::string>() != "all_bars") {
        return err("risk.risk_reporting.type must be \"carver\" and window \"all_bars\" on day "
                   "one (HD Q5)");
    }
    {
        auto r = parse_gating_fields(
            prefix, rep, "risk.risk_reporting", &out.reporting.var_limit,
            &out.reporting.jump_risk_limit, &out.reporting.max_correlation,
            &out.reporting.max_gross_leverage, &out.reporting.max_net_leverage,
            &out.reporting.confidence_level, &out.reporting.lookback_period, overlay_book);
        if (r.is_error()) return forward(r);
    }
    out.reporting.type = "carver";
    out.reporting.window = "all_bars";
    out.reporting.overlay_book = overlay_book;

    // While a carver module gates the book, the reporter measures the same book with the
    // same numbers: a reporter that has drifted from the gate publishes a risk figure
    // the book was never held to.
    for (size_t i = 0; i < out.portfolio.size(); ++i) {
        const auto* c = std::get_if<CarverModuleConfig>(&out.portfolio[i].params);
        if (c == nullptr) continue;
        const std::string path = "risk.modules[" + std::to_string(i) + "]";
        const std::pair<const char*, std::pair<double, double>> fields[] = {
            {"var_limit", {out.reporting.var_limit, c->var_limit}},
            {"jump_risk_limit", {out.reporting.jump_risk_limit, c->jump_risk_limit}},
            {"max_correlation", {out.reporting.max_correlation, c->max_correlation}},
            {"max_gross_leverage", {out.reporting.max_gross_leverage, c->max_gross_leverage}},
            {"max_net_leverage", {out.reporting.max_net_leverage, c->max_net_leverage}},
            {"confidence_level", {out.reporting.confidence_level, c->confidence_level}},
            {"lookback_period",
             {static_cast<double>(out.reporting.lookback_period),
              static_cast<double>(c->lookback_period)}}};
        for (const auto& [name, values] : fields) {
            if (values.first != values.second) {
                return err("risk.risk_reporting." + std::string(name) + " = " +
                           nlohmann::json(values.first).dump() + " differs from " + path + "." +
                           name + " = " + nlohmann::json(values.second).dump() +
                           "; on day one the reporter mirrors the gate (HD Q5)");
            }
        }
    }

    return Result<RiskSchema>(std::move(out));
}

}  // namespace trade_ngin
