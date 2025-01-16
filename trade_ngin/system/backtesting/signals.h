// Header gaurds
#ifndef SIGNALS_H
#define SIGNALS_H


#include <vector>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <iostream>

double calculateEMA(double price, double prevEMA, double alpha) {
    return (price * alpha) + (prevEMA * (1 - alpha));
}

// Function to calculate EMAC with EMA initialization matching Pandas
std::vector<double> calculateEMAC(const std::vector<double>& prices, int shortSpan, int longSpan) {
    int n = prices.size();
    std::vector<double> emac(n, std::numeric_limits<double>::quiet_NaN());

    if (n == 0 || shortSpan <= 0 || longSpan <= 0 || n < longSpan) {
        // Handle invalid input cases
        return emac;
    }

    // Calculate smoothing factors
    double alphaShort = 2.0 / (shortSpan + 1);
    double alphaLong = 2.0 / (longSpan + 1);

    // Initialize EMAs
    double emaShort = prices[0]; // EMA starts with the first price
    double emaLong = prices[0]; // EMA starts with the first price

    // Iterate through prices to calculate EMA and EMAC
    for (int i = 1; i < n; ++i) {
        // Update EMAs using the recursive formula
        emaShort = calculateEMA(prices[i], emaShort, alphaShort);
        emaLong = calculateEMA(prices[i], emaLong, alphaLong);

        // Calculate EMAC starting from the second price
        emac[i] = emaShort - emaLong;
    }

    return emac;
}


// Function to calculate the standard deviation over the past `n` days
double calculateStdDev(const std::vector<double>& prices, size_t end, size_t n) {
    if (end < n) {
        throw std::invalid_argument("Not enough data to calculate standard deviation.");
    }

    // Calculate mean
    double mean = std::accumulate(prices.begin() + end - n, prices.begin() + end, 0.0) / n;

    // Calculate variance
    double variance = 0.0;
    for (size_t i = end - n; i < end; ++i) {
        double diff = prices[i] - mean;
        variance += diff * diff;
    }

    return std::sqrt(variance / n); // Return the standard deviation
}

// Function to calculate short and long window standard deviations with dynamic long window
void calculateShortAndDynamicLongStdDev(
    const std::vector<double>& prices,
    size_t shortWindow,
    size_t longWindow,
    std::vector<double>& combinedStdDev
) {
    size_t n = prices.size();
    combinedStdDev.resize(n, std::numeric_limits<double>::quiet_NaN());

    for (size_t i = 1; i <= n; ++i) {
        try {
            double shortStdDev = std::numeric_limits<double>::quiet_NaN();
            double longStdDev = std::numeric_limits<double>::quiet_NaN();

            // Calculate short window standard deviation if sufficient data is available
            if (i >= shortWindow) {
                shortStdDev = calculateStdDev(prices, i, shortWindow);
            }

            // Calculate dynamic long window standard deviation
            if (i >= shortWindow) {
                size_t dynamicLongWindow = std::min(longWindow, i);
                longStdDev = calculateStdDev(prices, i, dynamicLongWindow);
            }

            // Combine short and long standard deviations using a 70:30 ratio
            if (!std::isnan(shortStdDev) && !std::isnan(longStdDev)) {
                combinedStdDev[i - 1] = 0.7 * shortStdDev + 0.3 * longStdDev;
            }
        } catch (const std::invalid_argument& e) {
            // Handle insufficient data (values will remain NaN)
        }
    }
}

/*
Some Notes:
EMAC is calculated the same way pandas does it and has been verified against Pandas.

for StdDev, we only really value index 256 and on in the index, but however there aren't 10 years of data behind 256. Thus the long portion of stddeviation is based off all the data befor it until we have 10 years.
*/
#endif
