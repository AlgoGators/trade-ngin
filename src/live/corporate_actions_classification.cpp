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

// Which ticker a class-3 row is keyed on (E2-F26). Separate axis from the
// effect class: every key here also appears in label_table() as TERMINATION,
// and vendor_labels_for_termination_keying() asserts the partition by
// construction below.
const std::unordered_map<std::string, TerminationKeying>& keying_table() {
    static const std::unordered_map<std::string, TerminationKeying> table = {
        // The row's own ticker is the one that stops trading.
        {"acquisitionby",         TerminationKeying::ROW_TICKER_TERMINATES},
        {"mergerto",              TerminationKeying::ROW_TICKER_TERMINATES},
        {"delisted",              TerminationKeying::ROW_TICKER_TERMINATES},
        {"voluntarydelisting",    TerminationKeying::ROW_TICKER_TERMINATES},
        {"regulatorydelisting",   TerminationKeying::ROW_TICKER_TERMINATES},
        {"bankruptcyliquidation", TerminationKeying::ROW_TICKER_TERMINATES},
        // The row's own ticker SURVIVES; contraticker is what terminated.
        {"acquisitionof",         TerminationKeying::COUNTERPARTY_ROW},
        {"mergerfrom",            TerminationKeying::COUNTERPARTY_ROW},
        {"spunofffrom",           TerminationKeying::COUNTERPARTY_ROW},
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

TerminationKeying termination_keying(const std::string& vendor_label) {
    const auto& table = keying_table();
    auto it = table.find(vendor_label);
    // A label with no entry is not a counterparty row. Callers must have
    // established that it is class 3 first; this default keeps a hypothetical
    // new TERMINATION label behaving as it does today rather than silently
    // becoming un-actionable.
    return it == table.end() ? TerminationKeying::ROW_TICKER_TERMINATES : it->second;
}

const std::vector<std::string>& vendor_labels_for_termination_keying(TerminationKeying k) {
    // Built from keying_table() the same way the class lists are built from
    // label_table(), so the SQL filter and the per-row predicate cannot drift.
    static const std::unordered_map<TerminationKeying, std::vector<std::string>> by_keying = [] {
        std::unordered_map<TerminationKeying, std::vector<std::string>> m;
        // Seed both keys so an empty side returns an empty list rather than
        // falling through to the shared `empty` and hiding a lost label.
        m[TerminationKeying::ROW_TICKER_TERMINATES];
        m[TerminationKeying::COUNTERPARTY_ROW];
        for (const auto& [label, keying] : keying_table()) {
            m[keying].push_back(label);
        }
        for (auto& [keying, labels] : m) {
            std::sort(labels.begin(), labels.end());  // deterministic SQL IN-lists
        }
        return m;
    }();

    static const std::vector<std::string> empty;
    auto it = by_keying.find(k);
    return it == by_keying.end() ? empty : it->second;
}

}  // namespace trade_ngin
