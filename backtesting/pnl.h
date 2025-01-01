    #ifndef PNL_H
    #define PNL_H

    #include <vector>
    #include <numeric>
    #include <cmath>
    #include <iostream>
    #include <limits>

    // For GNUplot might delete
    #include <fstream>
    #include <cstdio>

    class PNL {
    private:
        std::vector<double> profits;
        double initialCapital;
        double contractSize;
        std::vector<double> positions;

    public:
        explicit PNL(double capital, double contractSize) : initialCapital(capital), contractSize(contractSize)  {}

        // Method to calculate PNL from full position and price data
        void calculate(const std::vector<double>& inputPositions, const std::vector<double>& prices) {
            if (inputPositions.size() != prices.size()) {
                std::cerr << "Error: Positions size must match prices size." << std::endl;
                return;
            }

            this->positions = inputPositions;

            profits.clear(); // Clear previous profits in case this is called multiple times

            for (size_t i = 1; i < prices.size(); ++i) {
                // Check for NaN in positions or prices
                if (std::isnan(inputPositions[i - 1]) || std::isnan(prices[i]) || std::isnan(prices[i - 1])) {
                    profits.push_back(std::numeric_limits<double>::quiet_NaN());
                    continue;
                }

                // Calculate PNL
                double pnl = inputPositions[i] * (prices[i] - prices[i - 1]) * contractSize;
                profits.push_back(pnl);
            }
        }

        double cumulativeProfit() const {
            if (profits.empty()) return 0.0;
            return std::accumulate(profits.begin(), profits.end(), 0.0, [](double sum, double profit) {
                return std::isnan(profit) ? sum : sum + profit;
            });
        }

        double sharpeRatio() const {
            if (profits.empty()) return 0.0;

            // Filter out NaN profits
            std::vector<double> validProfits;
            for (const auto& profit : profits) {
                if (!std::isnan(profit)) validProfits.push_back(profit);
            }

            if (validProfits.empty()) return 0.0;

            double mean = std::accumulate(validProfits.begin(), validProfits.end(), 0.0) / validProfits.size();

            double variance = 0.0;
            for (const auto& profit : validProfits) {
                variance += (profit - mean) * (profit - mean);
            }
            variance /= validProfits.size();
            double stdDev = std::sqrt(variance);

            return stdDev == 0.0 ? 0.0 : mean / stdDev;
        }

        // Placeholder plotting function
        void plotCumulativeProfit() const {
            if (profits.empty()) {
                std::cerr << "Error: No profits available to plot." << std::endl;
                return;
            }

            double runningTotal = 0.0;
            std::cout << "Cumulative Profit (%):" << std::endl;
            for (const auto& profit : profits) {
                if (!std::isnan(profit)) {
                    runningTotal += profit;
                    double percentage = (runningTotal / initialCapital) * 100.0;
                    std::cout << "  Running Total: " << runningTotal
                            << " | Percentage of Initial Capital: " << percentage << "%" << std::endl;
                }
            }
        }

        void plotPositions() const {
            if (positions.empty()) {
                std::cerr << "Error: No positions available to plot." << std::endl;
                return;
            }

            std::cout << "Positions:" << std::endl;
            int index = 0;
            for (const auto& position : positions) {
                    if(!std::isnan(position)){
                    index += 1;
                    std::cout << "  Index: " << index
                            << " | " << position << std::endl;
                    }
            }
        }

    /*
    void plotCumulativeProfit() const {
            if (profits.empty()) {
                std::cerr << "Error: No profits available to plot." << std::endl;
                return;
            }

            // Calculate cumulative profits
            std::vector<double> cumulativeProfits;
            double runningTotal = 0.0;
            for (const auto& profit : profits) {
                if (!std::isnan(profit)) {
                    runningTotal += profit;
                }
                cumulativeProfits.push_back(runningTotal);
            }

            // Write cumulative profits to a data file
            std::ofstream dataFile("cumulative_profits.dat");
            for (size_t i = 0; i < cumulativeProfits.size(); ++i) {
                double percentage = (cumulativeProfits[i] / initialCapital) * 100.0;
                dataFile << i + 1 << " " << percentage << "\n"; // Day vs. Cumulative profit percentage
            }
            dataFile.close();

            // Use gnuplot to plot the data
            FILE* gnuplot = _popen("gnuplot -persistent", "w");
            if (gnuplot) {
                fprintf(gnuplot, "set terminal pngcairo size 800,600\n");
                fprintf(gnuplot, "set output 'cumulative_profit_plot.png'\n");
                fprintf(gnuplot, "set title 'Cumulative Profit as Percentage of Initial Capital'\n");
                fprintf(gnuplot, "set xlabel 'Day'\n");
                fprintf(gnuplot, "set ylabel 'Cumulative Profit (%)'\n");
                fprintf(gnuplot, "set grid\n");
                fprintf(gnuplot, "plot 'cumulative_profits.dat' using 1:2 with lines title 'Cumulative Profit'\n");
                _pclose(gnuplot);

                std::cout << "Plot saved as 'cumulative_profit_plot.png'" << std::endl;
            } else {
                std::cerr << "Error: Could not open gnuplot" << std::endl;
            }
        }*/

    };

    #endif
