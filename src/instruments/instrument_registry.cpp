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
    
    // Add this line to detect multiple initializations
    if (initialized_) {
        WARN("InstrumentRegistry already initialized - not reinitializing");
        if (db_) {
            INFO("Registry already has a database connection");
        }
        INFO("Registry currently contains " + std::to_string(instruments_.size()) + " instruments");
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

    std::string cleaned_symbol;

    // Handle special cases for micro futures
    if (symbol == "ES") {
        cleaned_symbol = "MES";
    } else if (symbol == "YM") {
        cleaned_symbol = "MYM";
    } else if (symbol == "NQ") {
        cleaned_symbol = "MNQ";
    } else {
        cleaned_symbol = symbol;
    }
    
    auto it = instruments_.find(cleaned_symbol);
    if (it != instruments_.end()) {
        return it->second;
    }

    std::string available_symbols = "";
    for (const auto& [sym, instrument] : instruments_) {
        available_symbols += sym + ", ";
    } 
    
    ERROR("Instrument not found: " + cleaned_symbol + ". Available symbols: " + available_symbols);
    return nullptr;
}

std::shared_ptr<FuturesInstrument> InstrumentRegistry::get_futures_instrument(const std::string& symbol) const {
    auto instrument = get_instrument(symbol);
    if (!instrument || instrument->get_type() != AssetType::FUTURE) {
        WARN("Invalid futures instrument: " + symbol);
        return nullptr;
    }
    
    return std::dynamic_pointer_cast<FuturesInstrument>(instrument);
}

std::shared_ptr<EquityInstrument> InstrumentRegistry::get_equity_instrument(const std::string& symbol) const {
    auto instrument = get_instrument(symbol);
    if (!instrument || instrument->get_type() != AssetType::EQUITY) {
        WARN("Invalid equity instrument: " + symbol);
        return nullptr;
    }
    
    return std::dynamic_pointer_cast<EquityInstrument>(instrument);
}

std::shared_ptr<OptionInstrument> InstrumentRegistry::get_option_instrument(const std::string& symbol) const {
    auto instrument = get_instrument(symbol);
    if (!instrument || instrument->get_type() != AssetType::OPTION) {
        WARN("Invalid option instrument: " + symbol);
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
        INFO("Contract metadata table schema:");
        for (int i = 0; i < table->num_columns(); i++) {
            auto field = table->schema()->field(i);
            INFO("  Column " + std::to_string(i) + ": " + field->name() + " (" + 
            field->type()->ToString() + ")");
        }
        
        // Check first row data for each column
        if (table->num_rows() > 0) {
            INFO("First row values:");
            for (int i = 0; i < table->num_columns(); i++) {
                auto field = table->schema()->field(i);
                auto column = table->column(i);
                if (column->num_chunks() > 0) {
                    auto chunk = column->chunk(0);
                    std::string value = "NULL";
                    if (field->type()->id() == arrow::Type::DOUBLE) {
                        auto array = std::static_pointer_cast<arrow::DoubleArray>(chunk);
                        if (!array->IsNull(0)) {
                            value = std::to_string(array->Value(0));
                        }
                    } else if (field->type()->id() == arrow::Type::STRING) {
                        auto array = std::static_pointer_cast<arrow::StringArray>(chunk);
                        if (!array->IsNull(0)) {
                            value = array->GetString(0);
                        }
                    }
                    INFO("    " + field->name() + ": " + value);
                }
            }
        }
        
        int rows_loaded = 0;

        // Create a temporary map to hold all the instruments
        std::unordered_map<std::string, std::shared_ptr<Instrument>> temp_instruments;
        
        for (int64_t i = 0; i < table->num_rows(); i++) {
            auto instrument = create_instrument_from_db(table, i);
            if (instrument) {
                temp_instruments[instrument->get_symbol()] = instrument;
                rows_loaded++;
                DEBUG("Loaded instrument: " + instrument->get_symbol());
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            instruments_ = std::move(temp_instruments); // Swap the temporary map with the main map
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

    std::string cleaned_symbol;

    // Handle special cases for micro futures
    if (symbol == "ES") {
        cleaned_symbol = "MES";
    } else if (symbol == "YM") {
        cleaned_symbol = "MYM";
    } else if (symbol == "NQ") {
        cleaned_symbol = "MNQ";
    } else {
        cleaned_symbol = symbol;
    }

    return instruments_.find(cleaned_symbol) != instruments_.end();
}

std::shared_ptr<Instrument> InstrumentRegistry::create_instrument_from_db(
    const std::shared_ptr<arrow::Table>& table, int64_t row) {

    INFO("Creating instrument from database row: " + std::to_string(row));
    
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
        INFO("Contract Size for " + symbol + ": " + std::to_string(contract_size) + 
        " (raw column value exists: " + 
        (table->GetColumnByName("Contract Size") ? "yes" : "no") + ")");

        // Default to 1.0 if contract size is missing or zero
        if (contract_size <= 0.0) {
            WARN("Using default contract size (1.0) for " + symbol);
            contract_size = 1.0;
        }
        
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
                DEBUG("Created futures instrument: " + symbol);
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
    if (asset_type_str == "FUTURE" || asset_type_str == "FUT" || asset_type_str == "Futures") {
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