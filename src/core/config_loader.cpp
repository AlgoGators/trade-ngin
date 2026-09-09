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
        if (merged.contains("reserve_capital_pct")) {
            config.reserve_capital_pct = merged.at("reserve_capital_pct").get<double>();
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
            config.opt_config.from_json(merged.at("optimization"));
        }
        // Set capital in opt_config
        config.opt_config.capital = config.initial_capital;

        // Risk configuration - from risk_defaults and risk section
        if (merged.contains("risk_defaults")) {
            const auto& risk_defaults = merged.at("risk_defaults");
            if (risk_defaults.contains("confidence_level")) {
                config.risk_config.confidence_level =
                    risk_defaults.at("confidence_level").get<double>();
            }
            if (risk_defaults.contains("lookback_period")) {
                config.risk_config.lookback_period =
                    risk_defaults.at("lookback_period").get<int>();
            }
            if (risk_defaults.contains("max_correlation")) {
                config.risk_config.max_correlation =
                    risk_defaults.at("max_correlation").get<double>();
            }
        }

        if (merged.contains("risk")) {
            config.risk_config.from_json(merged.at("risk"));

            // Additional risk limits
            const auto& risk = merged.at("risk");
            if (risk.contains("max_drawdown")) {
                config.max_drawdown = risk.at("max_drawdown").get<double>();
            }
            if (risk.contains("max_leverage")) {
                config.max_leverage = risk.at("max_leverage").get<double>();
            }
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
    if (config.reserve_capital_pct < 0.0 || config.reserve_capital_pct >= 1.0) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "reserve_capital_pct must be in [0.0, 1.0)",
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
            const bool enabled = def.value("enabled_backtest", false) ||
                                 def.value("enabled_live", false);
            if (!enabled) continue;

            int longest = kDefaultLongestEma;
            if (def.contains("config") && def.at("config").contains("ema_windows")) {
                longest = 0;
                for (const auto& pair : def.at("config").at("ema_windows")) {
                    if (pair.is_array() && pair.size() == 2) {
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
         ", initial_capital=" + std::to_string(config.initial_capital) +
         ", reserve_pct=" + std::to_string(config.reserve_capital_pct));
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
