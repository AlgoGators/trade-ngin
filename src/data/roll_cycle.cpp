// src/data/roll_cycle.cpp
#include "trade_ngin/data/roll_cycle.hpp"

#include <set>

namespace trade_ngin::contract_rolls {

Result<std::string> roll_cycle(const std::string& root) {
    const auto& t = roll_cycle_table();
    auto it = t.find(root);
    if (it == t.end()) {
        return make_error<std::string>(ErrorCode::INVALID_ARGUMENT,
                                       "no roll cycle for root " + root, "contract_rolls");
    }
    return it->second;
}

bool in_cycle(const std::string& cycle, unsigned month) {
    return cycle.find(ContractKey::month_code(month)) != std::string::npos;
}

int next_cycle_delivery(const std::string& cycle, int year, unsigned month) {
    int index = year * 12 + static_cast<int>(month) - 1;
    for (int i = 0; i < 24; ++i) {
        ++index;
        if (in_cycle(cycle, static_cast<unsigned>(index % 12) + 1)) return index;
    }
    return -1;
}

std::string CycleMeasure::cycle() const {
    std::string out;
    for (unsigned m = 1; m <= 12; ++m) {
        if (years > 0 && 2 * held_years[m - 1] >= years) out += ContractKey::month_code(m);
    }
    return out;
}

std::string CycleMeasure::near_cut() const {
    std::string out;
    for (unsigned m = 1; m <= 12; ++m) {
        if (years == 0) continue;
        const double share = static_cast<double>(held_years[m - 1]) / years;
        if (share >= 0.35 && share <= 0.65) out += ContractKey::month_code(m);
    }
    return out;
}

Result<CycleMeasure> measure_cycle(const std::string& root, const std::vector<VendorBar>& grid,
                                   const ContractBook& book,
                                   const std::function<std::string(Day)>& root_on) {
    CycleMeasure m;
    m.root = root;
    std::vector<const Contract*> named(grid.size(), nullptr);
    for (size_t i = 0; i < grid.size(); ++i) {
        named[i] = book.named_by_id(root_on(grid[i].date), grid[i].instrument_id, grid[i].date);
        if (!named[i]) ++m.unresolved_bars;
    }
    std::set<int> years;
    std::array<std::set<int>, 12> by_month;
    for (size_t i = 0; i + 1 < grid.size(); ++i) {
        if (!named[i] || named[i] != named[i + 1]) continue;
        auto key = ContractBook::require(*named[i]);
        if (key.is_error()) {
            return make_error<CycleMeasure>(key.error()->code(), key.error()->what(),
                                            "contract_rolls");
        }
        years.insert(key.value().delivery_year);
        by_month[key.value().delivery_month - 1].insert(key.value().delivery_year);
    }
    m.years = static_cast<int>(years.size());
    for (size_t k = 0; k < 12; ++k) m.held_years[k] = static_cast<int>(by_month[k].size());
    return m;
}

}  // namespace trade_ngin::contract_rolls
