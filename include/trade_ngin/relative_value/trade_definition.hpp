// include/trade_ngin/relative_value/trade_definition.hpp
#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "trade_ngin/relative_value/param_schema.hpp"

namespace trade_ngin::relative_value {

/// A trade definition: one JSON object holding legs, inputs, how the series is built, which signals
/// to compute and how it is paper traded (section 6). It carries only values the author chose,
/// never values the code derives (no multipliers, fitted betas or derived contract counts), so it
/// stays storable through the config lane.

struct Leg {
    std::string symbol;
    std::string asset_class;  ///< "futures" or "equity"
};

struct InputSpec {
    std::string name;
    std::string source;  ///< "leg", "instrument", "series" or "trade"
    std::string symbol;       ///< leg and instrument
    std::string asset_class;  ///< instrument
    std::string table;        ///< series
    std::string key;          ///< series
    std::string trade_id;     ///< trade
};

struct PackageSpec {
    std::string mode;                         ///< "fixed" or "derived"
    std::map<std::string, long> contracts;    ///< fixed: signed contracts per leg symbol
    double scale{0.0};                        ///< derived
    double rebalance_threshold{0.10};         ///< derived
    nlohmann::json weights;                   ///< derived, when the series rule has none (null if absent)
};

struct SeriesSpec {
    std::string rule;
    nlohmann::json params;  ///< every key but "rule"
};

struct ForecastSpec {
    std::string method;
    nlohmann::json params;  ///< every key but "method"
};

struct SignalSpec {
    std::string name;
    std::string type;
    int sign{1};
    std::optional<std::string> input;  ///< a signal reads the definition's series unless it names an input
    std::optional<ForecastSpec> forecast;
    nlohmann::json params;  ///< every key but name, type, sign, input, forecast
};

struct TradeBlock {
    std::map<std::string, double> blend;  ///< signal name -> weight
    double risk_target{0.0};
    double capital{0.0};
};

struct TradeDefinition {
    std::string id;
    std::string description;
    std::vector<Leg> legs;
    std::optional<PackageSpec> package;
    std::vector<InputSpec> inputs;
    SeriesSpec series;
    std::vector<SignalSpec> signals;
    std::vector<std::string> diagnostics;
    std::optional<TradeBlock> trade;
    std::optional<std::string> history_start;  ///< ISO date
    std::optional<std::string> strategy;       ///< a specialized strategy, or absent
    nlohmann::json body;                       ///< the definition as given

    bool analysis_only() const { return legs.empty(); }
};

struct ParseOutcome {
    std::optional<TradeDefinition> definition;  ///< empty when the structure is wrong
    std::vector<ValidationIssue> issues;
};

/// Read the structure of a definition: right JSON types, no unknown keys, the blocks that are
/// required to exist. Whether the values mean something (symbols, names, bounds) is the
/// validator's job (definition_validator.hpp).
ParseOutcome parse_trade_definition(const nlohmann::json& body);

/// Parse a definition from JSON text.
ParseOutcome parse_trade_definition_text(const std::string& text);

/// The definition's canonical text (sorted keys, no whitespace): what is hashed and stored.
std::string canonical_text(const TradeDefinition& definition);

}  // namespace trade_ngin::relative_value
