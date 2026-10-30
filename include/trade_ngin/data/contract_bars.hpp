// include/trade_ngin/data/contract_bars.hpp
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <pqxx/pqxx>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/roll_calendar.hpp"

namespace trade_ngin::contract_rolls {

/**
 * @brief One row of futures_data.contract_ohlcv_1d (algogators #37): one outright contract's daily
 *        bar, raw prices, never adjusted.
 *
 * `instrument_id` is the vendor's id string. It is unique on a date within a root, but the vendor
 * REUSES ids across contracts years apart (2017 is 6BX0 in 2020 and GCQ4 in 2023-24) and
 * `raw_symbol` has a one-digit year (GCZ6 is the 2016 and the 2026 contract), so neither names a
 * contract across dates on its own: see ContractKey. `expiration` is the contract's last trading
 * day; NULL on production until the data side fills it from the definition records.
 */
struct ContractBar {
    Day date{};
    std::string instrument_id;
    std::string root;
    std::string raw_symbol;
    std::optional<Day> expiration;
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    int64_t volume{0};
};

/**
 * @brief One bar of the vendor's continuous series (futures_data.ohlcv_1d, the date grid the
 *        engine trades on) with the vendor's contract id of the same bar, read from
 *        futures_data.ohlcv_1d_raw by symbol and instant.
 *
 * The id is joined by (symbol, time) only, never by matching the print: R0 (class d) found six
 * kept bars whose print differs from the raw row, and the raw table has an id on every bar. An
 * empty id means the raw table has no row for that instant.
 */
struct VendorBar {
    Day date{};
    std::string symbol;  ///< "GC.v.0"
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    int64_t volume{0};
    std::string instrument_id;
};

/// SELECT of contract_ohlcv_1d for $1 (text[] of roots), dates $2 to $3 (inclusive, "YYYY-MM-DD"),
/// ORDER BY root, date, raw_symbol, instrument_id.
std::string contract_bars_query();
/// SELECT of the vendor's grid for $1 (one symbol, "GC.v.0"), dates $2 to $3 (inclusive), ORDER BY
/// date. The bar's date is its UTC date (every futures bar is stamped 00:00:00 UTC).
std::string vendor_grid_query();

/// Reads contract bars of `roots` over [start, end]. Read only; throws what pqxx throws.
std::vector<ContractBar> read_contract_bars(pqxx::transaction_base& txn,
                                            const std::vector<std::string>& roots, Day start,
                                            Day end);
/// Reads the vendor's grid of `symbol` over [start, end]. Read only; throws what pqxx throws.
std::vector<VendorBar> read_vendor_grid(pqxx::transaction_base& txn, const std::string& symbol,
                                        Day start, Day end);

/**
 * @brief A contract's identity: (root, raw_symbol, expiration).
 *
 * The delivery month is the raw symbol's month code; the delivery year is resolved by the
 * expiration (a one-digit year names the year with that last digit within one year of the
 * expiration; a two-digit year, as on far CL and NG contracts, is read as written). Ordering is by
 * expiration, then raw symbol: the rule's "expiry order".
 */
struct ContractKey {
    std::string root;
    std::string raw_symbol;
    Day expiration{};
    int delivery_year{0};
    unsigned delivery_month{0};  ///< 1..12

    /// Builds and checks a key: the raw symbol must be root + month code + one or two digits, and
    /// the expiration must fall between two months before the delivery month and its end.
    static Result<ContractKey> make(const std::string& root, const std::string& raw_symbol,
                                    Day expiration);
    /// The month code letter of `month` (1..12), "FGHJKMNQUVXZ".
    static char month_code(unsigned month);
    /// 1..12 for a month code letter, 0 for anything else.
    static unsigned month_of_code(char code);

    /// "GCZ6 (2026-12, expires 2026-12-29)".
    std::string to_string() const;
    /// Delivery month as year * 12 + month - 1, to step through months.
    int delivery_index() const { return delivery_year * 12 + static_cast<int>(delivery_month) - 1; }

    bool operator==(const ContractKey& o) const {
        return root == o.root && raw_symbol == o.raw_symbol && expiration == o.expiration;
    }
    bool operator!=(const ContractKey& o) const { return !(*this == o); }
    bool operator<(const ContractKey& o) const {
        if (expiration != o.expiration) return expiration < o.expiration;
        if (root != o.root) return root < o.root;
        return raw_symbol < o.raw_symbol;
    }
};

/**
 * @brief One contract's bars, grouped by identity.
 *
 * A row whose expiration is NULL cannot be given an identity: its bars are grouped by
 * (root, raw_symbol, instrument_id) and the contract is `unresolved`. The rule refuses such a
 * contract the moment it needs it (ContractBook::require), with an error naming it.
 */
struct Contract {
    std::optional<ContractKey> key;  ///< nullopt when the rows have no expiration
    std::string root;
    std::string raw_symbol;
    std::vector<std::string> instrument_ids;  ///< every id the contract's rows carry, in date order
    Day first_bar{};
    Day last_bar{};
    std::map<Day, ContractBar> bars;

    bool resolved() const { return key.has_value(); }
    const ContractBar* bar_on(Day day) const {
        auto it = bars.find(day);
        return it == bars.end() ? nullptr : &it->second;
    }
    /// The contract named for an error or a WARN: the key, else "GCZ6 (instrument_id 41271, no
    /// expiration, bars 2025-01-02 to 2026-10-02)".
    std::string describe() const;
};

/**
 * @brief Every contract of one or more roots, by identity, with the lookups the rule makes.
 */
class ContractBook {
public:
    /// Groups the rows. An error only for a row whose raw symbol or expiration cannot make a key
    /// (ContractKey::make), or for two rows of one root, date and instrument_id.
    static Result<ContractBook> build(const std::vector<ContractBar>& rows);

    const std::vector<Contract>& contracts() const { return contracts_; }

    /// The contracts of `root` with a bar on `day`.
    std::vector<const Contract*> printing_on(const std::string& root, Day day) const;

    /**
     * @brief The contract the vendor's id names on `day` (class d: by id and date, never by price).
     *
     * The contract of `root` whose row on `day` carries the id; else the one contract of `root`
     * that carries the id on its rows and whose first and last bars span `day` (the vendor names a
     * contract on a date the table has no row for it: R0's 16 Sunday no-row days); else nullptr.
     */
    const Contract* named_by_id(const std::string& root, const std::string& instrument_id,
                                Day day) const;

    /// The contract of `root` with this identity, or nullptr.
    const Contract* find(const ContractKey& key) const;

    /// The contract's key, or an error naming the contract when its expiration is NULL.
    static Result<ContractKey> require(const Contract& contract);

    /// Whether any contract of `root` has a bar in [from, to].
    bool has_bars(const std::string& root, Day from, Day to) const;

private:
    std::vector<Contract> contracts_;
    // (root, date) -> indexes of the contracts printing that day
    std::map<std::pair<std::string, Day>, std::vector<size_t>> by_day_;
    // (root, instrument_id) -> indexes of the contracts carrying the id
    std::map<std::pair<std::string, std::string>, std::vector<size_t>> by_id_;
};

}  // namespace trade_ngin::contract_rolls
