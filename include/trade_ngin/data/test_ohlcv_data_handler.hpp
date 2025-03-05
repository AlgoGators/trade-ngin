#ifndef TEST_OHLCV_DATA_HANDLER_HPP
#define TEST_OHLCV_DATA_HANDLER_HPP

#include "ohlcv_data_handler.hpp"

class TestOHLCVDataHandler : public OHLCVDataHandler {
public:
    TestOHLCVDataHandler(std::shared_ptr<DatabaseClient> db_client) 
        : OHLCVDataHandler(db_client) {}

    void setCallback(std::function<void(const OHLCV&)> callback) override {
        callback_ = callback;
    }

private:
    std::function<void(const OHLCV&)> callback_;
};

#endif // TEST_OHLCV_DATA_HANDLER_HPP 