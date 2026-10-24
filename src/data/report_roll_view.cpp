// src/data/report_roll_view.cpp
#include "trade_ngin/data/report_roll_view.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <set>

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/per_contract_source.hpp"

namespace trade_ngin::contract_rolls {

namespace {

std::string root_of(const std::string& symbol) {
    const auto dot = symbol.find(".v.");
    return dot == std::string::npos ? symbol : symbol.substr(0, dot);
}

// The metadata's name without its surrounding blanks ("Silver " is stored with one).
std::string trimmed(const std::string& s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return std::string();
    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

// The contracts of `root` around `day` (a year before it, for the ids of a contract still held).
Result<ContractBook> book_around(pqxx::transaction_base& txn, const std::string& root, Day day) {
    return ContractBook::build(read_contract_bars(txn, {root}, day - std::chrono::days{400}, day));
}

}  // namespace

bool report_deadline_is_first_notice(const ContractDates& dates) {
    return dates.first_notice_day.has_value() && *dates.first_notice_day == dates.deadline;
}

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
    out.held_symbol = o.held.raw_symbol;
    out.cycle = cycle;
    out.rule_rolls = true;
    out.in_window = date >= o.dates.window_first;
    out.roll_by = ymd(o.dates.last_actionable + std::chrono::days{1});
    out.deadline = ymd(o.dates.deadline);
    out.first_notice = report_deadline_is_first_notice(o.dates);
    out.target = o.target ? o.target->raw_symbol : o.cycle_front_symbol;
    return out;
}

ReportRolls report_roll_view(const ReportRollQuery& query, const ReportRollSources& sources,
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
    std::map<std::string, ReportContractName> names;
    try {
        for (const auto& [symbol, name] : sources.names()) names[symbol] = {trimmed(name.name), trimmed(name.ib)};
    } catch (const std::exception& e) {
        WARN(std::string("report_roll_view: the contract names could not be read (") + e.what() +
             "): the roll lines name the symbol");
    }
    const auto ib_of = [&](const std::string& root) {
        const auto it = names.find(root);
        return it == names.end() || it->second.ib.empty() ? root : it->second.ib;
    };
    std::set<std::string> roots;
    std::map<std::string, std::string> held_id;  // root -> the id the book holds
    for (const auto& s : query.held_symbols) roots.insert(root_of(s));
    for (const auto& [symbol, id] : query.held_ids) held_id[root_of(symbol)] = id;
    for (const auto& root : roots) {
        auto cycle = roll_cycle(root);
        if (cycle.is_error()) {
            WARN("report_roll_view " + root + ": " + cycle.error()->what() + "; no outlook");
            continue;
        }
        try {
            // The contract the book holds after the settled day's bar (the roll tracker's: the old one
            // until its legs are booked), named by its id on that day.
            const Contract* book_held = nullptr;
            std::optional<ContractBook> book;
            const auto id = held_id.find(root);
            if (id != held_id.end() && !id->second.empty()) {
                auto built = sources.contracts_around(root, date);
                if (built.is_ok()) {
                    book = std::move(built.value());
                    book_held = book->named_by_id(root, id->second, date);
                }
            }
            ReportRollOutlook o;
            if (reads_vendor_series(root)) {
                // The micros roll with the vendor's series: the contract held, its last trading day and
                // the five CME trading days before it.
                o.root = root;
                o.cycle = cycle.value();
                if (!book_held) {
                    const auto grid = sources.vendor_grid(root, date - std::chrono::days{14}, date);
                    if (!book) {
                        auto built = sources.contracts_around(root, date);
                        if (built.is_ok()) book = std::move(built.value());
                    }
                    if (!grid.empty() && book) book_held = book->named_by_id(root, grid.back().instrument_id, grid.back().date);
                }
                if (book_held) {
                    o.held_symbol = book_held->raw_symbol;
                    if (book_held->key) {
                        auto cal = sources.calendar(root);
                        const Day last_trade = book_held->key->expiration;
                        o.deadline = ymd(last_trade);
                        if (cal.is_ok()) {
                            o.in_window = date >= cal.value().trading_day_before(last_trade, 5) && date <= last_trade;
                        }
                    }
                }
            } else {
                auto series = sources.series(root, date);
                if (series.is_error()) {
                    WARN("report_roll_view " + root + ": " + series.error()->what() + "; no outlook");
                    continue;
                }
                auto made = report_outlook_of(series.value(), root, cycle.value(), date);
                if (made.is_error()) {
                    WARN("report_roll_view " + root + ": " + made.error()->what() + "; no outlook");
                    continue;
                }
                o = made.value();
                if (book_held && book_held->raw_symbol != o.held_symbol) {
                    // The rule has rolled on this bar and the legs are booked by the next run (a vendor
                    // switch the next bar confirms): the book still holds the old contract; no line yet.
                    o.held_symbol = book_held->raw_symbol;
                    o.in_window = false;
                }
            }
            o.label = ib_of(root);
            o.held = o.held_symbol.empty() ? std::string() : report_ib_contract(o.label, root, o.held_symbol);
            if (!o.target.empty()) o.target = report_ib_contract(o.label, root, o.target);
            out.outlook[root] = o;
        } catch (const std::exception& e) {
            WARN("report_roll_view " + root + ": " + e.what() + "; no outlook");
        }
    }
    if (query.rolls.empty()) return out;
    std::map<std::string, std::shared_ptr<ContractBook>> books;
    for (const auto& roll : query.rolls) {
        const std::string root = root_of(roll.symbol);
        ReportRollNames n;
        const auto name = names.find(root);
        n.name = name == names.end() ? std::string() : name->second.name;
        n.gap_places = report_tick_places(tick_of ? tick_of(roll.symbol) : 0.0);
        try {
            const Day day = parse_ymd(roll.date);
            auto& contracts = books[root + "|" + roll.date];
            if (!contracts) {
                auto built = sources.contracts_around(root, day);
                if (built.is_ok()) contracts = std::make_shared<ContractBook>(built.value());
            }
            if (contracts) {
                const auto* from = contracts->named_by_id(root, roll.outgoing_id, day);
                const auto* to = contracts->named_by_id(root, roll.incoming_id, day);
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

ReportRolls report_roll_view(pqxx::transaction_base& txn, const ReportRollQuery& query,
                             const FuturesCalendar& calendars,
                             const std::function<double(const std::string&)>& tick_of) {
    ReportRollSources sources;
    sources.series = [&](const std::string& root, Day date) -> Result<ContractRollSeries> {
        auto start = series_start(txn, root);
        if (start.is_error()) {
            return make_error<ContractRollSeries>(start.error()->code(), start.error()->what(), "report_roll_view");
        }
        return build_held_series(txn, root, start.value(), date, calendars, date + std::chrono::days{1});
    };
    sources.vendor_grid = [&](const std::string& root, Day from, Day to) {
        return read_vendor_grid(txn, root + ".v.0", from, to);
    };
    sources.contracts_around = [&](const std::string& root, Day day) { return book_around(txn, root, day); };
    sources.calendar = [&](const std::string& root) { return calendars.for_root(root); };
    sources.names = [&]() {
        std::map<std::string, ReportContractName> names;
        for (const auto& row : txn.exec("SELECT \"Databento Symbol\", \"Name\", \"IB Symbol\" FROM metadata.contract_metadata")) {
            if (row[0].is_null()) continue;
            names[row[0].as<std::string>()] = {row[1].is_null() ? std::string() : row[1].as<std::string>(),
                                               row[2].is_null() ? std::string() : row[2].as<std::string>()};
        }
        return names;
    };
    return report_roll_view(query, sources, tick_of);
}

}  // namespace trade_ngin::contract_rolls
