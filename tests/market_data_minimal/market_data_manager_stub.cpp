#include <vector>
#include <mutex>
#include <iostream>
#include <thread>
#include <chrono>
#include <memory>

class MarketDataManagerStub {
public:
    MarketDataManagerStub() { std::cout << "MarketDataManagerStub constructed\n"; }
    ~MarketDataManagerStub() { std::cout << "MarketDataManagerStub destructed\n"; }

    void add_data(double value) {
        std::lock_guard<std::mutex> lock(mtx_);
        data_.push_back(value);
    }

    double get_data(size_t idx) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (idx < data_.size()) return data_[idx];
        return 0.0;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_.size();
    }

private:
    mutable std::mutex mtx_;
    std::vector<double> data_;
}; 