#pragma once
#include "dataframe.hpp"
#include <vector>
#include <memory>
#include <unordered_map>

namespace signals {

// Base Signal interface
class Signal {
public:
    virtual ~Signal() = default;
    virtual std::vector<double> calculate(const DataFrame& market_data) = 0;
    virtual void configure(const std::unordered_map<std::string, double>& params) {}
};

// Technical Analysis Functions (moved from signals.h)
double calculateEMA(double price, double prevEMA, double alpha);
std::vector<double> calculateEMAC(const std::vector<double>& prices, int shortSpan, int longSpan);
void calculateShortAndDynamicLongStdDev(const std::vector<double>& prices, 
                                      size_t shortWindow, 
                                      size_t longWindow,
                                      std::vector<double>& combinedStdDev);

// Signal Combiner interface
class SignalCombiner {
public:
    virtual ~SignalCombiner() = default;
    virtual std::vector<double> combine(
        const std::vector<std::vector<double>>& signals,
        const std::vector<double>& weights
    ) = 0;
};

// Signal Processor
class SignalProcessor {
public:
    void addSignal(std::shared_ptr<Signal> signal, double weight) {
        signals_.emplace_back(signal, weight);
    }

    void setCombiner(std::shared_ptr<SignalCombiner> combiner) {
        combiner_ = std::move(combiner);
    }

    std::vector<double> processSignals(const DataFrame& market_data) {
        std::vector<std::vector<double>> all_signals;
        std::vector<double> weights;

        for (const auto& [signal, weight] : signals_) {
            all_signals.push_back(signal->calculate(market_data));
            weights.push_back(weight);
        }

        return combiner_->combine(all_signals, weights);
    }

private:
    std::vector<std::pair<std::shared_ptr<Signal>, double>> signals_;
    std::shared_ptr<SignalCombiner> combiner_;
};

} // namespace signals 