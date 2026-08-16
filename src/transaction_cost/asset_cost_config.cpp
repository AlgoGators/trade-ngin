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
    // Very liquid, typically 1 tick spread during regular hours
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
    // CME: tick $0.10/oz, 100 oz → tick_value = 0.10 * 100 = $10. Price feed in $/oz.
    // Very liquid, typically 1 tick spread during regular hours
    {
        AssetCostConfig config;
        config.symbol = "GC";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 50.0;
        config.tick_size = 0.10;   // $0.10 per oz
        config.point_value = 100.0;  // $ per 1 $/oz (100 oz)
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
    // Price feed in cents (e.g. 435.25). 1 tick = 0.25 cents; $50 per 1 cent on 5000 bu → tick_value = 0.25 * 50 = $12.50
    {
        AssetCostConfig config;
        config.symbol = "ZC";
        config.baseline_spread_ticks = 2.0;  // match observed avg
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.25;    // cents
        config.point_value = 50.0;  // $ per 1 cent
        configs_[config.symbol] = config;
    }

    // Soybeans (ZS)
    // Price feed in cents. 1 tick = 0.25 cents; $50 per 1 cent on 5000 bu → tick_value = 0.25 * 50 = $12.50
    {
        AssetCostConfig config;
        config.symbol = "ZS";
        config.baseline_spread_ticks = 2.0;  // match observed avg
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.25;    // cents
        config.point_value = 50.0;  // $ per 1 cent
        configs_[config.symbol] = config;
    }

    // Wheat (ZW)
    // Price feed in cents. 1 tick = 0.25 cents; $50 per 1 cent on 5000 bu → tick_value = 0.25 * 50 = $12.50
    {
        AssetCostConfig config;
        config.symbol = "ZW";
        config.baseline_spread_ticks = 2.0;  // match observed avg
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.25;    // cents
        config.point_value = 50.0;  // $ per 1 cent
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
    // Price feed in cents/lb (e.g. 364.45). 1 tick = 0.025 cents/lb; $500 per 1 cent on 50k lbs → tick_value = 0.025 * 500 = $12.50
    {
        AssetCostConfig config;
        config.symbol = "GF";
        config.baseline_spread_ticks = 2.0;  // observed table has 2
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 80.0;
        config.tick_size = 0.025;   // cents/lb (0.00025 dollars)
        config.point_value = 500.0;  // $ per 1 cent/lb
        configs_[config.symbol] = config;
    }

    // Lean Hogs (HE)
    // Price feed in cents/lb (e.g. 98.575). 1 tick = 0.025 cents/lb; $400 per 1 cent on 40k lbs → tick_value = 0.025 * 400 = $10
    {
        AssetCostConfig config;
        config.symbol = "HE";
        config.baseline_spread_ticks = 2.0;  // observed table has 2
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 80.0;
        config.tick_size = 0.025;   // cents/lb (0.00025 dollars)
        config.point_value = 400.0;  // $ per 1 cent/lb
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
    // Price feed in cents. 1 tick = 0.25 cents; $50 per 1 cent on 5000 bu → tick_value = 0.25 * 50 = $12.50
    {
        AssetCostConfig config;
        config.symbol = "KE";
        config.baseline_spread_ticks = 2.0;  // match observed avg
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.25;    // cents
        config.point_value = 50.0;  // $ per 1 cent
        configs_[config.symbol] = config;
    }

    // Live Cattle (LE)
    // Price feed in cents/lb (e.g. 235.65). 1 tick = 0.025 cents/lb; $400 per 1 cent on 40k lbs → tick_value = 0.025 * 400 = $10
    {
        AssetCostConfig config;
        config.symbol = "LE";
        config.baseline_spread_ticks = 2.0;  // observed table has 2
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 80.0;
        config.tick_size = 0.025;   // cents/lb (0.00025 dollars)
        config.point_value = 400.0;  // $ per 1 cent/lb
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

    // Micro Bitcoin (MBT)
    {
        AssetCostConfig config;
        config.symbol = "MBT";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 10.0;
        config.max_impact_bps = 80.0;
        config.tick_size = 5.0;
        config.point_value = 0.10;
        configs_[config.symbol] = config;
    }

    // Micro E-mini S&P 500 (MES)
    {
        AssetCostConfig config;
        config.symbol = "MES";
        config.baseline_spread_ticks = 1.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 50.0;
        config.tick_size = 0.25;
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
    // Price feed in cents/lb (e.g. 55.63). 1 tick = 0.01 cents/lb; $600 per 1 cent on 60k lbs → tick_value = 0.01 * 600 = $6
    {
        AssetCostConfig config;
        config.symbol = "ZL";
        config.baseline_spread_ticks = 2.0;
        config.min_spread_ticks = 1.0;
        config.max_spread_ticks = 5.0;
        config.max_impact_bps = 60.0;
        config.tick_size = 0.01;     // cents/lb
        config.point_value = 600.0;  // $ per 1 cent/lb
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
