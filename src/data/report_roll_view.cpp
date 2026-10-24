// src/data/report_roll_view.cpp
#include "trade_ngin/data/report_roll_view.hpp"

#include <chrono>
#include <map>
#include <set>

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/per_contract_source.hpp"

namespace trade_ngin::contract_rolls {

namespace {

std::string root_of(const std::string& symbol) {
    const auto dot = symbol.find(".v.");
    return dot == std::string::npos ? symbol : symbol.substr(0, dot);
}

// The contracts of `root` around `day` (a year before it, for the ids of a contract still held).
Result<ContractBook> book_around(pqxx::transaction_base& txn, const std::string& root, Day day) {
    return ContractBook::build(read_contract_bars(txn, {root}, day - std::chrono::days{400}, day));
}

}  // namespace

Result<ReportRollOutlook> report_outlook_of(const ContractRollSeries& series, const std::string& root,
                                            const std::string& cycle, Day date) {
    auto outlook = series.outlook(date);
    if (outlook.is_error()) {
        return make_error<ReportRollOutlook>(outlook.error()->code(), outlook.error()->what(), "report_roll_view");
    }
    const RollOutlook& o = outlook.value();
    ReportRollOutlook out;
    out.root = root;
    out.held = o.held.raw_symbol;
    out.cycle = cycle;
    out.rule_rolls = true;
    out.in_window = date >= o.dates.window_first;
    out.roll_by = ymd(o.dates.last_actionable + std::chrono::days{1});
    out.deadline = ymd(o.dates.deadline);
    out.first_notice = o.dates.first_notice_day && *o.dates.first_notice_day == o.dates.deadline;
    out.target = o.target ? o.target->raw_symbol : o.cycle_front_symbol;
    return out;
}

ReportRolls report_roll_view(pqxx::transaction_base& txn, const ReportRollQuery& query,
                             const FuturesCalendar& calendars,
                             const std::function<double(const std::string&)>& tick_of) {
    ReportRolls out;
    out.per_contract_rolls = true;
    Day date;
    try {
        date = parse_ymd(query.date);
    } catch (const std::exception& e) {
        WARN(std::string("report_roll_view: the settled day is not a date (") + e.what() + "): no roll lines");
        return out;
    }
    std::set<std::string> roots;
    for (const auto& s : query.held_symbols) roots.insert(root_of(s));
    for (const auto& root : roots) {
        auto cycle = roll_cycle(root);
        if (cycle.is_error()) {
            WARN("report_roll_view " + root + ": " + cycle.error()->what() + "; no outlook");
            continue;
        }
        try {
            if (reads_vendor_series(root)) {
                // The micros: the vendor's contract on its latest grid bar, no window.
                const auto grid = read_vendor_grid(txn, root + ".v.0", date - std::chrono::days{14}, date);
                auto book = book_around(txn, root, date);
                ReportRollOutlook o;
                o.root = root;
                o.cycle = cycle.value();
                if (!grid.empty() && book.is_ok()) {
                    const auto* c = book.value().named_by_id(root, grid.back().instrument_id, grid.back().date);
                    if (c) o.held = c->raw_symbol;
                }
                out.outlook[root] = o;
                continue;
            }
            auto start = series_start(txn, root);
            if (start.is_error()) {
                WARN("report_roll_view " + root + ": " + start.error()->what() + "; no outlook");
                continue;
            }
            auto series = build_held_series(txn, root, start.value(), date, calendars, date + std::chrono::days{1});
            if (series.is_error()) {
                WARN("report_roll_view " + root + ": " + series.error()->what() + "; no outlook");
                continue;
            }
            auto o = report_outlook_of(series.value(), root, cycle.value(), date);
            if (o.is_error()) {
                WARN("report_roll_view " + root + ": " + o.error()->what() + "; no outlook");
                continue;
            }
            out.outlook[root] = o.value();
        } catch (const std::exception& e) {
            WARN("report_roll_view " + root + ": " + e.what() + "; no outlook");
        }
    }
    if (query.rolls.empty()) return out;
    std::map<std::string, std::string> names;
    try {
        for (const auto& row : txn.exec("SELECT \"Databento Symbol\", \"Name\" FROM metadata.contract_metadata")) {
            if (!row[0].is_null() && !row[1].is_null()) names[row[0].as<std::string>()] = row[1].as<std::string>();
        }
    } catch (const std::exception& e) {
        WARN(std::string("report_roll_view: the contract names could not be read (") + e.what() +
             "): the roll lines name the symbol");
    }
    std::map<std::string, std::shared_ptr<ContractBook>> books;
    for (const auto& roll : query.rolls) {
        const std::string root = root_of(roll.symbol);
        ReportRollNames n;
        const auto name = names.find(root);
        n.name = name == names.end() ? std::string() : name->second;
        n.gap_places = report_tick_places(tick_of ? tick_of(roll.symbol) : 0.0);
        try {
            const Day day = parse_ymd(roll.date);
            auto& book = books[root + "|" + roll.date];
            if (!book) {
                auto built = book_around(txn, root, day);
                if (built.is_ok()) book = std::make_shared<ContractBook>(built.value());
            }
            if (book) {
                const auto* from = book->named_by_id(root, roll.outgoing_id, day);
                const auto* to = book->named_by_id(root, roll.incoming_id, day);
                if (from) n.from_month = report_month_name(report_contract_month(root, from->raw_symbol));
                if (to) n.to_month = report_month_name(report_contract_month(root, to->raw_symbol));
            }
        } catch (const std::exception& e) {
            WARN("report_roll_view " + roll.symbol + " " + roll.date + ": " + e.what() + "; the line names no months");
        }
        out.names[roll.closing_exec_id] = n;
    }
    return out;
}

}  // namespace trade_ngin::contract_rolls
