// src/data/listing_dates.cpp
#include "trade_ngin/data/listing_dates.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <stdexcept>

namespace trade_ngin {

namespace {

bool parse_ymd(const std::string& text, Timestamp* out) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (text[i] < '0' || text[i] > '9') return false;
    }
    const std::chrono::year_month_day ymd{std::chrono::year{std::stoi(text.substr(0, 4))},
                                          std::chrono::month{static_cast<unsigned>(std::stoi(text.substr(5, 2)))},
                                          std::chrono::day{static_cast<unsigned>(std::stoi(text.substr(8, 2)))}};
    if (!ymd.ok()) return false;
    *out = std::chrono::sys_days{ymd};
    return true;
}

std::string suffix_of(const std::string& symbol) {
    const auto dot = symbol.find('.');
    return dot == std::string::npos ? std::string() : symbol.substr(dot);
}

}  // namespace

ListingDates& ListingDates::instance() {
    static ListingDates instance;
    return instance;
}

std::string ListingDates::root_of(const std::string& symbol) {
    return symbol.substr(0, symbol.find('.'));
}

std::vector<ListingDates::Entry> ListingDates::build(const std::vector<ListedContract>& contracts) {
    std::vector<Entry> entries;
    std::set<std::string> roots;
    for (const auto& contract : contracts) {
        Entry entry;
        entry.contract = contract;
        if (contract.symbol.empty() || contract.before.empty() ||
            contract.symbol.find('.') != std::string::npos ||
            contract.before.find('.') != std::string::npos) {
            throw std::invalid_argument(
                "listing_dates: \"symbol\" and \"before\" are non-empty roots without a suffix");
        }
        if (!parse_ymd(contract.listed, &entry.listed_at)) {
            throw std::invalid_argument("listing_dates: \"listed\" of " + contract.symbol +
                                        " is not a YYYY-MM-DD date: " + contract.listed);
        }
        if (!(contract.ratio > 0.0) || !std::isfinite(contract.ratio)) {
            throw std::invalid_argument("listing_dates: \"ratio\" of " + contract.symbol +
                                        " is not a positive number");
        }
        if (!roots.insert(contract.symbol).second || !roots.insert(contract.before).second) {
            throw std::invalid_argument("listing_dates: a root is named twice (" + contract.symbol +
                                        " / " + contract.before + ")");
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

void ListingDates::validate(const std::vector<ListedContract>& contracts) {
    (void)build(contracts);
}

void ListingDates::set(const std::vector<ListedContract>& contracts) {
    std::vector<Entry> entries = build(contracts);
    std::lock_guard<std::mutex> lock(mutex_);
    entries_ = std::move(entries);
}

void ListingDates::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

bool ListingDates::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !entries_.empty();
}

std::vector<ListedContract> ListingDates::contracts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ListedContract> out;
    for (const auto& entry : entries_) out.push_back(entry.contract);
    return out;
}

bool ListingDates::tradeable(const std::string& symbol, const Timestamp& bar_time) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) return true;
    const std::string root = root_of(symbol);
    for (const auto& entry : entries_) {
        if (root == entry.contract.symbol) return !(bar_time < entry.listed_at);
        if (root == entry.contract.before) return bar_time < entry.listed_at;
    }
    return true;
}

bool ListingDates::is_predecessor(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string root = root_of(symbol);
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const Entry& entry) { return root == entry.contract.before; });
}

std::string ListingDates::pair_root(const std::string& root) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        if (root == entry.contract.before) return entry.contract.symbol;
    }
    return root;
}

std::vector<std::string> ListingDates::predecessor_symbols(const std::vector<std::string>& symbols,
                                                           const Timestamp& window_start) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& entry : entries_) {
        if (!(window_start < entry.listed_at)) continue;
        for (const auto& symbol : symbols) {
            if (root_of(symbol) != entry.contract.symbol) continue;
            const std::string predecessor = entry.contract.before + suffix_of(symbol);
            if (std::find(symbols.begin(), symbols.end(), predecessor) == symbols.end() &&
                std::find(out.begin(), out.end(), predecessor) == out.end()) {
                out.push_back(predecessor);
            }
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> ListingDates::pairs_in(
    const std::vector<std::string>& symbols) const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& symbol : symbols) {
        const std::string root = root_of(symbol);
        for (const auto& entry : entries_) {
            if (root == entry.contract.before) {
                out.emplace_back(symbol, entry.contract.symbol + suffix_of(symbol));
            }
        }
    }
    return out;
}

std::vector<std::string> ListingDates::stored_symbols(const std::vector<std::string>& symbols) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) return symbols;
    std::vector<std::string> out;
    for (const auto& symbol : symbols) {
        const std::string root = root_of(symbol);
        const bool predecessor =
            std::any_of(entries_.begin(), entries_.end(),
                        [&](const Entry& entry) { return root == entry.contract.before; });
        if (!predecessor) out.push_back(symbol);
    }
    return out;
}

std::vector<ListingConversion> ListingDates::conversions_due(
    const std::vector<Bar>& signal_feed) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ListingConversion> out;
    for (const auto& entry : entries_) {
        for (const auto& to_bar : signal_feed) {
            if (root_of(to_bar.symbol) != entry.contract.symbol) continue;
            if (to_bar.timestamp < entry.listed_at) continue;
            const std::string from = entry.contract.before + suffix_of(to_bar.symbol);
            for (const auto& from_bar : signal_feed) {
                if (from_bar.symbol != from) continue;
                // the last bar of each symbol in the feed is its signal close
                ListingConversion c{from, to_bar.symbol, entry.contract.ratio,
                                    static_cast<double>(from_bar.close),
                                    static_cast<double>(to_bar.close)};
                auto same = std::find_if(out.begin(), out.end(), [&](const ListingConversion& x) {
                    return x.from == c.from && x.to == c.to;
                });
                if (same == out.end()) {
                    out.push_back(c);
                } else {
                    *same = c;
                }
            }
        }
    }
    return out;
}

const char* to_string(ListingSwitchRule rule) {
    switch (rule) {
        case ListingSwitchRule::kCloseReenter: return "close_reenter";
        case ListingSwitchRule::kOpenAtTarget: return "open_at_target";
        case ListingSwitchRule::kCarryToTarget: return "carry_to_target";
        default: return "convert";
    }
}

bool parse_listing_switch_rule(const std::string& text, ListingSwitchRule* out) {
    for (const auto rule : {ListingSwitchRule::kCloseReenter, ListingSwitchRule::kConvert,
                            ListingSwitchRule::kOpenAtTarget, ListingSwitchRule::kCarryToTarget}) {
        if (text == to_string(rule)) {
            *out = rule;
            return true;
        }
    }
    return false;
}

void ListingDates::set_switch_rule(ListingSwitchRule rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    rule_ = rule;
}

ListingSwitchRule ListingDates::switch_rule() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rule_;
}

ListingSwitch plan_listing_switch(ListingSwitchRule rule, double ratio, double held_from,
                                  double held_to, double target_to, double cap_to,
                                  bool in_deferral_band) {
    if (in_deferral_band && held_from != 0.0 &&
        (rule == ListingSwitchRule::kOpenAtTarget || rule == ListingSwitchRule::kCarryToTarget)) {
        rule = ListingSwitchRule::kConvert;
    }
    ListingSwitch plan;
    plan.new_to = held_to;
    if (rule == ListingSwitchRule::kCloseReenter) return plan;
    if (rule == ListingSwitchRule::kCarryToTarget && held_from == 0.0) return plan;
    plan.close_from = -held_from;
    if (rule == ListingSwitchRule::kConvert) {
        plan.new_to = held_to + ratio * held_from;
    } else {
        double target = target_to;
        if (cap_to > 0.0 && std::abs(target) > cap_to) target = target > 0.0 ? cap_to : -cap_to;
        // nearest whole contract, a half away from zero; never beyond the cap
        // (the pass's own rounding, optimization/one_pass.cpp)
        double whole = (target < 0.0 ? -1.0 : 1.0) * std::floor(std::abs(target) + 0.5);
        if (cap_to > 0.0 && std::abs(whole) > cap_to) whole = std::trunc(target);
        plan.new_to = whole;
    }
    plan.trade_to = plan.new_to - held_to;
    return plan;
}

std::vector<ExecutionReport> make_listing_switch_fills(
    const ListingConversion& conversion, const ListingSwitch& plan, const Timestamp& fill_time,
    const std::string& id_close, const std::string& id_open,
    const std::function<ListingLegCost(const std::string&, double, double)>& cost_of) {
    std::vector<ExecutionReport> legs;
    if (plan.close_from == 0.0 && plan.trade_to == 0.0) return legs;
    if (!(conversion.from_close > 0.0) || !(conversion.to_close > 0.0)) {
        throw std::invalid_argument("listing switch " + conversion.from + " -> " + conversion.to +
                                    ": a close is not positive");
    }
    auto leg = [&](const std::string& symbol, double signed_qty, double price, const std::string& id) {
        ExecutionReport e;
        e.symbol = symbol;
        e.execution_type = ExecutionType::STRATEGY;
        e.side = signed_qty > 0 ? Side::BUY : Side::SELL;
        e.filled_quantity = Decimal(std::abs(signed_qty));
        e.fill_price = Decimal(price);
        e.fill_time = fill_time;
        e.exec_id = id;
        e.order_id = id;
        const ListingLegCost c = cost_of(symbol, signed_qty, price);
        e.commissions_fees = Decimal(c.commissions_fees);
        e.implicit_price_impact = Decimal(c.implicit_price_impact);
        e.slippage_market_impact = Decimal(c.slippage_market_impact);
        e.total_transaction_costs = Decimal(c.total_transaction_costs);
        e.netting_adjustment = Decimal();
        e.is_partial = false;
        return e;
    };
    if (plan.close_from != 0.0) legs.push_back(leg(conversion.from, plan.close_from, conversion.from_close, id_close));
    if (plan.trade_to != 0.0) legs.push_back(leg(conversion.to, plan.trade_to, conversion.to_close, id_open));
    return legs;
}

std::vector<ListingDates::RelabelEntry> ListingDates::build_relabels(
    const std::vector<InstrumentIdRelabel>& relabels) {
    std::vector<RelabelEntry> out;
    for (const auto& r : relabels) {
        RelabelEntry e;
        e.relabel = r;
        if (r.symbol.empty() || r.symbol.find('.') != std::string::npos || r.from.empty() ||
            r.to.empty() || r.from == r.to) {
            throw std::invalid_argument(
                "instrument_id_relabels: \"symbol\" is a root and \"from\" and \"to\" are two ids");
        }
        if (!parse_ymd(r.date, &e.from_time)) {
            throw std::invalid_argument("instrument_id_relabels: \"date\" of " + r.symbol +
                                        " is not a YYYY-MM-DD date: " + r.date);
        }
        for (const auto& earlier : out) {
            if (earlier.relabel.symbol == r.symbol && earlier.relabel.to == r.to) {
                throw std::invalid_argument("instrument_id_relabels: " + r.symbol + " id " + r.to +
                                            " is named twice");
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

void ListingDates::validate_relabels(const std::vector<InstrumentIdRelabel>& relabels) {
    (void)build_relabels(relabels);
}

void ListingDates::set_relabels(const std::vector<InstrumentIdRelabel>& relabels) {
    auto built = build_relabels(relabels);
    std::lock_guard<std::mutex> lock(mutex_);
    relabels_ = std::move(built);
}

bool ListingDates::has_relabels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !relabels_.empty();
}

std::string ListingDates::read_id(const std::string& symbol, const Timestamp& bar_time,
                                  const std::string& instrument_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (relabels_.empty()) return instrument_id;
    const std::string root = root_of(symbol);
    std::string id = instrument_id;
    for (const auto& e : relabels_) {
        if (root == e.relabel.symbol && id == e.relabel.to && !(bar_time < e.from_time)) {
            id = e.relabel.from;
        }
    }
    return id;
}

void ListingDates::apply_relabels(std::vector<Bar>& bars) const {
    if (!has_relabels()) return;
    for (auto& bar : bars) bar.instrument_id = read_id(bar.symbol, bar.timestamp, bar.instrument_id);
}

void ListingDates::apply_relabels(std::vector<market_data_utils::FuturesInstrumentId>& ids) const {
    if (!has_relabels()) return;
    for (auto& row : ids) {
        Timestamp at;
        if (!parse_ymd(row.date, &at)) continue;
        row.instrument_id = read_id(row.symbol, at, row.instrument_id);
    }
}

Result<std::vector<market_data_utils::FuturesInstrumentId>> ListingDates::read_ids(
    const std::vector<std::string>& symbols,
    Result<std::vector<market_data_utils::FuturesInstrumentId>> ids) const {
    if (ids.is_error() || (!enabled() && !has_relabels())) return ids;
    auto rows = ids.value();
    // a predecessor reads its listed contract's rows and no other: rows stored under its own symbol
    // are left out, as the bar loader leaves its bars out
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&](const market_data_utils::FuturesInstrumentId& row) {
                                  return is_predecessor(row.symbol);
                              }),
               rows.end());
    apply_relabels(rows);
    add_predecessor_ids(symbols, rows);
    return Result<std::vector<market_data_utils::FuturesInstrumentId>>(std::move(rows));
}

void ListingDates::add_predecessor_bars(const std::vector<std::string>& symbols,
                                        std::vector<Bar>& bars) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) return;
    const auto pairs = pairs_in(symbols);
    const size_t stored = bars.size();
    for (const auto& [predecessor, listed] : pairs) {
        for (size_t i = 0; i < stored; ++i) {
            if (bars[i].symbol != listed) continue;
            Bar copy = bars[i];
            copy.symbol = predecessor;
            bars.push_back(std::move(copy));
        }
    }
}

void ListingDates::add_predecessor_ids(
    const std::vector<std::string>& symbols,
    std::vector<market_data_utils::FuturesInstrumentId>& ids) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) return;
    const auto pairs = pairs_in(symbols);
    const size_t stored = ids.size();
    for (const auto& [predecessor, listed] : pairs) {
        for (size_t i = 0; i < stored; ++i) {
            if (ids[i].symbol != listed) continue;
            auto copy = ids[i];
            copy.symbol = predecessor;
            ids.push_back(std::move(copy));
        }
    }
}

}  // namespace trade_ngin
