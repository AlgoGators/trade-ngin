// src/portfolio/desk_book.cpp
#include "trade_ngin/portfolio/desk_book.hpp"

#include <cmath>

#include "trade_ngin/portfolio/allocation_split.hpp"

namespace trade_ngin {

namespace {

std::map<std::string, double> weights_from(const SleeveQuantities& by_sleeve,
                                           const std::string& symbol) {
    std::map<std::string, double> weights;
    for (const auto& [sleeve, book] : by_sleeve) {
        const auto q = book.find(symbol);
        if (q != book.end() && std::abs(q->second) > 1e-12) weights[sleeve] = std::abs(q->second);
    }
    return weights;
}

}  // namespace

Result<DeskSplitWeights> resolve_desk_split(
    const std::map<std::string, double>& totals, const SleeveQuantities& system_by_sleeve,
    const SleeveQuantities& previous_by_sleeve,
    const std::map<std::string, std::set<std::string>>& universe_by_sleeve) {
    DeskSplitWeights out;
    for (const auto& [symbol, total] : totals) {
        auto weights = weights_from(system_by_sleeve, symbol);
        if (weights.empty()) weights = weights_from(previous_by_sleeve, symbol);
        if (weights.empty()) {
            // std::map iterates the sleeves alphabetically
            for (const auto& [sleeve, universe] : universe_by_sleeve) {
                if (universe.count(symbol) > 0) {
                    weights[sleeve] = 1.0;
                    break;
                }
            }
        }
        if (weights.empty()) {
            if (std::abs(total) <= 1e-12) continue;
            return make_error<DeskSplitWeights>(
                ErrorCode::INVALID_ARGUMENT,
                "the desk book holds " + symbol +
                    ", which no sleeve holds today or held yesterday and no sleeve's universe "
                    "contains, so it cannot be split back to a sleeve",
                "DeskBook");
        }
        out[symbol] = std::move(weights);
    }
    return out;
}

std::vector<double> split_desk_quantity(double quantity, const std::vector<std::string>& sleeves,
                                        const std::map<std::string, double>& weights) {
    std::vector<SleeveContribution> parts;
    parts.reserve(sleeves.size());
    double total_weight = 0.0;
    for (const auto& sleeve : sleeves) {
        const auto w = weights.find(sleeve);
        const double weight = w == weights.end() ? 0.0 : w->second;
        total_weight += weight;
        parts.push_back({sleeve, weight});
    }
    std::vector<double> out(sleeves.size(), 0.0);
    if (sleeves.empty()) return out;
    if (total_weight <= 1e-12) {
        // no weight at all: the whole quantity on the first sleeve
        out[0] = std::round(quantity);
        return out;
    }
    const SleeveDistribution split = distribute_optimizer_contracts(quantity, parts);
    for (std::size_t s = 0; s < sleeves.size(); ++s) out[s] = static_cast<double>(split.stored[s]);
    return out;
}

}  // namespace trade_ngin
