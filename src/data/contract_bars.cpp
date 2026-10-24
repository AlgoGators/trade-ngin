// src/data/contract_bars.cpp
#include "trade_ngin/data/contract_bars.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <tuple>

namespace trade_ngin::contract_rolls {

namespace {

constexpr const char* kMonthCodes = "FGHJKMNQUVXZ";

std::optional<Day> optional_day(const pqxx::field& f) {
    if (f.is_null()) return std::nullopt;
    return parse_ymd(f.as<std::string>());
}

}  // namespace

std::string contract_bars_query() {
    return "SELECT root, raw_symbol, instrument_id, date::text, expiration::text, open, high, low,"
           " close, volume FROM futures_data.contract_ohlcv_1d"
           " WHERE root = ANY($1) AND date BETWEEN $2::date AND $3::date"
           " ORDER BY root, date, raw_symbol, instrument_id";
}

std::string vendor_grid_query() {
    // The grid is the loader's: one bar per instant, the copy the B0 loader keeps where the table
    // ever holds two (market_data_utils kFuturesBarKeepOrder). The vendor's id is the raw row of the
    // same symbol and instant, with no condition on the print (R0 class d).
    return "SELECT ((k.time AT TIME ZONE 'UTC')::date)::text, k.symbol, k.open, k.high, k.low,"
           " k.close, k.volume, r.instrument_id FROM ("
           "SELECT DISTINCT ON (time) time, symbol, open, high, low, close, volume"
           " FROM futures_data.ohlcv_1d WHERE symbol = $1"
           " AND time >= ($2::date AT TIME ZONE 'UTC') AND time < (($3::date + 1) AT TIME ZONE 'UTC')"
           " ORDER BY time, volume DESC, close ASC, open ASC, high ASC, low ASC"
           ") AS k LEFT JOIN futures_data.ohlcv_1d_raw AS r ON r.symbol = k.symbol AND r.ts_event = k.time"
           " ORDER BY k.time";
}

std::vector<ContractBar> read_contract_bars(pqxx::transaction_base& txn,
                                            const std::vector<std::string>& roots, Day start,
                                            Day end) {
    const pqxx::result result =
        txn.exec(contract_bars_query(), pqxx::params{roots, ymd(start), ymd(end)});
    std::vector<ContractBar> rows;
    rows.reserve(result.size());
    for (const auto& row : result) {
        ContractBar b;
        b.root = row[0].as<std::string>();
        b.raw_symbol = row[1].as<std::string>();
        b.instrument_id = row[2].as<std::string>();
        b.date = parse_ymd(row[3].as<std::string>());
        b.expiration = optional_day(row[4]);
        b.open = row[5].as<double>();
        b.high = row[6].as<double>();
        b.low = row[7].as<double>();
        b.close = row[8].as<double>();
        b.volume = row[9].as<int64_t>();
        rows.push_back(std::move(b));
    }
    return rows;
}

std::vector<VendorBar> read_vendor_grid(pqxx::transaction_base& txn, const std::string& symbol,
                                        Day start, Day end) {
    const pqxx::result result =
        txn.exec(vendor_grid_query(), pqxx::params{symbol, ymd(start), ymd(end)});
    std::vector<VendorBar> rows;
    rows.reserve(result.size());
    for (const auto& row : result) {
        VendorBar b;
        b.date = parse_ymd(row[0].as<std::string>());
        b.symbol = row[1].as<std::string>();
        b.open = row[2].is_null() ? 0.0 : row[2].as<double>();
        b.high = row[3].is_null() ? 0.0 : row[3].as<double>();
        b.low = row[4].is_null() ? 0.0 : row[4].as<double>();
        b.close = row[5].is_null() ? 0.0 : row[5].as<double>();
        b.volume = row[6].as<int64_t>();
        b.instrument_id = row[7].is_null() ? std::string() : row[7].as<std::string>();
        rows.push_back(std::move(b));
    }
    return rows;
}

char ContractKey::month_code(unsigned month) {
    return month >= 1 && month <= 12 ? kMonthCodes[month - 1] : '?';
}

unsigned ContractKey::month_of_code(char code) {
    for (unsigned i = 0; i < 12; ++i) {
        if (kMonthCodes[i] == code) return i + 1;
    }
    return 0;
}

Result<ContractKey> ContractKey::make(const std::string& root, const std::string& raw_symbol,
                                      Day expiration) {
    auto bad = [&](const std::string& why) {
        return make_error<ContractKey>(ErrorCode::INVALID_DATA,
                                       "contract " + raw_symbol + " of root " + root + ": " + why,
                                       "contract_rolls");
    };
    if (raw_symbol.size() < root.size() + 2 || raw_symbol.compare(0, root.size(), root) != 0) {
        return bad("the raw symbol is not the root, a month code and a year");
    }
    const std::string rest = raw_symbol.substr(root.size());
    const unsigned m = month_of_code(rest[0]);
    const std::string digits = rest.substr(1);
    if (m == 0 || digits.empty() || digits.size() > 2 ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(c) != 0; })) {
        return bad("the raw symbol is not the root, a month code and a year");
    }
    const int exp_year = static_cast<int>(std::chrono::year_month_day{expiration}.year());
    int y = 0;
    if (digits.size() == 2) {
        y = 2000 + std::stoi(digits);
    } else {
        // The one year with this last digit within one year of the expiration: a last trading day
        // falls in the delivery month or up to two months before it.
        const int d = digits[0] - '0';
        for (int c = exp_year - 1; c <= exp_year + 1; ++c) {
            if (((c % 10) + 10) % 10 == d) y = c;
        }
        if (y == 0) return bad("no delivery year with digit " + digits + " near " + ymd(expiration));
    }
    ContractKey k;
    k.root = root;
    k.raw_symbol = raw_symbol;
    k.expiration = expiration;
    k.delivery_year = y;
    k.delivery_month = m;
    // The expiration must fall between the first day two months before the delivery month and the
    // delivery month's last day.
    const int exp_index = exp_year * 12 +
                          static_cast<int>(static_cast<unsigned>(
                              std::chrono::year_month_day{expiration}.month())) - 1;
    if (exp_index > k.delivery_index() || exp_index < k.delivery_index() - 2) {
        return bad("expiration " + ymd(expiration) + " is not within two months before delivery " +
                   std::to_string(y) + "-" + std::to_string(m));
    }
    return k;
}

std::string ContractKey::to_string() const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), " (%04d-%02u, expires ", delivery_year, delivery_month);
    return raw_symbol + buf + ymd(expiration) + ")";
}

std::string Contract::describe() const {
    if (key) return key->to_string();
    std::string ids;
    for (const auto& id : instrument_ids) ids += (ids.empty() ? "" : ",") + id;
    return raw_symbol + " (root " + root + ", instrument_id " + ids + ", no expiration, bars " +
           ymd(first_bar) + " to " + ymd(last_bar) + ")";
}

Result<ContractBook> ContractBook::build(const std::vector<ContractBar>& rows) {
    ContractBook book;
    // Identity: (root, raw_symbol, expiration). A NULL expiration groups by
    // (root, raw_symbol, instrument_id) instead and leaves the contract unresolved.
    std::map<std::tuple<std::string, std::string, std::string>, size_t> index;
    std::set<std::tuple<std::string, std::string, Day>> seen_id_day;
    for (const auto& b : rows) {
        const auto dup = std::make_tuple(b.root, b.instrument_id, b.date);
        if (!seen_id_day.insert(dup).second) {
            return make_error<ContractBook>(
                ErrorCode::INVALID_DATA,
                "two contract bars of root " + b.root + " carry instrument_id " + b.instrument_id +
                    " on " + ymd(b.date),
                "contract_rolls");
        }
        const std::string third = b.expiration ? "x" + ymd(*b.expiration) : "i" + b.instrument_id;
        const auto id = std::make_tuple(b.root, b.raw_symbol, third);
        auto it = index.find(id);
        if (it == index.end()) {
            Contract c;
            c.root = b.root;
            c.raw_symbol = b.raw_symbol;
            if (b.expiration) {
                auto key = ContractKey::make(b.root, b.raw_symbol, *b.expiration);
                if (key.is_error()) {
                    return make_error<ContractBook>(key.error()->code(), key.error()->what(),
                                                    "contract_rolls");
                }
                c.key = key.value();
            }
            c.first_bar = b.date;
            c.last_bar = b.date;
            it = index.emplace(id, book.contracts_.size()).first;
            book.contracts_.push_back(std::move(c));
        }
        Contract& c = book.contracts_[it->second];
        c.first_bar = std::min(c.first_bar, b.date);
        c.last_bar = std::max(c.last_bar, b.date);
        if (std::find(c.instrument_ids.begin(), c.instrument_ids.end(), b.instrument_id) ==
            c.instrument_ids.end()) {
            c.instrument_ids.push_back(b.instrument_id);
        }
        c.bars.emplace(b.date, b);
    }
    for (size_t i = 0; i < book.contracts_.size(); ++i) {
        const Contract& c = book.contracts_[i];
        for (const auto& [day, bar] : c.bars) book.by_day_[{c.root, day}].push_back(i);
        for (const auto& id : c.instrument_ids) book.by_id_[{c.root, id}].push_back(i);
    }
    return book;
}

std::vector<const Contract*> ContractBook::printing_on(const std::string& root, Day day) const {
    std::vector<const Contract*> out;
    auto it = by_day_.find({root, day});
    if (it == by_day_.end()) return out;
    for (size_t i : it->second) out.push_back(&contracts_[i]);
    return out;
}

const Contract* ContractBook::named_by_id(const std::string& root,
                                          const std::string& instrument_id, Day day) const {
    if (instrument_id.empty()) return nullptr;
    auto it = by_id_.find({root, instrument_id});
    if (it == by_id_.end()) return nullptr;
    const Contract* spanning = nullptr;
    int spans = 0;
    for (size_t i : it->second) {
        const Contract& c = contracts_[i];
        const ContractBar* b = c.bar_on(day);
        if (b && b->instrument_id == instrument_id) return &c;
        // rolls step 4: to the expiration, never to the last bar read (no look-ahead)
        if (c.first_bar <= day && day <= (c.key ? c.key->expiration : c.last_bar)) {
            spanning = &c;
            ++spans;
        }
    }
    return spans == 1 ? spanning : nullptr;
}

const Contract* ContractBook::find(const ContractKey& key) const {
    for (const auto& c : contracts_) {
        if (c.key && *c.key == key) return &c;
    }
    return nullptr;
}

Result<ContractKey> ContractBook::require(const Contract& contract) {
    if (contract.key) return *contract.key;
    return make_error<ContractKey>(
        ErrorCode::INVALID_DATA,
        "contract " + contract.describe() +
            " has no expiration in futures_data.contract_ohlcv_1d: the roll rule orders contracts "
            "by it and refuses this one",
        "contract_rolls");
}

bool ContractBook::has_bars(const std::string& root, Day from, Day to) const {
    auto it = by_day_.lower_bound({root, from});
    return it != by_day_.end() && it->first.first == root && it->first.second <= to;
}

}  // namespace trade_ngin::contract_rolls
