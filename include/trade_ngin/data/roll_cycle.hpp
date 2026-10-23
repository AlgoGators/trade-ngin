// include/trade_ngin/data/roll_cycle.hpp
#pragma once

#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/contract_bars.hpp"

namespace trade_ngin::contract_rolls {

/**
 * @brief The roll cycle per root: the delivery months the roll rule holds (HD 2026-10-23, ruling 2).
 *
 * The months the vendor's own continuous series has held in at least half of the years it covers,
 * measured from futures_data.ohlcv_1d_raw over the whole history (2010-06 to 2026-10-02, measured
 * 2026-10-23 on the 10-21 production copy, report R2): "held" is named by the vendor on two
 * consecutive grid bars (today's confirmation rule, so a one-bar stray does not count), and the
 * years are the delivery years of the contracts held. The table is FIXED here, so the history does
 * not move as data grows; test_roll_cycle_db recomputes it from the data and must match. The roll
 * rule's target and the email's rollover warning (step 4) both read this table.
 *
 * Months are the month codes F G H J K M N Q U V X Z. ES, NQ, YM and RTY (the micros' contracts
 * before the vendor's 2026-02-22 change) are not loaded yet; their cycle is the micros' own.
 */
inline const std::map<std::string, std::string>& roll_cycle_table() {
    static const std::map<std::string, std::string> table = {
        // Currencies: quarterly; 6L every month.
        {"6A", "HMUZ"}, {"6B", "HMUZ"}, {"6C", "HMUZ"}, {"6E", "HMUZ"}, {"6J", "HMUZ"},
        {"6M", "HMUZ"}, {"6N", "HMUZ"}, {"6S", "HMUZ"}, {"6L", "FGHJKMNQUVXZ"},
        // Energies: every month.
        {"CL", "FGHJKMNQUVXZ"}, {"HO", "FGHJKMNQUVXZ"}, {"NG", "FGHJKMNQUVXZ"},
        {"RB", "FGHJKMNQUVXZ"},
        // Metals: GC Feb Apr Jun Aug Dec (never Oct); SI, HG Mar May Jul Sep Dec; PL Jan Apr Jul Oct.
        {"GC", "GJMQZ"}, {"SI", "HKNUZ"}, {"HG", "HKNUZ"}, {"PL", "FJNV"},
        // Grains: ZC Mar May Jul Dec (Sep held 4 of 17 years); ZS Jan Mar May Jul Nov.
        {"ZC", "HKNZ"}, {"ZS", "FHKNX"}, {"ZW", "HKNUZ"}, {"KE", "HKNUZ"}, {"ZL", "FHKNZ"},
        {"ZM", "FHKNZ"}, {"ZR", "FHKNUX"},
        // Livestock.
        {"LE", "GJMQVZ"}, {"HE", "GJMNQVZ"}, {"GF", "FHJKQUVX"},
        // Treasuries: quarterly.
        {"ZT", "HMUZ"}, {"ZF", "HMUZ"}, {"ZN", "HMUZ"}, {"UB", "HMUZ"},
        // Equity index (micros, and their full-size contracts before 2026-02-22): quarterly.
        {"MES", "HMUZ"}, {"MNQ", "HMUZ"}, {"MYM", "HMUZ"}, {"M2K", "HMUZ"},
        {"ES", "HMUZ"}, {"NQ", "HMUZ"}, {"YM", "HMUZ"}, {"RTY", "HMUZ"},
        // Bitcoin: every month.
        {"MBT", "FGHJKMNQUVXZ"},
    };
    return table;
}

/// The root's cycle (month codes), or an error for a root the table does not have.
Result<std::string> roll_cycle(const std::string& root);

/// Whether `month` (1..12) is in `cycle` (month codes).
bool in_cycle(const std::string& cycle, unsigned month);

/// The first delivery month of `cycle` strictly after (year, month), as year * 12 + month - 1.
int next_cycle_delivery(const std::string& cycle, int year, unsigned month);

/// What the measurement found for one root.
struct CycleMeasure {
    std::string root;
    int years{0};                    ///< delivery years of the contracts the vendor held
    std::array<int, 12> held_years{};  ///< per month (index 0 = January), years held
    int unresolved_bars{0};          ///< grid bars whose vendor contract could not be named
    /// Months held in at least half the years, as month codes in calendar order.
    std::string cycle() const;
    /// Months whose share of years is in [0.35, 0.65]: near the 50% cut.
    std::string near_cut() const;
};

/**
 * @brief Measures one root's cycle from the vendor's grid (with its raw ids) and the contracts.
 *
 * `root_on(day)` is the root whose contracts the vendor's id names on that day (the micros' full-size
 * root before 2026-02-22). A bar is held when the vendor names the same contract on it and on the
 * next grid bar. A contract with no expiration is refused (the year needs it).
 */
Result<CycleMeasure> measure_cycle(const std::string& root, const std::vector<VendorBar>& grid,
                                   const ContractBook& book,
                                   const std::function<std::string(Day)>& root_on);

}  // namespace trade_ngin::contract_rolls
