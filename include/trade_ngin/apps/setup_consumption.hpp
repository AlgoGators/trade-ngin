#pragma once

#include <optional>
#include <string>
#include <vector>

#include "trade_ngin/core/error.hpp"

namespace trade_ngin {

// Per-call, caller-owned evidence of reached setup reads. Names identify the
// selected entries; no strategy definition or arbitrary type text is stored.
struct SelectionRead {
    std::string name;
    bool enabled_live_read{false};
    std::optional<bool> enabled_live_present;
    std::optional<bool> enabled_live_value;
    std::optional<bool> enabled_live_defaulted;
    bool allocation_read{false};
    std::optional<bool> allocation_defaulted;
    std::optional<double> allocation_value;
    std::optional<double> effective_allocation;
};

struct SelectionConsumption {
    std::vector<SelectionRead> controlled_validation;
    std::vector<SelectionRead> ordinary_selection;
    std::optional<double> controlled_sum;
    std::optional<double> ordinary_sum;
    std::optional<bool> ordinary_normalized;
};

enum class FactoryProfile { Standard, Fast, Slow, Unsupported };
enum class SetupStage { NotReached, Attempted, Succeeded, Failed };

struct FactoryRead {
    std::string name;
    std::optional<bool> type_defaulted;
    std::optional<FactoryProfile> profile;
    std::optional<double> effective_allocation;
    std::optional<double> initial_capital_argument;
    std::optional<double> allocated_capital;
    SetupStage construction{SetupStage::NotReached};
    SetupStage initialize{SetupStage::NotReached};
    std::optional<ErrorCode> initialize_error;
    SetupStage start{SetupStage::NotReached};
    std::optional<ErrorCode> start_error;
};

struct FactoryConsumption {
    std::vector<FactoryRead> entries;
};

}  // namespace trade_ngin
