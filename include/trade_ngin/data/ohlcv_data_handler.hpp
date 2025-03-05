#ifndef OHLCV_DATA_HANDLER_HPP
#define OHLCV_DATA_HANDLER_HPP

#include <string>
#include <vector>
#include <pqxx/pqxx>
#include <arrow/api.h>
#include "database_client.hpp"
#include <functional>
#include <memory>

struct OHLCV {
    std::string symbol;
    std::time_t timestamp;
    double open;
    double high;
    double low;
    double close;
    double volume;
};

class OHLCVDataHandler {
public:
    OHLCVDataHandler(std::shared_ptr<DatabaseClient> db_client);
    virtual ~OHLCVDataHandler() = default;

    // Callback interface
    virtual void setCallback(std::function<void(const OHLCV&)> callback) = 0;
    void setDataCallback(std::function<void(const std::vector<OHLCV>&)> callback);
    void fetchData(const std::string& symbol, const std::string& timeframe);

    // Arrow table interface
    std::shared_ptr<arrow::Table> getOHLCVArrowTable(
        const std::string& start_date,
        const std::string& end_date,
        const std::vector<std::string>& symbols = {}
    );

    std::shared_ptr<arrow::Table> getSymbolsAsArrowTable();
    std::string getEarliestDate();
    std::string getLatestDate();
    std::shared_ptr<arrow::Table> getLatestDataAsArrowTable(const std::string& symbol);

private:
    std::shared_ptr<DatabaseClient> db_client_;
    std::function<void(const std::vector<OHLCV>&)> dataCallback_;
};

#endif // OHLCV_DATA_HANDLER_HPP
