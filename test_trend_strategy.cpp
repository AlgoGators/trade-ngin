#include "database_interface.hpp"       // or "data_client_database.hpp"
#include "strategy.h"                   // your TrendFollowing strategy class
#include <arrow/api.h>

int main() {
    try {
        // 1) Instantiate DB interface
        DatabaseInterface db;

        // 2) Fetch Arrow data (example for futures_data.ohlcv_1d table)
        std::vector<std::string> symbols = {"MES.c.0"};
        auto arrow_table = db.getOHLCVArrowTable("2023-01-01", "2023-12-31", symbols);

        // 3) Extract close prices (assuming the table columns are [time, symbol, open, high, low, close, volume])
        auto close_col =
            std::static_pointer_cast<arrow::DoubleArray>(arrow_table->column(5)->chunk(0));
        std::vector<double> close_prices(close_col->length());
        for (int64_t i = 0; i < close_col->length(); ++i) {
            close_prices[i] = close_col->Value(i);
        }

        // 4) Instantiate strategy
        double initialCapital = 100000.0;
        double contractSize   = 50.0;
        trendFollowing strategy(initialCapital, contractSize);

        // 5) Generate positions
        std::vector<double> positions = strategy.generatePositions(close_prices);

        // 6) Optionally, do a PnL calculation or print out signals
        // ...
        // e.g., print the first 10 signals
        for (size_t i = 0; i < 10 && i < positions.size(); ++i) {
            std::cout << "Day " << i << ", Position: " << positions[i] << "\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
} 