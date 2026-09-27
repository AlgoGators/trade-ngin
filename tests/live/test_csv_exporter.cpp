// Fixture coverage for current-position CSV exports and HTML tables, plus
// standalone helpers and constructor/setter paths. Other full export paths
// may require live DB dependencies and are documented in deliverables/unit_testing/.

#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>

// Pre-load std headers BEFORE flipping private→public so libc++ internals
// stay valid.
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#define private public
#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/live/csv_exporter.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#undef private

using namespace trade_ngin;

namespace {

std::chrono::system_clock::time_point at_local(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

class RegistryInstrumentsGuard {
public:
    RegistryInstrumentsGuard() : registry_(InstrumentRegistry::instance()) {
        std::lock_guard<std::mutex> lock(registry_.mutex_);
        original_ = registry_.instruments_;
    }

    ~RegistryInstrumentsGuard() {
        std::lock_guard<std::mutex> lock(registry_.mutex_);
        registry_.instruments_ = std::move(original_);
    }

    void install(const std::string& symbol, const FuturesSpec& spec) {
        std::lock_guard<std::mutex> lock(registry_.mutex_);
        registry_.instruments_[symbol] = std::make_shared<FuturesInstrument>(symbol, spec);
    }

private:
    InstrumentRegistry& registry_;
    std::unordered_map<std::string, std::shared_ptr<Instrument>> original_;
};

Position position_with_raw_quantity(const std::string& symbol, int64_t raw) {
    return Position(symbol, Quantity::from_raw(raw), Price(10.0), Decimal(0.0),
                    Decimal(0.0), at_local(2026, 4, 28));
}

std::string html_row_for_symbol(const std::string& html, const std::string& symbol,
                                size_t from = 0) {
    const auto start = html.find("<td>" + symbol + "</td>", from);
    if (start == std::string::npos) return {};
    const auto row_start = html.rfind("<tr>\n", start);
    const auto end = html.find("</tr>\n", start);
    if (row_start == std::string::npos || end == std::string::npos) return {};
    return html.substr(row_start, end + 6 - row_start);
}

std::vector<std::string> csv_fields(const std::string& row) {
    std::vector<std::string> fields;
    std::istringstream in(row);
    std::string field;
    while (std::getline(in, field, ',')) fields.push_back(field);
    return fields;
}

std::vector<std::vector<std::string>> csv_data_rows(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);  // Portfolio summary.
    std::getline(in, line);  // Column names.
    std::vector<std::vector<std::string>> rows;
    while (std::getline(in, line)) rows.push_back(csv_fields(line));
    return rows;
}

}  // namespace

class CSVExporterTest : public ::testing::Test {
protected:
    void SetUp() override {
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path() /
               ("csv_exporter_" + std::string(info->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }
    std::filesystem::path dir_;
};

TEST_F(CSVExporterTest, ConstructorDefaultAppendsTrailingSlash) {
    CSVExporter exp;
    EXPECT_EQ(exp.output_directory_, "./");
}

TEST_F(CSVExporterTest, ConstructorWithExplicitDirectoryAppendsTrailingSlash) {
    CSVExporter exp(dir_.string());
    EXPECT_TRUE(exp.output_directory_.starts_with(dir_.string()));
    EXPECT_EQ(exp.output_directory_.back(), '/');
}

TEST_F(CSVExporterTest, ConstructorPreservesTrailingSlashIfPresent) {
    CSVExporter exp("/tmp/already_has_slash/");
    EXPECT_EQ(exp.output_directory_, "/tmp/already_has_slash/");
}

TEST_F(CSVExporterTest, SetOutputDirectoryAppendsTrailingSlashIfMissing) {
    CSVExporter exp;
    exp.set_output_directory(dir_.string());
    EXPECT_EQ(exp.output_directory_.back(), '/');
}

// ===== format_date_for_filename / format_date_for_display =====

TEST_F(CSVExporterTest, FormatDateForFilenameYYYYMMDD) {
    CSVExporter exp;
    auto s = exp.format_date_for_filename(at_local(2026, 4, 28));
    EXPECT_EQ(s, "2026-04-28");
}

TEST_F(CSVExporterTest, FormatDateForDisplayYYYYMMDD) {
    CSVExporter exp;
    auto s = exp.format_date_for_display(at_local(2026, 1, 5));
    EXPECT_EQ(s, "2026-01-05");
}

TEST_F(CSVExporterTest, FormatDateZeroPadsMonthAndDay) {
    CSVExporter exp;
    EXPECT_EQ(exp.format_date_for_filename(at_local(2026, 9, 7)), "2026-09-07");
}

// ===== get_clean_symbol =====

TEST_F(CSVExporterTest, GetCleanSymbolStripsVariantSuffix) {
    CSVExporter exp;
    EXPECT_EQ(exp.get_clean_symbol("ES.v.0"), "ES");
}

TEST_F(CSVExporterTest, GetCleanSymbolStripsContinuousSuffix) {
    CSVExporter exp;
    EXPECT_EQ(exp.get_clean_symbol("ES.c.0"), "ES");
}

TEST_F(CSVExporterTest, GetCleanSymbolPlainSymbolUnchanged) {
    CSVExporter exp;
    EXPECT_EQ(exp.get_clean_symbol("ES"), "ES");
}

// ===== format_strategy_display_name =====

TEST_F(CSVExporterTest, FormatStrategyDisplayNameTitleCases) {
    CSVExporter exp;
    auto s = exp.format_strategy_display_name("TREND_FOLLOWING_FAST");
    // The exact format isn't part of the public contract, but it should
    // produce a non-empty human-friendly string.
    EXPECT_FALSE(s.empty());
    // Should not contain the raw underscore-uppercase form.
    EXPECT_NE(s, "TREND_FOLLOWING_FAST");
}

// ===== calculate_notional =====

TEST_F(CSVExporterTest, CalculateNotionalForUnknownSymbolThrows) {
    CSVExporter exp;
    EXPECT_THROW(exp.calculate_notional("UNKNOWN_SYMBOL_XYZ", 2.0, 100.0),
                 std::runtime_error);
}

// ===== write_portfolio_header =====

TEST_F(CSVExporterTest, WritePortfolioHeaderProducesCommentLines) {
    CSVExporter exp;
    auto path = dir_ / "header.csv";
    {
        std::ofstream f(path);
        exp.write_portfolio_header(f, /*portfolio=*/100000.0, /*gross=*/200000.0,
                                    /*net=*/150000.0, "2026-04-28");
    }
    std::ifstream in(path);
    std::string line;
    bool found_value = false;
    while (std::getline(in, line)) {
        if (line.find("100000") != std::string::npos) found_value = true;
    }
    EXPECT_TRUE(found_value);
}

TEST_F(CSVExporterTest, StrictSnapshotRowsDoNotResurrectClosedUniverseSymbols) {
    // Mutation caught: a QT display map must not grow a zero NG row merely
    // because the strategy can trade NG.
    TrendFollowingStrategy strategy("TREND", StrategyConfig{}, TrendFollowingConfig{}, nullptr);
    strategy.instrument_data_["ES.v.0"] = InstrumentData{};
    strategy.instrument_data_["NG.v.0"] = InstrumentData{};
    RegistryInstrumentsGuard registry;
    const FuturesSpec es{"ES", "CME", "USD", 50.0, 0.25, 0.0, 0.0, 0.0, 1.0,
                         "", std::nullopt, std::nullopt};
    const FuturesSpec ng{"NG", "NYMEX", "USD", 10000.0, 0.001, 0.0, 0.0, 0.0, 1.0,
                         "", std::nullopt, std::nullopt};
    registry.install("ES", es);
    registry.install("NG", ng);
    StrategyPositionsMap snapshot{{"TREND", {{"ES.v.0", Position("ES.v.0", Quantity(2.0),
        Price(5000.0), Decimal(0.0), Decimal(0.0), at_local(2026, 4, 28))}}}};
    StrategyInstancesMap instances{{"TREND", &strategy}};
    CSVExporter exporter(dir_.string());

    const auto default_export = exporter.export_current_positions(
        at_local(2026, 4, 28), snapshot, {{"ES.v.0", 5000.0}, {"NG.v.0", 3.0}},
        1000000.0, 1000000.0, 0.0, instances);
    ASSERT_TRUE(default_export.is_ok());
    std::ifstream default_file(default_export.value());
    const std::string default_contents((std::istreambuf_iterator<char>(default_file)), {});
    EXPECT_NE(default_contents.find("NG.v.0"), std::string::npos);
    const auto default_rows = csv_data_rows(default_export.value());
    const auto missing = std::find_if(default_rows.begin(), default_rows.end(),
        [](const auto& row) { return row.size() > 2 && row[1] == "NG.v.0"; });
    ASSERT_NE(missing, default_rows.end());
    EXPECT_EQ((*missing)[2], "0");

    const auto strict_export = exporter.export_current_positions(
        at_local(2026, 4, 28), snapshot, {{"ES.v.0", 5000.0}, {"NG.v.0", 3.0}},
        1000000.0, 1000000.0, 0.0, instances, true);
    ASSERT_TRUE(strict_export.is_ok());
    std::ifstream strict_file(strict_export.value());
    const std::string strict_contents((std::istreambuf_iterator<char>(strict_file)), {});
    EXPECT_NE(strict_contents.find("ES.v.0"), std::string::npos);
    EXPECT_EQ(strict_contents.find("NG.v.0"), std::string::npos);
}

TEST_F(CSVExporterTest, CurrentCombinedHtmlPreservesExactSignedQuantities) {
    RegistryInstrumentsGuard registry;
    registry.install("ES", FuturesSpec{"ES", "CME", "USD", 50.0, 0.25, 0.0, 1.0,
                                       0.0, 1.0, "", std::nullopt, std::nullopt});
    const std::unordered_map<std::string, Position> positions{
        {"ES.v.0", position_with_raw_quantity("ES.v.0", 123456789)},
        {"ES.v.1", position_with_raw_quantity("ES.v.1", -123456789)},
        {"ES.v.2", position_with_raw_quantity("ES.v.2", 1)},
        {"ES.v.3", position_with_raw_quantity("ES.v.3", -1)},
        {"ES.v.4", position_with_raw_quantity("ES.v.4", 700000000)},
        {"ES.v.5", position_with_raw_quantity("ES.v.5", 0)},
        {"ES.v.6", position_with_raw_quantity("ES.v.6",
            std::numeric_limits<int64_t>::max())},
        {"ES.v.7", position_with_raw_quantity("ES.v.7",
            std::numeric_limits<int64_t>::min())},
    };
    EmailSender sender(EmailSenderConfig{});
    const auto html = sender.format_positions_table(positions, true, {}, {});

    const std::map<std::string, std::string> expected{
        {"ES.v.0", "1.23456789"}, {"ES.v.1", "-1.23456789"},
        {"ES.v.2", "0.00000001"}, {"ES.v.3", "-0.00000001"},
        {"ES.v.4", "7"}, {"ES.v.6", "92233720368.54775807"},
        {"ES.v.7", "-92233720368.54775808"},
    };
    for (const auto& [symbol, quantity] : expected) {
        const auto row = html_row_for_symbol(html, symbol);
        ASSERT_FALSE(row.empty()) << symbol;
        EXPECT_NE(row.find("<td>" + quantity + "</td>"), std::string::npos) << symbol;
        EXPECT_NE(row.find("<td>$10.00</td>"), std::string::npos) << symbol;
    }
    EXPECT_TRUE(html_row_for_symbol(html, "ES.v.5").empty());
    EXPECT_NE(html.find("<tr><th>Symbol</th><th>Quantity</th><th>Market Price</th>"),
              std::string::npos);
}

TEST_F(CSVExporterTest, CurrentStrategyHtmlKeepsSameSymbolQuantitiesSeparate) {
    RegistryInstrumentsGuard registry;
    registry.install("ES", FuturesSpec{"ES", "CME", "USD", 50.0, 0.25, 0.0, 1.0,
                                       0.0, 1.0, "", std::nullopt, std::nullopt});
    const StrategyPositionsMap positions{
        {"ALPHA", {{"ES.v.0", position_with_raw_quantity("ES.v.0", 123456789)}}},
        {"ZETA", {{"ES.v.0", position_with_raw_quantity("ES.v.0", -1)}}},
    };
    EmailSender sender(EmailSenderConfig{});
    const auto html = sender.format_strategy_positions_tables(positions, {}, {});
    const auto first = html_row_for_symbol(html, "ES.v.0");
    const auto second = html_row_for_symbol(
        html, "ES.v.0", html.find("<td>ES.v.0</td>") + 1);

    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    EXPECT_NE(first.find("<td>1.23456789</td>"), std::string::npos);
    EXPECT_NE(second.find("<td>-0.00000001</td>"), std::string::npos);
    EXPECT_NE(first.find("<td>$10.00</td>"), std::string::npos);
    EXPECT_NE(second.find("<td>$10.00</td>"), std::string::npos);
}

TEST_F(CSVExporterTest, CurrentCombinedCsvUsesExactTextOnFirstAndLaterRows) {
    RegistryInstrumentsGuard registry;
    registry.install("ES", FuturesSpec{"ES", "CME", "USD", 50.0, 0.25, 0.0, 1.0,
                                       0.0, 1.0, "", std::nullopt, std::nullopt});
    const std::unordered_map<std::string, Position> positions{
        {"ES.v.0", position_with_raw_quantity("ES.v.0", 123456789)},
        {"ES.v.1", position_with_raw_quantity("ES.v.1", -123456789)},
        {"ES.v.2", position_with_raw_quantity("ES.v.2",
            std::numeric_limits<int64_t>::max())},
        {"ES.v.3", position_with_raw_quantity("ES.v.3",
            std::numeric_limits<int64_t>::min())},
        {"ES.v.4", position_with_raw_quantity("ES.v.4", 1)},
        {"ES.v.5", position_with_raw_quantity("ES.v.5", -1)},
        {"ES.v.6", position_with_raw_quantity("ES.v.6", 0)},
        {"ES.v.7", position_with_raw_quantity("ES.v.7", 700000000)},
    };
    CSVExporter exporter(dir_.string());
    const auto result = exporter.export_current_positions(
        at_local(2026, 4, 28), positions, {}, 1000000.0, 1000000.0, 0.0, nullptr);
    ASSERT_TRUE(result.is_ok());
    {
        std::ifstream in(result.value());
        std::string summary, header;
        ASSERT_TRUE(static_cast<bool>(std::getline(in, summary)));
        ASSERT_TRUE(static_cast<bool>(std::getline(in, header)));
        EXPECT_EQ(summary, "# Portfolio Value: 1000000.00, Gross Notional: 1000000.00, Net Notional: 0.00, Date: 2026-04-28");
        EXPECT_EQ(header, "symbol,quantity,market_price,notional,pct_of_gross_notional,pct_of_portfolio_value,forecast,volatility,ema_8,ema_32,ema_64,ema_256");
    }
    const auto rows = csv_data_rows(result.value());
    ASSERT_EQ(rows.size(), positions.size());
    const std::map<std::string, std::string> expected{
        {"ES.v.0", "1.23456789"}, {"ES.v.1", "-1.23456789"},
        {"ES.v.2", "92233720368.54775807"},
        {"ES.v.3", "-92233720368.54775808"},
        {"ES.v.4", "0.00000001"}, {"ES.v.5", "-0.00000001"},
        {"ES.v.6", "0"}, {"ES.v.7", "7"},
    };
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        ASSERT_EQ(row.size(), 12u);
        ASSERT_TRUE(expected.contains(row[0]));
        EXPECT_EQ(row[1], expected.at(row[0])) << row[0];
        EXPECT_EQ(row[2], i == 0 ? "10.00" : "10.000000") << row[0];
        EXPECT_EQ(row[7], "0.000000") << row[0];
    }
}

TEST_F(CSVExporterTest, CurrentStrategyCsvKeepsSameSymbolQuantitiesSeparate) {
    RegistryInstrumentsGuard registry;
    registry.install("ES", FuturesSpec{"ES", "CME", "USD", 50.0, 0.25, 0.0, 1.0,
                                       0.0, 1.0, "", std::nullopt, std::nullopt});
    const StrategyPositionsMap positions{
        {"ALPHA", {{"ES.v.0", position_with_raw_quantity("ES.v.0", 123456789)}}},
        {"ZETA", {{"ES.v.0", position_with_raw_quantity("ES.v.0", -1)}}},
    };
    CSVExporter exporter(dir_.string());
    const auto result = exporter.export_current_positions(
        at_local(2026, 4, 28), positions, {}, 1000000.0, 1000000.0, 0.0,
        StrategyInstancesMap{}, true);
    ASSERT_TRUE(result.is_ok());
    {
        std::ifstream in(result.value());
        std::string summary, header;
        ASSERT_TRUE(static_cast<bool>(std::getline(in, summary)));
        ASSERT_TRUE(static_cast<bool>(std::getline(in, header)));
        EXPECT_EQ(header, "strategy,symbol,quantity,market_price,notional,pct_of_gross_notional,pct_of_portfolio_value,forecast,volatility,ema_8,ema_32,ema_64,ema_256");
    }
    const auto rows = csv_data_rows(result.value());
    ASSERT_EQ(rows.size(), 2u);
    ASSERT_EQ(rows[0].size(), 13u);
    ASSERT_EQ(rows[1].size(), 13u);
    EXPECT_EQ(rows[0][0], "Alpha");
    EXPECT_EQ(rows[0][1], "ES.v.0");
    EXPECT_EQ(rows[0][2], "1.23456789");
    EXPECT_EQ(rows[1][0], "Zeta");
    EXPECT_EQ(rows[1][1], "ES.v.0");
    EXPECT_EQ(rows[1][2], "-0.00000001");
    EXPECT_EQ(rows[0][3], "10.00");
    EXPECT_EQ(rows[1][3], "10.000000");
}

TEST_F(CSVExporterTest, CurrentCombinedHtmlKeepsEntireRowsAndSummary) {
    RegistryInstrumentsGuard registry;
    registry.install("ES", FuturesSpec{"ES", "CME", "USD", 50.0, 0.25, 0.0, 1.0,
                                       0.0, 1.0, "", std::nullopt, std::nullopt});
    const std::unordered_map<std::string, Position> positions{
        {"ES.v.0", position_with_raw_quantity("ES.v.0", 125000000)},
        {"ES.v.1", position_with_raw_quantity("ES.v.1", -50000000)},
    };
    EmailSender sender(EmailSenderConfig{});
    const auto html = sender.format_positions_table(
        positions, true, {{"ES.v.0", 20.0}}, {{"Portfolio VaR", 2.25}});

    EXPECT_TRUE(html.starts_with("<table>\n<tr><th>Symbol</th><th>Quantity</th>"
        "<th>Market Price</th><th>Notional</th><th>% of Total</th></tr>\n"));
    EXPECT_EQ(html_row_for_symbol(html, "ES.v.0"),
        "<tr>\n<td>ES.v.0</td>\n<td>1.25</td>\n<td>$20.00</td>\n"
        "<td>$625.00</td>\n<td>71.43%</td>\n</tr>\n");
    EXPECT_EQ(html_row_for_symbol(html, "ES.v.1"),
        "<tr>\n<td>ES.v.1</td>\n<td>-0.5</td>\n<td>$10.00</td>\n"
        "<td>$250.00</td>\n<td>28.57%</td>\n</tr>\n");
    const auto summary = html.find("<div class=\"summary-stats\">");
    ASSERT_NE(summary, std::string::npos);
    EXPECT_EQ(html.substr(summary),
        "<div class=\"summary-stats\">\n"
        "<strong>Active Positions:</strong> 2<br>\n"
        "<strong>Portfolio VaR:</strong> 2.25%<br>\n"
        "<strong>Total Notional:</strong> $875.00<br>\n"
        "<strong>Total Margin Posted:</strong> $1.75\n</div>\n");
}

TEST_F(CSVExporterTest, CurrentStrategyHtmlKeepsEntireSameSymbolTablesAndSummaries) {
    RegistryInstrumentsGuard registry;
    registry.install("ES", FuturesSpec{"ES", "CME", "USD", 50.0, 0.25, 0.0, 1.0,
                                       0.0, 1.0, "", std::nullopt, std::nullopt});
    const StrategyPositionsMap positions{
        {"ALPHA", {{"ES.v.0", position_with_raw_quantity("ES.v.0", 125000000)}}},
        {"ZETA", {{"ES.v.0", position_with_raw_quantity("ES.v.0", -50000000)}}},
    };
    EmailSender sender(EmailSenderConfig{});
    const auto html = sender.format_strategy_positions_tables(
        positions, {{"ES.v.0", 20.0}}, {{"Portfolio VaR", 2.25}});

    EXPECT_EQ(html,
        "<h3 style=\"margin-top: 20px; margin-bottom: 10px; color: #333; "
        "border-left: 4px solid #2c5aa0; padding-left: 12px;\">Alpha</h3>\n"
        "<table>\n<tr><th>Symbol</th><th>Quantity</th><th>Market Price</th>"
        "<th>Notional</th><th>% of Total</th></tr>\n"
        "<tr>\n<td>ES.v.0</td>\n<td>1.25</td>\n<td>$20.00</td>\n"
        "<td>$625.00</td>\n<td>100.00%</td>\n</tr>\n</table>\n"
        "<div style=\"font-size: 13px; color: #666; margin: 8px 0 20px 0; "
        "padding-left: 16px;\">\n"
        "<strong>Positions:</strong> 1 | <strong>Notional:</strong> $625.00 | "
        "<strong>Margin:</strong> $1.25\n</div>\n"
        "<h3 style=\"margin-top: 20px; margin-bottom: 10px; color: #333; "
        "border-left: 4px solid #2c5aa0; padding-left: 12px;\">Zeta</h3>\n"
        "<table>\n<tr><th>Symbol</th><th>Quantity</th><th>Market Price</th>"
        "<th>Notional</th><th>% of Total</th></tr>\n"
        "<tr>\n<td>ES.v.0</td>\n<td>-0.5</td>\n<td>$20.00</td>\n"
        "<td>$250.00</td>\n<td>100.00%</td>\n</tr>\n</table>\n"
        "<div style=\"font-size: 13px; color: #666; margin: 8px 0 20px 0; "
        "padding-left: 16px;\">\n"
        "<strong>Positions:</strong> 1 | <strong>Notional:</strong> $250.00 | "
        "<strong>Margin:</strong> $0.50\n</div>\n"
        "<div class=\"summary-stats\" style=\"margin-top: 20px; border-top: 2px solid #2c5aa0; "
        "padding-top: 15px;\">\n"
        "<div class=\"metric\"><strong>Active Positions:</strong> 2</div>\n"
        "<div class=\"metric\"><strong>Portfolio VaR:</strong> 2.25%</div>\n"
        "<div class=\"metric\"><strong>Total Notional:</strong> $875.00</div>\n"
        "<div class=\"metric\"><strong>Total Margin Posted:</strong> $1.75</div>\n"
        "</div>\n");
}

TEST_F(CSVExporterTest, CurrentCombinedCsvKeepsEntireRowAndSummary) {
    RegistryInstrumentsGuard registry;
    registry.install("ES", FuturesSpec{"ES", "CME", "USD", 50.0, 0.25, 0.0, 1.0,
                                       0.0, 1.0, "", std::nullopt, std::nullopt});
    TrendFollowingStrategy strategy("TREND", StrategyConfig{}, TrendFollowingConfig{}, nullptr);
    strategy.instrument_data_["ES.v.0"].current_forecast = 3.5;
    strategy.instrument_data_["ES.v.0"].current_volatility = 0.125;
    strategy.instrument_data_["ES.v.0"].price_history.push_back(20.0);
    const std::unordered_map<std::string, Position> positions{
        {"ES.v.0", position_with_raw_quantity("ES.v.0", 125000000)},
    };
    CSVExporter exporter(dir_.string());
    const auto result = exporter.export_current_positions(
        at_local(2026, 4, 28), positions, {{"ES.v.0", 20.0}}, 20000.0, 10000.0,
        500.0, &strategy);
    ASSERT_TRUE(result.is_ok());
    std::ifstream in(result.value());
    std::string summary, header, row, extra;
    ASSERT_TRUE(static_cast<bool>(std::getline(in, summary)));
    ASSERT_TRUE(static_cast<bool>(std::getline(in, header)));
    ASSERT_TRUE(static_cast<bool>(std::getline(in, row)));
    EXPECT_FALSE(static_cast<bool>(std::getline(in, extra)));
    EXPECT_EQ(summary, "# Portfolio Value: 20000.00, Gross Notional: 10000.00, "
                       "Net Notional: 500.00, Date: 2026-04-28");
    EXPECT_EQ(header, "symbol,quantity,market_price,notional,pct_of_gross_notional,"
                      "pct_of_portfolio_value,forecast,volatility,ema_8,ema_32,ema_64,ema_256");
    EXPECT_EQ(row, "ES.v.0,1.25,20.00,1250.00,12.50,6.25,3.50,0.125000,"
                   "20.000000,20.000000,20.000000,20.000000");
}

TEST_F(CSVExporterTest, CurrentStrategyCsvKeepsEntireSameSymbolRowsAndSummary) {
    RegistryInstrumentsGuard registry;
    registry.install("ES", FuturesSpec{"ES", "CME", "USD", 50.0, 0.25, 0.0, 1.0,
                                       0.0, 1.0, "", std::nullopt, std::nullopt});
    TrendFollowingStrategy alpha("ALPHA", StrategyConfig{}, TrendFollowingConfig{}, nullptr);
    alpha.instrument_data_["ES.v.0"].current_forecast = 3.5;
    alpha.instrument_data_["ES.v.0"].current_volatility = 0.125;
    alpha.instrument_data_["ES.v.0"].price_history.push_back(20.0);
    TrendFollowingStrategy zeta("ZETA", StrategyConfig{}, TrendFollowingConfig{}, nullptr);
    zeta.instrument_data_["ES.v.0"].current_forecast = -2.5;
    zeta.instrument_data_["ES.v.0"].current_volatility = 0.25;
    zeta.instrument_data_["ES.v.0"].price_history.push_back(30.0);
    const StrategyPositionsMap positions{
        {"ALPHA", {{"ES.v.0", position_with_raw_quantity("ES.v.0", 125000000)}}},
        {"ZETA", {{"ES.v.0", position_with_raw_quantity("ES.v.0", -50000000)}}},
    };
    CSVExporter exporter(dir_.string());
    const auto result = exporter.export_current_positions(
        at_local(2026, 4, 28), positions, {{"ES.v.0", 20.0}}, 20000.0, 10000.0,
        500.0, StrategyInstancesMap{{"ALPHA", &alpha}, {"ZETA", &zeta}}, true);
    ASSERT_TRUE(result.is_ok());
    std::ifstream in(result.value());
    std::string summary, header, alpha_row, zeta_row, extra;
    ASSERT_TRUE(static_cast<bool>(std::getline(in, summary)));
    ASSERT_TRUE(static_cast<bool>(std::getline(in, header)));
    ASSERT_TRUE(static_cast<bool>(std::getline(in, alpha_row)));
    ASSERT_TRUE(static_cast<bool>(std::getline(in, zeta_row)));
    EXPECT_FALSE(static_cast<bool>(std::getline(in, extra)));
    EXPECT_EQ(summary, "# Portfolio Value: 20000.00, Gross Notional: 10000.00, "
                       "Net Notional: 500.00, Date: 2026-04-28");
    EXPECT_EQ(header, "strategy,symbol,quantity,market_price,notional,"
                      "pct_of_gross_notional,pct_of_portfolio_value,forecast,volatility,"
                      "ema_8,ema_32,ema_64,ema_256");
    EXPECT_EQ(alpha_row, "Alpha,ES.v.0,1.25,20.00,1250.00,12.50,6.25,3.50,0.125000,"
                         "20.000000,20.000000,20.000000,20.000000");
    EXPECT_EQ(zeta_row, "Zeta,ES.v.0,-0.5,20.000000,-500.000000,5.000000,"
                        "2.500000,-2.500000,0.250000,30.000000,30.000000,"
                        "30.000000,30.000000");
}
