// src/data/session_classifier.cpp
#include "trade_ngin/data/session_classifier.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace trade_ngin {

const char* to_string(SessionVerdict verdict) {
    switch (verdict) {
        case SessionVerdict::SESSION:
            return "SESSION";
        case SessionVerdict::JUNK:
            return "JUNK";
        case SessionVerdict::NO_BAR_CLOSURE:
            return "NO_BAR(closure)";
        case SessionVerdict::NO_BAR_FEED_HOLE:
            return "NO_BAR(feed hole)";
    }
    return "UNKNOWN";
}

namespace {

bool is_weekday(SessionClassifier::Day day) {
    const std::chrono::weekday wd{day};
    return wd != std::chrono::Saturday && wd != std::chrono::Sunday;
}

std::string lots(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
}

std::string num(double v) {
    std::ostringstream os;
    os.precision(6);
    os << v;
    return os.str();
}

}  // namespace

SessionClassifier::SessionClassifier(SessionClassifierConfig config) : config_(config) {}

std::string SessionClassifier::ymd(Day day) {
    const std::chrono::year_month_day d{day};
    char buf[11];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", static_cast<int>(d.year()),
                  static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day()));
    return buf;
}

bool SessionClassifier::keeps_over(const DayBar& c, const DayBar& h) {
    // The B0 loader's keep order (market_data_utils kFuturesBarKeepOrder):
    // volume DESC, then close, open, high, low ascending.
    if (c.volume != h.volume) return c.volume > h.volume;
    if (c.close != h.close) return c.close < h.close;
    if (c.open != h.open) return c.open < h.open;
    if (c.high != h.high) return c.high < h.high;
    return c.low < h.low;
}

void SessionClassifier::add_bar(const Bar& bar) {
    DayBar b;
    b.volume = bar.volume;
    b.open = static_cast<double>(bar.open);
    b.high = static_cast<double>(bar.high);
    b.low = static_cast<double>(bar.low);
    b.close = static_cast<double>(bar.close);
    auto& series = bars_[bar.symbol];
    const Day day = day_of(bar.timestamp);
    auto it = series.find(day);
    if (it == series.end()) {
        series.emplace(day, b);
    } else if (keeps_over(b, it->second)) {
        it->second = b;
    }
}

void SessionClassifier::add_bars(const std::vector<Bar>& bars) {
    for (const auto& bar : bars) add_bar(bar);
}

std::optional<double> SessionClassifier::norm(const std::string& symbol, Day date,
                                              int* bars_used) const {
    if (bars_used) *bars_used = 0;
    auto sit = bars_.find(symbol);
    if (sit == bars_.end()) return std::nullopt;
    const auto& series = sit->second;
    std::vector<double> window;
    window.reserve(static_cast<std::size_t>(std::max(0, config_.norm_window_bars)));
    // Strictly before `date`: lower_bound is the first bar at or after it.
    for (auto it = std::make_reverse_iterator(series.lower_bound(date));
         it != series.rend() && static_cast<int>(window.size()) < config_.norm_window_bars;
         ++it) {
        if (is_weekday(it->first)) window.push_back(it->second.volume);
    }
    if (window.empty()) return std::nullopt;
    if (bars_used) *bars_used = static_cast<int>(window.size());
    std::sort(window.begin(), window.end());
    const std::size_t n = window.size();
    return n % 2 == 1 ? window[n / 2] : 0.5 * (window[n / 2 - 1] + window[n / 2]);
}

SymbolDayVerdict SessionClassifier::classify_symbol_day(const std::string& symbol, Day date,
                                                        const HolidayLookup& holidays) const {
    SymbolDayVerdict v;
    v.symbol = symbol;
    v.date = ymd(date);

    const std::map<Day, DayBar>* series = nullptr;
    if (auto sit = bars_.find(symbol); sit != bars_.end()) series = &sit->second;

    if (series) {
        if (auto bit = series->find(date); bit != series->end()) {
            const DayBar& b = bit->second;
            v.has_bar = true;
            v.volume = b.volume;
            v.close = b.close;
            v.norm = norm(symbol, date, &v.norm_bars);
            if (v.norm && *v.norm > 0.0) v.ratio = b.volume / *v.norm;

            if (b.high == b.low) {
                v.verdict = SessionVerdict::JUNK;
                v.reason = "locked (high == low == " + num(b.high) + ", " + lots(b.volume) +
                           " lots): a bar the market was locked at or a stub, not traded";
            } else if (!v.norm) {
                if (b.volume < config_.floor_lots) {
                    v.verdict = SessionVerdict::JUNK;
                    v.reason = "absolute floor with no norm yet (" + lots(b.volume) + " lots < " +
                               lots(config_.floor_lots) + ")";
                } else {
                    v.verdict = SessionVerdict::SESSION;
                    v.reason = "no norm yet; " + lots(b.volume) + " lots is above the floor";
                }
            } else if (b.volume < config_.junk_fraction * *v.norm &&
                       b.volume < config_.junk_ceiling_lots) {
                v.verdict = SessionVerdict::JUNK;
                v.reason = "corrupt print (" + lots(b.volume) + " lots < " +
                           num(config_.junk_fraction) + " x norm " + lots(*v.norm) + " and < " +
                           lots(config_.junk_ceiling_lots) + " lots)";
            } else if (b.volume < config_.floor_lots &&
                       b.volume < config_.floor_fraction * *v.norm) {
                v.verdict = SessionVerdict::JUNK;
                v.reason = "absolute floor (" + lots(b.volume) + " lots < " +
                           lots(config_.floor_lots) + " and < " + num(config_.floor_fraction) +
                           " x norm " + lots(*v.norm) + ")";
            } else {
                v.verdict = SessionVerdict::SESSION;
                v.reason = lots(b.volume) + " lots against a norm of " + lots(*v.norm);
            }
            return v;
        }
        // The latest bar strictly before `date`, for the hole's age.
        auto lb = series->lower_bound(date);
        if (lb != series->begin()) {
            --lb;
            v.last_bar_date = ymd(lb->first);
            v.hole_age_days = static_cast<int>((date - lb->first).count());
        }
    }

    // No bar for (symbol, date).
    const std::string d = ymd(date);
    const std::string next = ymd(date + std::chrono::days{1});
    if (holidays) {
        if (auto h = holidays(d)) {
            v.verdict = SessionVerdict::NO_BAR_CLOSURE;
            v.reason = "holiday: " + h->name;
            return v;
        }
        if (auto h = holidays(next); h && h->type == "fixed") {
            v.verdict = SessionVerdict::NO_BAR_CLOSURE;
            v.reason = "eve of a fixed-date holiday: " + h->name + " (" + next + ")";
            return v;
        }
    }
    if (series) {
        for (int k = 1; k <= config_.expected_lookback_weeks; ++k) {
            const Day prior = date - std::chrono::days{7 * k};
            if (series->count(prior)) {
                v.verdict = SessionVerdict::NO_BAR_FEED_HOLE;
                v.reason = "printed on " + ymd(prior) + ", " + std::to_string(k) +
                           " week(s) back on the same weekday";
                return v;
            }
        }
    }
    v.verdict = SessionVerdict::NO_BAR_CLOSURE;
    v.reason = "not expected: no print on any of the " +
               std::to_string(config_.expected_lookback_weeks) + " preceding same-weekday dates";
    return v;
}

const SymbolDayVerdict* T1Classification::find(const std::string& symbol) const {
    auto it = std::lower_bound(
        verdicts.begin(), verdicts.end(), symbol,
        [](const SymbolDayVerdict& v, const std::string& s) { return v.symbol < s; });
    if (it != verdicts.end() && it->symbol == symbol) return &*it;
    return nullptr;
}

bool T1Classification::is_session(const std::string& symbol) const {
    const SymbolDayVerdict* v = find(symbol);
    return v != nullptr && v->is_session();
}

nlohmann::json T1Classification::to_json(const std::vector<std::string>& held_symbols) const {
    nlohmann::json j;
    j["t1_date"] = t1_date;
    j["session"] = session;
    j["junk"] = junk;
    j["closure"] = closure;
    j["feed_hole"] = feed_hole;
    j["max_hole_age_days"] = max_hole_age_days;
    j["junk_symbols"] = junk_symbols;
    j["feed_hole_symbols"] = feed_hole_symbols;
    j["held_symbols"] = held_symbols;
    return j;
}

T1Classification classify_t1(const SessionClassifier& classifier,
                             const std::vector<std::string>& universe, SessionClassifier::Day t1,
                             const HolidayLookup& holidays) {
    T1Classification out;
    out.t1_date = SessionClassifier::ymd(t1);
    std::vector<std::string> symbols(universe);
    std::sort(symbols.begin(), symbols.end());
    symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
    out.verdicts.reserve(symbols.size());
    for (const auto& s : symbols) {
        SymbolDayVerdict v = classifier.classify_symbol_day(s, t1, holidays);
        switch (v.verdict) {
            case SessionVerdict::SESSION:
                ++out.session;
                break;
            case SessionVerdict::JUNK:
                ++out.junk;
                out.junk_symbols.push_back(s);
                break;
            case SessionVerdict::NO_BAR_CLOSURE:
                ++out.closure;
                break;
            case SessionVerdict::NO_BAR_FEED_HOLE:
                ++out.feed_hole;
                out.feed_hole_symbols.push_back(s);
                out.max_hole_age_days = std::max(out.max_hole_age_days, v.hole_age_days);
                break;
        }
        out.verdicts.push_back(std::move(v));
    }
    return out;
}

std::vector<SymbolDayVerdict> classify_bar_group(const SessionClassifier& classifier,
                                                 const std::vector<Bar>& group,
                                                 const HolidayLookup& holidays) {
    std::map<std::string, SessionClassifier::Day> days;
    for (const auto& bar : group) days.emplace(bar.symbol, SessionClassifier::day_of(bar.timestamp));
    std::vector<SymbolDayVerdict> out;
    out.reserve(days.size());
    for (const auto& [symbol, day] : days) {
        out.push_back(classifier.classify_symbol_day(symbol, day, holidays));
    }
    return out;
}

}  // namespace trade_ngin
