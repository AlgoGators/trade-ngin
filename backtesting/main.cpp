#include "strategy.h"
#include "pnl.h"


int main() {
    double initialCapital = 1000.0;

    BuyAndHoldStrategy strategy(initialCapital);

    std::vector<double> prices = {100, 102, 101, 105, 110};

    std::vector<double> positions = strategy.generatePositions(prices);

    PNL pnl(initialCapital);
    pnl.calculate(positions, prices);

    std::cout << "Cumulative Profit: " << pnl.cumulativeProfit() << std::endl;
    std::cout << "Sharpe Ratio: " << pnl.sharpeRatio() << std::endl;
    pnl.plotCumulativeProfit();

    return 0;
}