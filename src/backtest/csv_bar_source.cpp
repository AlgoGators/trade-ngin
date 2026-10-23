// src/backtest/csv_bar_source.cpp
#include "trade_ngin/backtest/csv_bar_source.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/market_data_utils.hpp"

namespace trade_ngin::backtest {

namespace {

const char* kComponent = "CsvBarSource";

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    std::stringstream ss(line);
    while (std::getline(ss, field, ',')) out.push_back(trim(field));
    if (!line.empty() && line.back() == ',') out.emplace_back();
    return out;
}

bool parse_double(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

}  // namespace

std::filesystem::path csv_bar_path(const std::filesystem::path& dir, const std::string& symbol) {
    return dir / (symbol + ".csv");
}

Result<std::vector<CsvBarRow>> read_csv_bar_file(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in.is_open()) {
        return make_error<std::vector<CsvBarRow>>(
            ErrorCode::DATA_NOT_FOUND, "CSV bar file not found: " + file.string(), kComponent);
    }

    std::string line;
    if (!std::getline(in, line)) {
        return make_error<std::vector<CsvBarRow>>(
            ErrorCode::INVALID_DATA, "CSV bar file is empty: " + file.string(), kComponent);
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();

    static const std::vector<std::string> kHeader = {"date",   "open",     "high",     "low",
                                                     "close",  "volume",   "div_cash",
                                                     "split_factor"};
    auto header = split_csv(line);
    for (auto& h : header) std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    for (const auto& h : header) {
        if (h == "adj_close" || h == "adjusted_close" || h == "closeadj" || h == "adj close") {
            return make_error<std::vector<CsvBarRow>>(
                ErrorCode::INVALID_DATA,
                "CSV bar file " + file.string() + " carries a vendor-adjusted column '" + h +
                    "'. The contract is raw OHLCV + div_cash + split_factor; regenerate it "
                    "with scripts/seed_etfs_yfinance.py",
                kComponent);
        }
    }
    if (header != kHeader) {
        return make_error<std::vector<CsvBarRow>>(
            ErrorCode::INVALID_DATA,
            "CSV bar file " + file.string() +
                " has an unexpected header; expected exactly "
                "date,open,high,low,close,volume,div_cash,split_factor",
            kComponent);
    }

    std::vector<CsvBarRow> rows;
    size_t line_no = 1;
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        auto fields = split_csv(line);
        if (fields.size() != kHeader.size()) {
            return make_error<std::vector<CsvBarRow>>(
                ErrorCode::INVALID_DATA,
                file.string() + ":" + std::to_string(line_no) + ": expected 8 fields, got " +
                    std::to_string(fields.size()),
                kComponent);
        }
        CsvBarRow row;
        if (!core::parse_utc_date(fields[0], row.time)) {
            return make_error<std::vector<CsvBarRow>>(
                ErrorCode::INVALID_DATA,
                file.string() + ":" + std::to_string(line_no) + ": bad date '" + fields[0] + "'",
                kComponent);
        }
        double* targets[] = {&row.open,   &row.high,     &row.low,         &row.close,
                             &row.volume, &row.div_cash, &row.split_factor};
        for (size_t i = 0; i < 7; ++i) {
            if (!parse_double(fields[i + 1], *targets[i])) {
                return make_error<std::vector<CsvBarRow>>(
                    ErrorCode::INVALID_DATA,
                    file.string() + ":" + std::to_string(line_no) + ": bad number in column '" +
                        kHeader[i + 1] + "': '" + fields[i + 1] + "'",
                    kComponent);
            }
        }
        if (row.close <= 0.0 || row.split_factor <= 0.0 || row.div_cash < 0.0) {
            return make_error<std::vector<CsvBarRow>>(
                ErrorCode::INVALID_DATA,
                file.string() + ":" + std::to_string(line_no) +
                    ": close and split_factor must be > 0 and div_cash >= 0",
                kComponent);
        }
        if (!rows.empty() && !(rows.back().time < row.time)) {
            return make_error<std::vector<CsvBarRow>>(
                ErrorCode::INVALID_DATA,
                file.string() + ":" + std::to_string(line_no) +
                    ": dates must be strictly ascending (duplicate or out-of-order row)",
                kComponent);
        }
        rows.push_back(row);
    }
    return Result<std::vector<CsvBarRow>>(std::move(rows));
}

Result<std::vector<Bar>> load_csv_bars(const std::string& symbol,
                                       const std::filesystem::path& dir,
                                       const Timestamp& start, const Timestamp& end) {
    auto rows_result = read_csv_bar_file(csv_bar_path(dir, symbol));
    if (rows_result.is_error()) {
        return make_error<std::vector<Bar>>(rows_result.error()->code(),
                                            rows_result.error()->what(), kComponent);
    }
    auto& rows = rows_result.value();

    // Prefix up to `end`: the anchor bar is the LAST bar of the requested window, which is
    // what build_equity_adjusted_query() does for the database path.
    std::vector<market_data_utils::AdjustmentBar> prefix;
    size_t kept = 0;
    for (const auto& row : rows) {
        if (row.time > end) break;
        prefix.push_back({row.close, row.div_cash, row.split_factor});
        ++kept;
    }
    auto factors = market_data_utils::compute_backward_adjustment_factors(prefix);

    std::vector<Bar> bars;
    int suspicious = 0;
    for (size_t i = 0; i < kept; ++i) {
        const auto& row = rows[i];
        if (i > 0 && row.split_factor == 1.0) {
            const double jump = std::abs(row.close / rows[i - 1].close - 1.0);
            if (jump > 0.40) ++suspicious;
        }
        if (row.time < start) continue;
        const double f = factors[i];
        bars.emplace_back(row.time, Price(row.open * f), Price(row.high * f), Price(row.low * f),
                          Price(row.close * f), row.volume, symbol);
    }
    if (suspicious > 0) {
        WARN("CSV bars for " + symbol + ": " + std::to_string(suspicious) +
             " raw close move(s) above 40% with split_factor 1 -- possible missing split");
    }
    if (bars.empty()) {
        return make_error<std::vector<Bar>>(
            ErrorCode::DATA_NOT_FOUND,
            "No CSV bars for " + symbol + " in the requested window (" +
                core::format_utc_date(start) + " .. " + core::format_utc_date(end) + ")",
            kComponent);
    }
    return Result<std::vector<Bar>>(std::move(bars));
}

}  // namespace trade_ngin::backtest
