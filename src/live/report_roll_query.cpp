// src/live/report_roll_query.cpp
#include "trade_ngin/live/report_roll_query.hpp"

#include <map>
#include <set>

#include "trade_ngin/instruments/instrument_registry.hpp"

namespace trade_ngin {

contract_rolls::ReportRollQuery report_roll_query(
    const std::string& settled_date, const std::unordered_map<std::string, Position>& book,
    const std::unordered_map<std::string, std::unordered_map<std::string, Position>>& sleeves,
    const std::unordered_map<std::string, std::vector<ExecutionReport>>& sleeve_executions,
    const std::unordered_map<std::string, roll_series::RollTracker::Status>& roll_status) {
    contract_rolls::ReportRollQuery q;
    q.date = settled_date;
    std::set<std::string> held;
    for (const auto& [symbol, position] : book) {
        if (position.quantity.as_double() != 0.0) held.insert(symbol);
    }
    for (const auto& [sleeve, positions] : sleeves) {
        (void)sleeve;
        for (const auto& [symbol, position] : positions) {
            if (position.quantity.as_double() != 0.0) held.insert(symbol);
        }
    }
    q.held_symbols.assign(held.begin(), held.end());
    for (const auto& symbol : held) {
        const auto st = roll_status.find(symbol);
        if (st != roll_status.end() && !st->second.held_id.empty()) q.held_ids[symbol] = st->second.held_id;
    }
    for (const auto& [sleeve, executions] : sleeve_executions) {
        (void)sleeve;
        std::map<std::string, std::string> opens;  // opening leg's exec id -> its contract
        for (const auto& e : executions) {
            if (e.execution_type == ExecutionType::ROLL) opens[e.exec_id] = e.instrument_id;
        }
        for (const auto& e : executions) {
            if (e.execution_type != ExecutionType::ROLL || e.exec_id.size() < 3 ||
                e.exec_id.compare(e.exec_id.size() - 3, 3, "_RC") != 0) {
                continue;
            }
            const std::string stem = e.exec_id.substr(0, e.exec_id.size() - 3);
            const auto open = opens.find(stem + "_RO");
            const std::string ymd8 = stem.size() >= 8 ? stem.substr(stem.size() - 8) : std::string();
            const bool dated = ymd8.size() == 8 && ymd8.find_first_not_of("0123456789") == std::string::npos;
            q.rolls.push_back({e.exec_id, e.symbol, e.instrument_id,
                               open == opens.end() ? std::string() : open->second,
                               dated ? ymd8.substr(0, 4) + "-" + ymd8.substr(4, 2) + "-" + ymd8.substr(6, 2)
                                     : settled_date});
        }
    }
    return q;
}

double report_tick_size(const std::string& symbol) {
    try {
        auto instrument = InstrumentRegistry::instance().get_instrument(symbol.substr(0, symbol.find(".v.")));
        return instrument ? instrument->get_tick_size() : 0.0;
    } catch (const std::exception&) {
        return 0.0;
    }
}

}  // namespace trade_ngin
