// src/instruments/instrument_registry.cpp
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/instruments/contract_multiplier.hpp"
#include <arrow/api.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/conversion_utils.hpp"

namespace trade_ngin {

namespace {

std::string futures_root_symbol(const std::string& symbol) {
    const auto variant_position = symbol.find(".v.");
    return variant_position == std::string::npos ? symbol : symbol.substr(0, variant_position);
}

std::string mapped_futures_root(const std::string& symbol) {
    auto root = futures_root_symbol(symbol);
    if (root == "ES") return "MES";
    if (root == "YM") return "MYM";
    if (root == "NQ") return "MNQ";
    return root;
}

int generic_lookup_priority(AssetType asset_type) {
    switch (asset_type) {
        case AssetType::EQUITY:
            return 3;
        case AssetType::FUTURE:
            return 2;
        case AssetType::OPTION:
            return 1;
        default:
            return 0;
    }
}

}  // namespace

Result<void> InstrumentRegistry::initialize(std::shared_ptr<PostgresDatabase> db) {
    std::lock_guard<std::mutex> lock(mutex_);

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
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Postgres interface cannot be null",
                                "InstrumentRegistry");
    }

    db_ = std::move(db);
    initialized_ = true;

    INFO("InstrumentRegistry initialized successfully");
    return Result<void>();
}

std::shared_ptr<Instrument> InstrumentRegistry::get_instrument(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const bool has_variant = symbol.find(".v.") != std::string::npos;
    if (!has_variant) {
        if (auto exact = instruments_.find(symbol); exact != instruments_.end()) {
            return exact->second;
        }
    }

    const auto mapped = mapped_futures_root(symbol);
    if (has_variant || mapped != symbol) {
        auto future = futures_.find(mapped);
        if (future != futures_.end()) {
            return future->second;
        }
        if (auto generic = instruments_.find(mapped); generic != instruments_.end() &&
            generic->second->get_type() == AssetType::FUTURE) {
            return generic->second;
        }
    }

    std::string available_symbols = "";
    for (const auto& [sym, instrument] : instruments_) {
        available_symbols += sym + ", ";
    }

    ERROR("Instrument not found: " + symbol + ". Available symbols: " + available_symbols);
    return nullptr;
}

std::shared_ptr<FuturesInstrument> InstrumentRegistry::get_futures_instrument(
    const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto mapped = mapped_futures_root(symbol);
    if (auto future = futures_.find(mapped); future != futures_.end()) {
        return future->second;
    }
    if (auto generic = instruments_.find(mapped); generic != instruments_.end()) {
        return std::dynamic_pointer_cast<FuturesInstrument>(generic->second);
    }
    WARN("Invalid futures instrument: " + symbol);
    return nullptr;
}

std::shared_ptr<EquityInstrument> InstrumentRegistry::get_equity_instrument(
    const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto equity = equities_.find(symbol);
    if (equity != equities_.end()) return equity->second;
    if (auto generic = instruments_.find(symbol); generic != instruments_.end()) {
        return std::dynamic_pointer_cast<EquityInstrument>(generic->second);
    }
    WARN("Invalid equity instrument: " + symbol);
    return nullptr;
}

std::shared_ptr<OptionInstrument> InstrumentRegistry::get_option_instrument(
    const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto option = options_.find(symbol);
    if (option != options_.end()) return option->second;
    if (auto generic = instruments_.find(symbol); generic != instruments_.end()) {
        return std::dynamic_pointer_cast<OptionInstrument>(generic->second);
    }
    WARN("Invalid option instrument: " + symbol);
    return nullptr;
}

Result<void> InstrumentRegistry::load_instruments() {
    if (!initialized_) {
        return make_error<void>(ErrorCode::NOT_INITIALIZED, "InstrumentRegistry not initialized",
                                "InstrumentRegistry");
    }

    try {
        auto result = db_->get_contract_metadata();

        if (result.is_error()) {
            return make_error<void>(
                result.error()->code(),
                "Failed to query contract metadata: " + std::string(result.error()->what()),
                "InstrumentRegistry");
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

        // Create temporary indexes so readers never observe a partially loaded registry.
        std::unordered_map<std::string, std::shared_ptr<Instrument>> temp_instruments;
        std::unordered_map<std::string, std::shared_ptr<FuturesInstrument>> temp_futures;
        std::unordered_map<std::string, std::shared_ptr<EquityInstrument>> temp_equities;
        std::unordered_map<std::string, std::shared_ptr<OptionInstrument>> temp_options;

        for (int64_t i = 0; i < table->num_rows(); i++) {
            auto instrument = create_instrument_from_db(table, i);
            if (instrument) {
                std::string generic_symbol = instrument->get_symbol();
                switch (instrument->get_type()) {
                    case AssetType::FUTURE:
                        generic_symbol = futures_root_symbol(generic_symbol);
                        temp_futures[generic_symbol] =
                            std::dynamic_pointer_cast<FuturesInstrument>(instrument);
                        break;
                    case AssetType::EQUITY:
                        temp_equities[generic_symbol] =
                            std::dynamic_pointer_cast<EquityInstrument>(instrument);
                        break;
                    case AssetType::OPTION:
                        temp_options[generic_symbol] =
                            std::dynamic_pointer_cast<OptionInstrument>(instrument);
                        break;
                    default:
                        break;
                }

                auto generic = temp_instruments.find(generic_symbol);
                if (generic == temp_instruments.end() ||
                    generic_lookup_priority(instrument->get_type()) >
                        generic_lookup_priority(generic->second->get_type())) {
                    temp_instruments[generic_symbol] = instrument;
                }
                rows_loaded++;
                DEBUG("Loaded instrument: " + instrument->get_symbol());
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            instruments_ = std::move(temp_instruments);
            futures_ = std::move(temp_futures);
            equities_ = std::move(temp_equities);
            options_ = std::move(temp_options);
        }

        INFO("Loaded " + std::to_string(rows_loaded) + " instruments from database");

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                std::string("Error loading instruments: ") + e.what(),
                                "InstrumentRegistry");
    }
}

std::unordered_map<std::string, std::shared_ptr<Instrument>>
InstrumentRegistry::get_all_instruments() const {
    std::lock_guard<std::mutex> lock(mutex_);

    return instruments_;
}

std::vector<std::shared_ptr<Instrument>> InstrumentRegistry::get_instruments_by_asset_class(
    AssetClass asset_class) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<Instrument>> result;

    AssetType target_type;
    switch (asset_class) {
        case AssetClass::FUTURES:
            target_type = AssetType::FUTURE;
            break;
        case AssetClass::EQUITIES:
            target_type = AssetType::EQUITY;
            break;
        case AssetClass::OPTIONS:
            target_type = AssetType::OPTION;
            break;
        case AssetClass::CURRENCIES:
            target_type = AssetType::FOREX;
            break;
        case AssetClass::CRYPTO:
            target_type = AssetType::CRYPTO;
            break;
        default:
            return result;
    }

    std::unordered_set<const Instrument*> seen;
    for (const auto& [symbol, instrument] : instruments_) {
        if (instrument->get_type() == target_type) {
            result.push_back(instrument);
            seen.insert(instrument.get());
        }
    }

    const auto append_typed = [&](const auto& index) {
        for (const auto& [symbol, instrument] : index) {
            if (instrument && instrument->get_type() == target_type &&
                seen.insert(instrument.get()).second) result.push_back(instrument);
        }
    };
    if (target_type == AssetType::FUTURE) append_typed(futures_);
    if (target_type == AssetType::EQUITY) append_typed(equities_);
    if (target_type == AssetType::OPTION) append_typed(options_);

    return result;
}

bool InstrumentRegistry::has_instrument(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (symbol.find(".v.") == std::string::npos && instruments_.contains(symbol)) return true;
    const auto mapped = mapped_futures_root(symbol);
    if (futures_.contains(mapped)) return true;
    const auto generic = instruments_.find(mapped);
    return generic != instruments_.end() && generic->second->get_type() == AssetType::FUTURE;
}

void InstrumentRegistry::register_instrument(
    const std::string& symbol, std::shared_ptr<Instrument> instrument) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string generic_symbol = symbol;
    if (auto future = std::dynamic_pointer_cast<FuturesInstrument>(instrument)) {
        generic_symbol = futures_root_symbol(symbol);
        futures_[generic_symbol] = std::move(future);
    } else if (auto equity = std::dynamic_pointer_cast<EquityInstrument>(instrument)) {
        equities_[symbol] = std::move(equity);
    } else if (auto option = std::dynamic_pointer_cast<OptionInstrument>(instrument)) {
        options_[symbol] = std::move(option);
    }
    const auto generic = instruments_.find(generic_symbol);
    if (generic == instruments_.end() ||
        generic_lookup_priority(instrument->get_type()) >=
            generic_lookup_priority(generic->second->get_type())) {
        instruments_[generic_symbol] = instrument;
    }
    DEBUG("Registered instrument: " + symbol);
}

std::shared_ptr<Instrument> InstrumentRegistry::create_instrument_from_db(
    const std::shared_ptr<arrow::Table>& table, int64_t row) {
    INFO("Creating instrument from database row: " + std::to_string(row));

    try {
        auto get_string = [&table, row](const std::string& col_name) -> std::string {
            auto col = table->GetColumnByName(col_name);
            if (!col) return "";
            auto value = DataConversionUtils::safe_get_string(col, row, col_name);
            return value.is_ok() ? value.value() : "";
        };

        auto get_double = [&table, row](const std::string& col_name) -> double {
            auto col = table->GetColumnByName(col_name);
            if (!col) return 0.0;
            auto value = DataConversionUtils::safe_get_double(col, row, col_name);
            return value.is_ok() ? value.value() : 0.0;
        };

        // Extract common fields
        std::string symbol = get_string("Databento Symbol");
        if (symbol.empty()) {
            symbol = get_string("IB Symbol");  // Fallback to IB symbol
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

        // A futures instrument's multiplier is the currency value of one point
        // of the QUOTED price, which is not always the contract size: a ten-year
        // note is $100,000 of face quoted as a percentage of par, so its point
        // value is $1,000. Reading the column straight into spec.multiplier
        // priced every treasury, grain and livestock contract 100x too large.
        //
        // Which of the two quantities metadata.contract_metadata holds is not
        // settled -- the column is spelled "Contract Size" but was seeded in at
        // least one place with point values -- so the resolver recognises either
        // and says which it saw.
        double price_multiplier = contract_size;
        if (contract_size > 0.0) {
            auto resolved = resolve_price_multiplier(symbol, contract_size);
            price_multiplier = resolved.value;
            if (resolved.source == MultiplierSource::ScaledContractSize &&
                price_multiplier != contract_size) {
                INFO("Scaled contract size for " + symbol + ": " +
                     std::to_string(contract_size) + " -> point value " +
                     std::to_string(price_multiplier));
            } else if (resolved.source != MultiplierSource::ScaledContractSize &&
                       resolved.source != MultiplierSource::AlreadyPointValue) {
                WARN("Contract size for " + symbol + " taken as a point value unchecked: " +
                     std::string(describe(resolved.source)));
            }
        } else {
            // Missing or zero. The contract table is a better answer than 1.0,
            // which silently prices an S&P contract at its index level.
            auto known = fallback_price_multiplier(symbol);
            if (known) {
                WARN("No contract size for " + symbol + "; using known point value " +
                     std::to_string(*known));
                price_multiplier = *known;
            } else {
                WARN("Using default multiplier (1.0) for " + symbol);
                price_multiplier = 1.0;
            }
        }

        double min_tick = get_double("Minimum Price Fluctuation");
        std::string tick_size = get_string("Tick Size");
        double commission = asset_type == AssetType::EQUITY ? 0.005 : 0.0;

        // Create instrument based on asset type
        switch (asset_type) {
            case AssetType::FUTURE: {
                FuturesSpec spec;
                spec.root_symbol = symbol;
                spec.exchange = exchange;
                spec.currency = "USD";  // Default
                spec.multiplier = price_multiplier;
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
                spec.currency = "USD";  // Default
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

Result<void> InstrumentRegistry::load_equity_instruments(
    const std::vector<std::string>& symbols,
    const std::string& exchange_lookup_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return make_error<void>(ErrorCode::NOT_INITIALIZED, "InstrumentRegistry not initialized",
                                "InstrumentRegistry");
    }

    for(const auto& symbol:symbols) {
        if(symbol.empty() || symbol.size()>20) return make_error<void>(ErrorCode::INVALID_ARGUMENT,"invalid equity symbol");
        for(unsigned char c:symbol)
            if(!std::isalnum(c) && c!='_' && c!='.' && c!='-')
                return make_error<void>(ErrorCode::INVALID_ARGUMENT,"invalid equity symbol");
    }
    // Load exchange lookup table from JSON if provided
    std::unordered_map<std::string, std::string> exchange_map;
    if (!exchange_lookup_path.empty()) {
        try {
            std::ifstream file(exchange_lookup_path);
            if (file.is_open()) {
                nlohmann::json j;
                file >> j;
                for (auto& [exchange, symbols_array] : j.items()) {
                    if (exchange.empty()) throw std::runtime_error("empty exchange lookup key");
                    if (exchange.front() == '_') continue;  // Skip comment fields
                    for (const auto& sym : symbols_array) {
                        exchange_map[sym.get<std::string>()] = exchange;
                    }
                }
                INFO("Loaded exchange lookup with " + std::to_string(exchange_map.size()) +
                     " symbols from " + exchange_lookup_path);
            } else {
                WARN("Could not open exchange lookup file: " + exchange_lookup_path +
                     " -- falling back to NYSE");
            }
        } catch (const std::exception& e) {
            WARN("Error loading exchange lookup: " + std::string(e.what()) +
                 " -- falling back to NYSE");
        }
    }

    int registered = 0;
    for (const auto& symbol : symbols) {
        if (equities_.find(symbol)!=equities_.end()) {
            continue;
        }

        EquitySpec spec;
        // Determine exchange from lookup table, default to NYSE
        auto ex_it = exchange_map.find(symbol);
        spec.exchange = (ex_it != exchange_map.end()) ? ex_it->second : "NYSE";
        spec.currency = "USD";
        spec.tick_size = 0.01;
        // Match IBKR Pro default (also used by AssetCostConfigRegistry::get_equity_default_config).
        // Production cost path is TransactionCostManager; this default keeps the
        // instrument-level commission accessor consistent for callers that query it.
        spec.commission_per_share = 0.005;

        auto instrument=std::make_shared<EquityInstrument>(symbol,std::move(spec));
        equities_[symbol]=instrument;
        instruments_[symbol]=instrument;  // Existing generic priority prefers equity; futures_ is preserved.
        registered++;
    }

    INFO("Registered " + std::to_string(registered) + " equity instruments (" +
         std::to_string(symbols.size()) + " total symbols, " +
         std::to_string(symbols.size() - registered) + " already existed)");

    return Result<void>();
}

}  // namespace trade_ngin
