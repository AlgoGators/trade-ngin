// src/data/macro_csv_loader.cpp
#include "trade_ngin/data/macro_csv_loader.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace trade_ngin {

Result<std::vector<MonthlyMacroRecord>> MacroCSVLoader::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return make_error<std::vector<MonthlyMacroRecord>>(
            ErrorCode::FILE_NOT_FOUND,
            "Cannot open macro CSV file: " + filepath,
            "MacroCSVLoader");
    }

    std::vector<MonthlyMacroRecord> records;
    std::string line;

    // Skip header line
    if (!std::getline(file, line)) {
        return make_error<std::vector<MonthlyMacroRecord>>(
            ErrorCode::INVALID_DATA,
            "Macro CSV file is empty: " + filepath,
            "MacroCSVLoader");
    }

    int line_num = 1;
    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) continue;

        std::istringstream ss(line);
        std::string token;
        MonthlyMacroRecord rec;

        try {
            // year
            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing year");
            rec.year = std::stoi(token);

            // month
            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing month");
            rec.month = std::stoi(token);

            // bpgv
            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing bpgv");
            rec.bpgv = std::stod(token);

            // bpgv_ewma
            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing bpgv_ewma");
            rec.bpgv_ewma = std::stod(token);

            // bpgv_percentile
            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing bpgv_percentile");
            rec.bpgv_percentile = std::stod(token);

            // yield_curve_spread
            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing yield_curve_spread");
            rec.yield_curve_spread = std::stod(token);

            // ewma_slope
            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing ewma_slope");
            rec.ewma_slope = std::stod(token);

            // regime_score
            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing regime_score");
            rec.regime_score = std::stod(token);

            // permit_growth
            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing permit_growth");
            rec.permit_growth = std::stod(token);

            // strong_risk_on
            if (!std::getline(ss, token, ',') && !std::getline(ss, token)) {
                throw std::runtime_error("missing strong_risk_on");
            }
            // Handle potential trailing whitespace/newline
            while (!token.empty() && (token.back() == '\r' || token.back() == '\n' || token.back() == ' ')) {
                token.pop_back();
            }
            rec.strong_risk_on = (std::stoi(token) != 0);

            records.push_back(rec);

        } catch (const std::exception& e) {
            return make_error<std::vector<MonthlyMacroRecord>>(
                ErrorCode::INVALID_DATA,
                "Error parsing macro CSV line " + std::to_string(line_num) + ": " + e.what(),
                "MacroCSVLoader");
        }
    }

    if (records.empty()) {
        return make_error<std::vector<MonthlyMacroRecord>>(
            ErrorCode::INVALID_DATA,
            "Macro CSV file contains no data rows: " + filepath,
            "MacroCSVLoader");
    }

    // Sort by (year, month) — should already be sorted but be defensive
    std::sort(records.begin(), records.end(), [](const MonthlyMacroRecord& a, const MonthlyMacroRecord& b) {
        return (a.year < b.year) || (a.year == b.year && a.month < b.month);
    });

    return records;
}

std::optional<MonthlyMacroRecord> MacroCSVLoader::find_record(
    const std::vector<MonthlyMacroRecord>& records, int year, int month) {

    // Binary search for exact match
    auto it = std::lower_bound(records.begin(), records.end(), std::make_pair(year, month),
        [](const MonthlyMacroRecord& rec, const std::pair<int, int>& target) {
            return (rec.year < target.first) ||
                   (rec.year == target.first && rec.month < target.second);
        });

    if (it != records.end() && it->year == year && it->month == month) {
        return *it;
    }
    return std::nullopt;
}

std::optional<MonthlyMacroRecord> MacroCSVLoader::find_record_before(
    const std::vector<MonthlyMacroRecord>& records, int year, int month) {

    if (records.empty()) return std::nullopt;

    // Find first record strictly after (year, month)
    auto it = std::upper_bound(records.begin(), records.end(), std::make_pair(year, month),
        [](const std::pair<int, int>& target, const MonthlyMacroRecord& rec) {
            return (target.first < rec.year) ||
                   (target.first == rec.year && target.second < rec.month);
        });

    // The record at or before is the one just before 'it'
    if (it == records.begin()) {
        return std::nullopt;
    }
    --it;
    return *it;
}

}  // namespace trade_ngin
