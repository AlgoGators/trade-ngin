// src/data/macro_csv_loader.cpp
#include "trade_ngin/data/macro_csv_loader.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace trade_ngin {

namespace {

constexpr size_t kRequiredColumns = 10;  // year .. strong_risk_on

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> cells;
    std::istringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) cells.push_back(cell);
    return cells;
}

// Strips a trailing CR (CRLF files), newline or space.
std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

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

    if (!std::getline(file, line)) {  // the header
        return make_error<std::vector<MonthlyMacroRecord>>(
            ErrorCode::INVALID_DATA,
            "Macro CSV file is empty: " + filepath,
            "MacroCSVLoader");
    }

    int line_num = 1;
    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) continue;

        const auto cells = split_csv_line(line);
        MonthlyMacroRecord rec;
        try {
            if (cells.size() < kRequiredColumns) {
                throw std::runtime_error("expected at least " + std::to_string(kRequiredColumns) +
                                         " columns, got " + std::to_string(cells.size()));
            }
            rec.year = std::stoi(cells[0]);
            rec.month = std::stoi(cells[1]);
            rec.bpgv = std::stod(cells[2]);
            rec.bpgv_ewma = std::stod(cells[3]);
            rec.bpgv_percentile = std::stod(cells[4]);
            rec.yield_curve_spread = std::stod(cells[5]);
            rec.ewma_slope = std::stod(cells[6]);
            rec.regime_score = std::stod(cells[7]);
            rec.permit_growth = std::stod(cells[8]);
            rec.strong_risk_on = (std::stoi(trimmed(cells[9])) != 0);

            // Optional GARCH columns: absent or empty keeps the default, anything else must
            // parse. garch_converged (the fifth) is a 0/1 flag.
            const char* names[] = {"bpg_sfh",         "bpgv_sfh",    "bpgv_garch",
                                   "bpgv_garch_percentile", "garch_converged", "garch_alpha",
                                   "garch_beta"};
            double* values[] = {&rec.bpg_sfh, &rec.bpgv_sfh, &rec.bpgv_garch,
                                &rec.bpgv_garch_percentile, nullptr, &rec.garch_alpha,
                                &rec.garch_beta};
            for (size_t k = 0; k < 7 && kRequiredColumns + k < cells.size(); ++k) {
                const std::string value = trimmed(cells[kRequiredColumns + k]);
                if (value.empty()) continue;
                try {
                    if (values[k] != nullptr) {
                        *values[k] = std::stod(value);
                    } else {
                        rec.garch_converged = (std::stoi(value) != 0);
                    }
                } catch (const std::exception&) {
                    throw std::runtime_error(std::string(names[k]) + " is not a number: '" +
                                             value + "'");
                }
            }
        } catch (const std::exception& e) {
            return make_error<std::vector<MonthlyMacroRecord>>(
                ErrorCode::INVALID_DATA,
                "Error parsing macro CSV line " + std::to_string(line_num) + ": " + e.what(),
                "MacroCSVLoader");
        }
        records.push_back(rec);
    }

    if (records.empty()) {
        return make_error<std::vector<MonthlyMacroRecord>>(
            ErrorCode::INVALID_DATA,
            "Macro CSV file contains no data rows: " + filepath,
            "MacroCSVLoader");
    }

    std::sort(records.begin(), records.end(), [](const MonthlyMacroRecord& a, const MonthlyMacroRecord& b) {
        return (a.year < b.year) || (a.year == b.year && a.month < b.month);
    });

    return records;
}

std::optional<MonthlyMacroRecord> MacroCSVLoader::find_record(
    const std::vector<MonthlyMacroRecord>& records, int year, int month) {

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

    // The first record strictly after (year, month); the one before it is the answer.
    auto it = std::upper_bound(records.begin(), records.end(), std::make_pair(year, month),
        [](const std::pair<int, int>& target, const MonthlyMacroRecord& rec) {
            return (target.first < rec.year) ||
                   (target.first == rec.year && target.second < rec.month);
        });

    if (it == records.begin()) {
        return std::nullopt;
    }
    --it;
    return *it;
}

}  // namespace trade_ngin
