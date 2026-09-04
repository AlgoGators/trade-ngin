#include "trade_ngin/live/corporate_actions_classification.hpp"

#include <algorithm>
#include <unordered_map>

namespace trade_ngin {
namespace {

// The complete vendor label -> effect class table. All 19 labels present in
// equities_data.corporate_action are covered; anything else is UNRECOGNIZED.
const std::unordered_map<std::string, CorpActionClass>& label_table() {
    static const std::unordered_map<std::string, CorpActionClass> table = {
        // Class 1 -- price-restating
        {"split", CorpActionClass::PRICE_RESTATING},
        {"adrratiosplit", CorpActionClass::PRICE_RESTATING},
        {"spinoff", CorpActionClass::PRICE_RESTATING},
        {"spinoffdividend", CorpActionClass::PRICE_RESTATING},
        {"dividend", CorpActionClass::PRICE_RESTATING},
        // Class 2 -- series continuity
        {"tickerchangefrom", CorpActionClass::SERIES_CONTINUITY},
        {"tickerchangeto", CorpActionClass::SERIES_CONTINUITY},
        // Class 3 -- termination / holding transformation
        {"mergerfrom", CorpActionClass::TERMINATION},
        {"mergerto", CorpActionClass::TERMINATION},
        {"acquisitionby", CorpActionClass::TERMINATION},
        {"acquisitionof", CorpActionClass::TERMINATION},
        {"delisted", CorpActionClass::TERMINATION},
        {"voluntarydelisting", CorpActionClass::TERMINATION},
        {"regulatorydelisting", CorpActionClass::TERMINATION},
        {"bankruptcyliquidation", CorpActionClass::TERMINATION},
        {"spunofffrom", CorpActionClass::TERMINATION},
        // Class 4 -- informational
        {"listed", CorpActionClass::INFORMATIONAL},
        {"relation", CorpActionClass::INFORMATIONAL},
        {"initiated", CorpActionClass::INFORMATIONAL},
    };
    return table;
}

}  // namespace

CorpActionClass classify_action(const std::string& vendor_label) {
    const auto& table = label_table();
    auto it = table.find(vendor_label);
    return it == table.end() ? CorpActionClass::UNRECOGNIZED : it->second;
}

const char* corp_action_class_to_string(CorpActionClass c) {
    switch (c) {
        case CorpActionClass::PRICE_RESTATING:   return "PRICE_RESTATING";
        case CorpActionClass::SERIES_CONTINUITY: return "SERIES_CONTINUITY";
        case CorpActionClass::TERMINATION:       return "TERMINATION";
        case CorpActionClass::INFORMATIONAL:     return "INFORMATIONAL";
        case CorpActionClass::UNRECOGNIZED:      return "UNRECOGNIZED";
    }
    return "UNRECOGNIZED";
}

const std::vector<std::string>& vendor_labels_for_class(CorpActionClass c) {
    // Built once from the same table that classify_action() consults, so the
    // two can never drift.
    static const std::unordered_map<CorpActionClass, std::vector<std::string>> by_class = [] {
        std::unordered_map<CorpActionClass, std::vector<std::string>> m;
        for (const auto& [label, cls] : label_table()) {
            m[cls].push_back(label);
        }
        for (auto& [cls, labels] : m) {
            std::sort(labels.begin(), labels.end());  // deterministic SQL IN-lists
        }
        return m;
    }();

    static const std::vector<std::string> empty;
    auto it = by_class.find(c);
    return it == by_class.end() ? empty : it->second;
}

}  // namespace trade_ngin
