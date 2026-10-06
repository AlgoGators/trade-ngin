#pragma once
#include <string_view>
#include "trade_ngin/core/config_loader.hpp"
namespace trade_ngin {
// Native JSON dump bytes are canonical for this version; callers must not reserialize hashes.
inline constexpr size_t live_config_max_request_bytes = 1024 * 1024;
Result<nlohmann::json> build_runtime_trading_snapshot(const AppConfig& config);
// Typed file configuration hash preserves legacy selection behavior; JSON overload
// requires a governed, complete v2 baseline.
Result<std::string> live_config_snapshot_sha256(const AppConfig& config);
Result<std::string> live_config_snapshot_sha256(const nlohmann::json& snapshot);
Result<AppConfig> parse_runtime_trading_snapshot(const nlohmann::json& snapshot);
Result<AppConfig> apply_live_config_override(const AppConfig& base, const nlohmann::json& changes);
Result<nlohmann::json> validate_live_config_request(const nlohmann::json& request);
// Explicit governed baseline receipt for a separately approved reset candidate.
// Ordinary override validation still rejects empty/baseline-equal changes.
Result<nlohmann::json> validate_live_config_baseline_request(const nlohmann::json& request);
// Wire boundary rejects duplicate keys, nonfinite numbers, excessive depth and size.
Result<nlohmann::json> parse_live_config_request(std::string_view bytes);
}
