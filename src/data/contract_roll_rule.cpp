// src/data/contract_roll_rule.cpp
#include "trade_ngin/data/contract_roll_rule.hpp"

#include <algorithm>
#include <stdexcept>

#include "trade_ngin/core/logger.hpp"

namespace trade_ngin::contract_rolls {

namespace {

/// A contract the rule needs cannot be used (no expiration, no start): the series is refused.
class RuleRefusal : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Rule {
public:
    explicit Rule(const RuleInputs& in) : in_(in), book_(*in.book), cal_(*in.cal) {}

    /// The root whose contracts the series reads on `day`: the full-size root before a micro's change.
    const std::string& root_on(Day day) const {
        return in_.alias && day < in_.alias->change ? in_.alias->full_root : in_.root;
    }

    HeldSeries run();

private:
    const ContractKey& key_of(const Contract& c) {
        if (!c.key) {
            throw RuleRefusal(ContractBook::require(c).error()->what());
        }
        return *c.key;
    }

    const ContractDates& dates_of(const ContractKey& k) {
        auto it = dates_.find(k);
        if (it != dates_.end()) return it->second;
        ContractDates d = contract_dates(k, cal_);
        if (d.rule_last_trading_day && *d.rule_last_trading_day != d.last_trading_day) {
            ++out_.report.ltd_mismatches;
            warn(k.root + " " + k.raw_symbol + ": expiration " + ymd(d.last_trading_day) +
                 " differs from the exchange rule's last trading day " +
                 ymd(*d.rule_last_trading_day));
        }
        return dates_.emplace(k, d).first->second;
    }

    bool month_in_cycle(const Contract& c) const {
        const std::string& root = c.root;
        if (c.raw_symbol.size() <= root.size()) return false;
        return in_cycle(in_.cycle, ContractKey::month_of_code(c.raw_symbol[root.size()]));
    }

    /// The target on `day` for the held contract: the earliest-expiring contract printing that day
    /// whose month is in the cycle and which expires after the held one.
    const Contract* target_on(const Contract& held, Day day, bool* skipped_front) {
        const ContractKey& hk = key_of(held);
        auto found = cycle_target(book_, root_on(day), in_.cycle, hk, day);
        if (found.is_error()) throw RuleRefusal(found.error()->what());
        const Contract* best = found.value();
        if (best && skipped_front) {
            *skipped_front = best->key->delivery_index() !=
                             next_cycle_delivery(in_.cycle, hk.delivery_year, hk.delivery_month);
        }
        return best;
    }

    const Contract* vendor_on(size_t i) {
        if (i >= in_.grid.size()) return nullptr;
        return book_.named_by_id(root_on(in_.grid[i].date), in_.grid[i].instrument_id,
                                 in_.grid[i].date);
    }

    const Contract* start_contract(Day d0, const Contract* vendor) {
        if (vendor && month_in_cycle(*vendor) && d0 <= dates_of(key_of(*vendor)).last_actionable) {
            return vendor;
        }
        const Contract* best = nullptr;
        for (const Contract* c : book_.printing_on(root_on(d0), d0)) {
            if (!month_in_cycle(*c)) continue;
            const ContractKey& ck = key_of(*c);
            if (d0 > dates_of(ck).last_actionable) continue;
            if (!best || ck < *best->key) best = c;
        }
        if (!best) {
            throw RuleRefusal("root " + in_.root + ": no contract of the cycle " + in_.cycle +
                              " prints on the first date " + ymd(d0) +
                              " before its last actionable bar");
        }
        return best;
    }

    void warn(const std::string& msg) {
        WARN("contract_rolls: " + msg);
        out_.report.warnings.push_back(msg);
    }

    void fill_bar(HeldDay& day, const Contract& held, const Contract* vendor, const VendorBar& vb) {
        if (const ContractBar* b = held.bar_on(day.date)) {
            day.source = BarSource::OWN;
            day.bar_id = b->instrument_id;
            day.open = b->open;
            day.high = b->high;
            day.low = b->low;
            day.close = b->close;
            day.volume = b->volume;
            return;
        }
        if (vendor == &held) {
            // Class c, the vendor names the held contract and the table has no row: its own print.
            day.source = BarSource::VENDOR;
            day.bar_id = vb.instrument_id;
            day.open = vb.open;
            day.high = vb.high;
            day.low = vb.low;
            day.close = vb.close;
            day.volume = vb.volume;
            ++out_.report.vendor_bar_days;
            warn(in_.root + " " + ymd(day.date) + ": " + held.raw_symbol +
                 " has no contract row; the vendor names it, so its bar is the vendor's");
            return;
        }
        // Class c, the vendor names another contract: the held contract's last close, carried.
        day.source = BarSource::CARRY;
        day.bar_id.clear();
        day.volume = 0;
        ++out_.report.carry_days;
        auto it = held.bars.lower_bound(day.date);
        if (it == held.bars.begin()) {
            day.has_close = false;
            return;
        }
        --it;
        day.open = day.high = day.low = day.close = it->second.close;
    }

    const RuleInputs& in_;
    const ContractBook& book_;
    const TradingCalendar& cal_;
    std::map<ContractKey, ContractDates> dates_;
    HeldSeries out_;
};

HeldSeries Rule::run() {
    out_.root = in_.root;
    const auto& grid = in_.grid;
    if (grid.empty()) return out_;
    out_.days.reserve(grid.size());

    const Contract* held = nullptr;
    const Contract* last_known_vendor = nullptr;
    // A confirmed forward vendor switch, waiting for a trading-day bar.
    bool vendor_pending = false;
    Day vendor_since{};
    const Contract* vendor_contract = nullptr;
    // A roll whose trigger fired and which waits for the bars it needs.
    RollTrigger pending = RollTrigger::NONE;
    Day pending_since{};
    bool pending_off_cycle = false;

    for (size_t i = 0; i < grid.size(); ++i) {
        const VendorBar& vb = grid[i];
        const Day d = vb.date;
        const Contract* vendor = vendor_on(i);
        HeldDay day;
        day.date = d;
        if (vendor) {
            day.vendor = key_of(*vendor);
        } else {
            ++out_.report.unknown_vendor_days;
        }

        if (i == 0) {
            held = start_contract(d, vendor);
            day.trigger = RollTrigger::START;
        } else {
            // (a) The vendor's confirmed forward switch: a new vendor contract, later than the held
            // one, kept by the next bar. Detected on any bar; it rolls on a trading-day bar.
            if (vendor && vendor != last_known_vendor && !vendor_pending &&
                key_of(*held).expiration < key_of(*vendor).expiration && vendor_on(i + 1) == vendor) {
                vendor_pending = true;
                vendor_since = d;
                vendor_contract = vendor;
            }
            if (in_.alias && d >= in_.alias->change && held->root == in_.alias->full_root) {
                // The micro's change: one roll from the full-size contract into the micro's own of
                // the same expiry, on a trading-day bar where both print. Nothing else rolls first.
                if (cal_.is_trading_day(d)) {
                    const ContractKey& hk = key_of(*held);
                    const Contract* micro = nullptr;
                    for (const Contract* c : book_.printing_on(in_.root, d)) {
                        if (key_of(*c).expiration == hk.expiration &&
                            key_of(*c).delivery_month == hk.delivery_month) {
                            micro = c;
                        }
                    }
                    if (micro && held->bar_on(d)) {
                        day.roll = true;
                        day.old = hk;
                        day.old_close = held->bar_on(d)->close;
                        day.trigger = RollTrigger::ALIAS;
                        day.trigger_date = in_.alias->change;
                        day.deferred = d > in_.alias->change;
                        ++out_.report.rolls;
                        ++out_.report.rolls_by_trigger[to_string(RollTrigger::ALIAS)];
                        if (day.deferred) ++out_.report.deferred;
                        held = micro;
                        pending = RollTrigger::NONE;
                        vendor_pending = false;
                        vendor_contract = nullptr;
                    } else {
                        warn(in_.root + " " + ymd(d) + ": the change from " + hk.raw_symbol +
                             " to the micro's own contract waits for a bar where both print");
                    }
                }
            } else if (cal_.is_trading_day(d)) {
                const ContractKey& hk = key_of(*held);
                const ContractDates& hd = dates_of(hk);
                if (pending == RollTrigger::NONE) {
                    if (vendor_pending) {
                        pending = RollTrigger::VENDOR;
                        pending_since = vendor_since;
                        pending_off_cycle = !month_in_cycle(*vendor_contract);
                    } else {
                        const Contract* t = target_on(*held, d, nullptr);
                        const ContractBar* hb = held->bar_on(d);
                        const ContractBar* tb = t ? t->bar_on(d) : nullptr;
                        if (hd.window_first <= d && d <= hd.last_actionable && hb && tb &&
                            tb->volume > hb->volume) {
                            pending = RollTrigger::CROSSOVER;
                            pending_since = d;
                        } else if (d >= hd.last_actionable) {
                            pending = RollTrigger::DEADLINE;
                            pending_since = d;
                        }
                    }
                }
                if (pending != RollTrigger::NONE) {
                    bool skipped = false;
                    const Contract* target = target_on(*held, d, &skipped);
                    const ContractBar* hb = held->bar_on(d);
                    const bool fallback = target && !hb && d >= hd.last_actionable;
                    if (!target) {
                        ++out_.report.no_target_bars;
                        warn(in_.root + " " + ymd(d) + ": a roll from " + hk.raw_symbol +
                             " is due and no cycle contract after it prints");
                    } else if (hb || fallback) {
                        day.roll = true;
                        day.old = hk;
                        day.trigger = pending;
                        day.trigger_date = pending_since;
                        day.deferred = d > pending_since;
                        day.vendor_off_cycle = pending == RollTrigger::VENDOR && pending_off_cycle;
                        day.late = pending == RollTrigger::DEADLINE && d > hd.last_actionable;
                        day.target_skipped_front = skipped;
                        if (hb) {
                            day.old_close = hb->close;
                        } else {
                            const ContractBar* prev = target->bar_on(grid[i - 1].date);
                            day.fallback = prev ? RollFallback::NEW_CONTRACT_MOVE : RollFallback::ZERO;
                            if (prev) day.fallback_new_previous_close = prev->close;
                            const std::string c = in_.root + " " + ymd(d) + ": " + hk.raw_symbol +
                                                  " has no bar up to its last actionable bar " +
                                                  ymd(hd.last_actionable) + "; rolled to " +
                                                  target->raw_symbol + " with the fallback (" +
                                                  to_string(day.fallback) + ")";
                            out_.report.fallback_cases.push_back(c);
                            ++out_.report.fallbacks;
                            warn(c);
                        }
                        if (skipped) {
                            ++out_.report.target_skipped_front;
                            warn(in_.root + " " + ymd(d) + ": the cycle front after " +
                                 hk.raw_symbol + " has no bar; rolled to " + target->raw_symbol);
                        }
                        ++out_.report.rolls;
                        ++out_.report.rolls_by_trigger[to_string(pending)];
                        if (day.deferred) ++out_.report.deferred;
                        if (day.vendor_off_cycle) ++out_.report.vendor_off_cycle;
                        if (day.late) ++out_.report.late;
                        held = target;
                        pending = RollTrigger::NONE;
                        vendor_pending = false;
                        vendor_contract = nullptr;
                    }
                }
            }
        }
        if (vendor) last_known_vendor = vendor;
        day.held = key_of(*held);
        fill_bar(day, *held, vendor, vb);
        out_.days.push_back(std::move(day));
    }
    return out_;
}

}  // namespace

std::optional<MicroAlias> micro_alias(const std::string& root) {
    static const std::map<std::string, std::string> full = {
        {"MES", "ES"}, {"MNQ", "NQ"}, {"MYM", "YM"}, {"M2K", "RTY"}};
    auto it = full.find(root);
    if (it == full.end()) return std::nullopt;
    return MicroAlias{it->second, parse_ymd("2026-02-22")};
}

const char* to_string(RollTrigger trigger) {
    switch (trigger) {
        case RollTrigger::NONE:
            return "";
        case RollTrigger::START:
            return "start";
        case RollTrigger::VENDOR:
            return "vendor";
        case RollTrigger::CROSSOVER:
            return "crossover";
        case RollTrigger::DEADLINE:
            return "deadline";
        case RollTrigger::ALIAS:
            return "alias";
    }
    return "";
}

const char* to_string(BarSource source) {
    switch (source) {
        case BarSource::OWN:
            return "own";
        case BarSource::VENDOR:
            return "vendor";
        case BarSource::CARRY:
            return "carry";
    }
    return "";
}

const char* to_string(RollFallback fallback) {
    switch (fallback) {
        case RollFallback::NONE:
            return "";
        case RollFallback::NEW_CONTRACT_MOVE:
            return "new_move";
        case RollFallback::ZERO:
            return "zero";
    }
    return "";
}

Result<const Contract*> cycle_target(const ContractBook& book, const std::string& root,
                                     const std::string& cycle, const ContractKey& held, Day day) {
    const Contract* best = nullptr;
    for (const Contract* c : book.printing_on(root, day)) {
        if (c->raw_symbol.size() <= root.size() ||
            !in_cycle(cycle, ContractKey::month_of_code(c->raw_symbol[root.size()]))) {
            continue;
        }
        auto ck = ContractBook::require(*c);
        if (ck.is_error()) {
            return make_error<const Contract*>(ck.error()->code(), ck.error()->what(),
                                               "contract_rolls");
        }
        if (!(held.expiration < ck.value().expiration)) continue;
        if (!best || ck.value() < *best->key) best = c;
    }
    return best;
}

ContractDates contract_dates(const ContractKey& key, const TradingCalendar& cal) {
    ContractDates d;
    d.last_trading_day = key.expiration;
    d.first_notice_day =
        ExchangeRules::first_notice_day(key.root, key.delivery_year, key.delivery_month, cal);
    d.deadline = d.first_notice_day && *d.first_notice_day < d.last_trading_day
                     ? *d.first_notice_day
                     : d.last_trading_day;
    d.window_first = cal.trading_day_before(d.deadline, 5);
    // The latest trading day t whose next calendar day is still before D*.
    d.last_actionable = cal.trading_day_before(d.deadline - std::chrono::days{1}, 1);
    if (ExchangeRules::knows(key.root)) {
        d.rule_last_trading_day =
            ExchangeRules::last_trading_day(key.root, key.delivery_year, key.delivery_month, cal);
    }
    return d;
}

Result<HeldSeries> apply_roll_rule(const RuleInputs& in) {
    if (!in.book || !in.cal) {
        return make_error<HeldSeries>(ErrorCode::INVALID_ARGUMENT,
                                      "apply_roll_rule needs a contract book and a calendar",
                                      "contract_rolls");
    }
    if (in.cycle.empty()) {
        return make_error<HeldSeries>(ErrorCode::INVALID_ARGUMENT,
                                      "root " + in.root + " has no roll cycle", "contract_rolls");
    }
    try {
        Rule rule(in);
        return rule.run();
    } catch (const RuleRefusal& e) {
        return make_error<HeldSeries>(ErrorCode::INVALID_DATA, e.what(), "contract_rolls");
    } catch (const CalendarCoverageError& e) {
        return make_error<HeldSeries>(ErrorCode::INVALID_DATA, e.what(), "contract_rolls");
    }
}

}  // namespace trade_ngin::contract_rolls
