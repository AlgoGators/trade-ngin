// src/relative_value/analysis_registry.cpp
#include "trade_ngin/relative_value/analysis_registry.hpp"

#include <cctype>

namespace trade_ngin::relative_value {

std::string to_string(AnalysisKind kind) {
    switch (kind) {
        case AnalysisKind::SeriesRule: return "series_rule";
        case AnalysisKind::Signal: return "signal";
        case AnalysisKind::Diagnostic: return "diagnostic";
        case AnalysisKind::ForecastMethod: return "forecast_method";
    }
    return "unknown";
}

Result<void> AnalysisRegistry::add(AnalysisDescriptor descriptor) {
    if (descriptor.name.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "an analysis needs a name",
                                "AnalysisRegistry");
    }
    const auto key = std::make_pair(static_cast<int>(descriptor.kind), descriptor.name);
    if (items_.count(key)) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "already registered: " + to_string(descriptor.kind) + " '" +
                                    descriptor.name + "'",
                                "AnalysisRegistry");
    }
    items_.emplace(key, std::move(descriptor));
    return Result<void>();
}

const AnalysisDescriptor* AnalysisRegistry::find(AnalysisKind kind, const std::string& name) const {
    auto it = items_.find(std::make_pair(static_cast<int>(kind), name));
    return it == items_.end() ? nullptr : &it->second;
}

std::vector<const AnalysisDescriptor*> AnalysisRegistry::all(AnalysisKind kind) const {
    std::vector<const AnalysisDescriptor*> out;
    for (const auto& [key, d] : items_) {
        if (key.first == static_cast<int>(kind)) out.push_back(&d);
    }
    return out;
}

std::vector<std::string> AnalysisRegistry::names(AnalysisKind kind) const {
    std::vector<std::string> out;
    for (const auto* d : all(kind)) out.push_back(d->name);
    return out;
}

bool split_diagnostic(const std::string& text, std::string& name, int& window) {
    const auto cut = text.rfind('_');
    if (cut == std::string::npos || cut == 0 || cut + 1 >= text.size()) return false;
    const std::string tail = text.substr(cut + 1);
    for (char c : tail) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    if (tail.size() > 6) return false;
    const int w = std::stoi(tail);
    if (w <= 0) return false;
    name = text.substr(0, cut);
    window = w;
    return true;
}

}  // namespace trade_ngin::relative_value
