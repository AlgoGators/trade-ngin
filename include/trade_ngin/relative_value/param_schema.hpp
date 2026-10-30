// include/trade_ngin/relative_value/param_schema.hpp
#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace trade_ngin::relative_value {

/// One thing wrong with a definition. `path` locates it ("signals[2].window"); the message says
/// what is wrong and what would be right.
struct ValidationIssue {
    std::string path;
    std::string message;

    std::string str() const { return path.empty() ? message : path + ": " + message; }
};

enum class ParamType {
    Int,       ///< a JSON integer
    Number,    ///< any JSON number
    Bool,
    String,
    Enum,      ///< a string from `allowed`
    InputRef,  ///< a string naming a declared input (the validator resolves it)
    Any,       ///< any JSON value; the descriptor's own check inspects it
};

/// One parameter of an analysis, series rule or forecast method.
struct ParamSpec {
    std::string name;
    ParamType type{ParamType::Any};
    bool required{false};
    nlohmann::json default_value;  ///< null: no default
    std::optional<double> min;
    bool min_exclusive{false};
    std::optional<double> max;
    std::vector<std::string> allowed;  ///< for Enum
    std::string doc;
};

/// The parameters an analysis accepts: their types, defaults and bounds (section 8.5). The
/// validator reads it, and the catalog table is written from it.
struct ParamSchema {
    std::vector<ParamSpec> params;

    const ParamSpec* find(const std::string& name) const;

    /// Check `values` (a JSON object) against the schema: every required parameter present, types,
    /// bounds and enum members right, and no unknown key. `path` prefixes the issue paths.
    std::vector<ValidationIssue> check(const nlohmann::json& values, const std::string& path) const;

    /// `values` with the defaults of absent optional parameters filled in.
    nlohmann::json with_defaults(const nlohmann::json& values) const;
};

}  // namespace trade_ngin::relative_value
