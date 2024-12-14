// Header gaurds
#ifndef STRATEGY_H
#define STRATEGY_H

#include <vector>
#include <iostream>

// Base Strategy Class
class Strategy {
protected:
    double initialCapital;

public:
    explicit Strategy(double capital) : initialCapital(capital) {}

    virtual std::vector<double> generatePositions(const std::vector<double>& prices) = 0;

    virtual ~Strategy() = default;
};

// Buy and Hold Strategy Class for example
class BuyAndHoldStrategy : public Strategy {
public:
    explicit BuyAndHoldStrategy(double capital) : Strategy(capital) {}

    std::vector<double> generatePositions(const std::vector<double>& prices) override {
        if (prices.empty()) {
            std::cerr << "Error: Price data is empty." << std::endl;
            return {};
        }

        std::vector<double> positions;
        double remainingCapital = initialCapital;

        // Calculate the max number of shares affordable at the initial price
        double position = static_cast<int>(remainingCapital / prices[0]); // Automatically floors the division
        remainingCapital -= position * prices[0];

        // Hold the same position for all time steps
        positions.resize(prices.size() - 1, position); // Going to want to work on the modularity of disparity between price and position vectors. Right now positions has to be one less than price in size

        return positions;
    }
};

class trendFollowing  : public Strategy {
public:
    explicit trendFollowing(double capital) : Strategy(capital) {}

    std::vector<double> generateBuySignal(const std::vector<double>& prices) override {
        
    }

    std::vector<double> generateSellSignal(const std::vector<double>& prices) override {
        
    }
};

class generatePositions {
    std::vector<double> generatePositions(const std::vector<double>& prices, const std::vector<double>& buySignal, const std::vector<double>& sellSignal) override {
        
    }
};

/*
I'm thinking about having a generate positions class that takes in buy signals, sell signals, and whatever else and generates positions

This would reduce and segment strategy into generating buy signals, sell signals, etc, and I think it could be a good way to format this.

generatePositions would be very small, and we could honestly just call it within the strategy class that way strategy still technically passes out positions.

This is just something to brainstorm
*/

// Decision is to try out the generate positions for the trendfollowing strategy and see how seamless it feels

#endif
