// The account's notional and posted margin, from the sleeves' books (both futures runners).
//
// A sleeve sizes its contracts on its own capital slice, so its rows are contracts of the
// account's book, and the runners sum the sleeves' rows per symbol (|q| x per-contract figure).
// On a symbol the sleeves hold on the same side that sum is the account's: |a| + |b| = |a + b|.
// On a symbol they hold on OPPOSITE sides it is not: TF +1 / FAST -1 posts margin on two
// contracts the broker never holds (stored BASE rows: ZT.v.0 2025-11-08/09, MBT.v.0 2025-01-29).
// HD 2026-09-19 and 2026-09-25 item 23 (T-7b-2 8b): opposite sleeves are kept gross per sleeve
// for attribution while the ACCOUNT holds the net (no dollars tied up at the broker). So an
// opposed symbol's notional and margin are taken once, on the sleeves' net; every other symbol
// keeps the per-sleeve accumulation in the same order, so a book with no opposed symbol reads
// exactly as before. active_positions keeps counting the sleeves' rows.

#pragma once

#include <cmath>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

struct BookExposure {
    double gross_notional{0.0};
    double net_notional{0.0};
    double posted_margin{0.0};       ///< initial margin
    double maintenance_margin{0.0};
    int active_positions{0};         ///< the sleeves' non-zero rows, as before
    bool failed{false};              ///< a symbol could not be priced: the caller falls back
    std::string failed_symbol;
    std::string failed_strategy;
    std::vector<std::string> net_lines;  ///< one BOOK_NET line per opposed symbol
};

/// The price a row is marked at (the runners: the T-1 close, else the row's average price).
using ExposurePriceFn = std::function<double(const std::string& symbol, const Position& row)>;
/// Signed notional of a quantity (MarginManager::calculate_position_notional).
using ExposureNotionalFn =
    std::function<Result<double>(const std::string& symbol, double quantity, double price)>;
/// (initial, maintenance) margin of a quantity (MarginManager::calculate_position_margin).
using ExposureMarginFn = std::function<Result<std::pair<double, double>>(
    const std::string& symbol, double quantity, double price)>;

inline BookExposure account_book_exposure(
    const std::unordered_map<std::string, std::unordered_map<std::string, Position>>& books,
    const ExposurePriceFn& price_of, const ExposureNotionalFn& notional_of,
    const ExposureMarginFn& margin_of) {
    constexpr double kZero = 1e-6;
    BookExposure out;

    // The symbols held long by one sleeve and short by another.
    std::map<std::string, std::pair<bool, bool>> sides;
    for (const auto& [sid, rows] : books) {
        (void)sid;
        for (const auto& [symbol, row] : rows) {
            const double q = row.quantity.as_double();
            if (std::abs(q) < kZero) continue;
            (q > 0.0 ? sides[symbol].first : sides[symbol].second) = true;
        }
    }
    std::map<std::string, double> opposed_net;
    std::map<std::string, double> opposed_price;
    std::map<std::string, std::string> opposed_legs;
    for (const auto& [symbol, s] : sides) {
        if (s.first && s.second) opposed_net[symbol] = 0.0;
    }

    // Every other symbol: the per-sleeve accumulation, in the order and arithmetic it always had.
    for (const auto& [strategy_id, rows] : books) {
        for (const auto& [symbol, row] : rows) {
            const double qty = row.quantity.as_double();
            if (std::abs(qty) < kZero) continue;
            out.active_positions++;
            const double price = price_of(symbol, row);
            auto net_it = opposed_net.find(symbol);
            if (net_it != opposed_net.end()) {
                net_it->second += qty;
                opposed_price.emplace(symbol, price);
                std::ostringstream leg;
                leg << " " << strategy_id << "=" << qty;
                opposed_legs[symbol] += leg.str();
                continue;
            }
            auto notional = notional_of(symbol, qty, price);
            auto margin = margin_of(symbol, qty, price);
            if (notional.is_error() || margin.is_error()) {
                out.failed = true;
                out.failed_symbol = symbol;
                out.failed_strategy = strategy_id;
                return out;
            }
            out.gross_notional += std::abs(notional.value());
            out.net_notional += notional.value();
            out.posted_margin += margin.value().first;
            out.maintenance_margin += margin.value().second;
        }
    }

    // Opposed symbols: once, on the account's net.
    for (const auto& [symbol, net] : opposed_net) {
        const double price = opposed_price.at(symbol);
        std::ostringstream line;
        line << "BOOK_NET sym=" << symbol << " net=" << net << " sleeves:" << opposed_legs[symbol];
        if (std::abs(net) >= kZero) {
            auto notional = notional_of(symbol, net, price);
            auto margin = margin_of(symbol, net, price);
            if (notional.is_error() || margin.is_error()) {
                out.failed = true;
                out.failed_symbol = symbol;
                out.failed_strategy = "ACCOUNT_NET";
                return out;
            }
            out.gross_notional += std::abs(notional.value());
            out.net_notional += notional.value();
            out.posted_margin += margin.value().first;
            out.maintenance_margin += margin.value().second;
            line << " notional=" << notional.value() << " margin=" << margin.value().first;
        } else {
            line << " notional=0 margin=0";
        }
        line << ": opposite sleeves are kept gross per sleeve; the account posts margin on the net";
        out.net_lines.push_back(line.str());
    }
    return out;
}

}  // namespace trade_ngin
