// include/trade_ngin/portfolio/cut_delivery.hpp
//
// The risk gate's cut, delivered in whole contracts (T-7b-2 9e, ledger LOOP-cut-lands-by-refusal).
//
// On a lap where the gate scales the book by f < 1, the scaled book is fractional. Before 9e the
// next lap re-optimised that fractional target and its deadband, measured against the held book,
// refused most of the trade back: the cut reached the stored book only through that refusal (T-VOL
// arm A), and forced rounding restored the pre-cut integers when the loop ran out of laps. Here the
// cut is applied to the lap's book directly, in whole contracts, and the result is the next lap's
// input:
//
//   start from the lap's book (rounded), and while its gross notional is above f x the lap book's
//   gross notional, remove one contract (toward zero). The contracts the day's request added
//   (beyond the held book on the same side, or the whole position when the held book is flat or on
//   the other side) go first; held contracts only when no added one is left. Within a class the
//   contract whose removal leaves the book nearest the gate's target f x (the lap's book) in the
//   optimizer's own tracking error (the lap's covariance, weight = contracts x notional / capital)
//   goes first; a symbol outside the covariance is not ranked, and when no candidate of the class
//   is in it the best fit on notional is taken (the smallest contract that covers the remaining
//   excess, else the largest).
//
// The book it returns is at or below the gate's target by construction, so neither the next lap's
// deadband nor forced rounding can undo the cut; the next lap still re-optimises it from the held
// anchor (the greedy's cost anchor and the deadband's anchor stay the held book).
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace trade_ngin {

struct CutDeliveryInput {
    std::map<std::string, double> lap_book;  ///< account contracts the lap's optimizer produced
    std::map<std::string, double> held;      ///< the held account book (rounded to whole contracts)
    /// notional of one contract by symbol; a symbol absent or <= 0 is never cut and not counted
    std::unordered_map<std::string, double> notional_per_contract;
    double factor = 1.0;   ///< the factor the gate multiplied the book by on this lap
    double capital = 0.0;  ///< the optimizer's weight denominator (notional / capital = weight)
    std::vector<std::string> covariance_symbols;  ///< the lap's covariance order; empty: none
    std::vector<std::vector<double>> covariance;
};

struct CutDelivery {
    std::map<std::string, double> book;  ///< whole contracts, every symbol of lap_book
    double lap_notional = 0.0;           ///< gross notional of the rounded lap book
    double held_notional = 0.0;          ///< gross notional of the held book on the same symbols
    double target_notional = 0.0;        ///< factor x lap_notional
    double cut_notional = 0.0;           ///< gross notional of book
    int removed_new = 0;                 ///< contracts removed that the day's request added
    int removed_held = 0;                ///< held contracts removed
    int unknown_notional = 0;            ///< non-zero positions without a notional (never cut)
    std::vector<std::pair<std::string, bool>> removed;  ///< in order: symbol, added (true) or held
};

CutDelivery deliver_cut_in_whole_contracts(const CutDeliveryInput& in);

}  // namespace trade_ngin
