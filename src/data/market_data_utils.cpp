// src/data/market_data_utils.cpp

#include "trade_ngin/data/market_data_utils.hpp"

namespace trade_ngin {
namespace market_data_utils {

std::string get_market_data_columns(AssetClass asset_class) {
    switch (asset_class) {
        case AssetClass::EQUITIES:
            // Equities require adjusted columns to account for splits and dividends.
            // ADR-000 C-2 contract names: adj_open, adj_high, adj_low, adjusted_close, adj_volume.
            // Aliased to plain names so downstream consumers (Arrow table schema, Bar conversion)
            // remain unchanged. This is the honest representation of equity backtest prices.
            return "time, symbol, adj_open AS open, adj_high AS high, adj_low AS low, "
                   "adjusted_close AS close, adj_volume AS volume";

        case AssetClass::FUTURES:
        case AssetClass::FIXED_INCOME:
        case AssetClass::CURRENCIES:
        case AssetClass::COMMODITIES:
        case AssetClass::CRYPTO:
            // These asset classes use unadjusted prices directly.
            // Futures are already back-adjusted per contract conventions.
            // Other asset classes do not have adjustment concepts (splits, dividends).
            return "time, symbol, open, high, low, close, volume";

        default:
            // Defensive: unknown asset classes default to unadjusted.
            return "time, symbol, open, high, low, close, volume";
    }
}

}  // namespace market_data_utils
}  // namespace trade_ngin
