// include/trade_ngin/risk/risk_module_config.hpp
//
// Schema 2 of risk.json: the parsed, validated description of the risk modules a
// book runs, per portfolio and per sleeve.
//
// Schema 1 resolved a single flat RiskConfig out of three files with two layers of
// silent defaults (config/defaults.json risk_defaults, then the RiskConfig struct's
// own member initialisers) and switched the whole gate on a `use_risk_management`
// boolean. Two different things -- "this book has no risk layer" and "somebody
// forgot the key" -- had the same encoding, and a value nobody wrote could end up
// gating a live book. Schema 2 has no defaults at all: every gating value is
// written literally in the book's own risk.json, a book that runs no risk layer
// says so with a `none` module carrying who ruled it and when, and every rule below
// is a load ERROR rather than a fallback.
#pragma once

#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/risk/basic_risk_modules.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/risk/risk_module.hpp"

namespace trade_ngin {

/// The Carver gate's parameters, written literally per book (no defaults layer).
struct CarverModuleConfig {
    double var_limit{0.0};
    double jump_risk_limit{0.0};
    double max_correlation{0.0};
    double max_gross_leverage{0.0};
    double max_net_leverage{0.0};
    double confidence_level{0.0};
    int lookback_period{0};
    /// Always "dates": the Carver window keeps the newest lookback_period DISTINCT dates (a
    /// date on which only some symbols printed still counts), of which the sparse-date filter
    /// keeps the complete ones -- about 187-200 of 252 on the shipped futures books. The key is
    /// validated and never read; the loader refuses the old "bars", which no longer describes
    /// the code.
    std::string lookback_unit{"dates"};
    /// Fewer complete dates than this in the window and the module reports `blind` on
    /// its decision: it measured, but not on enough dates for the measurement to mean
    /// anything.
    int min_gate_dates{21};
    /// The overlay's three risk limits as RATIOS to tau (LOOP_SPEC sections 4, 7.5.1 and 12):
    /// R_max, R_jump_max and R_shock_max. All three or none. Present, the module reads the book in
    /// capital terms (risk/overlay.hpp) with max_gross_leverage and max_net_leverage as L_max and
    /// L_net_max; absent (0), it reads it as before. A futures book requires them
    /// (ConfigLoader::require_loop_keys).
    double r_max{0.0};
    double r_jump_max{0.0};
    double r_shock_max{0.0};
    bool overlay_limits() const { return r_max > 0.0 && r_jump_max > 0.0 && r_shock_max > 0.0; }
    /// With the overlay's limits (LOOP_SPEC sections 5.3, 6.4 and 7.7), both required: the
    /// per-name cap L on the sizing capital and the most contracts the trim removes in a day.
    /// The three limits of the old gate (var_limit, jump_risk_limit, max_correlation) are RETIRED
    /// on such a module and on its book's risk_reporting block: the loader refuses them there,
    /// and the fields above hold RiskConfig's defaults.
    double per_name_cap{0.0};
    int trim_max{0};
    /// What the gate does with a configured symbol that has no bar in the window.
    /// "ignore" is today's fail-open behaviour and has to say why it is chosen.
    std::string missing_symbol_policy{"ignore"};
    std::string missing_symbol_policy_reason;

    /// The seven gating fields, in a RiskConfig. capital and version keep the struct's
    /// defaults: the caller sets capital, exactly as every runner does today.
    RiskConfig to_risk_config() const;
};

/// "This book runs no risk layer", with the ruling that made it so. Never a default.
struct NoneModuleConfig {
    std::string reason;
    std::string ruled_by;
    std::string ruled_on;  ///< YYYY-MM-DD
};

/// ConstantScaleRiskModule's constructor arguments.
struct ConstantScaleModuleConfig {
    double scale{1.0};
    bool every_lap{false};
};

/// WarnRiskModule's / RefuseOnConditionRiskModule's constructor arguments.
struct ConditionModuleConfig {
    RiskCondition condition;
    std::string reason;
};

/// One module assignment: what it is, what it is called, and its parameters.
struct RiskModuleConfig {
    std::string id;
    /// "carver", "none", "constant_scale", "warn", "refuse" -- the type() strings of the
    /// modules commit 4 shipped, so a config names a module by the name the module
    /// answers to.
    std::string type;
    std::variant<CarverModuleConfig, NoneModuleConfig, ConstantScaleModuleConfig,
                 ConditionModuleConfig>
        params;
    /// The `_`-prefixed keys of the module object, kept verbatim so a round trip does
    /// not drop a rationale somebody wrote down.
    nlohmann::json comments = nlohmann::json::object();

    std::set<RiskTerm> terms() const;
    bool can_refuse() const;
    nlohmann::json to_json() const;
};

/// The reporting RiskManager the live runners snapshot the book with (it stores the
/// risk columns of live_results and the email's risk block). Separate from the gate
/// because a book may stop gating without losing its measurement.
struct RiskReportingConfig {
    std::string type{"carver"};
    std::string window{"all_bars"};
    double var_limit{0.0};
    double jump_risk_limit{0.0};
    double max_correlation{0.0};
    double max_gross_leverage{0.0};
    double max_net_leverage{0.0};
    double confidence_level{0.0};
    int lookback_period{0};

    /// The book's carver module carries the overlay's limits: var_limit, jump_risk_limit and
    /// max_correlation are retired in this block (they hold RiskConfig's defaults, unread) and
    /// to_json leaves them out.
    bool overlay_book{false};
    RiskConfig to_risk_config() const;
    nlohmann::json to_json() const;
};

/// A whole book's risk.json plus its portfolio.json sleeve block, parsed and validated.
struct RiskSchema {
    int schema{2};
    std::vector<RiskModuleConfig> portfolio;
    std::map<std::string, std::vector<RiskModuleConfig>> sleeves;
    RiskReportingConfig reporting;
    double max_drawdown{0.0};
    double max_leverage{0.0};

    /// The risk.json object: schema, modules, risk_reporting, max_drawdown, max_leverage.
    nlohmann::json to_json() const;
    /// The portfolio.json `sleeve_risk_modules` object (empty object when no sleeve has
    /// modules). Kept out of to_json(): the two live in different files, and nesting the
    /// sleeve block inside a strategies entry would move live_run_metadata.strategy_configs.
    nlohmann::json sleeves_to_json() const;
    /// True when no module gates anything: a single portfolio-scope `none` and no sleeve
    /// modules.
    bool is_none() const;
};

/// T0, run before anything else so an unmigrated production box gets the one message
/// that tells it what to do. An error iff `risk` has no "schema" key and carries at
/// least one of schema 1's flat gating keys.
Result<void> check_not_schema1(const nlohmann::json& risk, const std::string& portfolio_id);

/**
 * @brief Parse and validate a book's schema-2 risk configuration.
 *
 * Fails closed on the first broken rule; there is no path on which a missing or
 * unreadable value is replaced by one the caller did not write.
 *
 * @param risk         the merged `risk` object (risk.json)
 * @param sleeve_block portfolio.json's `sleeve_risk_modules` (null/absent = no sleeve modules)
 * @param strategies   portfolio.json's `strategies` object (sleeve keys and the sleeve count)
 * @param portfolio_id names the book in every message
 */
Result<RiskSchema> parse_risk_schema(const nlohmann::json& risk,
                                     const nlohmann::json& sleeve_block,
                                     const nlohmann::json& strategies,
                                     const std::string& portfolio_id);

/// A code-built `none` assignment (bt_equity_validation), running S3's checks so an
/// in-code decision has to carry the same attribution a config one does.
Result<RiskModuleConfig> make_none_module(const std::string& reason, const std::string& ruled_by,
                                          const std::string& ruled_on);

/**
 * @brief Build the module a validated assignment names.
 * @param capital the book's capital, set on the Carver module's RiskConfig exactly as
 *        every runner sets it today.
 * @return a null pointer, and no error, for type "none": it is an assignment that
 *         builds nothing.
 */
Result<RiskModulePtr> make_risk_module(const RiskModuleConfig& config, Decimal capital);

}  // namespace trade_ngin
