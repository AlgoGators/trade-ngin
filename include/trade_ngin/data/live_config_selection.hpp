#pragma once
#include "trade_ngin/core/live_config_override.hpp"
namespace trade_ngin {
class PostgresDatabase;
struct ConfigSelection {
    AppConfig config;
    nlohmann::json receipt;
};
// A successful lookup with no active row preserves the typed file config.
// Any database/schema, source, build, baseline or approved-output mismatch refuses.
Result<ConfigSelection> select_live_configuration(PostgresDatabase& database,
    const AppConfig& base, const std::string& engine_build);
} // namespace trade_ngin
