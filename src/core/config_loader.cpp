// src/core/config_loader.cpp

#include "trade_ngin/core/config_loader.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

Result<nlohmann::json> ConfigLoader::load_json_file(const std::filesystem::path& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        return make_error<nlohmann::json>(ErrorCode::FILE_NOT_FOUND,
                                          "Failed to open config file: " + file_path.string(),
                                          "ConfigLoader");
    }

    try {
        nlohmann::json j;
        file >> j;
        return j;
    } catch (const nlohmann::json::parse_error& e) {
        return make_error<nlohmann::json>(
            ErrorCode::JSON_PARSE_ERROR,
            "Failed to parse JSON file " + file_path.string() + ": " + e.what(), "ConfigLoader");
    } catch (const std::exception& e) {
        return make_error<nlohmann::json>(ErrorCode::FILE_IO_ERROR,
                                          "Error reading config file " + file_path.string() + ": " +
                                              e.what(),
                                          "ConfigLoader");
    }
}

void ConfigLoader::merge_json(nlohmann::json& target, const nlohmann::json& source) {
    for (auto it = source.begin(); it != source.end(); ++it) {
        const auto& key = it.key();
        const auto& value = it.value();

        if (target.contains(key) && target[key].is_object() && value.is_object()) {
            // Recursive merge for nested objects
            merge_json(target[key], value);
        } else {
            // Override or add
            target[key] = value;
        }
    }
}

namespace {

/// Walks every object below `node` looking for one key, and names where it found it.
/// Only the PATH is ever reported, never a value: the same tree carries the database
/// and email passwords.
bool find_key(const nlohmann::json& node, const std::string& key, const std::string& path,
              std::string* found) {
    if (node.is_object()) {
        for (const auto& item : node.items()) {
            const std::string child = path.empty() ? item.key() : path + "." + item.key();
            if (item.key() == key) {
                *found = child;
                return true;
            }
            if (find_key(item.value(), key, child, found)) return true;
        }
    } else if (node.is_array()) {
        for (size_t i = 0; i < node.size(); ++i) {
            if (find_key(node.at(i), key, path + "[" + std::to_string(i) + "]", found)) return true;
        }
    }
    return false;
}

/**
 * @brief S7 -- the keys schema 2 removed are load ERRORS, not ignored leftovers.
 *
 * Ignoring them is what makes a migration silently half-applied: a `use_risk_management:
 * false` left behind in a file nobody re-read would read as "risk is off" to a human and
 * as nothing at all to the loader, and the book would gate while its config says it does
 * not. Each message names the key's path and what replaced it.
 *
 * removed-key guard: delete after the first production run on schema 2 (the lead names
 * the release; LEAD_RULINGS_C7 item 15 records it as an open question for HD).
 */
Result<void> check_removed_keys(const nlohmann::json& merged, const std::string& portfolio_id) {
    const std::string config_prefix = "config for " + portfolio_id + ": ";
    std::string where;
    if (find_key(merged, "use_risk_management", "", &where)) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            config_prefix + where +
                " (use_risk_management) was removed in schema 2; risk is assigned by risk.json "
                "\"modules\". Delete the key (a leftover false would silently turn risk back on, "
                "T-RISK-ARCH_ADVERSARIAL E2)",
            "ConfigLoader");
    }
    if (merged.contains("risk_defaults")) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            config_prefix +
                "risk_defaults was removed in schema 2; every gating value is written literally "
                "in each portfolio's risk.json (run scripts/migrate_risk_json.py)",
            "ConfigLoader");
    }
    if (merged.contains("strategy_defaults") && merged.at("strategy_defaults").is_object() &&
        merged.at("strategy_defaults").contains("use_optimization")) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            config_prefix +
                "strategy_defaults.use_optimization moved to portfolio.json \"use_optimization\" "
                "in schema 2",
            "ConfigLoader");
    }
    if (merged.contains("risk")) {
        for (const char* key : {"corr_shock_threshold", "jump_shock_threshold"}) {
            if (find_key(merged.at("risk"), key, "risk", &where)) {
                return make_error<void>(
                    ErrorCode::INVALID_DATA,
                    "risk config for " + portfolio_id + ": " + where +
                        " has had no reader since the carver_shock methods were deleted; delete "
                        "it",
                    "ConfigLoader");
            }
        }
    }
    return Result<void>();
}

}  // namespace

Result<AppConfig> ConfigLoader::extract_config(const nlohmann::json& merged) {
    try {
        AppConfig config;

        // Portfolio identification
        if (merged.contains("portfolio_id")) {
            config.portfolio_id = merged.at("portfolio_id").get<std::string>();
        }

        // Capital settings
        if (merged.contains("initial_capital")) {
            config.initial_capital = merged.at("initial_capital").get<double>();
        }
        // "reserve_capital_pct" (OPT-N4) is no longer read: nothing sized on it (J3, deleted
        // T-7a). A deployed portfolio.json that still carries the key (or "reserve_capital",
        // the name the stored run metadata used) loads unchanged, the key is ignored, and the
        // run says so once.
        for (const char* key : {"reserve_capital_pct", "reserve_capital"}) {
            if (merged.contains(key)) {
                WARN("config for " + config.portfolio_id + ": \"" + key +
                     "\" was deleted by J3 and is no longer read (nothing sized on it); the "
                     "value is ignored. Delete the key from portfolio.json");
                break;
            }
        }

        // Database configuration
        if (merged.contains("database")) {
            config.database.from_json(merged.at("database"));
        }

        // Execution configuration
        if (merged.contains("execution")) {
            config.execution.from_json(merged.at("execution"));
        }

        // Optimization configuration
        if (merged.contains("optimization")) {
            const auto& optimization = merged.at("optimization");
            config.opt_config.from_json(optimization);
            // LOOP_SPEC section 7.7: the one pass's keys, strictly when present (a futures book
            // requires them: require_loop_keys), and the optimiser's retired keys, noted.
            config.has_cost_penalty_scalar = optimization.contains("cost_penalty_scalar");
            for (const char* key : {"sign_close_band", "b_sigma_floor"}) {
                if (!optimization.contains(key)) continue;
                const auto& v = optimization.at(key);
                if (!v.is_number() || !(v.get<double>() > 0.0)) {
                    return make_error<AppConfig>(
                        ErrorCode::INVALID_DATA,
                        "config for " + config.portfolio_id + ": defaults.json optimization." +
                            key + " must be a positive number, got " + v.dump(),
                        "ConfigLoader");
                }
                (std::string(key) == "sign_close_band" ? config.sign_close_band
                                                       : config.b_sigma_floor) = v.get<double>();
            }
            for (const char* key : {"tau", "asymmetric_risk_buffer", "buffer_size_factor"}) {
                if (optimization.contains(key)) {
                    config.retired_loop_keys.push_back(std::string("defaults.json optimization: ") +
                                                       key);
                }
            }
        }
        if (merged.contains("strategy_defaults") && merged.at("strategy_defaults").is_object()) {
            for (const char* key : {"carver_buffer_floor", "carver_buffer_position_factor"}) {
                if (merged.at("strategy_defaults").contains(key)) {
                    config.retired_loop_keys.push_back(
                        std::string("defaults.json strategy_defaults: ") + key);
                }
            }
        }
        if (merged.contains("strategies") && merged.at("strategies").is_object()) {
            const nlohmann::json& sleeves = merged.at("strategies");
            for (const auto& sleeve : sleeves.items()) {
                if (!sleeve.value().is_object() || !sleeve.value().contains("config") ||
                    !sleeve.value().at("config").is_object()) {
                    continue;
                }
                for (const char* key :
                     {"weight", "max_symbol_concentration", "use_position_buffering",
                      "carver_buffer_floor", "carver_buffer_position_factor"}) {
                    if (sleeve.value().at("config").contains(key)) {
                        config.retired_loop_keys.push_back("portfolio.json strategies." +
                                                           sleeve.key() + ".config: " + key);
                    }
                }
            }
        }
        // Set capital in opt_config
        config.opt_config.capital = config.initial_capital;

        // Risk configuration - schema 2. Named first, so an unmigrated production box
        // is told what to run instead of being told about a key it never wrote.
        {
            auto schema1 = check_not_schema1(merged.value("risk", nlohmann::json::object()),
                                             config.portfolio_id);
            if (schema1.is_error()) {
                return make_error<AppConfig>(ErrorCode::INVALID_DATA, schema1.error()->what(),
                                             "ConfigLoader");
            }
            auto removed = check_removed_keys(merged, config.portfolio_id);
            if (removed.is_error()) {
                return make_error<AppConfig>(ErrorCode::INVALID_DATA, removed.error()->what(),
                                             "ConfigLoader");
            }
        }

        // P1: portfolio.json owns use_optimization now. Required and boolean; there is no
        // default, because a default is how an optimizer gets switched on for a book
        // nobody decided to switch it on for.
        if (!merged.contains("use_optimization") || !merged.at("use_optimization").is_boolean()) {
            return make_error<AppConfig>(
                ErrorCode::INVALID_DATA,
                "config for " + config.portfolio_id +
                    ": portfolio.json must set \"use_optimization\" (true or false) at its top "
                    "level; schema 2 has no default",
                "ConfigLoader");
        }
        config.use_optimization = merged.at("use_optimization").get<bool>();

        // T-6c commit B: the PortfolioManager's covariance history length, in prices per
        // symbol. Optional; absent means 756 (the trend sleeve's own history cap). A value
        // that is not a whole number of at least 2 is refused: one price gives no return.
        if (merged.contains("covariance_history_prices")) {
            const auto& v = merged.at("covariance_history_prices");
            if (!v.is_number_integer() || v.get<int64_t>() < 2) {
                return make_error<AppConfig>(
                    ErrorCode::INVALID_DATA,
                    "config for " + config.portfolio_id +
                        ": portfolio.json \"covariance_history_prices\" must be a whole number "
                        "of at least 2 (prices per symbol kept for the optimiser's covariance; "
                        "absent means 756), got " + v.dump(),
                    "ConfigLoader");
            }
            config.covariance_history_prices = v.get<size_t>();
        }

        // T-7b-1 7d: how many union dates a covariance participant's last close may trail the
        // newest before the optimiser leaves it out of the date intersection. Optional; absent
        // means 5. 0 is allowed (every participant must print on the newest date).
        if (merged.contains("covariance_stale_dates")) {
            const auto& v = merged.at("covariance_stale_dates");
            if (!v.is_number_integer() || v.get<int64_t>() < 0) {
                return make_error<AppConfig>(
                    ErrorCode::INVALID_DATA,
                    "config for " + config.portfolio_id +
                        ": portfolio.json \"covariance_stale_dates\" must be a whole number of "
                        "at least 0 (union dates a covariance participant's last date may trail "
                        "the newest before the optimiser leaves it out of the date intersection; "
                        "absent means 5), got " + v.dump(),
                    "ConfigLoader");
            }
            config.covariance_stale_dates = v.get<size_t>();
        }

        // LOOP_SPEC sections 2.5 and 7.7 (D40): the equity slow rule. Parsed strictly when present;
        // the futures runners require it (require_loop_keys).
        if (merged.contains("equity_slow_rule")) {
            const auto& v = merged.at("equity_slow_rule");
            auto bad = [&](const std::string& what) {
                return make_error<AppConfig>(
                    ErrorCode::INVALID_DATA,
                    "config for " + config.portfolio_id +
                        ": portfolio.json \"equity_slow_rule\" " + what +
                        " (expected {\"symbols\": [\"MES\", ...], \"pairs\": [[32, 128], [64, 256]]}), "
                        "got " + v.dump(),
                    "ConfigLoader");
            };
            if (!v.is_object() || !v.contains("symbols") || !v.contains("pairs")) {
                return bad("must be an object with \"symbols\" and \"pairs\"");
            }
            const auto& symbols = v.at("symbols");
            const auto& pairs = v.at("pairs");
            if (!symbols.is_array() || symbols.empty()) {
                return bad("needs a non-empty \"symbols\" list");
            }
            if (!pairs.is_array() || pairs.empty()) {
                return bad("needs a non-empty \"pairs\" list");
            }
            EquitySlowRule rule;
            rule.present = true;
            for (const auto& symbol : symbols) {
                if (!symbol.is_string() || symbol.get<std::string>().empty()) {
                    return bad("names a symbol that is not a non-empty string");
                }
                rule.symbols.push_back(symbol.get<std::string>());
            }
            for (const auto& pair : pairs) {
                if (!pair.is_array() || pair.size() != 2 || !pair[0].is_number_integer() ||
                    !pair[1].is_number_integer() || pair[0].get<int64_t>() <= 0 ||
                    pair[1].get<int64_t>() <= 0) {
                    return bad("names a pair that is not two positive whole numbers");
                }
                rule.pairs.emplace_back(pair[0].get<int>(), pair[1].get<int>());
            }
            config.equity_slow_rule = rule;
        }

        // Declared vendor id relabellings: optional; parsed strictly when present.
        if (merged.contains("instrument_id_relabels")) {
            const auto& v = merged.at("instrument_id_relabels");
            auto bad = [&](const std::string& what) {
                return make_error<AppConfig>(
                    ErrorCode::INVALID_DATA,
                    "config for " + config.portfolio_id +
                        ": portfolio.json \"instrument_id_relabels\" " + what +
                        " (expected [{\"symbol\": \"MES\", \"date\": \"2026-02-22\", \"from\": "
                        "\"42140878\", \"to\": \"42003800\"}]), got " + v.dump(),
                    "ConfigLoader");
            };
            if (!v.is_array()) return bad("must be a list");
            for (const auto& entry : v) {
                for (const char* key : {"symbol", "date", "from", "to"}) {
                    if (!entry.is_object() || !entry.contains(key) || !entry.at(key).is_string()) {
                        return bad("names an entry without string \"symbol\", \"date\", \"from\" and \"to\"");
                    }
                }
                config.instrument_id_relabels.push_back(
                    {entry.at("symbol").get<std::string>(), entry.at("date").get<std::string>(),
                     entry.at("from").get<std::string>(), entry.at("to").get<std::string>()});
            }
            try {
                ListingDates::validate_relabels(config.instrument_id_relabels);
            } catch (const std::invalid_argument& e) {
                return bad(std::string("is not usable: ") + e.what());
            }
        }

        // Listing dates: optional; parsed strictly when present.
        if (merged.contains("listing_dates")) {
            const auto& v = merged.at("listing_dates");
            auto bad = [&](const std::string& what) {
                return make_error<AppConfig>(
                    ErrorCode::INVALID_DATA,
                    "config for " + config.portfolio_id + ": portfolio.json \"listing_dates\" " +
                        what +
                        " (expected {\"contracts\": [{\"symbol\": \"MES\", \"listed\": "
                        "\"2019-05-06\", \"before\": \"ES\", \"ratio\": 10}]}), got " + v.dump(),
                    "ConfigLoader");
            };
            if (!v.is_object() || !v.contains("contracts") || !v.at("contracts").is_array()) {
                return bad("must be an object with a \"contracts\" list");
            }
            if (v.contains("switch_rule") &&
                (!v.at("switch_rule").is_string() ||
                 !parse_listing_switch_rule(v.at("switch_rule").get<std::string>(),
                                            &config.listing_switch_rule))) {
                return bad("\"switch_rule\" must be \"close_reenter\", \"convert\", \"open_at_target\" or "
                           "\"carry_to_target\"");
            }
            for (const auto& entry : v.at("contracts")) {
                if (!entry.is_object() || !entry.contains("symbol") || !entry.contains("listed") ||
                    !entry.contains("before") || !entry.at("symbol").is_string() ||
                    !entry.at("listed").is_string() || !entry.at("before").is_string() ||
                    !entry.contains("ratio") || !entry.at("ratio").is_number()) {
                    return bad("names a contract without string \"symbol\", \"listed\" and "
                               "\"before\" and a number \"ratio\"");
                }
                config.listing_dates.push_back({entry.at("symbol").get<std::string>(),
                                                entry.at("before").get<std::string>(),
                                                entry.at("listed").get<std::string>(),
                                                entry.at("ratio").get<double>()});
            }
            try {
                ListingDates::validate(config.listing_dates);  // the runner switches it on
            } catch (const std::invalid_argument& e) {
                return bad(std::string("is not usable: ") + e.what());
            }
        }

        // LOOP_SPEC sections 3.1 and 7.7 (D19): the sizing mode and the starting capital. Parsed
        // strictly when present; the futures runners require both (require_loop_keys).
        if (merged.contains("sizing_mode")) {
            const auto& v = merged.at("sizing_mode");
            if (!v.is_string() || v.get<std::string>() != "half_compounding") {
                return make_error<AppConfig>(
                    ErrorCode::INVALID_DATA,
                    "config for " + config.portfolio_id +
                        ": portfolio.json \"sizing_mode\" must be \"half_compounding\" (the one "
                        "sizing mode), got " + v.dump(),
                    "ConfigLoader");
            }
            config.sizing_mode = v.get<std::string>();
        }
        if (merged.contains("starting_capital")) {
            const auto& v = merged.at("starting_capital");
            if (!v.is_number() || !(v.get<double>() > 0.0)) {
                return make_error<AppConfig>(
                    ErrorCode::INVALID_DATA,
                    "config for " + config.portfolio_id +
                        ": portfolio.json \"starting_capital\" must be a positive number, got " +
                        v.dump(),
                    "ConfigLoader");
            }
            config.starting_capital = v.get<double>();
            if (config.starting_capital != config.initial_capital) {
                return make_error<AppConfig>(
                    ErrorCode::INVALID_DATA,
                    "config for " + config.portfolio_id +
                        ": portfolio.json \"starting_capital\" (" + v.dump() +
                        ") must equal \"initial_capital\" (" +
                        std::to_string(config.initial_capital) +
                        "): the starting capital of the sizing is the book's capital",
                    "ConfigLoader");
            }
        }

        if (!merged.contains("risk")) {
            return make_error<AppConfig>(ErrorCode::INVALID_DATA,
                                         "risk config for " + config.portfolio_id +
                                             ": risk.json is required",
                                         "ConfigLoader");
        }
        auto schema = parse_risk_schema(
            merged.at("risk"), merged.value("sleeve_risk_modules", nlohmann::json()),
            merged.value("strategies", nlohmann::json::object()), config.portfolio_id);
        if (schema.is_error()) {
            return make_error<AppConfig>(ErrorCode::INVALID_DATA, schema.error()->what(),
                                         "ConfigLoader");
        }
        config.risk_schema = schema.value();
        // The reporting block is the single source of AppConfig::risk_config: every
        // snapshot RiskManager and both equity start-up guards read it, and it survives a
        // book whose gate becomes `none`.
        config.risk_config = config.risk_schema.reporting.to_risk_config();

        // Additional risk limits
        const auto& risk = merged.at("risk");
        if (risk.contains("max_drawdown")) {
            config.max_drawdown = risk.at("max_drawdown").get<double>();
        }
        if (risk.contains("max_leverage")) {
            config.max_leverage = risk.at("max_leverage").get<double>();
        }
        // Set capital in risk_config
        config.risk_config.capital = Decimal(config.initial_capital);

        // Backtest settings
        if (merged.contains("backtest")) {
            config.backtest.from_json(merged.at("backtest"));
        }

        // Live settings
        if (merged.contains("live")) {
            config.live.from_json(merged.at("live"));
        }

        // Strategy defaults
        if (merged.contains("strategy_defaults")) {
            config.strategy_defaults.from_json(merged.at("strategy_defaults"));
        }

        // Email configuration
        if (merged.contains("email")) {
            config.email.from_json(merged.at("email"));
        }

        // Strategies (raw JSON for factory)
        if (merged.contains("strategies")) {
            config.strategies_config = merged.at("strategies");
        }

        return config;

    } catch (const std::exception& e) {
        return make_error<AppConfig>(ErrorCode::INVALID_DATA,
                                     "Failed to extract config: " + std::string(e.what()),
                                     "ConfigLoader");
    }
}

Result<void> ConfigLoader::validate_config(const AppConfig& config) {
    if (config.portfolio_id.empty()) {
        return make_error<void>(ErrorCode::INVALID_DATA, "Missing portfolio_id", "ConfigLoader");
    }
    if (config.database.host.empty() || config.database.username.empty() ||
        config.database.password.empty() || config.database.name.empty()) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "Missing required database configuration fields",
                                "ConfigLoader");
    }
    if (config.initial_capital <= 0.0) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "initial_capital must be positive",
                                "ConfigLoader");
    }
    if (config.strategies_config.is_null() || !config.strategies_config.is_object() ||
        config.strategies_config.empty()) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "strategies configuration is missing or empty",
                                "ConfigLoader");
    }

    // G-03: the lookback window has to be long enough for the strategies that
    // read it, and nothing checked that it was.
    //
    // config_template/defaults.json states the coupling in a COMMENT -- "Must
    // match backtest.lookback_years (2 yrs = 730 days). Strategy needs 256+
    // trading days for longest EMA and 252 for vol_lookback_long" -- and a
    // comment is not a check. A short window does not fail: the longest EMA
    // never warms up and emits a signal that looks exactly like a real one.
    //
    // The requirement is DERIVED from the enabled strategies' own ema_windows
    // rather than hardcoded, because the strategies do not agree on it:
    // TrendFollowing tops out at 256, Fast at 64, and Slow carries a {128, 512}
    // pair. A single constant would either nag every run of a book that does not
    // enable Slow, or miss the case of a book that does. The template's own
    // "256+" note is understated for exactly that reason.
    //
    // WARN ONLY, deliberately: a refusal would abort runs that work today, which
    // is a behaviour change and not this batch's business. The point is that a
    // short window now says so in the log instead of being invisible.
    {
        constexpr int kTradingDaysPerYear = 252;
        // Documented floor, used when a strategy does not spell out its windows.
        constexpr int kDefaultLongestEma = 256;

        int required = 0;
        std::string driver;
        for (const auto& entry : config.strategies_config.items()) {
            const auto& def = entry.value();
            // A documentation key such as "_description" is a string, not a
            // strategy definition. value() would throw on it; the runners
            // themselves use contains() and skip such entries, so do the same.
            if (!def.is_object()) continue;
            const auto flag = [&](const char* key) {
                return def.contains(key) && def.at(key).is_boolean() && def.at(key).get<bool>();
            };
            const bool enabled = flag("enabled_backtest") || flag("enabled_live");
            if (!enabled) continue;

            int longest = kDefaultLongestEma;
            if (def.contains("config") && def.at("config").contains("ema_windows")) {
                longest = 0;
                for (const auto& pair : def.at("config").at("ema_windows")) {
                    if (pair.is_array() && pair.size() == 2 && pair.at(1).is_number_integer()) {
                        longest = std::max(longest, pair.at(1).get<int>());
                    }
                }
                if (longest == 0) longest = kDefaultLongestEma;
            }
            if (longest > required) {
                required = longest;
                driver = entry.key();
            }
        }
        if (required == 0) required = kDefaultLongestEma;

        const int available = config.backtest.lookback_years * kTradingDaysPerYear;
        if (available < required) {
            WARN("backtest.lookback_years=" + std::to_string(config.backtest.lookback_years) +
                 " gives about " + std::to_string(available) + " trading days, fewer than the " +
                 std::to_string(required) + " the longest EMA window of enabled strategy " +
                 driver + " needs. That EMA will not be warmed up and its signal will be "
                 "meaningless rather than absent (G-03).");
        }

        // The live side reads the same history through a CALENDAR-day setting,
        // so the two must be put in the same units before they can be compared.
        // 365/252 is the ratio the template's own "2 yrs = 730 days" note uses.
        const int live_trading_days =
            static_cast<int>(config.live.historical_days * kTradingDaysPerYear / 365.0);
        if (live_trading_days < required) {
            WARN("live.historical_days=" + std::to_string(config.live.historical_days) +
                 " is about " + std::to_string(live_trading_days) +
                 " trading days, fewer than the " + std::to_string(required) +
                 " the longest EMA window of enabled strategy " + driver + " needs (G-03).");
        }
        if (live_trading_days < available) {
            WARN("live.historical_days (" + std::to_string(live_trading_days) +
                 " trading days) is shorter than backtest.lookback_years (" +
                 std::to_string(available) +
                 " trading days), so the live book warms up on less history than the "
                 "backtest it is compared against (G-03).");
        }
    }

    return Result<void>();
}

Result<void> ConfigLoader::require_loop_keys(const AppConfig& config) {
    if (!config.equity_slow_rule.present) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            "config for " + config.portfolio_id +
                ": portfolio.json \"equity_slow_rule\" is required on a futures book "
                "({\"symbols\": [\"M2K\", \"MES\", \"MNQ\", \"MYM\"], \"pairs\": [[32, 128], [64, 256]]})",
            "ConfigLoader");
    }
    // LOOP_SPEC sections 4, 7.7 and 12: the overlay's limits. The book's carver module carries
    // R_max, R_jump_max and R_shock_max (ratios to tau); its max_gross_leverage and
    // max_net_leverage are L_max and L_net_max.
    {
        bool carries = false;
        for (const auto& module : config.risk_schema.portfolio) {
            if (const auto* carver = std::get_if<CarverModuleConfig>(&module.params)) {
                carries = carries || carver->overlay_limits();
            }
        }
        if (!carries) {
            return make_error<void>(
                ErrorCode::INVALID_DATA,
                "config for " + config.portfolio_id +
                    ": risk.json's carver module needs \"R_max\", \"R_jump_max\" and "
                    "\"R_shock_max\" on a futures book (the overlay's risk limits as ratios to "
                    "tau: 2.25, 4.5, 4.0)",
                "ConfigLoader");
        }
    }
    if (config.sizing_mode.empty()) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "config for " + config.portfolio_id +
                                    ": portfolio.json \"sizing_mode\" is required on a futures "
                                    "book (\"half_compounding\")",
                                "ConfigLoader");
    }
    if (!(config.starting_capital > 0.0)) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "config for " + config.portfolio_id +
                                    ": portfolio.json \"starting_capital\" is required on a "
                                    "futures book (the book's initial_capital)",
                                "ConfigLoader");
    }
    // The one pass's constants (sections 5.2, 5.3 and 6.4). The carver module's per_name_cap and
    // trim_max come with its overlay limits (the schema requires them together).
    if (!config.has_cost_penalty_scalar) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "config for " + config.portfolio_id +
                                    ": defaults.json optimization.cost_penalty_scalar is required "
                                    "on a futures book (the search's cost multiplier: 100)",
                                "ConfigLoader");
    }
    if (!(config.sign_close_band > 0.0)) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "config for " + config.portfolio_id +
                                    ": defaults.json optimization.sign_close_band is required on "
                                    "a futures book (the deferral band of the forecast-sign "
                                    "close: 2)",
                                "ConfigLoader");
    }
    if (!(config.b_sigma_floor > 0.0)) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "config for " + config.portfolio_id +
                                    ": defaults.json optimization.b_sigma_floor is required on a "
                                    "futures book (B_sigma's floor as a ratio to tau: 0.05)",
                                "ConfigLoader");
    }
    // Section 7.7: a retired key is refused, never ignored.
    if (!config.retired_loop_keys.empty()) {
        std::string list;
        for (const auto& key : config.retired_loop_keys) list += (list.empty() ? "" : "; ") + key;
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "config for " + config.portfolio_id +
                                    ": retired key(s) on a futures book, remove them: " + list,
                                "ConfigLoader");
    }
    return Result<void>();
}

std::pair<Timestamp, Timestamp> ConfigLoader::resolve_backtest_window(
    const BacktestSpecificConfig& backtest, Timestamp now, bool* froze) {
    if (froze) *froze = false;

    // The now() path, byte-for-byte what the three bt runners did inline.
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm anchor_tm{};
    std::tm* local_tm = std::localtime(&now_time_t);
    if (local_tm != nullptr) anchor_tm = *local_tm;
    Timestamp end_date = now;

    // M-12: the frozen window, taken only when a config explicitly carries the
    // key. Anything unparseable is refused rather than silently ignored -- a
    // typo in a test config that quietly reverted to now() would reintroduce the
    // very drift this exists to remove, and it would do it invisibly.
    if (!backtest.frozen_end_date.empty()) {
        // Parsed by hand rather than with std::get_time: libc++'s "%Y-%m-%d"
        // accepts "03-05-2026" (year 3) and stops happily at "2026-05" without
        // setting failbit, so a typo would be taken as a real date and the run
        // would be frozen to the wrong window while looking fine.
        const std::string& fd = backtest.frozen_end_date;
        auto all_digits = [&fd](size_t off, size_t n) {
            for (size_t k = 0; k < n; ++k) {
                if (!std::isdigit(static_cast<unsigned char>(fd[off + k]))) return false;
            }
            return true;
        };
        if (fd.size() != 10 || fd[4] != '-' || fd[7] != '-' || !all_digits(0, 4) ||
            !all_digits(5, 2) || !all_digits(8, 2)) {
            throw std::runtime_error(
                "backtest.frozen_end_date is not YYYY-MM-DD: '" + fd + "'");
        }
        const int fy = std::stoi(fd.substr(0, 4));
        const int fm = std::stoi(fd.substr(5, 2));
        const int fdy = std::stoi(fd.substr(8, 2));
        if (fm < 1 || fm > 12 || fdy < 1 || fdy > 31) {
            throw std::runtime_error(
                "backtest.frozen_end_date is not a real calendar date: '" + fd + "'");
        }
        std::tm frozen_tm{};
        frozen_tm.tm_year = fy - 1900;
        frozen_tm.tm_mon = fm - 1;
        frozen_tm.tm_mday = fdy;
        frozen_tm.tm_hour = 0;
        frozen_tm.tm_min = 0;
        frozen_tm.tm_sec = 0;
        frozen_tm.tm_isdst = -1;  // let mktime resolve DST for that local date
        std::tm normalise = frozen_tm;
        auto frozen_time_t = std::mktime(&normalise);
        end_date = std::chrono::system_clock::from_time_t(frozen_time_t);
        anchor_tm = frozen_tm;
        if (froze) *froze = true;
    }

    std::tm start_tm = anchor_tm;
    start_tm.tm_year -= backtest.lookback_years;
    auto start_time_t = std::mktime(&start_tm);
    Timestamp start_date = std::chrono::system_clock::from_time_t(start_time_t);

    return {start_date, end_date};
}

void ConfigLoader::log_config_summary(const AppConfig& config) {
    auto& logger = Logger::instance();
    if (!logger.is_initialized()) {
        return;
    }
    INFO("Config summary: portfolio_id=" + config.portfolio_id +
         ", initial_capital=" + std::to_string(config.initial_capital));
    INFO("Config summary: db=" + config.database.host + ":" + config.database.port +
         "/" + config.database.name +
         ", connections=" + std::to_string(config.database.num_connections));
    INFO("Config summary: strategies=" + std::to_string(config.strategies_config.size()) +
         ", backtest_lookback_years=" + std::to_string(config.backtest.lookback_years) +
         ", live_historical_days=" + std::to_string(config.live.historical_days));
}

Result<AppConfig> ConfigLoader::load(const std::filesystem::path& config_base_path,
                                     const std::string& portfolio_name) {
    // 1. Load defaults.json
    auto defaults_path = config_base_path / "defaults.json";
    auto defaults_result = load_json_file(defaults_path);
    if (defaults_result.is_error()) {
        return make_error<AppConfig>(defaults_result.error()->code(),
                                     "Failed to load defaults.json: " +
                                         std::string(defaults_result.error()->what()),
                                     "ConfigLoader");
    }
    nlohmann::json merged = defaults_result.value();

    // 2. Load portfolio-specific configs
    auto portfolio_path = config_base_path / "portfolios" / portfolio_name;

    // Load portfolio.json
    auto portfolio_json_path = portfolio_path / "portfolio.json";
    auto portfolio_result = load_json_file(portfolio_json_path);
    if (portfolio_result.is_error()) {
        return make_error<AppConfig>(portfolio_result.error()->code(),
                                     "Failed to load portfolio.json: " +
                                         std::string(portfolio_result.error()->what()),
                                     "ConfigLoader");
    }
    merge_json(merged, portfolio_result.value());

    // Load risk.json
    auto risk_json_path = portfolio_path / "risk.json";
    auto risk_result = load_json_file(risk_json_path);
    if (risk_result.is_error()) {
        return make_error<AppConfig>(risk_result.error()->code(),
                                     "Failed to load risk.json: " +
                                         std::string(risk_result.error()->what()),
                                     "ConfigLoader");
    }
    merged["risk"] = risk_result.value();

    // Load email.json
    auto email_json_path = portfolio_path / "email.json";
    auto email_result = load_json_file(email_json_path);
    if (email_result.is_error()) {
        return make_error<AppConfig>(email_result.error()->code(),
                                     "Failed to load email.json: " +
                                         std::string(email_result.error()->what()),
                                     "ConfigLoader");
    }
    merged["email"] = email_result.value();

    // 3. Extract config
    auto config_result = extract_config(merged);
    if (config_result.is_error()) {
        return config_result;
    }

    auto validation_result = validate_config(config_result.value());
    if (validation_result.is_error()) {
        return make_error<AppConfig>(validation_result.error()->code(),
                                     validation_result.error()->what(),
                                     "ConfigLoader");
    }

    log_config_summary(config_result.value());
    return config_result;
}

Result<AppConfig> ConfigLoader::load_legacy(const std::filesystem::path& config_file_path) {
    // Load the single config file
    auto json_result = load_json_file(config_file_path);
    if (json_result.is_error()) {
        return make_error<AppConfig>(json_result.error()->code(),
                                     "Failed to load legacy config: " +
                                         std::string(json_result.error()->what()),
                                     "ConfigLoader");
    }

    const auto& config_json = json_result.value();

    try {
        AppConfig config;

        // Portfolio ID
        if (config_json.contains("portfolio_id")) {
            config.portfolio_id = config_json.at("portfolio_id").get<std::string>();
        }

        // Database configuration
        if (config_json.contains("database")) {
            config.database.from_json(config_json.at("database"));
        }

        // Email configuration
        if (config_json.contains("email")) {
            config.email.from_json(config_json.at("email"));
        }

        // Strategies (from portfolio.strategies)
        if (config_json.contains("portfolio") &&
            config_json.at("portfolio").contains("strategies")) {
            config.strategies_config = config_json.at("portfolio").at("strategies");
        }

        // Legacy configs don't have execution/optimization/risk in file
        // These are hardcoded in the application - return defaults
        // Applications should fill these in after loading

        auto validation_result = validate_config(config);
        if (validation_result.is_error()) {
            return make_error<AppConfig>(validation_result.error()->code(),
                                         validation_result.error()->what(),
                                         "ConfigLoader");
        }
        log_config_summary(config);
        return config;

    } catch (const std::exception& e) {
        return make_error<AppConfig>(ErrorCode::INVALID_DATA,
                                     "Failed to extract legacy config: " + std::string(e.what()),
                                     "ConfigLoader");
    }
}

}  // namespace trade_ngin
