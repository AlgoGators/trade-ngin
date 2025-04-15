#include <vector>
#include <mutex>
#include <iostream>
#include <thread>
#include <memory>

class SignalEngineStub {
public:
    SignalEngineStub() { std::cout << "SignalEngineStub constructed\n"; }
    ~SignalEngineStub() { std::cout << "SignalEngineStub destructed\n"; }

    void generate_signal(double value) {
        std::lock_guard<std::mutex> lock(mtx_);
        signals_.push_back(value);
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mtx_);
        signals_.clear();
    }

    size_t num_signals() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return signals_.size();
    }

    double get_signal(size_t idx) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (idx < signals_.size()) return signals_[idx];
        return 0.0;
    }

private:
    mutable std::mutex mtx_;
    std::vector<double> signals_;
}; 