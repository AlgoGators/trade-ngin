#pragma once
#include "../data/dataframe.hpp"
#include <memory>
#include <vector>
#include <unordered_map>

namespace signals {

class Signal {
public:
    virtual ~Signal() = default;
    virtual std::vector<double> generate(const DataFrame& data) = 0;
    virtual void configure(const std::unordered_map<std::string, double>& params) {}
};

class SignalCombiner {
public:
    virtual ~SignalCombiner() = default;
    virtual std::vector<double> combine(const std::vector<std::vector<double>>& signals,
                                      const std::vector<double>& weights) = 0;
};

class EqualWeightedCombiner : public SignalCombiner {
public:
    std::vector<double> combine(const std::vector<std::vector<double>>& signals,
                              const std::vector<double>& weights) override {
        if (signals.empty()) return {};
        
        size_t n = signals[0].size();
        std::vector<double> combined(n, 0.0);
        
        for (size_t i = 0; i < n; ++i) {
            double sum = 0.0;
            for (size_t j = 0; j < signals.size(); ++j) {
                sum += signals[j][i] * weights[j];
            }
            combined[i] = sum / signals.size();
        }
        
        return combined;
    }
};

class SignalProcessor {
public:
    void addSignal(std::shared_ptr<Signal> signal, double weight = 1.0) {
        signals_.push_back(signal);
        weights_.push_back(weight);
    }
    
    void setCombiner(std::shared_ptr<SignalCombiner> combiner) {
        combiner_ = std::move(combiner);
    }
    
    std::vector<double> processSignals(const DataFrame& data) {
        std::vector<std::vector<double>> signal_values;
        for (const auto& signal : signals_) {
            signal_values.push_back(signal->generate(data));
        }
        return combiner_->combine(signal_values, weights_);
    }

private:
    std::vector<std::shared_ptr<Signal>> signals_;
    std::vector<double> weights_;
    std::shared_ptr<SignalCombiner> combiner_;
}; 