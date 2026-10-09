// include/trade_ngin/portfolio/desk_book.hpp
//
// QT plan E5 (rulings 6 to 8; plan section 3b, Q2): the desk's book as the second caller of the
// one pass. The desk edits a portfolio TOTAL per symbol (the qt_proposal rows summed over the
// sleeves); the pass runs on those totals and the stored quantity is split back to the sleeves.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "trade_ngin/core/error.hpp"

namespace trade_ngin {

/// sleeve -> symbol -> quantity
using SleeveQuantities = std::map<std::string, std::map<std::string, double>>;
/// symbol -> sleeve -> weight
using DeskSplitWeights = std::map<std::string, std::map<std::string, double>>;

struct DeskBook {
    /// The desk's quantity per symbol (qt_proposal summed over the sleeves). A symbol the pass
    /// sees and the desk does not name is asked 0.
    std::map<std::string, double> totals;
    /// Per symbol, the sleeves' weights for the split back (resolve_desk_split).
    DeskSplitWeights weights;
    /// An approved override: the stored book is `totals` exactly; the pass still runs and is kept
    /// as a report (master document, day step 3).
    bool exact = false;
};

/// What the desk asked, what the engine gave back, and the step that moved it (ruling 10).
struct DeskSymbolOutcome {
    std::string symbol;
    double asked{0.0};
    double given{0.0};      ///< the stored quantity (on an override: the asked one)
    double pass_given{0.0}; ///< what the pass alone would have stored
    std::string moved_by;   ///< one_pass::attribute's name ("hold" for a symbol not in the pass)
};

/**
 * Plan section 3b, Q2: per symbol, the sleeves the desk's total is split over, in this order:
 *   1. the model's (system) sleeve quantities for the symbol that day;
 *   2. else yesterday's book (qt) sleeve quantities;
 *   3. else the first sleeve, alphabetically, whose universe contains the symbol (weight 1);
 *   4. else the run is refused, naming the symbol.
 * Weights are the absolute quantities. A symbol asked 0 with none of 1 to 3 needs no split and
 * gets no weights. Pure.
 */
Result<DeskSplitWeights> resolve_desk_split(const std::map<std::string, double>& totals,
                                            const SleeveQuantities& system_by_sleeve,
                                            const SleeveQuantities& previous_by_sleeve,
                                            const std::map<std::string, std::set<std::string>>&
                                                universe_by_sleeve);

/// The stored quantity of one symbol split over `sleeves` (in that order) by `weights`, largest
/// remainder, summing to round(quantity). A sleeve without a weight gets 0.
std::vector<double> split_desk_quantity(double quantity, const std::vector<std::string>& sleeves,
                                        const std::map<std::string, double>& weights);

}  // namespace trade_ngin
