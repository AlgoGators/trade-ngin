// src/data/csv_equity_loader.cpp
#include "trade_ngin/data/csv_equity_loader.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>

namespace trade_ngin {

namespace {

// Parse "YYYY-MM-DD" into a UTC Timestamp at 00:00:00.
Timestamp parse_date(const std::string& date_str) {
    int year = 0, month = 0, day = 0;
    char dash1 = '\0', dash2 = '\0';
    std::istringstream ss(date_str);
    ss >> year >> dash1 >> month >> dash2 >> day;
    if (ss.fail() || dash1 != '-' || dash2 != '-') {
        throw std::runtime_error("invalid date format (expected YYYY-MM-DD): " + date_str);
    }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    // timegm interprets tm as UTC. _mkgmtime on Windows (not a concern here).
    std::time_t tt = timegm(&tm);
    if (tt == -1) {
        throw std::runtime_error("timegm failed for: " + date_str);
    }
    return std::chrono::system_clock::from_time_t(tt);
}

// Strip CR/LF/whitespace from both ends of a token.
void trim(std::string& s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
}

}  // namespace

Result<std::vector<Bar>> CSVEquityLoader::load(const std::string& ticker,
                                               const Timestamp& start,
                                               const Timestamp& end,
                                               const std::string& dir) {
    std::string filepath = dir + "/" + ticker + ".csv";
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return make_error<std::vector<Bar>>(
            ErrorCode::FILE_NOT_FOUND,
            "Cannot open equity CSV file: " + filepath,
            "CSVEquityLoader");
    }

    std::string line;
    // Skip header
    if (!std::getline(file, line)) {
        return make_error<std::vector<Bar>>(
            ErrorCode::INVALID_DATA,
            "Equity CSV file is empty: " + filepath,
            "CSVEquityLoader");
    }

    std::vector<Bar> bars;
    int line_num = 1;
    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) continue;

        std::istringstream ss(line);
        std::string tok;
        try {
            // date
            if (!std::getline(ss, tok, ',')) throw std::runtime_error("missing date");
            trim(tok);
            Timestamp ts = parse_date(tok);

            // Filter by [start, end] early to avoid wasted parsing. Compare at daily
            // granularity — callers pass start/end as midnight-anchored timestamps.
            if (ts < start || ts > end) {
                // Continue reading rest of line to stay in sync, but drop it.
                continue;
            }

            auto next_double = [&](const char* field) {
                if (!std::getline(ss, tok, ',')) {
                    throw std::runtime_error(std::string("missing ") + field);
                }
                trim(tok);
                return std::stod(tok);
            };

            double open_raw = next_double("open");
            double high_raw = next_double("high");
            double low_raw = next_double("low");
            double close_raw = next_double("close");
            double adj_close = next_double("adj_close");

            // volume (last column — may or may not have trailing comma)
            double volume = 0.0;
            if (std::getline(ss, tok, ',')) {
                trim(tok);
                if (!tok.empty()) volume = std::stod(tok);
            }

            // Apply adjustment ratio to OHL, mirroring the SQL path that uses
            // close * (closeadj / close).
            double ratio = (std::abs(close_raw) > 1e-12) ? (adj_close / close_raw) : 1.0;
            double open_adj = open_raw * ratio;
            double high_adj = high_raw * ratio;
            double low_adj = low_raw * ratio;

            bars.emplace_back(ts, open_adj, high_adj, low_adj, adj_close, volume, ticker);
        } catch (const std::exception& e) {
            return make_error<std::vector<Bar>>(
                ErrorCode::INVALID_DATA,
                "Error parsing " + filepath + " line " + std::to_string(line_num) + ": " + e.what(),
                "CSVEquityLoader");
        }
    }

    if (bars.empty()) {
        return make_error<std::vector<Bar>>(
            ErrorCode::DATA_NOT_FOUND,
            "No bars in CSV for " + ticker + " within requested date range",
            "CSVEquityLoader");
    }

    // Sort by timestamp ascending — yfinance output is already sorted, but be defensive.
    std::sort(bars.begin(), bars.end(),
              [](const Bar& a, const Bar& b) { return a.timestamp < b.timestamp; });

    return bars;
}

}  // namespace trade_ngin
