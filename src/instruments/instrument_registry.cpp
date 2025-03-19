// src/instruments/instrument_registry.cpp
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/core/logger.hpp"
#include <arrow/api.h>

namespace trade_ngin {

Result<void> InstrumentRegistry::initialize(std::shared_ptr<PostgresDatabase> db) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Initialize the logger
    LoggerConfig logger_config;
    logger_config.min_level = LogLevel::DEBUG;
    logger_config.destination = LogDestination::BOTH;
    logger_config.log_directory = "logs";
    logger_config.filename_prefix = "instrument_registry";
    Logger::instance().initialize(logger_config);
    INFO("Logger initialized successfully");
    
    if (initialized_) {
        return Result<void>();
    }
    
    if (!db) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Postgres interface cannot be null",
            "InstrumentRegistry"
        );
    }
    
    db_ = std::move(db);
    initialized_ = true;
    
    INFO("InstrumentRegistry initialized successfully");
    return Result<void>();
}

std::shared_ptr<Instrument> InstrumentRegistry::get_instrument(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = instruments_.find(symbol);
    if (it != instruments_.end()) {
        return it->second;
    }
    
    return nullptr;
}

std::shared_ptr<FuturesInstrument> InstrumentRegistry::get_futures_instrument(const std::string& symbol) const {
    auto instrument = get_instrument(symbol);
    if (!instrument || instrument->get_type() != AssetType::FUTURE) {
        return nullptr;
    }
    
    return std::dynamic_pointer_cast<FuturesInstrument>(instrument);
}

std::shared_ptr<EquityInstrument> InstrumentRegistry::get_equity_instrument(const std::string& symbol) const {
    auto instrument = get_instrument(symbol);
    if (!instrument || instrument->get_type() != AssetType::EQUITY) {
        return nullptr;
    }
    
    return std::dynamic_pointer_cast<EquityInstrument>(instrument);
}

std::shared_ptr<OptionInstrument> InstrumentRegistry::get_option_instrument(const std::string& symbol) const {
    auto instrument = get_instrument(symbol);
    if (!instrument || instrument->get_type() != AssetType::OPTION) {
        return nullptr;
    }
    
    return std::dynamic_pointer_cast<OptionInstrument>(instrument);
}

Result<void> InstrumentRegistry::load_instruments() {
    if (!initialized_) {
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED,
            "InstrumentRegistry not initialized",
            "InstrumentRegistry"
        );
    }
    
    try {
        auto result = db_->get_contract_metadata();
        
        if (result.is_error()) {
            return make_error<void>(
                result.error()->code(),
                "Failed to query contract metadata: " + std::string(result.error()->what()),
                "InstrumentRegistry"
            );
        }
        
        auto table = result.value();
        int rows_loaded = 0;
        
        for (int64_t i = 0; i < table->num_rows(); i++) {
            auto instrument = create_instrument_from_db(table, i);
            if (instrument) {
                std::lock_guard<std::mutex> lock(mutex_);
                instruments_[instrument->get_symbol()] = instrument;
                rows_loaded++;
            }
        }
        
        INFO("Loaded " + std::to_string(rows_loaded) + " instruments from database");
        return Result<void>();
        
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            std::string("Error loading instruments: ") + e.what(),
            "InstrumentRegistry"
        );
    }
}

Result<void> InstrumentRegistry::load_instruments(AssetClass asset_class) {
    if (!initialized_) {
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED,
            "InstrumentRegistry not initialized",
            "InstrumentRegistry"
        );
    }
    
    try {
        std::string asset_type_str;
        switch (asset_class) {
            case AssetClass::FUTURES: asset_type_str = "Futures"; break;
            case AssetClass::EQUITIES: asset_type_str = "EQUITY"; break;
            case AssetClass::CURRENCIES: asset_type_str = "FOREX"; break;
            case AssetClass::COMMODITIES: asset_type_str = "COMMODITY"; break;
            case AssetClass::CRYPTO: asset_type_str = "CRYPTO"; break;
            default: asset_type_str = "";
        }
        
        std::string query = "SELECT * FROM metadata.contract_metadata WHERE \"Asset Type\" = '" + asset_type_str + "'";
        auto result = db_->execute_query(query);
        
        if (result.is_error()) {
            return make_error<void>(
                result.error()->code(),
                "Failed to query contract metadata: " + std::string(result.error()->what()),
                "InstrumentRegistry"
            );
        }
        
        auto table = result.value();
        int rows_loaded = 0;
        
        for (int64_t i = 0; i < table->num_rows(); i++) {
            auto instrument = create_instrument_from_db(table, i);
            if (instrument) {
                std::lock_guard<std::mutex> lock(mutex_);
                instruments_[instrument->get_symbol()] = instrument;
                rows_loaded++;
            }
        }
        
        INFO("Loaded " + std::to_string(rows_loaded) + " " + asset_type_str + " instruments from database");
        return Result<void>();
        
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            std::string("Error loading instruments: ") + e.what(),
            "InstrumentRegistry"
        );
    }
}

std::unordered_map<std::string, std::shared_ptr<Instrument>> InstrumentRegistry::get_all_instruments() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return instruments_;
}

std::vector<std::shared_ptr<Instrument>> InstrumentRegistry::get_instruments_by_asset_class(AssetClass asset_class) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<Instrument>> result;
    
    AssetType target_type;
    switch (asset_class) {
        case AssetClass::FUTURES: target_type = AssetType::FUTURE; break;
        case AssetClass::EQUITIES: target_type = AssetType::EQUITY; break;
        case AssetClass::OPTIONS: target_type = AssetType::OPTION; break;
        case AssetClass::CURRENCIES: target_type = AssetType::FOREX; break;
        case AssetClass::CRYPTO: target_type = AssetType::CRYPTO; break;
        default: return result;
    }
    
    for (const auto& [symbol, instrument] : instruments_) {
        if (instrument->get_type() == target_type) {
            result.push_back(instrument);
        }
    }
    
    return result;
}

bool InstrumentRegistry::has_instrument(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return instruments_.find(symbol) != instruments_.end();
}

std::shared_ptr<Instrument> InstrumentRegistry::create_instrument_from_db(
    const std::shared_ptr<arrow::Table>& table, int64_t row) {
    
    try {
        // Helper to get string from table
        auto get_string = [&table, row](const std::string& col_name) -> std::string {
            auto col = table->GetColumnByName(col_name);
            if (!col || col->num_chunks() == 0) return "";
            
            auto string_array = std::static_pointer_cast<arrow::StringArray>(col->chunk(0));
            if (string_array->IsNull(row)) return "";
            
            return string_array->GetString(row);
        };
        
        // Helper to get double from table
        auto get_double = [&table, row](const std::string& col_name) -> double {
            auto col = table->GetColumnByName(col_name);
            if (!col || col->num_chunks() == 0) return 0.0;
            
            auto double_array = std::static_pointer_cast<arrow::DoubleArray>(col->chunk(0));
            if (double_array->IsNull(row)) return 0.0;
            
            return double_array->Value(row);
        };
        
        // Extract common fields
        std::string symbol = get_string("Databento Symbol");
        if (symbol.empty()) {
            symbol = get_string("IB Symbol"); // Fallback to IB symbol
        }
        
        if (symbol.empty()) {
            WARN("Skipping instrument with empty symbol");
            return nullptr;
        }
        
        std::string asset_type_str = get_string("Asset Type");
        AssetType asset_type = string_to_asset_type(asset_type_str);
        
        std::string exchange = get_string("Exchange");
        double contract_size = get_double("Contract Size");
        // Default to 1.0 if contract size is missing or zero
        if (contract_size <= 0.0) contract_size = 1.0;
        
        double min_tick = get_double("Minimum Price Fluctuation");
        std::string tick_size = get_string("Tick Size");
        double commission = 0.0; // Not in the metadata, set a default
        
        // Create instrument based on asset type
        switch (asset_type) {
            case AssetType::FUTURE: {
                FuturesSpec spec;
                spec.root_symbol = symbol;
                spec.exchange = exchange;
                spec.currency = "USD"; // Default
                spec.multiplier = contract_size;
                spec.tick_size = min_tick;
                spec.commission_per_contract = commission;
                
                // Extract futures-specific fields
                spec.initial_margin = get_double("Overnight Initial Margin");
                spec.maintenance_margin = get_double("Overnight Maintenance Margin");
                spec.trading_hours = get_string("Trading Hours (EST)");
                
                // We don't have expiry in the metadata, so leave it as std::nullopt
                
                return std::make_shared<FuturesInstrument>(symbol, std::move(spec));
            }
            
            case AssetType::EQUITY: {
                EquitySpec spec;
                spec.exchange = exchange;
                spec.currency = "USD"; // Default
                spec.tick_size = min_tick;
                spec.commission_per_share = commission;
                spec.sector = get_string("Sector");
                spec.trading_hours = get_string("Trading Hours (EST)");
                
                return std::make_shared<EquityInstrument>(symbol, std::move(spec));
            }
            
            case AssetType::OPTION: {
                // Would need additional data for options that isn't in the metadata
                WARN("Option instruments not fully supported with current metadata");
                return nullptr;
            }
            
            default:
                WARN("Unsupported asset type: " + asset_type_str);
                return nullptr;
        }
        
    } catch (const std::exception& e) {
        ERROR("Error creating instrument: " + std::string(e.what()));
        return nullptr;
    }
}

AssetType InstrumentRegistry::string_to_asset_type(const std::string& asset_type_str) const {
    if (asset_type_str == "FUTURE" || asset_type_str == "FUT") {
        return AssetType::FUTURE;
    } else if (asset_type_str == "EQUITY" || asset_type_str == "STK") {
        return AssetType::EQUITY;
    } else if (asset_type_str == "OPTION" || asset_type_str == "OPT") {
        return AssetType::OPTION;
    } else if (asset_type_str == "FOREX" || asset_type_str == "FX") {
        return AssetType::FOREX;
    } else if (asset_type_str == "CRYPTO") {
        return AssetType::CRYPTO;
    } else {
        return AssetType::NONE;
    }
}

} // namespace trade_ngin