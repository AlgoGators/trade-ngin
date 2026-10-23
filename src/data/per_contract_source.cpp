// src/data/per_contract_source.cpp
#include "trade_ngin/data/per_contract_source.hpp"

#include "trade_ngin/core/logger.hpp"

namespace trade_ngin::contract_rolls {

PerContractSource& PerContractSource::instance() {
    static PerContractSource source;
    return source;
}

void PerContractSource::enable(FuturesCalendar calendar) {
    std::lock_guard<std::mutex> lock(mutex_);
    calendar_ = std::move(calendar);
    enabled_ = true;
    micros_warned_ = false;
}

void PerContractSource::disable() {
    std::lock_guard<std::mutex> lock(mutex_);
    calendar_.reset();
    enabled_ = false;
    micros_warned_ = false;
}

bool PerContractSource::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

FuturesCalendar PerContractSource::calendar() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calendar_ ? *calendar_ : FuturesCalendar{};
}

bool PerContractSource::first_micros_warning() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool first = !micros_warned_;
    micros_warned_ = true;
    return first;
}

Result<void> configure_per_contract_source(bool per_contract_rolls) {
    if (!per_contract_rolls) {
        PerContractSource::instance().disable();
        return Result<void>();
    }
    const std::string path = FuturesCalendar::resolve_path();
    auto calendar = FuturesCalendar::load(path);
    if (calendar.is_error()) {
        return make_error<void>(calendar.error()->code(),
                                "per_contract_rolls is on and the futures calendar cannot be "
                                "loaded: " + std::string(calendar.error()->what()),
                                "contract_rolls");
    }
    PerContractSource::instance().enable(calendar.value());
    INFO("PER_CONTRACT_ROLLS on: the futures bar source hands out the held contract's own bars "
         "(HD's roll rule, contract_roll_rule.hpp), roll bars carrying the old contract's close; "
         "futures calendar " + path);
    return Result<void>();
}

bool reads_vendor_series(const std::string& root) {
    return micro_alias(root).has_value();
}

std::string micros_warning_line() {
    return "PER_CONTRACT_ROLLS micros on the vendor's series: MES, MNQ, MYM and M2K read their "
           "`.v.0` bars and today's handling of the 2026-02-22 change (instrument_id_relabels), "
           "because their history before that date is the ES, NQ, YM and RTY contracts, which are "
           "not loaded in contract_ohlcv_1d (waiting on data)";
}

Result<std::vector<SourceBar>> bars_of_series(const HeldSeries& series,
                                              const std::map<Day, std::string>& window) {
    using Bars = std::vector<SourceBar>;
    Bars out;
    out.reserve(window.size());
    const std::string symbol = series.root + ".v.0";
    bool have_previous = false;
    double previous_close = 0.0;  // the held contract's close on the previous grid day
    std::string previous_id;
    size_t found = 0;
    for (const HeldDay& d : series.days) {
        SourceBar b;
        b.date = d.date;
        b.symbol = symbol;
        double held_close = d.close;
        std::string held_id = d.bar_id.empty() ? d.carried_id : d.bar_id;
        if (d.tentative) {
            const TentativeRoll& t = *d.tentative;
            b.open = t.bar.open;
            b.high = t.bar.high;
            b.low = t.bar.low;
            b.close = t.bar.close;
            b.volume = static_cast<double>(t.bar.volume);
            b.instrument_id = t.bar.instrument_id;
            b.roll_from = RollFrom{t.old_close, t.old_id, RollFromKind::AWAITS_NEXT};
            held_close = t.old_close;
            held_id = t.old_id;
        } else {
            if (!d.has_close) {
                return make_error<Bars>(ErrorCode::INVALID_DATA,
                                        "per_contract_rolls: " + symbol + " " + ymd(d.date) +
                                            ": the held contract " + d.held.raw_symbol +
                                            " has no close to carry",
                                        "contract_rolls");
            }
            b.open = d.open;
            b.high = d.high;
            b.low = d.low;
            b.close = d.close;
            b.volume = static_cast<double>(d.volume);
            b.instrument_id = held_id;
            if (d.roll) {
                RollFrom from;
                from.kind = d.awaits_next_bar ? RollFromKind::AWAITS_NEXT : RollFromKind::KNOWN;
                if (d.old_close) {
                    from.close = *d.old_close;
                    from.instrument_id = d.old_id;
                } else {
                    // Plan 5c: the old contract printed nothing up to its last actionable bar.
                    if (!have_previous) {
                        return make_error<Bars>(ErrorCode::INVALID_DATA,
                                                "per_contract_rolls: " + symbol + " " +
                                                    ymd(d.date) +
                                                    ": a fallback roll on the series' first day",
                                                "contract_rolls");
                    }
                    from.close = previous_close;
                    if (d.fallback == RollFallback::NEW_CONTRACT_MOVE &&
                        d.fallback_new_previous_close && *d.fallback_new_previous_close > 0.0) {
                        from.close = previous_close * (d.close / *d.fallback_new_previous_close);
                    }
                    from.instrument_id = previous_id;
                    WARN("PER_CONTRACT_ROLLS fallback roll " + symbol + " " + ymd(d.date) + " " +
                         (d.old ? d.old->raw_symbol : std::string("?")) + "->" +
                         d.held.raw_symbol + " (" + to_string(d.fallback) +
                         "): the old contract has no bar; its close on the roll bar is read as " +
                         std::to_string(from.close));
                }
                b.roll_from = from;
            } else if (d.left_close) {
                // The bar after a roll: the old contract's close here, for a consumer that withheld
                // the roll bar (it books the roll on this bar, HD 10-23).
                b.roll_from = RollFrom{*d.left_close, d.left_id, RollFromKind::AFTER_ROLL};
            }
        }
        auto w = window.find(d.date);
        if (w != window.end()) {
            b.time_text = w->second;
            out.push_back(std::move(b));
            ++found;
        }
        have_previous = true;
        previous_close = held_close;
        previous_id = held_id;
    }
    if (found != window.size()) {
        for (const auto& [date, text] : window) {
            bool in = false;
            for (const auto& b : out) {
                if (b.date == date) {
                    in = true;
                    break;
                }
            }
            if (!in) {
                return make_error<Bars>(ErrorCode::INVALID_DATA,
                                        "per_contract_rolls: " + symbol + " has no held day on " +
                                            ymd(date) + " of the vendor's grid",
                                        "contract_rolls");
            }
        }
    }
    return out;
}

Result<std::vector<SourceBar>> per_contract_bars(pqxx::transaction_base& txn,
                                                 const std::string& root,
                                                 const std::string& start_ts,
                                                 const std::string& end_ts,
                                                 const FuturesCalendar& calendars) {
    using Bars = std::vector<SourceBar>;
    std::map<Day, std::string> window;
    try {
        // The loader's own instants (build_futures_bar_query's predicate), with their text.
        const pqxx::result rows = txn.exec(
            "SELECT t.time::text, ((t.time AT TIME ZONE 'UTC')::date)::text FROM ("
            "SELECT DISTINCT time FROM futures_data.ohlcv_1d"
            " WHERE symbol = $1 AND time BETWEEN $2 AND $3) AS t ORDER BY t.time",
            pqxx::params{root + ".v.0", start_ts, end_ts});
        for (const auto& row : rows) {
            const Day date = parse_ymd(row[1].as<std::string>());
            if (!window.emplace(date, row[0].as<std::string>()).second) {
                return make_error<Bars>(ErrorCode::INVALID_DATA,
                                        "per_contract_rolls: " + root + ".v.0 has two instants on " +
                                            ymd(date),
                                        "contract_rolls");
            }
        }
    } catch (const std::exception& e) {
        return make_error<Bars>(ErrorCode::DATABASE_ERROR,
                                "per_contract_rolls: " + root + ": " + std::string(e.what()),
                                "contract_rolls");
    }
    if (window.empty()) return Bars{};
    auto start = series_start(txn, root);
    if (start.is_error()) {
        return make_error<Bars>(start.error()->code(), start.error()->what(), "contract_rolls");
    }
    size_t before_start = 0;
    for (auto it = window.begin(); it != window.end() && it->first < start.value();) {
        it = window.erase(it);
        ++before_start;
    }
    if (before_start > 0) {
        WARN("PER_CONTRACT_ROLLS " + root + ".v.0: " + std::to_string(before_start) +
             " grid bars before the series start " + ymd(start.value()) +
             " (no contract row) are not handed out");
    }
    if (window.empty()) return Bars{};
    auto series = build_held_series(txn, root, start.value(), window.rbegin()->first, calendars);
    if (series.is_error()) {
        return make_error<Bars>(series.error()->code(), series.error()->what(), "contract_rolls");
    }
    return bars_of_series(series.value().series(), window);
}

}  // namespace trade_ngin::contract_rolls
