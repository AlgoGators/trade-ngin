#pragma once
#include <string>

class Contract {
public:
    Contract(std::string symbol, double multiplier = 1.0)
        : symbol_(std::move(symbol)), multiplier_(multiplier) {}

    const std::string& symbol() const { return symbol_; }
    double multiplier() const { return multiplier_; }

private:
    std::string symbol_;
    double multiplier_;
}; 