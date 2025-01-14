#include "database_interface.hpp"       // or "data_client_database.hpp"
#include "strategy.h"                   // your TrendFollowing strategy class
#include <arrow/api.h>
#include "system/dyn_opt.hpp"

int main() {
    try {
        // 1) Instantiate DB interface
        DatabaseInterface db;

        // 2) Fetch Arrow data for all futures contracts
        std::vector<std::string> symbols = {
            "6B.c.0", "6C.c.0", "6E.c.0", "6J.c.0", "6M.c.0",
            "6N.c.0", "6S.c.0", "CL.c.0", "GC.c.0", "GF.c.0",
            "HE.c.0", "HG.c.0", "KE.c.0", "LE.c.0", "MES.c.0",
            "MNQ.c.0", "MYM.c.0", "PL.c.0", "RB.c.0", "RTY.c.0",
            "SI.c.0", "UB.c.0", "ZC.c.0", "ZL.c.0", "ZM.c.0",
            "ZN.c.0", "ZR.c.0", "ZS.c.0", "ZW.c.0"
        };
        auto arrow_table = db.getOHLCVArrowTable("2023-01-01", "2023-12-31", symbols);

        // 3) Extract close prices for each symbol
        auto symbol_col = std::static_pointer_cast<arrow::StringArray>(arrow_table->column(1)->chunk(0));
        auto close_col = std::static_pointer_cast<arrow::DoubleArray>(arrow_table->column(5)->chunk(0));
        
        // Group prices by symbol
        std::map<std::string, std::vector<double>> symbol_prices;
        for (int64_t i = 0; i < close_col->length(); ++i) {
            std::string symbol = symbol_col->GetString(i);
            symbol_prices[symbol].push_back(close_col->Value(i));
        }

        // 4) Instantiate strategy
        double initialCapital = 500000.0;  // Increased for multiple symbols
        double contractSize = 50.0;
        trendFollowing strategy(initialCapital, contractSize);

        // 5) Generate positions for each symbol
        std::map<std::string, std::vector<double>> all_positions;
        for (const auto& symbol : symbols) {
            all_positions[symbol] = strategy.generatePositions(symbol_prices[symbol]);
        }

        // 6) This is where DynOpt would go
        // For each timestamp:
        // - Gather positions for all symbols
        // - Create optimization inputs
        // - Run DynOpt
        // Example structure:
        /*
        size_t num_days = symbol_prices[symbols[0]].size();
        std::vector<double> heldPositions(symbols.size(), 0.0);
        
        for (size_t day = 0; day < num_days; ++day) {
            // Get ideal positions for this day
            std::vector<double> idealPositions;
            for (const auto& symbol : symbols) {
                idealPositions.push_back(all_positions[symbol][day]);
            }

            // DynOpt inputs
            std::vector<double> costsPerContract(symbols.size(), 1.0);
            std::vector<double> weightsPerContract(symbols.size(), 1.0);
            std::vector<std::vector<double>> covarianceMatrix(
                symbols.size(), 
                std::vector<double>(symbols.size(), 0.0)
            );

            // Run optimization
            std::vector<double> optimizedPositions = DynOpt::singleDayOptimization(
                heldPositions,
                idealPositions,
                costsPerContract,
                weightsPerContract,
                covarianceMatrix,
                0.1,  // tau
                initialCapital,
                0.1,  // asymmetricRiskBuffer
                1     // costPenaltyScalar
            );

            heldPositions = optimizedPositions;
        }
        */

        // 7) Print signals (maintaining original format)
        std::cout << "First 10 positions for each symbol:\n";
        for (const auto& symbol : symbols) {
            std::cout << symbol << ":\n";
            for (size_t i = 0; i < 10 && i < all_positions[symbol].size(); ++i) {
                std::cout << "Day " << i << ", Position: " << all_positions[symbol][i] << "\n";
            }
            std::cout << "-------------------\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
} 