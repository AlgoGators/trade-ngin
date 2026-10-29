// src/data/per_contract_source.cpp
#include "trade_ngin/data/per_contract_source.hpp"

#include <arrow/api.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/carry_quote.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/data/roll_cycle.hpp"

namespace trade_ngin::contract_rolls {

PerContractSource& PerContractSource::instance() {
    static PerContractSource source;
    return source;
}

const char* to_string(SwitchBar switch_bar) {
    return switch_bar == SwitchBar::VENDOR ? "vendor" : "held";
}

std::optional<SwitchBar> switch_bar_from(const std::string& name) {
    if (name == "held")
        return SwitchBar::HELD;
    if (name == "vendor")
        return SwitchBar::VENDOR;
    return std::nullopt;
}

void PerContractSource::enable(FuturesCalendar calendar, SwitchBar switch_bar, bool carry_quotes) {
    std::lock_guard<std::mutex> lock(mutex_);
    calendar_ = std::move(calendar);
    switch_bar_ = switch_bar;
    carry_quotes_ = carry_quotes;
    enabled_ = true;
    log_floor_ = Day{};
    logged_.clear();
}

void PerContractSource::disable() {
    std::lock_guard<std::mutex> lock(mutex_);
    calendar_.reset();
    switch_bar_ = SwitchBar::HELD;
    carry_quotes_ = false;
    enabled_ = false;
    log_floor_ = Day{};
    logged_.clear();
}

void PerContractSource::set_log_floor(Day day) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_floor_ = day;
}

Day PerContractSource::log_floor() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_floor_;
}

bool PerContractSource::first_log(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    return logged_.insert(message).second;
}

SwitchBar PerContractSource::switch_bar() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return switch_bar_;
}

bool PerContractSource::carry_quotes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return carry_quotes_;
}

bool PerContractSource::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

FuturesCalendar PerContractSource::calendar() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calendar_ ? *calendar_ : FuturesCalendar{};
}

Result<void> configure_per_contract_source(bool per_contract_rolls, const std::string& switch_bar,
                                           bool carry_quotes) {
    if (!per_contract_rolls) {
        PerContractSource::instance().disable();
        if (carry_quotes) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "carry is on and per_contract_rolls is off: carry reads two "
                                    "contracts' own closes, which only the per-contract source has",
                                    "contract_rolls");
        }
        return Result<void>();
    }
    const std::optional<SwitchBar> setting = switch_bar_from(switch_bar);
    if (!setting) {
        PerContractSource::instance().disable();
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "per_contract_switch_bar must be \"held\" or \"vendor\", got \"" + switch_bar + "\"",
            "contract_rolls");
    }
    const std::string path = FuturesCalendar::resolve_path();
    auto calendar = FuturesCalendar::load(path);
    if (calendar.is_error()) {
        return make_error<void>(calendar.error()->code(),
                                "per_contract_rolls is on and the futures calendar cannot be "
                                "loaded: " + std::string(calendar.error()->what()),
                                "contract_rolls");
    }
    PerContractSource::instance().enable(calendar.value(), *setting, carry_quotes);
    INFO(
        "PER_CONTRACT_ROLLS on: the futures bar source hands out the held contract's own bars "
        "(HD's roll rule, contract_roll_rule.hpp), roll bars carrying the old contract's close; "
        "futures calendar " +
        path + "; per_contract_switch_bar " + to_string(*setting) +
        " (an unconfirmed vendor switch bar is " +
        (*setting == SwitchBar::VENDOR ? "the vendor's contract's own bar"
                                       : "the held contract's own bar") +
        ")" + (carry_quotes ? "; carry quotes on (the held contract's and its roll-cycle "
                              "neighbour's closes on every bar)"
                            : ""));
    return Result<void>();
}

Result<std::vector<SourceBar>> bars_of_series(const HeldSeries& series,
                                              const std::map<Day, std::string>& window,
                                              SwitchBar switch_bar, Day log_from,
                                              const std::function<bool(const std::string&)>& log_once) {
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
            // A vendor switch the next bar did not confirm (or the range ends on it): the held
            // contract's own bar, the field naming the target it would have rolled into.
            const TentativeRoll& t = *d.tentative;
            if (switch_bar == SwitchBar::VENDOR) {
                // Ruling 19 "vendor": the bar of the contract the vendor switched to, the field
                // naming the held contract (its close and id here).
                b.open = t.bar.open;
                b.high = t.bar.high;
                b.low = t.bar.low;
                b.close = t.bar.close;
                b.volume = static_cast<double>(t.bar.volume);
                b.instrument_id = t.bar.instrument_id;
                b.roll_from =
                    RollFrom{d.close, held_id, RollFromKind::AWAITS_NEXT_VENDOR, ymd(d.date)};
            } else {
                b.open = d.open;
                b.high = d.high;
                b.low = d.low;
                b.close = d.close;
                b.volume = static_cast<double>(d.volume);
                b.instrument_id = held_id;
                b.roll_from = RollFrom{t.bar.close, t.bar.instrument_id, RollFromKind::AWAITS_NEXT,
                                       ymd(d.date)};
            }
        } else if (d.roll && d.awaits_next_bar) {
            // A vendor switch the next bar confirms: handed out exactly as above, because a run
            // whose bars end here cannot tell the two apart. The old contract is still held on it.
            if (!d.old_bar) {
                return make_error<Bars>(ErrorCode::INVALID_DATA,
                                        "per_contract_rolls: " + symbol + " " + ymd(d.date) +
                                            ": a vendor roll awaiting the next bar has no old bar",
                                        "contract_rolls");
            }
            if (switch_bar == SwitchBar::VENDOR) {
                // Ruling 19 "vendor": the target's own bar, the field naming the old contract.
                b.open = d.open;
                b.high = d.high;
                b.low = d.low;
                b.close = d.close;
                b.volume = static_cast<double>(d.volume);
                b.instrument_id = d.bar_id;
                b.roll_from = RollFrom{d.old_bar->close, d.old_bar->instrument_id,
                                       RollFromKind::AWAITS_NEXT_VENDOR, ymd(d.date)};
            } else {
                b.open = d.old_bar->open;
                b.high = d.old_bar->high;
                b.low = d.old_bar->low;
                b.close = d.old_bar->close;
                b.volume = static_cast<double>(d.old_bar->volume);
                b.instrument_id = d.old_bar->instrument_id;
                b.roll_from = RollFrom{d.close, d.bar_id, RollFromKind::AWAITS_NEXT, ymd(d.date)};
            }
            held_close = d.old_bar->close;
            held_id = d.old_bar->instrument_id;
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
                // R6: the micros' change is a change of price source, not a trade.
                from.kind = d.trigger == RollTrigger::ALIAS ? RollFromKind::SOURCE_CHANGE
                                                            : RollFromKind::KNOWN;
                from.date = ymd(d.date);
                // MM fix round 1: the full-size contract's own volume, for a listing
                // predecessor's legs on this bar (ListingDates::add_predecessor_bars).
                if (from.kind == RollFromKind::SOURCE_CHANGE && d.old_bar) {
                    from.volume = static_cast<double>(d.old_bar->volume);
                }
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
                    const std::string line =
                        "PER_CONTRACT_ROLLS fallback roll " + symbol + " " + ymd(d.date) + " " +
                        (d.old ? d.old->raw_symbol : std::string("?")) + "->" + d.held.raw_symbol +
                        " (" + to_string(d.fallback) +
                        "): the old contract has no bar; its close on the roll bar is read as " +
                        std::to_string(from.close);
                    if (d.date >= log_from && (!log_once || log_once(line))) WARN(line);
                }
                b.roll_from = from;
            } else if (d.left_close) {
                // A bar after a roll, up to the next one: the old contract's close here, for a
                // consumer that withheld the roll bar and the bars since (it books the roll on its
                // first consumed bar, HD 10-23).
                b.roll_from = RollFrom{*d.left_close, d.left_id,
                                       d.left_by_alias ? RollFromKind::AFTER_SOURCE_CHANGE
                                                       : RollFromKind::AFTER_ROLL,
                                       ymd(d.left_roll_date)};
                if (d.left_by_alias && d.left_volume) {
                    b.roll_from->volume = static_cast<double>(*d.left_volume);
                }
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

void attach_carry_quotes(const HeldSeries& series, const ContractBook& book,
                         std::vector<SourceBar>& bars) {
    std::map<Day, const HeldDay*> by_date;
    for (const HeldDay& d : series.days) by_date[d.date] = &d;
    std::map<std::string, std::string> cycles;  // the held contract's root to its roll cycle
    for (SourceBar& b : bars) {
        b.carry.reset();
        const auto it = by_date.find(b.date);
        if (it == by_date.end()) continue;
        const HeldDay& d = *it->second;
        // The pair moves on the bar that confirms the roll: a vendor switch awaiting the next bar
        // still holds the old contract here.
        const ContractKey& held = d.roll && d.awaits_next_bar && d.old ? *d.old : d.held;
        auto cycle = cycles.find(held.root);
        if (cycle == cycles.end()) {
            const auto found = roll_cycle(held.root);
            cycle = cycles.emplace(held.root, found.is_ok() ? found.value() : std::string()).first;
        }
        if (cycle->second.empty()) continue;
        b.carry = carry_quote(book, held, d.date, cycle->second);
    }
}

Result<std::vector<std::string>> source_roots(const std::vector<std::string>& symbols) {
    std::vector<std::string> roots;
    const std::string suffix = ".v.0";
    for (const auto& symbol : symbols) {
        if (symbol.size() <= suffix.size() ||
            symbol.compare(symbol.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return make_error<std::vector<std::string>>(
                ErrorCode::INVALID_ARGUMENT,
                "per_contract_rolls: " + symbol + " is not a continuous futures symbol (<root>.v.0)",
                "PostgresDatabase");
        }
        roots.push_back(symbol.substr(0, symbol.size() - suffix.size()));
    }
    return roots;
}

void sort_source_bars(std::vector<SourceBar>& bars) {
    std::stable_sort(bars.begin(), bars.end(), [](const SourceBar& a, const SourceBar& b) {
        if (a.time_text != b.time_text) return a.time_text < b.time_text;
        return a.symbol < b.symbol;
    });
}

std::vector<market_data_utils::FuturesInstrumentId> instrument_id_rows(
    const std::vector<SourceBar>& bars) {
    std::vector<const SourceBar*> keyed;
    for (const auto& bar : bars) {
        if (!bar.instrument_id.empty()) keyed.push_back(&bar);
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const SourceBar* a, const SourceBar* b) {
        if (a->symbol != b->symbol) return a->symbol < b->symbol;
        return a->time_text < b->time_text;
    });
    std::vector<market_data_utils::FuturesInstrumentId> rows;
    rows.reserve(keyed.size());
    for (const auto* bar : keyed) {
        rows.push_back({bar->symbol, bar->time_text.substr(0, 10), bar->instrument_id});
    }
    return rows;
}

Timestamp source_bar_time(const std::string& time_text) {
    std::tm time_info = {};
    std::istringstream ss(time_text);
    ss >> std::get_time(&time_info, "%Y-%m-%d %H:%M:%S");
    time_t time_val = std::mktime(&time_info);
    trade_ngin::core::safe_gmtime(&time_val, &time_info);
    return std::chrono::system_clock::from_time_t(std::mktime(&time_info));
}

MarketDataEvent source_bar_event(const SourceBar& bar) {
    MarketDataEvent event;
    event.type = MarketDataEventType::BAR;
    event.symbol = bar.symbol;
    event.timestamp = source_bar_time(bar.time_text);
    event.numeric_fields["open"] = bar.open;
    event.numeric_fields["high"] = bar.high;
    event.numeric_fields["low"] = bar.low;
    event.numeric_fields["close"] = bar.close;
    event.numeric_fields["volume"] = bar.volume;
    return event;
}

Result<std::shared_ptr<arrow::Table>> source_bars_to_arrow(const std::vector<SourceBar>& bars,
                                                           bool with_carry) {
    using Table = std::shared_ptr<arrow::Table>;
    arrow::MemoryPool* pool = arrow::default_memory_pool();
    arrow::TimestampBuilder timestamp_builder(arrow::timestamp(arrow::TimeUnit::SECOND), pool);
    arrow::StringBuilder symbol_builder(pool);
    arrow::DoubleBuilder open_builder(pool);
    arrow::DoubleBuilder high_builder(pool);
    arrow::DoubleBuilder low_builder(pool);
    arrow::DoubleBuilder close_builder(pool);
    arrow::DoubleBuilder volume_builder(pool);
    arrow::StringBuilder instrument_id_builder(pool);
    arrow::DoubleBuilder roll_close_builder(pool);
    arrow::StringBuilder roll_id_builder(pool);
    arrow::Int8Builder roll_kind_builder(pool);
    arrow::StringBuilder roll_date_builder(pool);
    arrow::DoubleBuilder carry_held_close_builder(pool);
    arrow::DoubleBuilder carry_other_close_builder(pool);
    arrow::Int32Builder carry_months_builder(pool);
    arrow::StringBuilder carry_held_symbol_builder(pool);
    arrow::StringBuilder carry_other_symbol_builder(pool);
    arrow::DoubleBuilder roll_volume_builder(pool);  // MM fix round 1: RollFrom::volume
    auto failed = [](const std::string& operation) {
        return make_error<Table>(ErrorCode::CONVERSION_ERROR,
                                 "Arrow builder error during " + operation + " (per_contract_rolls)",
                                 "PostgresDatabase");
    };
    try {
        for (const auto& bar : bars) {
            const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                                       source_bar_time(bar.time_text).time_since_epoch())
                                       .count();
            if (timestamp_builder.Append(timestamp) != arrow::Status::OK() ||
                symbol_builder.Append(bar.symbol) != arrow::Status::OK() ||
                open_builder.Append(bar.open) != arrow::Status::OK() ||
                high_builder.Append(bar.high) != arrow::Status::OK() ||
                low_builder.Append(bar.low) != arrow::Status::OK() ||
                close_builder.Append(bar.close) != arrow::Status::OK() ||
                volume_builder.Append(bar.volume) != arrow::Status::OK()) {
                return failed("append");
            }
            const arrow::Status id_status = bar.instrument_id.empty()
                                                ? instrument_id_builder.AppendNull()
                                                : instrument_id_builder.Append(bar.instrument_id);
            if (id_status != arrow::Status::OK()) return failed("append");
            if (bar.roll_from) {
                if (roll_close_builder.Append(bar.roll_from->close) != arrow::Status::OK() ||
                    roll_id_builder.Append(bar.roll_from->instrument_id) != arrow::Status::OK() ||
                    roll_kind_builder.Append(static_cast<int8_t>(bar.roll_from->kind)) !=
                        arrow::Status::OK() ||
                    roll_date_builder.Append(bar.roll_from->date) != arrow::Status::OK() ||
                    (bar.roll_from->volume ? roll_volume_builder.Append(*bar.roll_from->volume)
                                           : roll_volume_builder.AppendNull()) != arrow::Status::OK()) {
                    return failed("append");
                }
            } else if (roll_close_builder.AppendNull() != arrow::Status::OK() ||
                       roll_id_builder.AppendNull() != arrow::Status::OK() ||
                       roll_kind_builder.AppendNull() != arrow::Status::OK() ||
                       roll_date_builder.AppendNull() != arrow::Status::OK() ||
                       roll_volume_builder.AppendNull() != arrow::Status::OK()) {
                return failed("append");
            }
            if (!with_carry) continue;
            if (bar.carry) {
                if (carry_held_close_builder.Append(bar.carry->held_close) != arrow::Status::OK() ||
                    carry_other_close_builder.Append(bar.carry->other_close) != arrow::Status::OK() ||
                    carry_months_builder.Append(bar.carry->months) != arrow::Status::OK() ||
                    carry_held_symbol_builder.Append(bar.carry->held_symbol) != arrow::Status::OK() ||
                    carry_other_symbol_builder.Append(bar.carry->other_symbol) !=
                        arrow::Status::OK()) {
                    return failed("append");
                }
            } else if (carry_held_close_builder.AppendNull() != arrow::Status::OK() ||
                       carry_other_close_builder.AppendNull() != arrow::Status::OK() ||
                       carry_months_builder.AppendNull() != arrow::Status::OK() ||
                       carry_held_symbol_builder.AppendNull() != arrow::Status::OK() ||
                       carry_other_symbol_builder.AppendNull() != arrow::Status::OK()) {
                return failed("append");
            }
        }
        std::vector<std::shared_ptr<arrow::Array>> arrays(with_carry ? 18 : 13);
        if (timestamp_builder.Finish(&arrays[0]) != arrow::Status::OK() ||
            symbol_builder.Finish(&arrays[1]) != arrow::Status::OK() ||
            open_builder.Finish(&arrays[2]) != arrow::Status::OK() ||
            high_builder.Finish(&arrays[3]) != arrow::Status::OK() ||
            low_builder.Finish(&arrays[4]) != arrow::Status::OK() ||
            close_builder.Finish(&arrays[5]) != arrow::Status::OK() ||
            volume_builder.Finish(&arrays[6]) != arrow::Status::OK() ||
            instrument_id_builder.Finish(&arrays[7]) != arrow::Status::OK() ||
            roll_close_builder.Finish(&arrays[8]) != arrow::Status::OK() ||
            roll_id_builder.Finish(&arrays[9]) != arrow::Status::OK() ||
            roll_kind_builder.Finish(&arrays[10]) != arrow::Status::OK() ||
            roll_date_builder.Finish(&arrays[11]) != arrow::Status::OK() ||
            roll_volume_builder.Finish(&arrays[12]) != arrow::Status::OK()) {
            return failed("finish");
        }
        if (with_carry && (carry_held_close_builder.Finish(&arrays[13]) != arrow::Status::OK() ||
                           carry_other_close_builder.Finish(&arrays[14]) != arrow::Status::OK() ||
                           carry_months_builder.Finish(&arrays[15]) != arrow::Status::OK() ||
                           carry_held_symbol_builder.Finish(&arrays[16]) != arrow::Status::OK() ||
                           carry_other_symbol_builder.Finish(&arrays[17]) != arrow::Status::OK())) {
            return failed("finish");
        }
        arrow::FieldVector fields{arrow::field("time", arrow::timestamp(arrow::TimeUnit::SECOND)),
                                     arrow::field("symbol", arrow::utf8()),
                                     arrow::field("open", arrow::float64()),
                                     arrow::field("high", arrow::float64()),
                                     arrow::field("low", arrow::float64()),
                                     arrow::field("close", arrow::float64()),
                                     arrow::field("volume", arrow::float64()),
                                     arrow::field("instrument_id", arrow::utf8()),
                                     arrow::field("roll_from_close", arrow::float64()),
                                     arrow::field("roll_from_id", arrow::utf8()),
                                     arrow::field("roll_from_kind", arrow::int8()),
                                     arrow::field("roll_from_date", arrow::utf8()),
                                     arrow::field("roll_from_volume", arrow::float64())};
        if (with_carry) {
            fields.push_back(arrow::field("carry_held_close", arrow::float64()));
            fields.push_back(arrow::field("carry_other_close", arrow::float64()));
            fields.push_back(arrow::field("carry_months", arrow::int32()));
            fields.push_back(arrow::field("carry_held_symbol", arrow::utf8()));
            fields.push_back(arrow::field("carry_other_symbol", arrow::utf8()));
        }
        return Result<Table>(arrow::Table::Make(arrow::schema(fields), arrays));
    } catch (const std::exception& e) {
        return make_error<Table>(ErrorCode::CONVERSION_ERROR,
                                 "Exception during Arrow table conversion (per_contract_rolls): " +
                                     std::string(e.what()),
                                 "PostgresDatabase");
    }
}

Result<std::map<Day, std::string>> grid_window(
    const std::string& root, const std::vector<std::pair<std::string, std::string>>& instants) {
    std::map<Day, std::string> window;
    for (const auto& [time_text, date_text] : instants) {
        const Day date = parse_ymd(date_text);
        if (!window.emplace(date, time_text).second) {
            return make_error<std::map<Day, std::string>>(
                ErrorCode::INVALID_DATA,
                "per_contract_rolls: " + root + ".v.0 has two instants on " + ymd(date),
                "contract_rolls");
        }
    }
    return window;
}

size_t drop_before_series_start(const std::string& root, std::map<Day, std::string>& window,
                                Day start) {
    size_t before_start = 0;
    for (auto it = window.begin(); it != window.end() && it->first < start;) {
        it = window.erase(it);
        ++before_start;
    }
    if (before_start > 0) {
        WARN("PER_CONTRACT_ROLLS " + root + ".v.0: " + std::to_string(before_start) +
             " grid bars before the series start " + ymd(start) +
             " (no contract row) are not handed out");
    }
    return before_start;
}

std::pair<std::string, pqxx::params> grid_window_query(const std::string& root,
                                                       const std::string& start_ts,
                                                       const std::string& end_ts) {
    // The loader's own instants (build_futures_bar_query's predicate), with their text; for MGC and
    // SIL before their change, the full-size root's (grid_rows).
    const GridRows grid = grid_rows(root, 3);
    pqxx::params params{start_ts, end_ts};
    params.append(grid.params);
    return {"SELECT t.time::text, ((t.time AT TIME ZONE 'UTC')::date)::text FROM ("
            "SELECT DISTINCT o.time FROM futures_data.ohlcv_1d o"
            " WHERE " + grid.predicate + " AND o.time BETWEEN $1 AND $2) AS t ORDER BY t.time",
            params};
}

Result<std::vector<SourceBar>> per_contract_bars(
    pqxx::transaction_base& txn, const std::string& root, const std::string& start_ts,
    const std::string& end_ts, const FuturesCalendar& calendars, SwitchBar switch_bar) {
    using Bars = std::vector<SourceBar>;
    std::map<Day, std::string> window;
    try {
        const auto [sql, params] = grid_window_query(root, start_ts, end_ts);
        const pqxx::result rows = txn.exec(sql, params);
        std::vector<std::pair<std::string, std::string>> instants;
        instants.reserve(rows.size());
        for (const auto& row : rows) {
            instants.emplace_back(row[0].as<std::string>(), row[1].as<std::string>());
        }
        auto w = grid_window(root, instants);
        if (w.is_error()) {
            return make_error<Bars>(w.error()->code(), w.error()->what(), "contract_rolls");
        }
        window = std::move(w.value());
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
    drop_before_series_start(root, window, start.value());
    if (window.empty()) return Bars{};
    // The rule runs from the series start; only the window's own warnings are logged, from the
    // run's log floor on (a live run: its T-1, the bar it consumes for the first time), each once
    // per run however many loads read the bar (R4a open question 4).
    const Day log_from = std::max(window.begin()->first, PerContractSource::instance().log_floor());
    const auto log_once = [](const std::string& message) {
        return PerContractSource::instance().first_log(message);
    };
    auto series = build_held_series(txn, root, start.value(), window.rbegin()->first, calendars,
                                    log_from, log_once);
    if (series.is_error()) {
        return make_error<Bars>(series.error()->code(), series.error()->what(), "contract_rolls");
    }
    auto bars = bars_of_series(series.value().series(), window, switch_bar, log_from, log_once);
    if (bars.is_error() || !PerContractSource::instance().carry_quotes()) return bars;
    Bars quoted = bars.value();
    attach_carry_quotes(series.value().series(), series.value().book(), quoted);
    return quoted;
}

}  // namespace trade_ngin::contract_rolls
