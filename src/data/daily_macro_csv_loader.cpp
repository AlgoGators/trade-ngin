// src/data/daily_macro_csv_loader.cpp
#include "trade_ngin/data/daily_macro_csv_loader.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace trade_ngin {

static double parse_double_or_nan(const std::string& token) {
    if (token.empty() || token == "NA" || token == "NaN" || token == "nan" || token == ".") {
        return 0.0;
    }
    return std::stod(token);
}

static std::string strip_whitespace(const std::string& s) {
    std::string out = s;
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

Result<std::vector<DailyMacroRecord>> DailyMacroCSVLoader::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return make_error<std::vector<DailyMacroRecord>>(
            ErrorCode::FILE_NOT_FOUND,
            "Cannot open daily macro CSV file: " + filepath,
            "DailyMacroCSVLoader");
    }

    std::vector<DailyMacroRecord> records;
    std::string line;

    // Skip header line
    if (!std::getline(file, line)) {
        return make_error<std::vector<DailyMacroRecord>>(
            ErrorCode::INVALID_DATA,
            "Daily macro CSV file is empty: " + filepath,
            "DailyMacroCSVLoader");
    }

    int line_num = 1;
    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) continue;

        std::istringstream ss(line);
        std::string token;
        DailyMacroRecord rec;

        try {
            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing year");
            rec.year = std::stoi(token);

            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing month");
            rec.month = std::stoi(token);

            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing day");
            rec.day = std::stoi(token);

            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing dxy");
            rec.dxy = parse_double_or_nan(token);

            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing vix");
            rec.vix = parse_double_or_nan(token);

            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing hy_spread");
            rec.hy_spread = parse_double_or_nan(token);

            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing breakeven_10y");
            rec.breakeven_10y = parse_double_or_nan(token);

            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing yield_10y");
            rec.yield_10y = parse_double_or_nan(token);

            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing tips_10y");
            rec.tips_10y = parse_double_or_nan(token);

            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing spx");
            rec.spx = parse_double_or_nan(token);

            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing fed_balance_sheet");
            rec.fed_balance_sheet = parse_double_or_nan(token);

            if (!std::getline(ss, token, ',')) throw std::runtime_error("missing china_cli");
            rec.china_cli = parse_double_or_nan(token);

            // Last field — may not have trailing comma
            if (!std::getline(ss, token, ',') && !std::getline(ss, token)) {
                throw std::runtime_error("missing cny_usd");
            }
            token = strip_whitespace(token);
            rec.cny_usd = parse_double_or_nan(token);

            records.push_back(rec);

        } catch (const std::exception& e) {
            return make_error<std::vector<DailyMacroRecord>>(
                ErrorCode::INVALID_DATA,
                "Error parsing daily macro CSV line " + std::to_string(line_num) + ": " + e.what(),
                "DailyMacroCSVLoader");
        }
    }

    if (records.empty()) {
        return make_error<std::vector<DailyMacroRecord>>(
            ErrorCode::INVALID_DATA,
            "Daily macro CSV file contains no data rows: " + filepath,
            "DailyMacroCSVLoader");
    }

    // Sort by date_key (YYYYMMDD)
    std::sort(records.begin(), records.end(),
              [](const DailyMacroRecord& a, const DailyMacroRecord& b) {
                  return a.date_key() < b.date_key();
              });

    return records;
}

std::optional<DailyMacroRecord> DailyMacroCSVLoader::find_record(
    const std::vector<DailyMacroRecord>& records, int year, int month, int day) {

    int target = year * 10000 + month * 100 + day;

    auto it = std::lower_bound(records.begin(), records.end(), target,
        [](const DailyMacroRecord& rec, int key) {
            return rec.date_key() < key;
        });

    if (it != records.end() && it->date_key() == target) {
        return *it;
    }
    return std::nullopt;
}

std::optional<DailyMacroRecord> DailyMacroCSVLoader::find_record_before(
    const std::vector<DailyMacroRecord>& records, int year, int month, int day) {

    if (records.empty()) return std::nullopt;

    int target = year * 10000 + month * 100 + day;

    // Find first record strictly after target
    auto it = std::upper_bound(records.begin(), records.end(), target,
        [](int key, const DailyMacroRecord& rec) {
            return key < rec.date_key();
        });

    // The record at or before is the one just before 'it'
    if (it == records.begin()) {
        return std::nullopt;
    }
    --it;
    return *it;
}

}  // namespace trade_ngin
