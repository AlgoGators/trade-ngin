// src/relative_value/contract_lookup.cpp
#include "trade_ngin/relative_value/contract_lookup.hpp"

#include <map>

#include "trade_ngin/data/conversion_utils.hpp"

namespace trade_ngin::relative_value {

std::string contract_root(const std::string& symbol) {
    const auto cut = symbol.find(".v.");
    return cut == std::string::npos ? symbol : symbol.substr(0, cut);
}

ValidationContext::ContractLookup make_contract_lookup(const std::shared_ptr<arrow::Table>& metadata) {
    auto sizes = std::make_shared<std::map<std::string, std::string>>();
    if (metadata) {
        auto symbols = metadata->GetColumnByName("Databento Symbol");
        auto size_col = metadata->GetColumnByName("Contract Size");
        if (symbols) {
            for (int64_t row = 0; row < metadata->num_rows(); ++row) {
                auto sym = DataConversionUtils::safe_get_string(symbols, row, "Databento Symbol");
                if (sym.is_error() || sym.value().empty()) continue;
                std::string size;
                if (size_col) {
                    // a null or unreadable size is "no size", not an error: the validator reports it
                    auto s = DataConversionUtils::safe_get_string(size_col, row, "Contract Size");
                    if (s.is_ok()) size = s.value();
                }
                (*sizes)[sym.value()] = size;
            }
        }
    }
    return [sizes](const std::string& symbol) -> std::optional<ContractInfo> {
        auto it = sizes->find(contract_root(symbol));
        if (it == sizes->end()) return std::nullopt;
        return ContractInfo{it->second};
    };
}

Result<ValidationContext::ContractLookup> make_contract_lookup(const PostgresDatabase& db) {
    auto table = db.get_contract_metadata();
    if (table.is_error()) {
        return make_error<ValidationContext::ContractLookup>(
            table.error()->code(),
            std::string("could not read the contract metadata: ") + table.error()->what(),
            "ContractLookup");
    }
    return Result<ValidationContext::ContractLookup>(make_contract_lookup(table.value()));
}

}  // namespace trade_ngin::relative_value
