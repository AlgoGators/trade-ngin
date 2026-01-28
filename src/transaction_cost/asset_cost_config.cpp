#include "trade_ngin/transaction_cost/asset_cost_config.hpp"

namespace trade_ngin {
namespace transaction_cost {

AssetCostConfigRegistry::AssetCostConfigRegistry() {
    initialize_default_configs();
}

void AssetCostConfigRegistry::initialize_default_configs() {
    // E-mini S&P 500 (ES)
    // Very liquid, typically 1 tick spread
    {
        AssetCostConfig config;
        config.symbol = "ES";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 50.0;
        config.tick_size = 0.25;
        config.point_value = 50.0;
        configs_[config.symbol] = config;
    }

    // Crude Oil (CL)
    // Liquid, typically 1-2 tick spread
    {
        AssetCostConfig config;
        config.symbol = "CL";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 50.0;
        config.tick_size = 0.01;
        config.point_value = 1000.0;
        configs_[config.symbol] = config;
    }

    // Gold (GC)
    // CME: tick_size = 0.01 ($0.10/oz), contract_size = 100 troy oz
    // Liquid, typically 1 tick spread
    {
        AssetCostConfig config;
        config.symbol = "GC";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 50.0;
        config.tick_size = 0.01;  // Fixed: was 0.10
        config.point_value = 100.0;
        configs_[config.symbol] = config;
    }

    // E-mini Nasdaq 100 (NQ)
    // Very liquid, typically 1 tick spread
    {
        AssetCostConfig config;
        config.symbol = "NQ";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 50.0;
        config.tick_size = 0.25;
        config.point_value = 20.0;
        configs_[config.symbol] = config;
    }

    // E-mini Russell 2000 (RTY)
    // Liquid, typically 1 tick spread
    {
        AssetCostConfig config;
        config.symbol = "RTY";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.10;
        config.point_value = 50.0;
        configs_[config.symbol] = config;
    }

    // 10-Year Treasury Note (ZN)
    // Very liquid, typically 1 tick spread
    {
        AssetCostConfig config;
        config.symbol = "ZN";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 3.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 30.0;
        config.tick_size = 0.015625;  // 1/64
        config.point_value = 1000.0;
        configs_[config.symbol] = config;
    }

    // 30-Year Treasury Bond (ZB)
    // Liquid, typically 1 tick spread
    {
        AssetCostConfig config;
        config.symbol = "ZB";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 3.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 30.0;
        config.tick_size = 0.03125;  // 1/32
        config.point_value = 1000.0;
        configs_[config.symbol] = config;
    }

    // Euro FX (6E)
    // Very liquid, typically 1 tick spread
    {
        AssetCostConfig config;
        config.symbol = "6E";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 40.0;
        config.tick_size = 0.00005;
        config.point_value = 125000.0;
        configs_[config.symbol] = config;
    }

    // Natural Gas (NG)
    // Less liquid, wider spreads
    {
        AssetCostConfig config;
        config.symbol = "NG";
        config.baseline_spread_ticks = 2.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 10.0;
        config.max_impact_bps = 80.0;
        config.tick_size = 0.001;
        config.point_value = 10000.0;
        configs_[config.symbol] = config;
    }

    // Silver (SI)
    // Moderately liquid
    {
        AssetCostConfig config;
        config.symbol = "SI";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.005;
        config.point_value = 5000.0;
        configs_[config.symbol] = config;
    }

    // Corn (ZC)
    // CME: tick_size = 0.0025 (1/4 cent/bushel), contract_size = 5,000 bushels
    // tick_value = 0.0025 * 5000 = $12.50
    {
        AssetCostConfig config;
        config.symbol = "ZC";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.0025;  // Fixed: was 0.25
        config.point_value = 5000.0;  // Fixed: was 50
        configs_[config.symbol] = config;
    }

    // Soybeans (ZS)
    // CME: tick_size = 0.0025 (1/4 cent/bushel), contract_size = 5,000 bushels
    // tick_value = 0.0025 * 5000 = $12.50
    {
        AssetCostConfig config;
        config.symbol = "ZS";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.0025;  // Fixed: was 0.25
        config.point_value = 5000.0;  // Fixed: was 50
        configs_[config.symbol] = config;
    }

    // Wheat (ZW)
    // CME: tick_size = 0.0025 (1/4 cent/bushel), contract_size = 5,000 bushels
    // tick_value = 0.0025 * 5000 = $12.50
    {
        AssetCostConfig config;
        config.symbol = "ZW";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.0025;  // Fixed: was 0.25
        config.point_value = 5000.0;  // Fixed: was 50
        configs_[config.symbol] = config;
    }

    // Australian Dollar (6A)
    {
        AssetCostConfig config;
        config.symbol = "6A";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 40.0;
        config.tick_size = 0.0001;
        config.point_value = 100000.0;
        configs_[config.symbol] = config;
    }

    // British Pound (6B)
    {
        AssetCostConfig config;
        config.symbol = "6B";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 40.0;
        config.tick_size = 0.0001;
        config.point_value = 62500.0;
        configs_[config.symbol] = config;
    }

    // Canadian Dollar (6C)
    {
        AssetCostConfig config;
        config.symbol = "6C";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 40.0;
        config.tick_size = 0.00005;
        config.point_value = 100000.0;
        configs_[config.symbol] = config;
    }

    // Japanese Yen (6J)
    {
        AssetCostConfig config;
        config.symbol = "6J";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 40.0;
        config.tick_size = 0.0000005;
        config.point_value = 12500000.0;
        configs_[config.symbol] = config;
    }

    // Brazilian Real (6L)
    // CME: tick_size = 0.0001 (0.01 cents/BRL), contract_size = 100,000 BRL
    {
        AssetCostConfig config;
        config.symbol = "6L";
        config.baseline_spread_ticks = 2.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 10.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 80.0;
        config.tick_size = 0.0001;  // Fixed: was 0.00005
        config.point_value = 100000.0;
        configs_[config.symbol] = config;
    }

    // Mexican Peso (6M)
    {
        AssetCostConfig config;
        config.symbol = "6M";
        config.baseline_spread_ticks = 2.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 10.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 80.0;
        config.tick_size = 0.00001;
        config.point_value = 500000.0;
        configs_[config.symbol] = config;
    }

    // New Zealand Dollar (6N)
    // CME: tick_size = 0.00005 (0.005 cents/NZD), contract_size = 100,000 NZD
    {
        AssetCostConfig config;
        config.symbol = "6N";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 50.0;
        config.tick_size = 0.00005;  // Fixed: was 0.0001
        config.point_value = 100000.0;
        configs_[config.symbol] = config;
    }

    // Swiss Franc (6S)
    // CME: tick_size = 0.00005 (0.005 cents/CHF), contract_size = 125,000 CHF
    {
        AssetCostConfig config;
        config.symbol = "6S";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 40.0;
        config.tick_size = 0.00005;  // Fixed: was 0.0001
        config.point_value = 125000.0;
        configs_[config.symbol] = config;
    }

    // Feeder Cattle (GF)
    // CME: tick_size = 0.00025 (2.5 cents/cwt = $0.00025/lb), contract_size = 50,000 lbs
    // tick_value = 0.00025 * 50000 = $12.50
    {
        AssetCostConfig config;
        config.symbol = "GF";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 80.0;
        config.tick_size = 0.00025;  // Fixed: was 0.025
        config.point_value = 50000.0;  // Fixed: was 500
        configs_[config.symbol] = config;
    }

    // Lean Hogs (HE)
    // CME: tick_size = 0.00025 (2.5 cents/cwt = $0.00025/lb), contract_size = 40,000 lbs
    // tick_value = 0.00025 * 40000 = $10.00
    {
        AssetCostConfig config;
        config.symbol = "HE";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 80.0;
        config.tick_size = 0.00025;  // Fixed: was 0.025
        config.point_value = 40000.0;  // Fixed: was 400
        configs_[config.symbol] = config;
    }

    // Copper (HG)
    {
        AssetCostConfig config;
        config.symbol = "HG";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.0005;
        config.point_value = 25000.0;
        configs_[config.symbol] = config;
    }

    // Heating Oil (HO)
    // CME: tick_size = 0.0001 ($0.0001/gallon), contract_size = 42,000 gallons
    // tick_value = 0.0001 * 42000 = $4.20
    {
        AssetCostConfig config;
        config.symbol = "HO";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.0001;  // Verified: tick_value = $4.20
        config.point_value = 42000.0;
        configs_[config.symbol] = config;
    }

    // KC Wheat (KE)
    // CME: tick_size = 0.0025 (1/4 cent/bushel), contract_size = 5,000 bushels
    // tick_value = 0.0025 * 5000 = $12.50
    {
        AssetCostConfig config;
        config.symbol = "KE";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.0025;  // Fixed: was 0.25
        config.point_value = 5000.0;  // Fixed: was 50
        configs_[config.symbol] = config;
    }

    // Live Cattle (LE)
    // CME: tick_size = 0.00025 (2.5 cents/cwt = $0.00025/lb), contract_size = 40,000 lbs
    // tick_value = 0.00025 * 40000 = $10.00
    {
        AssetCostConfig config;
        config.symbol = "LE";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 80.0;
        config.tick_size = 0.00025;  // Fixed: was 0.025
        config.point_value = 40000.0;  // Fixed: was 400
        configs_[config.symbol] = config;
    }

    // Micro E-mini Russell 2000 (M2K)
    {
        AssetCostConfig config;
        config.symbol = "M2K";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.10;
        config.point_value = 5.0;
        configs_[config.symbol] = config;
    }

    // Micro E-mini Nasdaq-100 (MNQ)
    {
        AssetCostConfig config;
        config.symbol = "MNQ";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 50.0;
        config.tick_size = 0.25;
        config.point_value = 2.0;
        configs_[config.symbol] = config;
    }

    // Micro E-mini Dow (MYM)
    {
        AssetCostConfig config;
        config.symbol = "MYM";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 50.0;
        config.tick_size = 1.0;
        config.point_value = 0.50;
        configs_[config.symbol] = config;
    }

    // Platinum (PL)
    {
        AssetCostConfig config;
        config.symbol = "PL";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 70.0;
        config.tick_size = 0.10;
        config.point_value = 50.0;
        configs_[config.symbol] = config;
    }

    // RBOB Gasoline (RB)
    {
        AssetCostConfig config;
        config.symbol = "RB";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.0001;
        config.point_value = 42000.0;
        configs_[config.symbol] = config;
    }

    // Ultra Treasury Bond (UB)
    {
        AssetCostConfig config;
        config.symbol = "UB";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 3.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 30.0;
        config.tick_size = 0.03125;
        config.point_value = 1000.0;
        configs_[config.symbol] = config;
    }

    // E-mini Dow (YM)
    {
        AssetCostConfig config;
        config.symbol = "YM";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 50.0;
        config.tick_size = 1.0;
        config.point_value = 5.0;
        configs_[config.symbol] = config;
    }

    // 5-Year Treasury Note (ZF)
    {
        AssetCostConfig config;
        config.symbol = "ZF";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 3.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 30.0;
        config.tick_size = 0.0078125;  // 1/128
        config.point_value = 1000.0;
        configs_[config.symbol] = config;
    }

    // Soybean Oil (ZL)
    // CME: tick_size = 0.0001 (1/100 cent/lb), contract_size = 60,000 lbs
    // tick_value = 0.0001 * 60000 = $6.00
    {
        AssetCostConfig config;
        config.symbol = "ZL";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.0001;  // Fixed: was 0.01
        config.point_value = 60000.0;  // Fixed: was 600
        configs_[config.symbol] = config;
    }

    // Soybean Meal (ZM)
    {
        AssetCostConfig config;
        config.symbol = "ZM";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.10;
        config.point_value = 100.0;
        configs_[config.symbol] = config;
    }

    // Rough Rice (ZR)
    {
        AssetCostConfig config;
        config.symbol = "ZR";
        config.baseline_spread_ticks = 2.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 10.0;
        config.max_impact_bps = 100.0;
        config.tick_size = 0.005;
        config.point_value = 2000.0;
        configs_[config.symbol] = config;
    }

    // 2-Year Treasury Note (ZT)
    {
        AssetCostConfig config;
        config.symbol = "ZT";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 3.0;
        config.spread_cost_multiplier = 0.25;  // Limit orders: lower effective spread cost
        config.max_impact_bps = 30.0;
        config.tick_size = 0.0078125;  // 1/128
        config.point_value = 2000.0;
        configs_[config.symbol] = config;
    }
}

AssetCostConfig AssetCostConfigRegistry::get_config(const std::string& symbol) const {
    // First try exact match
    auto it = configs_.find(symbol);
    if (it != configs_.end()) {
        return it->second;
    }

    // Strip continuous contract suffix (e.g., ".v.0", ".v.1") and try again
    std::string base_symbol = symbol;
    size_t dot_pos = symbol.find('.');
    if (dot_pos != std::string::npos) {
        base_symbol = symbol.substr(0, dot_pos);
        it = configs_.find(base_symbol);
        if (it != configs_.end()) {
            auto config = it->second;
            config.symbol = symbol;  // Keep original symbol for reference
            return config;
        }
    }

    // Return default config for unknown symbols
    auto default_config = get_default_config();
    default_config.symbol = symbol;
    return default_config;
}

void AssetCostConfigRegistry::register_config(const AssetCostConfig& config) {
    configs_[config.symbol] = config;
}

bool AssetCostConfigRegistry::has_config(const std::string& symbol) const {
    return configs_.find(symbol) != configs_.end();
}

AssetCostConfig AssetCostConfigRegistry::get_default_config() {
    // Conservative defaults for unknown instruments
    AssetCostConfig config;
    config.symbol = "UNKNOWN";
    config.baseline_spread_ticks = 2.0;
    config.min_spread_ticks = 1.0;
    config.max_spread_ticks = 10.0;
    config.max_impact_bps = 100.0;
    config.tick_size = 0.01;
    config.point_value = 100.0;
    config.max_total_implicit_bps = 200.0;
    return config;
}

}  // namespace transaction_cost
}  // namespace trade_ngin
