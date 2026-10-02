// src/data/session_classifier.cpp
#include "trade_ngin/data/session_classifier.hpp"

#include <algorithm>
#include <cmath>
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
    bool changed = false;
    if (it == series.end()) {
        series.emplace(day, b);
        changed = true;
    } else if (keeps_over(b, it->second)) {
        it->second = b;
        changed = true;
    }
    if (changed) {
        // A bar's verdict depends on the SESSION bars before it: every cached verdict from this
        // date on is stale, and (T-7b-2 C10a) so is the previous bar's, whose id-flip test reads
        // its next bar.
        invalidate_flags_from_previous(bar.symbol, day);
    }
}

void SessionClassifier::add_bars(const std::vector<Bar>& bars) {
    for (const auto& bar : bars) add_bar(bar);
}

void SessionClassifier::invalidate_flags_from_previous(const std::string& symbol, Day day) {
    auto fit = session_flags_.find(symbol);
    if (fit == session_flags_.end()) return;
    Day from = day;
    if (auto sit = bars_.find(symbol); sit != bars_.end()) {
        auto lb = sit->second.lower_bound(day);
        if (lb != sit->second.begin()) from = std::prev(lb)->first;
    }
    fit->second.erase(fit->second.lower_bound(from), fit->second.end());
}

void SessionClassifier::add_instrument_id(const std::string& symbol, Day day,
                                          const std::string& instrument_id) {
    auto& ids = ids_[symbol];
    auto it = ids.find(day);
    if (it != ids.end() && it->second == instrument_id) return;
    ids[day] = instrument_id;
    // The bar's own flag, the previous bar's (its next id) and every later one (the norm) move.
    invalidate_flags_from_previous(symbol, day);
}

std::size_t SessionClassifier::add_instrument_ids(
    const std::vector<market_data_utils::FuturesInstrumentId>& rows) {
    std::size_t taken = 0;
    for (const auto& r : rows) {
        if (r.date.size() < 10 || r.instrument_id.empty()) continue;
        int y = 0;
        unsigned m = 0, d = 0;
        if (std::sscanf(r.date.c_str(), "%4d-%2u-%2u", &y, &m, &d) != 3) continue;
        const std::chrono::year_month_day ymd{std::chrono::year{y}, std::chrono::month{m},
                                              std::chrono::day{d}};
        if (!ymd.ok()) continue;
        add_instrument_id(r.symbol, Day{ymd}, r.instrument_id);
        ++taken;
    }
    return taken;
}

std::size_t SessionClassifier::instrument_id_count() const {
    std::size_t n = 0;
    for (const auto& [symbol, ids] : ids_) n += ids.size();
    return n;
}

const std::string* SessionClassifier::id_of(const std::string& symbol, const Series& series,
                                            Series::const_iterator it) const {
    if (it == series.end()) return nullptr;
    auto sit = ids_.find(symbol);
    if (sit == ids_.end()) return nullptr;
    auto iit = sit->second.find(it->first);
    return iit == sit->second.end() ? nullptr : &iit->second;
}

bool SessionClassifier::is_id_flip(const std::string& symbol, const Series& series,
                                   Series::const_iterator it) const {
    if (it == series.end() || it == series.begin()) return false;
    const auto next = std::next(it);
    if (next == series.end()) return false;
    const std::string* prev_id = id_of(symbol, series, std::prev(it));
    const std::string* id = id_of(symbol, series, it);
    const std::string* next_id = id_of(symbol, series, next);
    return prev_id && id && next_id && *id != *prev_id && *id != *next_id;
}

const std::string* SessionClassifier::established_id(const std::string& symbol,
                                                     const Series& series,
                                                     Series::const_iterator it) const {
    if (it == series.begin()) return nullptr;
    auto j = std::prev(it);
    // Each step reads bars before `it` and `it` itself (a flip's next bar), never a later one.
    while (j != series.begin() && is_id_flip(symbol, series, j)) --j;
    return id_of(symbol, series, j);
}

bool SessionClassifier::is_unconfirmed_id_change(const std::string& symbol, const Series& series,
                                                 Series::const_iterator it, std::string* from,
                                                 std::string* established) const {
    if (it == series.end() || it == series.begin()) return false;
    const std::string* id = id_of(symbol, series, it);
    const std::string* prev_id = id_of(symbol, series, std::prev(it));
    if (!id || !prev_id || *id == *prev_id) return false;
    const std::string* est = established_id(symbol, series, it);
    // Back on the id the symbol printed before a confirmed one-day flip: not a change.
    if (est && *est == *id) return false;
    if (from) *from = *prev_id;
    if (established) *established = est ? *est : std::string("unknown");
    return true;
}

bool SessionClassifier::holds_id_change(const DayBar& bar, const std::optional<double>& nm) const {
    const double f = config_.id_change_hold_fraction;
    if (!(f > 0.0)) return false;
    if (std::isinf(f) || !nm) return true;
    return bar.volume < f * *nm;
}

std::optional<double> SessionClassifier::session_norm(const std::map<Day, DayBar>& series,
                                                      const std::map<Day, bool>& flags, Day date,
                                                      int* bars_used) const {
    if (bars_used) *bars_used = 0;
    std::vector<double> window;
    window.reserve(static_cast<std::size_t>(std::max(0, config_.norm_window_bars)));
    // Strictly before `date`: lower_bound is the first bar at or after it. Only a weekday bar
    // whose own verdict was SESSION counts (T-7b-1 C7b R7).
    for (auto it = std::make_reverse_iterator(series.lower_bound(date));
         it != series.rend() && static_cast<int>(window.size()) < config_.norm_window_bars;
         ++it) {
        if (!is_weekday(it->first)) continue;
        auto f = flags.find(it->first);
        if (f == flags.end() || !f->second) continue;
        window.push_back(it->second.volume);
    }
    if (window.empty()) {
        // No SESSION weekday bar yet: the symbol bootstraps from T-7a's norm over all its trailing
        // weekday bars, so a thin contract (6L.v.0 in 2011, a median of 6.5 lots) can start: under
        // the floor alone its first bars would be JUNK and none of them would ever count.
        for (auto it = std::make_reverse_iterator(series.lower_bound(date));
             it != series.rend() && static_cast<int>(window.size()) < config_.norm_window_bars;
             ++it) {
            if (is_weekday(it->first)) window.push_back(it->second.volume);
        }
    }
    if (window.empty()) return std::nullopt;
    std::sort(window.begin(), window.end());
    const std::size_t n = window.size();
    const double median = n % 2 == 1 ? window[n / 2] : 0.5 * (window[n / 2 - 1] + window[n / 2]);
    // A norm of 0 would disable both volume limbs (nothing is under 1 % or 25 % of 0): no norm.
    if (!(median > 0.0)) return std::nullopt;
    if (bars_used) *bars_used = static_cast<int>(n);
    return median;
}

const std::map<SessionClassifier::Day, bool>& SessionClassifier::session_flags(
    const std::string& symbol, Day date) const {
    auto& flags = session_flags_[symbol];
    auto sit = bars_.find(symbol);
    if (sit == bars_.end()) return flags;
    const auto& series = sit->second;
    // The cached flags are a prefix of the series (add_bar and add_instrument_id drop every flag
    // from the bar before a changed date on), so the next bar to judge is the first one after the
    // last cached date.
    auto it = flags.empty() ? series.begin() : series.upper_bound(flags.rbegin()->first);
    for (; it != series.end() && it->first < date; ++it) {
        const auto n = session_norm(series, flags, it->first, nullptr);
        bool session = judge_bar(it->second, n, nullptr) == SessionVerdict::SESSION;
        // T-7b-2 C10a, with hindsight: a bar whose next bar exists is judged by the confirmed
        // rule (a one-day flip is not a session; a roll counts on its volume); the last bar
        // (no next yet) by the day-of rule.
        if (session) {
            if (std::next(it) != series.end()) {
                session = !is_id_flip(symbol, series, it);
            } else if (is_unconfirmed_id_change(symbol, series, it)) {
                session = !holds_id_change(it->second, n);
            }
        }
        flags[it->first] = session;
    }
    return flags;
}

std::optional<double> SessionClassifier::norm(const std::string& symbol, Day date,
                                              int* bars_used) const {
    if (bars_used) *bars_used = 0;
    auto sit = bars_.find(symbol);
    if (sit == bars_.end()) return std::nullopt;
    return session_norm(sit->second, session_flags(symbol, date), date, bars_used);
}

SessionVerdict SessionClassifier::judge_bar(const DayBar& b, const std::optional<double>& nm,
                                            std::string* reason) const {
    auto say = [reason](std::string text) {
        if (reason) *reason = std::move(text);
    };
    if (b.high == b.low) {
        say("locked (high == low == " + num(b.high) + ", " + lots(b.volume) +
            " lots): a bar the market was locked at or a stub, not traded");
        return SessionVerdict::JUNK;
    }
    if (!nm) {
        if (b.volume < config_.floor_lots) {
            say("absolute floor with no norm yet (" + lots(b.volume) + " lots < " +
                lots(config_.floor_lots) + ")");
            return SessionVerdict::JUNK;
        }
        say("no norm yet; " + lots(b.volume) + " lots is above the floor");
        return SessionVerdict::SESSION;
    }
    if (b.volume < config_.junk_fraction * *nm && b.volume < config_.junk_ceiling_lots) {
        say("corrupt print (" + lots(b.volume) + " lots < " + num(config_.junk_fraction) +
            " x norm " + lots(*nm) + " and < " + lots(config_.junk_ceiling_lots) + " lots)");
        return SessionVerdict::JUNK;
    }
    if (b.volume < config_.floor_lots && b.volume < config_.floor_fraction * *nm) {
        say("absolute floor (" + lots(b.volume) + " lots < " + lots(config_.floor_lots) +
            " and < " + num(config_.floor_fraction) + " x norm " + lots(*nm) + ")");
        return SessionVerdict::JUNK;
    }
    say(lots(b.volume) + " lots against a norm of " + lots(*nm));
    return SessionVerdict::SESSION;
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
            v.verdict = judge_bar(b, v.norm, &v.reason);

            // T-7b-2 C10a: the instrument-id continuity limb. It reads the bars dated <= D only
            // (never the next one), so live's T-1 and the backtest's signal group see the same.
            if (const std::string* id = id_of(symbol, *series, bit)) v.instrument_id = *id;
            if (bit != series->begin()) {
                const auto pit = std::prev(bit);
                if (const std::string* pid = id_of(symbol, *series, pit)) {
                    v.previous_instrument_id = *pid;
                }
                // The previous bar's own day-of question, answered by this bar.
                std::string p_from, p_est;
                if (is_unconfirmed_id_change(symbol, *series, pit, &p_from, &p_est)) {
                    const std::string p_id = v.previous_instrument_id;
                    const std::string what =
                        !is_id_flip(symbol, *series, pit)
                            ? "confirmed a ROLL: this session keeps " + p_id
                        : v.instrument_id == p_est
                            ? "confirmed a ONE-DAY FLIP: this session is back on " +
                                  v.instrument_id
                            : "confirmed a ONE-DAY FLIP, and this session prints a third id " +
                                  v.instrument_id;
                    v.id_note = "the previous session's instrument id change on " +
                                ymd(pit->first) + " (" + p_from + " -> " + p_id + ") is " + what;
                }
            }
            std::string from, established;
            if (v.verdict == SessionVerdict::SESSION &&
                is_unconfirmed_id_change(symbol, *series, bit, &from, &established) &&
                holds_id_change(b, v.norm)) {
                v.verdict = SessionVerdict::JUNK;
                v.id_change_hold = true;
                v.reason = "instrument id change (" + from + " -> " + v.instrument_id +
                           ", established " + established +
                           ") not confirmed until the next session: a roll if the next bar keeps " +
                           v.instrument_id + ", a one-day flip if it returns to " + established +
                           " (" + lots(b.volume) + " lots)";
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
        // T-7b-1 C7b R8: the eve limb is for a weekend D (Christmas and New Year's Sundays).
        // A weekday before a fixed-date holiday (Dec 24, Dec 31, Jul 2 or 3) is a session day,
        // so a missing print there is judged like any other weekday's.
        if (auto h = holidays(next); h && h->type == "fixed" && !is_weekday(date)) {
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

std::vector<Bar> k01_consumed_bars(const SessionClassifier& classifier, const std::vector<Bar>& bars,
                                   std::vector<SymbolDayVerdict>* withheld) {
    std::vector<Bar> out;
    out.reserve(bars.size());
    for (const auto& bar : bars) {
        SymbolDayVerdict v =
            classifier.classify_symbol_day(bar.symbol, SessionClassifier::day_of(bar.timestamp), {});
        if (v.k01_withheld()) {
            if (withheld) withheld->push_back(std::move(v));
            continue;
        }
        out.push_back(bar);
    }
    return out;
}

InstrumentIdFeed feed_instrument_ids(
    SessionClassifier& classifier,
    const Result<std::vector<market_data_utils::FuturesInstrumentId>>& rows) {
    InstrumentIdFeed out;
    if (rows.is_error()) {
        out.line = "INSTRUMENT_ID_FEED no instrument ids (" + std::string(rows.error()->what()) +
                   "): the continuity limb judges no bar on this run";
        return out;
    }
    out.fed = true;
    out.ids = classifier.add_instrument_ids(rows.value());
    out.line = "INSTRUMENT_ID_FEED " + std::to_string(out.ids) +
               " kept bars carry a vendor instrument id (futures_data.ohlcv_1d_raw, the same "
               "print only); the continuity limb judges each bar's id against the previous "
               "session's";
    return out;
}

}  // namespace trade_ngin
