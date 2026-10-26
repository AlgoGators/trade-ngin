// src/relative_value/param_schema.cpp
#include "trade_ngin/relative_value/param_schema.hpp"

#include <cmath>
#include <sstream>

namespace trade_ngin::relative_value {

using nlohmann::json;

namespace {

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + v[i];
    return out;
}

std::string num(double x) {
    std::ostringstream os;
    os << x;
    return os.str();
}

std::string join_path(const std::string& path, const std::string& name) {
    return path.empty() ? name : path + "." + name;
}

}  // namespace

const ParamSpec* ParamSchema::find(const std::string& name) const {
    for (const auto& p : params) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

std::vector<ValidationIssue> ParamSchema::check(const json& values, const std::string& path) const {
    std::vector<ValidationIssue> issues;
    if (!values.is_object()) {
        issues.push_back({path, "expected an object of parameters"});
        return issues;
    }

    for (auto it = values.begin(); it != values.end(); ++it) {
        if (find(it.key()) == nullptr) {
            std::vector<std::string> known;
            for (const auto& p : params) known.push_back(p.name);
            issues.push_back({join_path(path, it.key()),
                              "unknown parameter" + (known.empty() ? std::string(" (none are accepted)")
                                                                   : " (accepted: " + join(known) + ")")});
        }
    }

    for (const auto& spec : params) {
        const std::string where = join_path(path, spec.name);
        auto it = values.find(spec.name);
        if (it == values.end()) {
            if (spec.required) issues.push_back({where, "is required"});
            continue;
        }
        const json& v = *it;
        auto bad = [&](const std::string& what) { issues.push_back({where, what}); };

        switch (spec.type) {
            case ParamType::Int:
                if (v.is_boolean() || !v.is_number_integer()) {
                    bad("must be an integer");
                    continue;
                }
                break;
            case ParamType::Number:
                if (v.is_boolean() || !v.is_number()) {
                    bad("must be a number");
                    continue;
                }
                break;
            case ParamType::Bool:
                if (!v.is_boolean()) {
                    bad("must be true or false");
                    continue;
                }
                break;
            case ParamType::String:
            case ParamType::InputRef:
                if (!v.is_string() || v.get<std::string>().empty()) {
                    bad(spec.type == ParamType::InputRef ? "must be the name of a declared input"
                                                         : "must be a non-empty string");
                    continue;
                }
                break;
            case ParamType::Enum: {
                bool ok = v.is_string();
                if (ok) {
                    ok = false;
                    for (const auto& a : spec.allowed) ok = ok || a == v.get<std::string>();
                }
                if (!ok) {
                    bad("must be one of: " + join(spec.allowed));
                    continue;
                }
                break;
            }
            case ParamType::Any:
                break;
        }

        if (v.is_number()) {
            const double x = v.get<double>();
            if (spec.min && (spec.min_exclusive ? x <= *spec.min : x < *spec.min)) {
                bad("must be " + std::string(spec.min_exclusive ? "greater than " : "at least ") +
                    num(*spec.min) + ", got " + num(x));
            }
            if (spec.max && x > *spec.max) {
                bad("must be at most " + num(*spec.max) + ", got " + num(x));
            }
        }
    }
    return issues;
}

json ParamSchema::with_defaults(const json& values) const {
    json out = values.is_object() ? values : json::object();
    for (const auto& spec : params) {
        if (!out.contains(spec.name) && !spec.default_value.is_null()) {
            out[spec.name] = spec.default_value;
        }
    }
    return out;
}

}  // namespace trade_ngin::relative_value
