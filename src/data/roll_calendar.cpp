// src/data/roll_calendar.cpp
#include "trade_ngin/data/roll_calendar.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

namespace trade_ngin::contract_rolls {

namespace {

using std::chrono::days;
using std::chrono::month;
using std::chrono::weekday;
using std::chrono::year;
using std::chrono::year_month_day;
using std::chrono::year_month_day_last;

const std::set<std::string> kCurrenciesTwoBefore{"6A", "6B", "6E", "6J", "6N", "6S", "6M"};
const std::set<std::string> kGrains{"ZC", "ZS", "ZW", "KE", "ZM", "ZL", "ZR"};
const std::set<std::string> kMetals{"GC", "SI", "HG", "PL"};
const std::set<std::string> kEquityIndex{"ES", "NQ", "YM", "RTY", "MES", "MNQ", "MYM", "M2K"};
// Physically delivered contracts whose notice period opens before the last trading day: a long can
// be handed delivery from the first notice day (r0lib.py NOTICE_BEFORE_LTD).
const std::set<std::string> kNoticeBeforeLastTrade{"GC", "SI", "HG", "PL", "ZC", "ZS", "ZW", "KE",
                                                   "ZM", "ZL", "ZR", "ZN", "ZF", "ZT", "UB", "LE"};

Day first_of(int y, unsigned m) {
    return Day{year{y} / month{m} / 1};
}

/// The n-th given weekday of the month (n >= 1).
Day nth_weekday(int y, unsigned m, weekday wd, int n) {
    const Day first = first_of(y, m);
    const auto off = (wd - weekday{first}).count();
    return first + days{off + 7 * (n - 1)};
}

Day last_weekday(int y, unsigned m, weekday wd) {
    const Day last = Day{year_month_day_last{year{y}, std::chrono::month_day_last{month{m}}}};
    const auto off = (weekday{last} - wd).count();
    return last - days{off};
}

void prior_month(int y, unsigned m, int* py, unsigned* pm) {
    if (m == 1) {
        *py = y - 1;
        *pm = 12;
    } else {
        *py = y;
        *pm = m - 1;
    }
}

}  // namespace

std::string ymd(Day day) {
    const year_month_day d{day};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", static_cast<int>(d.year()),
                  static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day()));
    return buf;
}

Day parse_ymd(const std::string& text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        throw std::invalid_argument("not a YYYY-MM-DD date: " + text);
    }
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (text[i] < '0' || text[i] > '9') throw std::invalid_argument("not a YYYY-MM-DD date: " + text);
    }
    const year_month_day d{year{std::stoi(text.substr(0, 4))},
                           month{static_cast<unsigned>(std::stoi(text.substr(5, 2)))},
                           std::chrono::day{static_cast<unsigned>(std::stoi(text.substr(8, 2)))}};
    if (!d.ok()) throw std::invalid_argument("not a calendar date: " + text);
    return Day{d};
}

TradingCalendar::TradingCalendar(std::function<bool(const std::string&)> is_holiday,
                                 std::function<bool(const std::string&)> covers)
    : is_holiday_(std::move(is_holiday)), covers_(std::move(covers)) {}

std::string FuturesCalendar::resolve_path() {
    namespace fs = std::filesystem;
    if (const char* env = std::getenv("TRADE_NGIN_FUTURES_HOLIDAYS_JSON")) return env;
    const std::string candidates[] = {"include/trade_ngin/core/futures_holidays.json",
                                      "futures_holidays.json",
                                      "/etc/trade_ngin/futures_holidays.json"};
    for (const auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path, ec)) return path;
    }
    return candidates[0];
}

Result<FuturesCalendar> FuturesCalendar::load(const std::string& path) {
    auto bad = [&](const std::string& why) {
        return make_error<FuturesCalendar>(ErrorCode::INVALID_DATA,
                                           "futures calendar " + path + ": " + why, "contract_rolls");
    };
    std::ifstream in(path);
    if (!in.is_open()) return bad("cannot be opened");
    auto exchanges = std::make_shared<std::map<std::string, Exchange>>();
    std::set<std::string> roots_seen;
    try {
        const nlohmann::json j = nlohmann::json::parse(in);
        const nlohmann::json& all = j.at("exchanges");
        for (const auto& [name, ex] : all.items()) {
            Exchange e;
            e.first_year = ex.at("covers").at("first_year").get<int>();
            e.last_year = ex.at("covers").at("last_year").get<int>();
            if (e.first_year > e.last_year) return bad(name + ": coverage is empty");
            const nlohmann::json& groups = ex.at("groups");
            for (const auto& [group, roots] : groups.items()) {
                e.closed_by_group[group];
                for (const auto& r : roots) {
                    const std::string root = r.get<std::string>();
                    if (!roots_seen.insert(root).second) return bad("root " + root + " is listed twice");
                    e.group_of_root[root] = group;
                }
            }
            for (const auto& c : ex.at("closures")) {
                const std::string date = c.at("date").get<std::string>();
                const int year = static_cast<int>(year_month_day{parse_ymd(date)}.year());
                if (year < e.first_year || year > e.last_year) {
                    return bad(name + ": closure " + date + " is outside the coverage");
                }
                for (const auto& g : c.at("closes")) {
                    const std::string group = g.get<std::string>();
                    if (group == "all") {
                        for (auto& [k, days] : e.closed_by_group) days.insert(date);
                    } else if (!e.closed_by_group.count(group)) {
                        return bad(name + ": closure " + date + " names unknown group " + group);
                    } else {
                        e.closed_by_group[group].insert(date);
                    }
                }
            }
            (*exchanges)[name] = std::move(e);
        }
    } catch (const std::exception& e) {
        return bad(e.what());
    }
    FuturesCalendar cal;
    cal.exchanges_ = std::move(exchanges);
    return cal;
}

std::string FuturesCalendar::exchange_of(const std::string& root) const {
    if (!exchanges_) return "";
    for (const auto& [name, ex] : *exchanges_) {
        if (ex.group_of_root.count(root)) return name;
    }
    return "";
}

std::string FuturesCalendar::group_of(const std::string& root) const {
    if (!exchanges_) return "";
    for (const auto& [name, ex] : *exchanges_) {
        auto it = ex.group_of_root.find(root);
        if (it != ex.group_of_root.end()) return it->second;
    }
    return "";
}

Result<TradingCalendar> FuturesCalendar::for_root(const std::string& root) const {
    const std::string name = exchange_of(root);
    if (name.empty()) {
        return make_error<TradingCalendar>(ErrorCode::INVALID_ARGUMENT,
                                           "no futures calendar lists root " + root,
                                           "contract_rolls");
    }
    const auto keep = exchanges_;  // the lambdas share the loaded data
    const Exchange* ex = &exchanges_->at(name);
    const std::set<std::string>* closed = &ex->closed_by_group.at(ex->group_of_root.at(root));
    return TradingCalendar(
        [keep, closed](const std::string& d) { return closed->count(d) > 0; },
        [keep, ex](const std::string& d) {
            if (d.size() < 4) return false;
            const int y = std::stoi(d.substr(0, 4));
            return y >= ex->first_year && y <= ex->last_year;
        });
}

void TradingCalendar::require_covered(Day day) const {
    const std::string d = ymd(day);
    if (!covers_(d)) {
        throw CalendarCoverageError("the holiday calendar does not cover " + d +
                                    ": the roll rule cannot count trading days there");
    }
}

bool TradingCalendar::is_trading_day(Day day) const {
    require_covered(day);
    const weekday wd{day};
    if (wd == std::chrono::Saturday || wd == std::chrono::Sunday) return false;
    return !is_holiday_(ymd(day));
}

Day TradingCalendar::trading_day_before(Day day, int n) const {
    Day d = day;
    for (int found = 0; found < n;) {
        d -= days{1};
        if (is_trading_day(d)) ++found;
    }
    return d;
}

Day TradingCalendar::trading_day_on_or_after(Day day) const {
    Day d = day;
    while (!is_trading_day(d)) d += days{1};
    return d;
}

Day TradingCalendar::last_trading_day_of_month(int y, unsigned m) const {
    const int ny = m == 12 ? y + 1 : y;
    const unsigned nm = m == 12 ? 1 : m + 1;
    return trading_day_before(first_of(ny, nm), 1);
}

Day TradingCalendar::nth_trading_day_of_month(int y, unsigned m, int n) const {
    Day d = trading_day_on_or_after(first_of(y, m));
    for (int i = 1; i < n; ++i) d = trading_day_on_or_after(d + days{1});
    return d;
}

bool ExchangeRules::knows(const std::string& root) {
    return kCurrenciesTwoBefore.count(root) || kGrains.count(root) || kMetals.count(root) ||
           kEquityIndex.count(root) || root == "6C" || root == "6L" || root == "CL" ||
           root == "NG" || root == "HO" || root == "RB" || root == "ZN" || root == "UB" ||
           root == "ZF" || root == "ZT" || root == "LE" || root == "HE" || root == "GF" ||
           root == "MBT";
}

Day ExchangeRules::last_trading_day(const std::string& root, int y, unsigned m,
                                    const TradingCalendar& cal) {
    int py = 0;
    unsigned pm = 0;
    prior_month(y, m, &py, &pm);
    if (kCurrenciesTwoBefore.count(root)) {
        // Two trading days before the third Wednesday of the contract month.
        return cal.trading_day_before(nth_weekday(y, m, std::chrono::Wednesday, 3), 2);
    }
    if (root == "6C") return cal.trading_day_before(nth_weekday(y, m, std::chrono::Wednesday, 3), 1);
    if (root == "6L" || root == "HO" || root == "RB") return cal.last_trading_day_of_month(py, pm);
    if (root == "CL") {
        // Three trading days before the 25th of the prior month, four when the 25th is not one.
        const Day d25 = first_of(py, pm) + days{24};
        return cal.trading_day_before(d25, cal.is_trading_day(d25) ? 3 : 4);
    }
    if (root == "NG") return cal.trading_day_before(first_of(y, m), 3);
    if (kMetals.count(root)) return cal.trading_day_before(cal.last_trading_day_of_month(y, m), 2);
    if (kGrains.count(root)) return cal.trading_day_before(first_of(y, m) + days{14}, 1);
    if (root == "ZN" || root == "UB") {
        return cal.trading_day_before(cal.last_trading_day_of_month(y, m), 7);
    }
    if (root == "ZF" || root == "ZT" || root == "LE") return cal.last_trading_day_of_month(y, m);
    if (root == "HE") return cal.nth_trading_day_of_month(y, m, 10);
    if (root == "GF") {
        // The last Thursday of the month; in November the Thursday before Thanksgiving (the fourth
        // Thursday); a week earlier while that is not a trading day.
        Day d = last_weekday(y, m, std::chrono::Thursday);
        if (m == 11) d = nth_weekday(y, m, std::chrono::Thursday, 4) - days{7};
        while (!cal.is_trading_day(d)) d -= days{7};
        return d;
    }
    if (kEquityIndex.count(root)) {
        const Day d = nth_weekday(y, m, std::chrono::Friday, 3);
        return cal.is_trading_day(d) ? d : cal.trading_day_before(d, 1);
    }
    if (root == "MBT") {
        const Day d = last_weekday(y, m, std::chrono::Friday);
        return cal.is_trading_day(d) ? d : cal.trading_day_before(d, 1);
    }
    throw std::invalid_argument("no last-trading-day rule for root " + root);
}

std::optional<Day> ExchangeRules::first_notice_day(const std::string& root, int y, unsigned m,
                                                   const TradingCalendar& cal) {
    if (!kNoticeBeforeLastTrade.count(root)) return std::nullopt;
    if (root == "LE") return cal.trading_day_on_or_after(first_of(y, m));
    int py = 0;
    unsigned pm = 0;
    prior_month(y, m, &py, &pm);
    return cal.last_trading_day_of_month(py, pm);
}

}  // namespace trade_ngin::contract_rolls
