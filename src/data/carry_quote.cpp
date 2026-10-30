// src/data/carry_quote.cpp
#include "trade_ngin/data/carry_quote.hpp"

#include "trade_ngin/data/roll_cycle.hpp"

namespace trade_ngin::contract_rolls {

CarryNeighbour carry_neighbour(const ContractBook& book, const ContractKey& held, Day day,
                               const std::string& cycle) {
    const Contract* nearer = nullptr;
    const Contract* further = nullptr;
    for (const Contract& c : book.contracts()) {
        if (!c.resolved() || c.root != held.root || c.first_bar > day) continue;
        const ContractKey& key = *c.key;
        if (!in_cycle(cycle, key.delivery_month)) continue;
        if (key.expiration < held.expiration) {
            if (key.expiration >= day && (nearer == nullptr || *nearer->key < key)) nearer = &c;
        } else if (key.expiration > held.expiration) {
            if (further == nullptr || key < *further->key) further = &c;
        }
    }
    const Contract* chosen = nearer != nullptr ? nearer : further;
    if (chosen == nullptr) return {};
    return {chosen, chosen->key->delivery_index() - held.delivery_index()};
}

std::optional<CarryQuote> carry_quote(const ContractBook& book, const ContractKey& held, Day day,
                                      const std::string& cycle) {
    const CarryNeighbour neighbour = carry_neighbour(book, held, day, cycle);
    if (neighbour.contract == nullptr || neighbour.months == 0) return std::nullopt;
    const Contract* own = book.find(held);
    const ContractBar* held_bar = own != nullptr ? own->bar_on(day) : nullptr;
    const ContractBar* other_bar = neighbour.contract->bar_on(day);
    if (held_bar == nullptr || other_bar == nullptr) return std::nullopt;
    return CarryQuote{held_bar->close, other_bar->close, neighbour.months, held.raw_symbol,
                      neighbour.contract->raw_symbol};
}

}  // namespace trade_ngin::contract_rolls
