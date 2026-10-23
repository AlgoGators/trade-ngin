// contract_rolls_dump: the per-contract roll rule's held series for every root, as CSV, for the
// independent reference check and the measures (R2). Read only. Nothing in the engine calls the rule
// yet; this tool and the unit tests are its only callers.
//
//   CONTRACT_ROLLS_DSN=postgresql://postgres@127.0.0.1:5433/<scratch db> \
//       contract_rolls_dump <out.csv> [end YYYY-MM-DD] [root ...]
//
// The range per root runs from the first grid date on which the root has a contract row to `end`
// (default: the last contract row). The calendar is the shipped holidays.json (found from the
// working directory, or TRADE_NGIN_HOLIDAYS_JSON).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include <pqxx/pqxx>

#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/data/contract_rolls.hpp"

using namespace trade_ngin;
using namespace trade_ngin::contract_rolls;

namespace {

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

std::string sym(const std::optional<ContractKey>& k) {
    return k ? k->raw_symbol : "";
}

std::string exp(const std::optional<ContractKey>& k) {
    return k ? ymd(k->expiration) : "";
}

}  // namespace

int main(int argc, char** argv) {
    const char* dsn = std::getenv("CONTRACT_ROLLS_DSN");
    if (argc < 2 || !dsn || !*dsn) {
        std::cerr << "usage: CONTRACT_ROLLS_DSN=<dsn> contract_rolls_dump <out.csv> [end] [root ...]\n";
        return 2;
    }
    HolidayChecker checker(HolidayChecker::resolve_holidays_path());
    if (!checker.loaded()) {
        std::cerr << "holiday calendar not loaded\n";
        return 3;
    }
    const TradingCalendar cal = TradingCalendar::from_checker(checker);
    pqxx::connection conn(dsn);
    pqxx::read_transaction t(conn);
    Day end{};
    if (argc >= 3) {
        end = parse_ymd(argv[2]);
    } else {
        end = parse_ymd(t.exec("SELECT max(date)::text FROM futures_data.contract_ohlcv_1d")[0][0]
                            .as<std::string>());
    }
    std::vector<std::string> roots;
    for (int i = 3; i < argc; ++i) roots.push_back(argv[i]);
    if (roots.empty()) {
        for (const auto& r : t.exec("SELECT DISTINCT replace(symbol, '.v.0', '') FROM "
                                    "futures_data.ohlcv_1d ORDER BY 1")) {
            roots.push_back(r[0].as<std::string>());
        }
    }
    std::ofstream out(argv[1]);
    out << "root,date,vendor_symbol,vendor_expiration,held_symbol,held_expiration,bar_id,close,"
           "bar_source,roll,old_symbol,old_expiration,old_close,trigger,fallback,"
           "target_skipped_front,vendor_off_cycle,late\n";
    std::ofstream summary(std::string(argv[1]) + ".summary.csv");
    summary << "root,first,rolls,vendor,crossover,deadline,alias,fallbacks,deferred,"
               "target_skipped_front,vendor_off_cycle,late,carry_days,vendor_bar_days,"
               "unknown_vendor_days,no_target_bars,ltd_mismatches,status\n";
    int failed = 0;
    for (const auto& root : roots) {
        auto first = series_start(t, root);
        if (first.is_error()) {
            summary << root << ",,,,,,,,,,,,,,,,," << first.error()->what() << "\n";
            ++failed;
            continue;
        }
        auto s = build_held_series(t, root, first.value(), end, cal);
        if (s.is_error()) {
            std::string why = s.error()->what();
            for (auto& c : why) if (c == ',' || c == '\n') c = ';';
            summary << root << "," << ymd(first.value()) << ",,,,,,,,,,,,,,,," << why << "\n";
            ++failed;
            continue;
        }
        const RuleReport& rep = s.value().report();
        auto by = [&](const char* k) {
            auto it = rep.rolls_by_trigger.find(k);
            return it == rep.rolls_by_trigger.end() ? 0 : it->second;
        };
        summary << root << "," << ymd(first.value()) << "," << rep.rolls << "," << by("vendor")
                << "," << by("crossover") << "," << by("deadline") << "," << by("alias") << ","
                << rep.fallbacks << "," << rep.deferred << "," << rep.target_skipped_front << ","
                << rep.vendor_off_cycle << "," << rep.late << "," << rep.carry_days << ","
                << rep.vendor_bar_days << "," << rep.unknown_vendor_days << ","
                << rep.no_target_bars << "," << rep.ltd_mismatches << ",ok\n";
        for (const auto& d : s.value().days()) {
            out << root << "," << ymd(d.date) << "," << sym(d.vendor) << "," << exp(d.vendor) << ","
                << d.held.raw_symbol << "," << ymd(d.held.expiration) << "," << d.bar_id << ","
                << (d.has_close ? num(d.close) : "") << "," << to_string(d.source) << ","
                << (d.roll ? 1 : 0) << "," << sym(d.old) << "," << exp(d.old) << ","
                << (d.old_close ? num(*d.old_close) : "") << "," << to_string(d.trigger) << ","
                << to_string(d.fallback) << "," << d.target_skipped_front << ","
                << d.vendor_off_cycle << "," << d.late << "\n";
        }
    }
    std::cerr << "roots " << roots.size() << ", failed " << failed << "\n";
    return 0;
}
