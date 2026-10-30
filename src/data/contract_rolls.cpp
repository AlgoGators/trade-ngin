// src/data/contract_rolls.cpp
#include "trade_ngin/data/contract_rolls.hpp"

namespace trade_ngin::contract_rolls {

namespace {

/// Contract rows read before the range's first date, so a held contract with no row on it can carry
/// its last close.
constexpr std::chrono::days kCarryLookback{40};

}  // namespace

ContractRollSeries::ContractRollSeries(HeldSeries series, std::shared_ptr<const ContractBook> book,
                                       std::string cycle, std::optional<MicroAlias> alias,
                                       TradingCalendar cal)
    : series_(std::move(series)),
      book_(std::move(book)),
      cycle_(std::move(cycle)),
      alias_(std::move(alias)),
      cal_(std::move(cal)) {}

Result<RollOutlook> ContractRollSeries::outlook(Day date) const {
    const HeldDay* on = nullptr;
    for (const auto& d : series_.days) {
        if (d.date > date) break;
        on = &d;
    }
    if (!on) {
        return make_error<RollOutlook>(ErrorCode::INVALID_ARGUMENT,
                                       "no held contract on " + ymd(date) + " for root " +
                                           series_.root + ": before the series' first date",
                                       "contract_rolls");
    }
    try {
        RollOutlook o;
        o.date = date;
        o.held = on->held;
        o.dates = contract_dates(on->held, cal_);
        const std::string& root =
            alias_ && on->date < alias_->change ? alias_->full_root : series_.root;
        auto target = cycle_target(*book_, root, cycle_, on->held, on->date);
        if (target.is_error()) {
            return make_error<RollOutlook>(target.error()->code(), target.error()->what(),
                                           "contract_rolls");
        }
        const int front = next_cycle_delivery(cycle_, on->held.delivery_year,
                                              on->held.delivery_month);
        o.cycle_front_symbol = root + ContractKey::month_code(static_cast<unsigned>(front % 12) + 1) +
                               std::to_string((front / 12) % 10);
        if (target.value()) {
            o.target = *target.value()->key;
            o.target_is_cycle_front =
                o.target->delivery_index() ==
                listed_cycle_front(*book_, root, cycle_, on->held, o.target->delivery_index());
        }
        return o;
    } catch (const CalendarCoverageError& e) {
        return make_error<RollOutlook>(ErrorCode::INVALID_DATA, e.what(), "contract_rolls");
    }
}

Result<Day> series_start(pqxx::transaction_base& txn, const std::string& root) {
    const auto alias = micro_alias(root);
    try {
        const auto r = txn.exec(
            "SELECT min((o.time AT TIME ZONE 'UTC')::date)::text FROM futures_data.ohlcv_1d o"
            " WHERE o.symbol = $1 AND EXISTS (SELECT 1 FROM futures_data.contract_ohlcv_1d c"
            " WHERE c.root = (CASE WHEN (o.time AT TIME ZONE 'UTC')::date < $3::date THEN $2 ELSE $4"
            " END) AND c.date = (o.time AT TIME ZONE 'UTC')::date)",
            pqxx::params{root + ".v.0", alias ? alias->full_root : root,
                         alias ? ymd(alias->change) : std::string("1900-01-01"), root});
        if (r.empty() || r[0][0].is_null()) {
            return make_error<Day>(ErrorCode::DATA_NOT_FOUND,
                                   "root " + root + ": no grid date with a contract row",
                                   "contract_rolls");
        }
        return parse_ymd(r[0][0].as<std::string>());
    } catch (const std::exception& e) {
        return make_error<Day>(ErrorCode::DATABASE_ERROR,
                               "root " + root + ": " + std::string(e.what()), "contract_rolls");
    }
}

Result<ContractRollSeries> build_held_series(pqxx::transaction_base& txn, const std::string& root,
                                             Day start, Day end, const FuturesCalendar& calendars,
                                             Day log_from) {
    auto cycle = roll_cycle(root);
    if (cycle.is_error()) {
        return make_error<ContractRollSeries>(cycle.error()->code(), cycle.error()->what(),
                                              "contract_rolls");
    }
    auto root_cal = calendars.for_root(root);
    if (root_cal.is_error()) {
        return make_error<ContractRollSeries>(root_cal.error()->code(), root_cal.error()->what(),
                                              "contract_rolls");
    }
    const TradingCalendar& cal = root_cal.value();
    const auto alias = micro_alias(root);
    std::vector<std::string> roots{root};
    if (alias && start < alias->change) roots.push_back(alias->full_root);
    RuleInputs in;
    std::shared_ptr<ContractBook> book;
    try {
        in.grid = read_vendor_grid(txn, root + ".v.0", start, end);
        auto built = ContractBook::build(read_contract_bars(txn, roots, start - kCarryLookback, end));
        if (built.is_error()) {
            return make_error<ContractRollSeries>(built.error()->code(), built.error()->what(),
                                                  "contract_rolls");
        }
        book = std::make_shared<ContractBook>(built.value());
    } catch (const std::exception& e) {
        return make_error<ContractRollSeries>(ErrorCode::DATABASE_ERROR,
                                              "root " + root + ": " + std::string(e.what()),
                                              "contract_rolls");
    }
    if (alias && start < alias->change &&
        !book->has_bars(alias->full_root, start, alias->change - std::chrono::days{1})) {
        return make_error<ContractRollSeries>(
            ErrorCode::DATA_NOT_FOUND,
            "waiting on data: " + root + " before " + ymd(alias->change) + " reads the " +
                alias->full_root + " contracts, which are not loaded in contract_ohlcv_1d",
            "contract_rolls");
    }
    in.root = root;
    in.book = book.get();
    in.cal = &cal;
    in.cycle = cycle.value();
    in.alias = alias;
    in.log_from = log_from;
    auto held = apply_roll_rule(in);
    if (held.is_error()) {
        return make_error<ContractRollSeries>(held.error()->code(), held.error()->what(),
                                              "contract_rolls");
    }
    return ContractRollSeries(held.value(), std::move(book), cycle.value(), alias, cal);
}

}  // namespace trade_ngin::contract_rolls
