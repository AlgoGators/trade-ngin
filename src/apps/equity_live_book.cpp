#include "trade_ngin/strategy/equity_strategy_builder.hpp"

#include <cmath>
#include <optional>
#include <set>

namespace trade_ngin::apps {
namespace {

bool valid_storage_id(const std::string& value) {
    return !value.empty() && value.size() <= 100 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
           });
}

Result<EquityLiveBookPlan> invalid_plan(const std::string& reason) {
    return make_error<EquityLiveBookPlan>(
        ErrorCode::INVALID_ARGUMENT, reason, "equity_live_book");
}

}  // namespace

Result<EquityLiveBookPlan> build_equity_live_book_plan(
    const std::vector<EquityStrategyEntry>& entries) {
    if (entries.empty()) return invalid_plan("No live equity sleeves were selected");

    std::vector<EquityStrategyEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });

    double allocation_total = 0.0;
    std::set<std::string> ids;
    std::set<std::string> all_symbols;
    std::optional<bool> fractional_policy;
    EquityLiveBookPlan plan;
    plan.legacy_single =
        sorted.size() == 1 && sorted.front().id == "MEAN_REVERSION";

    for (const auto& entry : sorted) {
        if (!valid_storage_id(entry.id) || !ids.insert(entry.id).second) {
            return invalid_plan("Live equity sleeve identity is invalid or duplicated");
        }
        if (entry.type != "MeanReversionStrategy" || !entry.def.is_object() ||
            !entry.def.contains("config") || !entry.def.at("config").is_object()) {
            return invalid_plan("Live equity sleeve definition is unsupported");
        }
        if (!std::isfinite(entry.allocation) || entry.allocation <= 0.0) {
            return invalid_plan("Live equity sleeve allocation must be finite and positive");
        }
        allocation_total += entry.allocation;
        if (!std::isfinite(allocation_total)) {
            return invalid_plan("Live equity sleeve allocation total is invalid");
        }

        const bool allows_fractional =
            build_mean_reversion_config(entry.def.at("config")).allow_fractional_shares;
        if (fractional_policy && *fractional_policy != allows_fractional) {
            return invalid_plan(
                "All live equity sleeves must use the same fractional-share policy");
        }
        fractional_policy = allows_fractional;

        EquityLiveSleevePlan sleeve;
        sleeve.source_id = entry.id;
        sleeve.strategy_name = plan.legacy_single ? "EQUITY_MEAN_REVERSION" : entry.id;
        sleeve.allocation = entry.allocation;
        sleeve.definition = entry.def;
        sleeve.uses_database_symbol_fallback = !entry.def.contains("symbols");
        if (entry.def.contains("symbols")) {
            if (!entry.def.at("symbols").is_array()) {
                return invalid_plan("Live equity sleeve symbols must be an array");
            }
            std::set<std::string> sleeve_symbols;
            for (const auto& raw : entry.def.at("symbols")) {
                if (!raw.is_string() || raw.get_ref<const std::string&>().empty()) {
                    return invalid_plan("Live equity sleeve contains an invalid symbol");
                }
                sleeve_symbols.insert(raw.get<std::string>());
            }
            sleeve.symbols.assign(sleeve_symbols.begin(), sleeve_symbols.end());
            sleeve.uses_database_symbol_fallback = sleeve.symbols.empty();
            all_symbols.insert(sleeve_symbols.begin(), sleeve_symbols.end());
        }
        plan.sleeves.push_back(std::move(sleeve));
    }

    if (allocation_total <= 0.0) {
        return invalid_plan("Live equity sleeve allocation total must be positive");
    }
    for (auto& sleeve : plan.sleeves) sleeve.allocation /= allocation_total;

    if (plan.legacy_single) {
        plan.combined_strategy_id = "LIVE_EQUITY_MEAN_REVERSION";
    } else {
        plan.combined_strategy_id = "LIVE_EQUITY_";
        for (std::size_t index = 0; index < plan.sleeves.size(); ++index) {
            if (index != 0) plan.combined_strategy_id += "_";
            plan.combined_strategy_id += plan.sleeves[index].strategy_name;
        }
    }
    if (!valid_storage_id(plan.combined_strategy_id)) {
        return invalid_plan("Combined live equity strategy identity is invalid");
    }
    plan.allow_fractional_shares = *fractional_policy;
    plan.symbols.assign(all_symbols.begin(), all_symbols.end());
    return Result<EquityLiveBookPlan>(std::move(plan));
}

}  // namespace trade_ngin::apps
