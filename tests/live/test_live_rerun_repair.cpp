// tests/live/test_live_rerun_repair.cpp
//
// Issue #188: the re-run repair after late bars that confirm a roll. When a day's bars arrive late,
// the first pass books the roll a day (or more) late; the repair re-runs the dates in order, oldest
// first. The re-run of the roll's own date books it there; the re-run of the date the first pass
// booked it on must not read its own stale legs as the contract held coming in (it did, and booked
// the roll a second time: the cost twice, the legs moved to the wrong date). The contract held
// coming in is decided by contract_held_coming_into, which both futures runners call.
//
// The chains below are driven as the runner drives them, one run per calendar date: each run
// consumes the bars dated before it that exist at that moment, reads the contract on the stored T-1
// row and the ROLL legs already stored for its date and for earlier dates, decides the contract
// held coming in, legs by state (live_rolls_by_state), settles T-1 on the consumed bars (less what
// the stored rows already book of a late roll), stores its legs as replace_roll_day_executions does
// (the day's rows swept, rows of the same order ids on any date replaced) and re-writes the T-1 row
// and its own row with the contract held after the bars it consumed. A repaired chain must end
// identical to the clean chain on every stored cell the model keeps: each row's contract and
// realised move, each leg's date, id and contract, and the roll fills charged on each date.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/live/live_roll_legs.hpp"
#include "trade_ngin/live/session_book_gate.hpp"

using namespace trade_ngin;

namespace {

Timestamp at(const std::string& ymd) {
    std::tm tm{};
    tm.tm_year = std::stoi(ymd.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(ymd.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(ymd.substr(8, 2));
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}
std::string plus(const std::string& ymd, int n) {
    return core::format_utc_date(at(ymd) + std::chrono::hours(24 * n));
}

Bar bar(const std::string& symbol, const std::string& ymd, double close, const std::string& id) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = at(ymd);
    b.open = Decimal(close);
    b.high = Decimal(close * 1.01);
    b.low = Decimal(close * 0.99);
    b.close = Decimal(close);
    b.volume = 100000.0;
    b.instrument_id = id;
    return b;
}

// One symbol's weekday bars, 2026-04-20 .. 2026-05-04 (no bar on 04-25, 04-26, 05-02, 05-03): the
// contract of each date and the closes.
std::vector<Bar> series(const std::string& symbol, const std::vector<std::string>& ids, double first) {
    static const std::vector<std::string> kDays = {"2026-04-20", "2026-04-21", "2026-04-22",
                                                   "2026-04-23", "2026-04-24", "2026-04-27",
                                                   "2026-04-28", "2026-04-29", "2026-04-30",
                                                   "2026-05-01", "2026-05-04"};
    std::vector<Bar> out;
    double close = first;
    for (size_t i = 0; i < kDays.size(); ++i) {
        // A move that differs every day, so a move booked on the wrong row is seen.
        close += (i % 3 == 0 ? 2.75 : (i % 3 == 1 ? -1.25 : 4.5)) + 0.01 * static_cast<double>(i);
        // A change bar trades at another level (the next contract's).
        const bool other = i > 0 && ids[i] != ids[0];
        out.push_back(bar(symbol, kDays[i], close + (other ? 26.0 : 0.0), ids[i]));
    }
    return out;
}

// ZS.v.0, the measured case (T-DATED case B): 42009683 to 194904, the change bar 04-27, the
// confirming bar 04-28.
const std::string A = "42009683", B = "194904";
std::vector<Bar> zs() { return series("ZS.v.0", {A, A, A, A, A, B, B, B, B, B, B}, 1160.0); }
// ZM.v.0 rolls on the same bars as ZS.
std::vector<Bar> zm() {
    return series("ZM.v.0", {"42282755", "42282755", "42282755", "42282755", "42282755", "748931",
                             "748931", "748931", "748931", "748931", "748931"},
                  330.0);
}
// ZL.v.0 rolls later, on time: the change bar 04-29, the confirming bar 04-30 (legged on the 05-01
// run, inside the repaired span).
std::vector<Bar> zl() {
    return series("ZL.v.0", {"42281352", "42281352", "42281352", "42281352", "42281352", "42281352",
                             "42281352", "608206", "608206", "608206", "608206"},
                  67.0);
}
// ZS.v.0 with a FLIP: the change bar 04-27 reverts on 04-28 (no fills), a new change bar 04-29,
// confirmed 04-30.
std::vector<Bar> zs_flip() { return series("ZS.v.0", {A, A, A, A, A, B, A, B, B, B, B}, 1160.0); }
// 6A.v.0 never rolls.
std::vector<Bar> six_a() { return series("6A.v.0", std::vector<std::string>(11, "1419"), 70.0); }

std::vector<Bar> join(const std::vector<std::vector<Bar>>& parts) {
    std::vector<Bar> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    std::sort(out.begin(), out.end(), [](const Bar& a, const Bar& b) {
        return a.timestamp != b.timestamp ? a.timestamp < b.timestamp : a.symbol < b.symbol;
    });
    return out;
}

constexpr double kPointValue = 100.0;  // every symbol held long 1

struct Leg {
    std::string date, exec_id, contract;
    bool operator<(const Leg& o) const {
        return std::tie(date, exec_id, contract) < std::tie(o.date, o.exec_id, o.contract);
    }
    bool operator==(const Leg& o) const {
        return date == o.date && exec_id == o.exec_id && contract == o.contract;
    }
};
struct Row {
    std::string contract;
    double realised{0.0};
};

// The stored state of one book (one sleeve).
struct Book {
    std::map<std::string, std::map<std::string, Row>> rows;  // date -> symbol -> row
    std::vector<Leg> legs;
    std::map<std::string, size_t> roll_fills;  // run date -> ROLL legs the run charged
    std::vector<std::string> stale_seen;       // "date symbol": legs the decision set aside
};

using Decision = std::function<HeldComingIn(const std::unordered_map<std::string, std::string>&,
                                            const std::unordered_map<std::string, std::string>&,
                                            const std::unordered_map<std::string, std::string>&)>;

// The rule before #188: the day's own legs always win.
HeldComingIn old_rule(const std::unordered_map<std::string, std::string>& t1_row,
                      const std::unordered_map<std::string, std::string>& todays_legs,
                      const std::unordered_map<std::string, std::string>&) {
    HeldComingIn out{t1_row, {}};
    for (const auto& [symbol, contract] : todays_legs) {
        const auto held = out.contract.find(symbol);
        if (held != out.contract.end() && !contract.empty()) held->second = contract;
    }
    return out;
}

const Decision kFixed = contract_held_coming_into;

std::string symbol_of(const std::string& exec_id) {  // EXEC_<symbol>_<YYYYMMDD>_RC
    return exec_id.substr(5, exec_id.size() - 5 - 12);
}

// One run of the runner's composition, dated `run_date`, on the bars `available` at that moment.
void run(Book& book, const std::vector<Bar>& available, const std::string& run_date,
         const Decision& decide) {
    const std::string t1 = plus(run_date, -1);
    std::vector<Bar> consumed;
    for (const auto& b : available) {
        if (core::format_utc_date(b.timestamp) < run_date) consumed.push_back(b);
    }
    // The stored T-1 book (load_positions_by_date): the latest row on or before T-1 is not read;
    // the row dated T-1 is, as in the runner.
    std::unordered_map<std::string, std::string> t1_row;
    if (const auto r = book.rows.find(t1); r != book.rows.end()) {
        for (const auto& [symbol, row] : r->second) t1_row[symbol] = row.contract;
    }
    // get_stored_roll_contracts: the earliest closing leg of the date, per symbol.
    // get_previous_roll_contracts: the latest date before the run's, then the latest confirming date.
    std::map<std::string, std::pair<std::string, std::string>> first_today, latest_before;  // symbol -> (key, contract)
    for (const auto& leg : book.legs) {
        if (leg.exec_id.size() < 3 || leg.exec_id.substr(leg.exec_id.size() - 3) != "_RC") continue;
        const std::string s = symbol_of(leg.exec_id);
        if (leg.date == run_date) {
            if (!first_today.count(s) || leg.exec_id < first_today[s].first) first_today[s] = {leg.exec_id, leg.contract};
        } else if (leg.date < run_date) {
            const std::string key = leg.date + "|" + leg.exec_id;
            if (!latest_before.count(s) || key > latest_before[s].first) latest_before[s] = {key, leg.contract};
        }
    }
    std::unordered_map<std::string, std::string> todays, earlier;
    for (const auto& [s, v] : first_today) todays[s] = v.second;
    for (const auto& [s, v] : latest_before) earlier[s] = v.second;
    const HeldComingIn held_in = decide(t1_row, todays, earlier);
    if (todays.empty()) {
        EXPECT_TRUE(held_in.stale.empty()) << run_date << ": a run with no leg of its own sets none aside";
    }
    for (const auto& s : held_in.stale) book.stale_seen.push_back(run_date + " " + s);

    auto state = live_rolls_by_state(consumed, held_in.contract, t1);
    EXPECT_TRUE(state.unplaced.empty()) << run_date;
    if (!state.late.empty()) {
        std::vector<StoredRealisedRow> stored;
        for (const auto& [date, by_symbol] : book.rows) {
            for (const auto& [symbol, row] : by_symbol) {
                stored.push_back({symbol, "TREND_FOLLOWING", date, 1.0, row.realised});
            }
        }
        for (auto& [symbol, late] : state.late) {
            const LateBooked booked = late_booked_points(symbol, late.booked_after, t1, stored, kPointValue);
            EXPECT_TRUE(booked.undecided.empty()) << run_date << " " << symbol << ": " << booked.undecided;
            late.settle_from += booked.points;
        }
    }

    // The legs, stored as replace_roll_day_executions stores them.
    std::vector<Leg> legs;
    for (const auto& r : state.rolls) {
        const std::string tag = r.symbol + "_" + compact_date(r.confirm_date);
        legs.push_back({run_date, "EXEC_" + tag + "_RC", r.outgoing_id});
        legs.push_back({run_date, "EXEC_" + tag + "_RO", r.incoming_id});
    }
    std::set<std::string> ids;
    for (const auto& l : legs) ids.insert(l.exec_id);
    std::erase_if(book.legs, [&](const Leg& l) { return l.date == run_date || ids.count(l.exec_id); });
    book.legs.insert(book.legs.end(), legs.begin(), legs.end());
    book.roll_fills[run_date] = legs.size();

    // The T-1 settlement on the consumed bars and the two rows the run writes.
    std::unordered_map<std::string, double> raw_t1, raw_t2;
    std::map<std::string, const Bar*> before;
    for (const auto& b : consumed) {
        if (core::format_utc_date(b.timestamp) == t1) {
            raw_t1[b.symbol] = static_cast<double>(b.close);
            if (before.count(b.symbol)) raw_t2[b.symbol] = static_cast<double>(before[b.symbol]->close);
        }
        before[b.symbol] = &b;
    }
    const auto status = roll_series::roll_status_of(consumed);
    const auto st = consumed_t1_settlement(consumed, t1, status, {}, raw_t2, raw_t1, state.late);
    for (const auto& [symbol, contract] : t1_row) {
        Row& row = book.rows[t1][symbol];
        const auto to = st.t1_close_prices.find(symbol);
        const auto from = st.t2_close_prices.find(symbol);
        row.realised = (to != st.t1_close_prices.end() && from != st.t2_close_prices.end() &&
                        raw_t1.count(symbol) && !st.zero_pnl_symbols.count(symbol))
                           ? kPointValue * (to->second - from->second)
                           : 0.0;
        if (const auto s = status.find(symbol); s != status.end()) row.contract = s->second.held_id;
    }
    for (const auto& [symbol, contract] : t1_row) {
        const auto s = status.find(symbol);
        book.rows[run_date][symbol] = {s != status.end() ? s->second.held_id : contract, 0.0};
    }
}

// A chain from `first` to `last`, the book seeded on the day before `first` in each symbol's first
// contract. Late bars: the bars in `late` are missing for the runs dated `late_from` .. `late_to`;
// they then arrive and the dates from `late_from` to `last` are re-run in order (the repair).
struct LateBars {
    std::set<std::pair<std::string, std::string>> bars;  // (symbol, date)
    std::string from, to;
};
LateBars whole_days(const std::vector<Bar>& all, const std::set<std::string>& dates,
                    const std::string& from, const std::string& to) {
    LateBars late{{}, from, to};
    for (const auto& b : all) {
        const std::string d = core::format_utc_date(b.timestamp);
        if (dates.count(d)) late.bars.insert({b.symbol, d});
    }
    return late;
}

Book seeded(const std::vector<Bar>& all, const std::string& first) {
    Book book;
    for (const auto& b : all) {
        auto& row = book.rows[plus(first, -1)];
        if (!row.count(b.symbol)) row[b.symbol] = {b.instrument_id, 0.0};
    }
    return book;
}

Book chain(const std::vector<Bar>& all, const std::string& first, const std::string& last,
           const Decision& decide, const LateBars* late = nullptr, Book* before_repair = nullptr) {
    Book book = seeded(all, first);
    const std::string first_pass_end = late ? late->to : last;
    for (std::string d = first; d <= first_pass_end; d = plus(d, 1)) {
        std::vector<Bar> available;
        for (const auto& b : all) {
            const bool missing = late && d >= late->from && d <= late->to &&
                                 late->bars.count({b.symbol, core::format_utc_date(b.timestamp)});
            if (!missing) available.push_back(b);
        }
        run(book, available, d, decide);
    }
    if (!late) return book;
    if (before_repair) *before_repair = book;
    for (std::string d = late->from; d <= last; d = plus(d, 1)) run(book, all, d, decide);
    return book;
}

// Every stored cell the model keeps, one line each, for an exact comparison with named differences.
std::vector<std::string> cells(const Book& book) {
    std::vector<std::string> out;
    for (const auto& [date, by_symbol] : book.rows) {
        for (const auto& [symbol, row] : by_symbol) {
            char pnl[64];
            std::snprintf(pnl, sizeof pnl, "%.6f", row.realised);
            out.push_back("positions " + date + " " + symbol + " contract=" + row.contract + " realised=" + pnl);
        }
    }
    std::vector<Leg> legs = book.legs;
    std::sort(legs.begin(), legs.end());
    for (const auto& l : legs) out.push_back("executions " + l.date + " " + l.exec_id + " " + l.contract);
    for (const auto& [date, n] : book.roll_fills) {
        out.push_back("live_results " + date + " roll_fills=" + std::to_string(n));
    }
    return out;
}

std::vector<std::string> differences(const Book& a, const Book& b) {
    const auto ca = cells(a), cb = cells(b);
    std::vector<std::string> out;
    std::set_symmetric_difference(ca.begin(), ca.end(), cb.begin(), cb.end(), std::back_inserter(out));
    return out;
}

std::string listing(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) out += "\n  " + l;
    return out;
}

const std::string kFirst = "2026-04-21", kLast = "2026-05-05";

}  // namespace

// ---- the decision itself ------------------------------------------------------------------------

// A first run of a date has no leg of its own: the T-1 row is the state, whatever earlier legs say.
TEST(ContractHeldComingInto, AFirstRunTakesTheT1Row) {
    const auto held = contract_held_coming_into({{"ZS.v.0", B}, {"6A.v.0", "1419"}}, {}, {{"ZS.v.0", A}});
    EXPECT_EQ(held.contract.at("ZS.v.0"), B);
    EXPECT_EQ(held.contract.at("6A.v.0"), "1419");
    EXPECT_TRUE(held.stale.empty());
}

// A re-run of a roll day whose bars were on time: the first run re-wrote the T-1 row with the
// incoming contract; the day's legs carry the outgoing one, and the latest earlier legs are the
// symbol's previous roll (another contract). The legs win, as before #188.
TEST(ContractHeldComingInto, AReRunOfARollDayTakesItsOwnLegs) {
    const auto held = contract_held_coming_into({{"ZS.v.0", B}, {"6A.v.0", "1419"}}, {{"ZS.v.0", A}},
                                                {{"ZS.v.0", "41000001"}, {"6A.v.0", "1300"}});
    EXPECT_EQ(held.contract.at("ZS.v.0"), A);
    EXPECT_EQ(held.contract.at("6A.v.0"), "1419");
    EXPECT_TRUE(held.stale.empty());
    // No earlier leg at all (the symbol's first roll in the book): the same.
    EXPECT_EQ(contract_held_coming_into({{"ZS.v.0", B}}, {{"ZS.v.0", A}}, {}).contract.at("ZS.v.0"), A);
}

// The repair after late bars: the day's legs roll out of the contract the latest earlier legs
// already rolled out of. They are stale; the T-1 row (written by the earlier date's re-run) is
// kept, and the symbol is named. Another symbol of the same day with fresh legs takes its legs.
TEST(ContractHeldComingInto, LegsOfARollAnEarlierDateBookedAreStale) {
    const auto held = contract_held_coming_into(
        {{"ZS.v.0", B}, {"ZM.v.0", "748931"}, {"ZL.v.0", "608206"}},
        {{"ZS.v.0", A}, {"ZM.v.0", "42282755"}, {"ZL.v.0", "42281352"}},
        {{"ZS.v.0", A}, {"ZM.v.0", "42282755"}, {"ZL.v.0", "41999999"}});
    EXPECT_EQ(held.contract.at("ZS.v.0"), B);
    EXPECT_EQ(held.contract.at("ZM.v.0"), "748931");
    EXPECT_EQ(held.contract.at("ZL.v.0"), "42281352");
    EXPECT_EQ(held.stale, (std::vector<std::string>{"ZM.v.0", "ZS.v.0"}));
}

// A contract that recurs: A to B, then B back to A (a confirmed return is a new roll), then A to C on
// the re-run date. Only the LATEST earlier legs are compared: they roll out of B, so the day's legs
// out of A are not stale.
TEST(ContractHeldComingInto, ARecurringContractIsToldApartByTheLatestEarlierLegs) {
    const auto held = contract_held_coming_into({{"ZS.v.0", "C"}}, {{"ZS.v.0", "A"}}, {{"ZS.v.0", "B"}});
    EXPECT_EQ(held.contract.at("ZS.v.0"), "A");
    EXPECT_TRUE(held.stale.empty());
}

// Legs of a symbol the T-1 book does not hold, and an empty stored contract, change nothing.
TEST(ContractHeldComingInto, LegsOfAnUnheldSymbolOrWithoutAContractChangeNothing) {
    const auto held = contract_held_coming_into({{"ZS.v.0", B}}, {{"ZM.v.0", "42282755"}, {"ZS.v.0", ""}},
                                                {{"ZM.v.0", "42282755"}});
    EXPECT_EQ(held.contract.size(), 1u);
    EXPECT_EQ(held.contract.at("ZS.v.0"), B);
    EXPECT_TRUE(held.stale.empty());
}

// ---- the chains: clean, and late bars then the re-run repair ---------------------------------------

// The control: the clean chain legs the ZS roll once, on the 04-29 run (T-1 = the confirming bar),
// ids dated 04-28; re-running every date of it in order (a repair with nothing late) changes nothing.
TEST(LiveRerunRepair, TheCleanChainLegsOnceAndARepairWithNothingLateChangesNothing) {
    const auto all = join({zs(), six_a()});
    const Book clean = chain(all, kFirst, kLast, kFixed);
    std::vector<Leg> legs = clean.legs;
    std::sort(legs.begin(), legs.end());
    ASSERT_EQ(legs.size(), 2u);
    EXPECT_EQ(legs[0], (Leg{"2026-04-29", "EXEC_ZS.v.0_20260428_RC", A}));
    EXPECT_EQ(legs[1], (Leg{"2026-04-29", "EXEC_ZS.v.0_20260428_RO", B}));
    const LateBars nothing{{}, "2026-04-24", "2026-04-30"};
    const Book again = chain(all, kFirst, kLast, kFixed, &nothing);
    EXPECT_TRUE(differences(clean, again).empty()) << listing(differences(clean, again));
    EXPECT_TRUE(again.stale_seen.empty()) << listing(again.stale_seen);
}

// Shape 1, the measured case: the bars of 04-28 (the confirming bar) arrive after the runs dated
// 04-29 .. 05-01. The first pass legs the roll on the 04-30 run, ids dated 04-29 (the next bar
// confirms). Re-run in order: identical to the clean chain. The 04-30 re-run sets ZS's stale legs
// aside. Under the old rule it booked the roll again on 04-30 (the legs moved there, the fills
// charged on both dates): the defect.
TEST(LiveRerunRepair, ARollConfirmedOneBarLateEndsIdenticalToTheCleanChain) {
    const auto all = join({zs(), six_a()});
    const Book clean = chain(all, kFirst, kLast, kFixed);
    const LateBars late = whole_days(all, {"2026-04-28"}, "2026-04-29", "2026-05-01");
    Book first_pass;
    const Book repaired = chain(all, kFirst, kLast, kFixed, &late, &first_pass);
    ASSERT_EQ(first_pass.roll_fills.at("2026-04-30"), 2u) << "the first pass legs the roll a day late";
    ASSERT_FALSE(differences(clean, first_pass).empty());
    EXPECT_TRUE(differences(clean, repaired).empty()) << listing(differences(clean, repaired));
    EXPECT_EQ(repaired.stale_seen, (std::vector<std::string>{"2026-04-30 ZS.v.0"}));

    const Book old = chain(all, kFirst, kLast, old_rule, &late);
    const auto old_diff = differences(clean, old);
    EXPECT_FALSE(old_diff.empty()) << "the old rule must fail this shape (the test has no teeth)";
    EXPECT_EQ(old.roll_fills.at("2026-04-29") + old.roll_fills.at("2026-04-30"), 4u)
        << "the old rule charges the roll on both dates";
    EXPECT_EQ(std::count_if(old.legs.begin(), old.legs.end(),
                            [](const Leg& l) { return l.date == "2026-04-30"; }),
              2)
        << "and moves the legs to 04-30";
}

// Shape 2: the bars of 04-28 and 04-29 arrive late (the runs 04-29 .. 05-01 are made without them).
// The first pass legs the roll on the 05-01 run, ids dated 04-30: two bars late. Re-run in order:
// identical to the clean chain; the 05-01 re-run sets the stale legs aside (the 04-30 re-run has no
// leg of its own: the first pass legged nothing on 04-30).
TEST(LiveRerunRepair, ARollConfirmedTwoBarsLateEndsIdenticalToTheCleanChain) {
    const auto all = join({zs(), six_a()});
    const Book clean = chain(all, kFirst, kLast, kFixed);
    const LateBars late = whole_days(all, {"2026-04-28", "2026-04-29"}, "2026-04-29", "2026-05-01");
    Book first_pass;
    const Book repaired = chain(all, kFirst, kLast, kFixed, &late, &first_pass);
    ASSERT_EQ(first_pass.roll_fills.at("2026-05-01"), 2u) << "the first pass legs the roll two days late";
    ASSERT_EQ(first_pass.roll_fills.at("2026-04-30"), 0u);
    EXPECT_TRUE(differences(clean, repaired).empty()) << listing(differences(clean, repaired));
    EXPECT_EQ(repaired.stale_seen, (std::vector<std::string>{"2026-05-01 ZS.v.0"}));
    EXPECT_FALSE(differences(clean, chain(all, kFirst, kLast, old_rule, &late)).empty())
        << "the old rule must fail this shape";
}

// Shape 3: two symbols (ZS, ZM) roll on the same late bar; a third (ZL) rolls on time inside the
// repaired span, so one re-run holds stale legs (ZS, ZM on 04-30) and a later one fresh legs it must
// still take (ZL on 05-01: its earlier legs are none). Identical to the clean chain.
TEST(LiveRerunRepair, TwoSymbolsRollingOnTheSameLateBarEndIdenticalToTheCleanChain) {
    const auto all = join({zs(), zm(), zl(), six_a()});
    const Book clean = chain(all, kFirst, kLast, kFixed);
    std::vector<Leg> clean_legs = clean.legs;
    std::sort(clean_legs.begin(), clean_legs.end());
    ASSERT_EQ(clean_legs.size(), 6u);
    EXPECT_EQ(clean_legs[4].date, "2026-05-01") << "ZL legged on the 05-01 run";
    const LateBars late = whole_days(all, {"2026-04-28"}, "2026-04-29", "2026-05-01");
    Book first_pass;
    const Book repaired = chain(all, kFirst, kLast, kFixed, &late, &first_pass);
    ASSERT_EQ(first_pass.roll_fills.at("2026-04-30"), 4u) << "both late rolls on the first pass's 04-30";
    EXPECT_TRUE(differences(clean, repaired).empty()) << listing(differences(clean, repaired));
    EXPECT_EQ(repaired.stale_seen, (std::vector<std::string>{"2026-04-30 ZM.v.0", "2026-04-30 ZS.v.0"}));
    EXPECT_FALSE(differences(clean, chain(all, kFirst, kLast, old_rule, &late)).empty())
        << "the old rule must fail this shape";
}

// Shape 4: a flip that resolves late. The change bar of 04-27 reverts on 04-28 (a flip, no fills);
// the roll is confirmed only on 04-30. With the 04-28 bar late, the first pass took the 04-29 bar as
// the confirmation and legged a roll on the 04-30 run. Re-run in order: the 04-30 re-run's legs are
// those of a roll that no longer happens there (and no earlier date booked it, so they are not
// stale by the rule: the bars show no roll and the sweep clears them); the roll is legged on the
// 05-01 run, as in the clean chain.
TEST(LiveRerunRepair, AFlipThatResolvesLateEndsIdenticalToTheCleanChain) {
    const auto all = join({zs_flip(), six_a()});
    const Book clean = chain(all, kFirst, kLast, kFixed);
    std::vector<Leg> clean_legs = clean.legs;
    std::sort(clean_legs.begin(), clean_legs.end());
    ASSERT_EQ(clean_legs.size(), 2u);
    EXPECT_EQ(clean_legs[0], (Leg{"2026-05-01", "EXEC_ZS.v.0_20260430_RC", A}));
    const LateBars late = whole_days(all, {"2026-04-28"}, "2026-04-29", "2026-05-01");
    Book first_pass;
    const Book repaired = chain(all, kFirst, kLast, kFixed, &late, &first_pass);
    ASSERT_EQ(first_pass.roll_fills.at("2026-04-30"), 2u) << "the first pass legs a roll the flip undoes";
    EXPECT_TRUE(differences(clean, repaired).empty()) << listing(differences(clean, repaired));
    EXPECT_TRUE(repaired.stale_seen.empty()) << listing(repaired.stale_seen);
}
