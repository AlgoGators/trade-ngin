// include/trade_ngin/relative_value/analysis_registry.hpp
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/relative_value/param_schema.hpp"

namespace trade_ngin::relative_value {

enum class AnalysisKind {
    SeriesRule,      ///< how a trade's series is built from its inputs (section 7.1)
    Signal,          ///< a named function of the series that can feed a forecast (8.1, 8.2)
    Diagnostic,      ///< a statistic that is stored and shown, never traded (8.3)
    ForecastMethod,  ///< the stage 2 mapping from a raw value to a forecast (8.4)
};

std::string to_string(AnalysisKind kind);

/// Everything the parser, the validator, the storage and the catalog need to know about one
/// registered analysis (section 8.5). Computing it is added next to this descriptor in the signal
/// module; nothing outside that module changes when an analysis is added.
struct AnalysisDescriptor {
    std::string name;  ///< the `type` / `rule` / `method` string used in definitions
    AnalysisKind kind{AnalysisKind::Signal};
    ParamSchema params;
    std::string description;

    /// Extra named outputs next to the headline value (stored as `<name>.<output>`).
    std::vector<std::string> outputs;
    /// Flags the analysis can raise (stored as `<name>.flag.<flag>`).
    std::vector<std::string> flags;
    /// The forecast method used when the signal has no `forecast` block ("none": no forecast).
    std::string default_forecast{"none"};

    /// Outputs that depend on the parameters (for example one set per factor). When set, it
    /// replaces `outputs` for key naming.
    std::function<std::vector<std::string>(const nlohmann::json& params)> expand_outputs;

    /// Checks across parameters that a flat schema cannot express (fast < slow, near < far, ...).
    std::function<void(const nlohmann::json& params, const std::string& path,
                       std::vector<ValidationIssue>& issues)>
        check;

    /// Input names that a parameter holds inside an `Any` value (terms, near/far, anchor, ...), as
    /// (path suffix, input name), so the validator can check they are declared inputs. `InputRef`
    /// parameters are found from the schema and need no entry here.
    std::function<std::vector<std::pair<std::string, std::string>>(const nlohmann::json& params)>
        input_refs;

    std::vector<std::string> output_names(const nlohmann::json& p) const {
        return expand_outputs ? expand_outputs(p) : outputs;
    }
};

/// The registered analyses, by kind and name.
class AnalysisRegistry {
public:
    /// Register one analysis. A second registration of the same (kind, name) is an error.
    Result<void> add(AnalysisDescriptor descriptor);

    const AnalysisDescriptor* find(AnalysisKind kind, const std::string& name) const;
    std::vector<const AnalysisDescriptor*> all(AnalysisKind kind) const;
    std::vector<std::string> names(AnalysisKind kind) const;

    /// The built-in series rules, signals, diagnostics and forecast methods.
    static AnalysisRegistry with_builtins();

private:
    std::map<std::pair<int, std::string>, AnalysisDescriptor> items_;
};

/// The diagnostic names a definition may list as `<name>_<window>`.
bool split_diagnostic(const std::string& text, std::string& name, int& window);

}  // namespace trade_ngin::relative_value
