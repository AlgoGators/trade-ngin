// LOOP_SPEC v6.2 section 9, the three worked days, through the engine: one test a day feeds the
// section's inputs (the illustrative covariance, the prices, the forecasts and slow forecasts, the
// held books, E_1 = E_2 = 500,000 and E_3 = 475,500) to the engine's own code and asserts every
// digit the three tables print.
//
// What runs. The equity slow rule is trend_estimator::equity_slow_ruled, N* is
// TrendFollowingStrategy::calculate_position, the sizing capital is half_compounded_capital, and
// everything from the capped target to the stored book, the fills and risk_detail is
// PortfolioManager::process_market_data on a book that names an overlay sleeve, that is
// rebalance_one_pass and one_pass::rebalance. The values asserted are read from the engine's own
// record of the rebalance (optimization/one_pass_record.hpp, the file the acceptance comparison
// reads), from its record of the day (OnePassDay, risk_detail_json) and from the fills it books.
//
// The covariance. Section 9 gives one illustrative covariance for the overlay's window and for
// the optimiser. The engine estimates both from returns, so the test feeds it 200 dates of returns
// whose sample covariance IS that matrix: four orthonormal zero-sum columns mixed by the matrix's
// Cholesky factor and scaled by the engine's own annualisation of 200 consecutive dates. No
// covariance is injected and no engine code is replaced. The cost of one contract is 1.50 with no
// spread and no impact, registered with the manager's cost model.
//
// Not engine outputs, and not asserted: the uncapped readings (printed "for the record"), the
// exhaustive optimum and its gap (the oracle's diagnostic search), and the order of the search's
// passes (the count is asserted).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#define private public
#define protected public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#undef protected
#undef private

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "one_pass_test_fixture.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/portfolio/sizing_capital.hpp"
#include "trade_ngin/risk/overlay.hpp"
#include "trade_ngin/risk/risk_detail.hpp"
#include "trade_ngin/strategy/trend_estimator.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

// Section 9's inputs, in its column order.
const std::vector<std::string> kSymbols = {"ZT", "MNQ", "MES", "M2K"};
const std::vector<double> kPrice = {104.13, 24772.0, 6706.0, 2453.0};
const std::vector<double> kMultiplier = {2000.0, 2.0, 5.0, 5.0};
const std::vector<double> kSigma = {0.030, 0.240, 0.160, 0.260};
const double kCorrelation[4][4] = {{1.0, -0.2, -0.2, -0.2},
                                   {-0.2, 1.0, 0.9, 0.8},
                                   {-0.2, 0.9, 1.0, 0.8},
                                   {-0.2, 0.8, 0.8, 1.0}};
const std::vector<std::string> kEquityIndex = {"M2K", "MES", "MNQ", "MYM"};
const std::vector<std::pair<int, int>> kPairs = {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
const std::vector<std::pair<int, int>> kSlowPairs = {{32, 128}, {64, 256}};
constexpr double kStart = 500'000.0;
constexpr int kDates = 200;
constexpr double kLastDay = 20600.0;
const char* const kSleeve = "TREND";

// A printed figure: the engine's value rounded to the printed decimals is the printed figure.
#define EXPECT_PRINTED(value, printed, decimals) \
    EXPECT_NEAR((value), (printed), 0.5 * std::pow(10.0, -(decimals)) + 1e-12) << #value

struct Row {
    std::map<std::string, std::string> cells;
    double num(const std::string& key) const { return std::stod(cells.at(key)); }
    const std::string& text(const std::string& key) const { return cells.at(key); }
};

std::vector<Row> read_csv(const std::filesystem::path& path) {
    std::vector<Row> out;
    std::ifstream in(path);
    std::string line;
    std::vector<std::string> header;
    while (std::getline(in, line)) {
        std::vector<std::string> cells;
        std::stringstream ss(line);
        for (std::string cell; std::getline(ss, cell, ',');) cells.push_back(cell);
        if (header.empty()) {
            header = cells;
            continue;
        }
        Row row;
        for (size_t i = 0; i < header.size() && i < cells.size(); ++i) row.cells[header[i]] = cells[i];
        out.push_back(row);
    }
    return out;
}

// What a day's forecasts are: the combined forecast, and the two slow speeds' scaled forecasts
// where section 9 gives them (an equity-index short).
struct Forecast {
    double combined;
    double slow_32_128{1.0};
    double slow_64_256{1.0};
};

// One day's answer, per symbol in section 9's column order, and the day's own figures.
struct Day {
    std::vector<double> ruled, n_star, u, capped, scaled, held, search, pre_trim, book, sign_fill, rest_fill;
    std::vector<int> free, fixed, band, sign_closed, cap_bound, clipped;
    Row day;
    OnePassDay record;
    nlohmann::json risk_detail;
    double stored_scale{0.0};
    std::vector<double> fills;  // the signed contracts filled, summed per symbol
    double fill_cost{0.0};
    int fill_contracts{0};
    std::string log;
};

}  // namespace

class WorkedDays : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://worked_days");
        ASSERT_TRUE(db_->connect().is_ok());
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::INFO;
        Logger::instance().initialize(lc);

        static int n = 0;
        record_dir_ = std::filesystem::temp_directory_path() /
                      ("trade_ngin_worked_days_" + std::to_string(::getpid()) + "_" + std::to_string(++n));
        std::filesystem::remove_all(record_dir_);
        std::filesystem::create_directories(record_dir_);
        ::setenv("TRADE_NGIN_SERIES_DUMP_DIR", record_dir_.c_str(), 1);

        // The book: one sleeve, the overlay sleeve, the locked design's constants (one_pass_config:
        // tau 0.20, cap 2, the section 12 limits, cost multiplier 100, band 2, floor 0.05, trim 5).
        pm_id_ = "PM_WORKED_" + std::to_string(n);
        pm_ = std::make_unique<PortfolioManager>(one_pass_config(kSleeve, kStart), pm_id_);
        pm_->set_backtest_mode(false);  // the held book is the book the day starts with
        sleeve_ = make_overlay_stub(kSleeve, kStart, db_);
        ASSERT_TRUE(sleeve_->initialize().is_ok());
        ASSERT_TRUE(sleeve_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(sleeve_, 1.0, true).is_ok());

        // 1.50 a contract, no spread, no impact.
        for (const auto& symbol : kSymbols) {
            transaction_cost::AssetCostConfig cost;
            cost.symbol = symbol;
            cost.commission_per_unit = 1.50;
            cost.spread_cost_multiplier = 0.0;
            cost.max_impact_bps = 0.0;
            pm_->cost_manager_.register_asset_config(cost);
        }

        // The sleeve whose position formula sizes N*: the engine's trend sleeve with section 9's
        // weights (0.25 each), IDM 2.5 and tau 0.20.
        StrategyConfig sc;
        sc.capital_allocation = kStart;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        TrendFollowingConfig tc;
        tc.risk_target = 0.20;
        tc.idm = 2.5;
        tc.ema_windows = kPairs;
        sizer_ = std::make_shared<TrendFollowingStrategy>("WORKED_SIZER", sc, tc, db_);
        for (size_t i = 0; i < kSymbols.size(); ++i) {
            sizer_->instrument_data_[kSymbols[i]].contract_size = kMultiplier[i];
            sizer_->instrument_data_[kSymbols[i]].weight = 0.25;
        }
        build_window();
    }

    void TearDown() override {
        ::unsetenv("TRADE_NGIN_SERIES_DUMP_DIR");
        std::filesystem::remove_all(record_dir_);
        pm_.reset();
        sleeve_.reset();
        sizer_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    // 200 consecutive dates of returns whose sample covariance, annualised as the engine
    // annualises 200 consecutive dates, is section 9's covariance; and the 201 closes that have
    // exactly those returns, ending at each symbol's price.
    void build_window() {
        const int n = kDates;
        double sigma[4][4], lower[4][4] = {};
        for (int a = 0; a < 4; ++a) {
            for (int b = 0; b < 4; ++b) sigma[a][b] = kCorrelation[a][b] * kSigma[a] * kSigma[b];
        }
        for (int a = 0; a < 4; ++a) {  // Cholesky: sigma = lower lower'
            for (int b = 0; b <= a; ++b) {
                double sum = sigma[a][b];
                for (int k = 0; k < b; ++k) sum -= lower[a][k] * lower[b][k];
                lower[a][b] = a == b ? std::sqrt(sum) : sum / lower[b][b];
            }
        }
        // the engine's annualisation of n consecutive dates (overlay.cpp, gate_window)
        const double bars_per_year = static_cast<double>(n - 1) / ((n - 1) / overlay::kDaysPerYear);
        const double pi = std::acos(-1.0);
        const double scale = std::sqrt(static_cast<double>(n - 1) / bars_per_year);
        for (size_t i = 0; i < kSymbols.size(); ++i) {
            OverlayStubStrategy::Row row;
            row.close = kPrice[i];
            row.multiplier = kMultiplier[i];
            row.jump_sigma_daily = 2.5 * kSigma[i] / std::sqrt(bars_per_year);
            for (int d = 0; d < n; ++d) {
                double r = 0.0;
                for (int k = 0; k <= static_cast<int>(i); ++k) {
                    // column k + 1 of the orthonormal cosine basis: sums to zero over the dates
                    const double q = std::sqrt(2.0 / n) * std::cos(pi * (k + 1) * (2 * d + 1) / (2.0 * n));
                    r += lower[i][k] * q;
                }
                row.day.push_back(kLastDay - (n - 1 - d));
                row.returns.push_back(scale * r);
            }
            row.opt_day.assign(n + 1, 0.0);
            row.opt_close.assign(n + 1, 0.0);
            double close = kPrice[i];
            for (int d = n; d >= 0; --d) {
                row.opt_day[d] = kLastDay - (n - d);
                row.opt_close[d] = close;
                if (d > 0) close /= 1.0 + row.returns[d - 1];
            }
            window_[kSymbols[i]] = row;
        }
    }

    void hold(const std::vector<double>& quantities) {
        for (size_t i = 0; i < kSymbols.size(); ++i) {
            Position p;
            p.symbol = kSymbols[i];
            p.quantity = Decimal(quantities[i]);
            p.average_price = Decimal(kPrice[i]);
            ASSERT_TRUE(pm_->update_strategy_position(kSleeve, kSymbols[i], p).is_ok());
        }
    }

    // One day: the slow rule, N*, the pass, and everything it stored.
    Day run_day(int index, double capital, double account, const std::vector<Forecast>& forecasts) {
        Day out;
        sizer_->config_.capital_allocation = capital;
        for (size_t i = 0; i < kSymbols.size(); ++i) {
            const bool equity_index =
                std::find(kEquityIndex.begin(), kEquityIndex.end(), kSymbols[i]) != kEquityIndex.end();
            const Forecast& f = forecasts[i];
            const double ruled =
                equity_index ? trend_estimator::equity_slow_ruled(
                                   f.combined, {0.0, 0.0, 0.0, 0.0, f.slow_32_128, f.slow_64_256}, kPairs,
                                   kSlowPairs)
                             : f.combined;
            double n_star = 0.0;
            sizer_->calculate_position(kSymbols[i], ruled, kPrice[i], kSigma[i], &n_star);
            out.ruled.push_back(ruled);
            out.n_star.push_back(n_star);
            OverlayStubStrategy::Row row = window_.at(kSymbols[i]);
            row.optimal = n_star;
            row.forecast = ruled;
            row.signalling = true;
            row.slow_rule_zeroed = ruled != f.combined;
            sleeve_->rows[kSymbols[i]] = row;
        }
        EXPECT_TRUE(pm_->set_sizing_capital(capital).is_ok());
        std::vector<Bar> bars;
        for (size_t i = 0; i < kSymbols.size(); ++i) {
            bars.push_back(one_pass_bar(kSymbols[i], index, kPrice[i]));
            pm_->update_cost_manager_market_data(kSymbols[i], 100000.0, kPrice[i], kPrice[i]);
        }
        const size_t fills_before = executions().size();
        ::testing::internal::CaptureStdout();
        const auto processed = pm_->process_market_data(bars);
        out.log = ::testing::internal::GetCapturedStdout();
        EXPECT_TRUE(processed.is_ok()) << (processed.is_error() ? processed.error()->what() : "");

        out.record = pm_->last_one_pass();
        out.risk_detail = risk_detail_json(out.record, account);
        std::map<std::string, double> stored;
        const auto books = pm_->get_strategy_positions();
        for (const auto& [symbol, position] : books.at(kSleeve)) {
            stored[symbol] = static_cast<double>(position.quantity);
        }
        out.stored_scale = pm_->delivered_scale_for_book(stored);

        const auto days = read_csv(record_dir_ / ("onepass_days_" + pm_id_ + ".csv"));
        const auto book = read_csv(record_dir_ / ("onepass_book_" + pm_id_ + ".csv"));
        EXPECT_EQ(days.size(), static_cast<size_t>(index));
        EXPECT_EQ(book.size(), static_cast<size_t>(index) * kSymbols.size());
        if (days.size() != static_cast<size_t>(index) || book.size() != days.size() * kSymbols.size()) {
            return out;
        }
        out.day = days.back();
        for (const auto& symbol : kSymbols) {
            const Row* row = nullptr;
            for (size_t k = book.size() - kSymbols.size(); k < book.size(); ++k) {
                if (book[k].text("symbol") == symbol) row = &book[k];
            }
            EXPECT_NE(row, nullptr) << symbol;
            if (row == nullptr) continue;
            out.u.push_back(row->num("u"));
            out.capped.push_back(row->num("capped_target"));
            out.scaled.push_back(row->num("scaled_target"));
            out.held.push_back(row->num("held"));
            out.search.push_back(row->num("search_book"));
            out.pre_trim.push_back(row->num("pre_trim"));
            out.book.push_back(row->num("book"));
            out.sign_fill.push_back(row->num("sign_fill"));
            out.rest_fill.push_back(row->num("rest_fill"));
            out.free.push_back(std::stoi(row->text("free")));
            out.fixed.push_back(std::stoi(row->text("fixed")));
            out.band.push_back(std::stoi(row->text("band")));
            out.sign_closed.push_back(std::stoi(row->text("sign_closed")));
            out.cap_bound.push_back(std::stoi(row->text("cap_bound")));
            out.clipped.push_back(std::stoi(row->text("clipped")));
            EXPECT_EQ(stored[symbol], out.book.back()) << symbol << ": the stored row is the record's";
        }
        // the fills the manager booked today
        out.fills.assign(kSymbols.size(), 0.0);
        const auto all = executions();
        for (size_t k = fills_before; k < all.size(); ++k) {
            const auto& e = all[k];
            const double signed_quantity =
                static_cast<double>(e.filled_quantity) * (e.side == Side::BUY ? 1.0 : -1.0);
            for (size_t i = 0; i < kSymbols.size(); ++i) {
                if (e.symbol == kSymbols[i]) out.fills[i] += signed_quantity;
            }
            EXPECT_EQ(e.execution_type, ExecutionType::STRATEGY);
            EXPECT_EQ(static_cast<double>(e.fill_price), kPrice[index_of(e.symbol)]);
            out.fill_cost += static_cast<double>(e.total_transaction_costs);
            out.fill_contracts += static_cast<int>(std::lround(static_cast<double>(e.filled_quantity)));
        }
        return out;
    }

    std::vector<ExecutionReport> executions() {
        const auto all = pm_->get_strategy_executions();
        const auto it = all.find(kSleeve);
        return it == all.end() ? std::vector<ExecutionReport>{} : it->second;
    }
    static size_t index_of(const std::string& symbol) {
        for (size_t i = 0; i < kSymbols.size(); ++i) {
            if (kSymbols[i] == symbol) return i;
        }
        return 0;
    }
    static std::vector<double> times(const std::vector<double>& a, const std::vector<double>& b) {
        std::vector<double> out;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) out.push_back(a[i] * b[i]);
        return out;
    }
    static void expect_printed(const std::vector<double>& values, const std::vector<double>& printed,
                               int decimals, const std::string& what) {
        ASSERT_EQ(values.size(), printed.size()) << what;
        for (size_t i = 0; i < values.size(); ++i) {
            EXPECT_NEAR(values[i], printed[i], 0.5 * std::pow(10.0, -decimals) + 1e-12)
                << what << " " << kSymbols[i];
        }
    }
    // The readings of the capped target, the raw multipliers limit / reading, m and its term; then
    // the re-read of the stored book.
    static void expect_readings(const Day& d, const std::vector<double>& target,
                                const std::vector<double>& ratios, const std::vector<double>& stored) {
        const char* const names[] = {"R", "R_jump", "R_shock", "L", "L_net"};
        const double limits[] = {0.45, 0.90, 0.80, 8.0, 6.0};
        for (int k = 0; k < 5; ++k) {
            EXPECT_PRINTED(d.day.num(names[k]), target[k], 6);
            EXPECT_PRINTED(limits[k] / d.day.num(names[k]), ratios[k], 6);
            EXPECT_PRINTED(d.day.num(std::string("stored_") + names[k]), stored[k], 6);
            EXPECT_LE(d.day.num(std::string("stored_") + names[k]), limits[k]) << "none above its limit";
        }
        EXPECT_PRINTED(d.day.num("m"), 1.000000, 6);
        EXPECT_EQ(d.day.text("binding"), "none");
        EXPECT_EQ(d.day.text("over_limit"), "-") << "no trim, no mark";
        EXPECT_EQ(d.day.text("mode"), "complete");
    }
    // risk_detail (section 7.3): nine keys, the request 1, no mark, the two capital figures.
    static void expect_risk_detail(const Day& d, double capital, double account) {
        ASSERT_TRUE(d.record.stores_detail());
        EXPECT_EQ(d.risk_detail.size(), 9u);
        EXPECT_PRINTED(d.risk_detail.at("risk_requested").get<double>(), 1.000000, 6);
        EXPECT_EQ(d.risk_detail.at("binding_term"), "none");
        EXPECT_TRUE(d.risk_detail.at("over_limit_after_rounding_terms").is_null());
        EXPECT_TRUE(d.risk_detail.at("over_limit_after_rounding_excess").is_null());
        EXPECT_TRUE(d.risk_detail.at("over_limit_by_hold_terms").is_null());
        EXPECT_TRUE(d.risk_detail.at("over_limit_by_hold_symbols").is_null());
        EXPECT_EQ(d.risk_detail.at("overlay_blind"), false);
        EXPECT_PRINTED(d.risk_detail.at("sizing_capital").get<double>(), capital, 2);
        EXPECT_PRINTED(d.risk_detail.at("account_value").get<double>(), account, 2);
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::shared_ptr<OverlayStubStrategy> sleeve_;
    std::shared_ptr<TrendFollowingStrategy> sizer_;
    std::map<std::string, OverlayStubStrategy::Row> window_;
    std::filesystem::path record_dir_;
    std::string pm_id_;

    // The three days' inputs: day 2 starts from day 1's stored book, day 3 from day 2's, as the
    // tables do. Each test runs the days before its own.
    Day day_one() {
        hold({4.0, 1.0, 2.0, -3.0});
        return run_day(1, 500'000.0, 500'000.0, {{12.0}, {8.0}, {6.0}, {-4.0, -5.0, -3.0}});
    }
    Day day_two() {
        return run_day(2, 500'000.0, 506'000.0, {{16.0}, {2.0}, {6.0}, {-4.0, -5.0, -3.0}});
    }
    Day day_three() {
        return run_day(3, 475'500.0, 481'500.0,
                       {{16.0}, {-1.0, -2.0, -1.0}, {-3.0, -4.0, -2.0}, {-4.0, -6.0, 1.0}});
    }
};

// 9.1 Day 1: F = ZT +12, MNQ +8, MES +6, M2K -4 (M2K slow -5, -3); held ZT 4, MNQ 1, MES 2,
// M2K -3; E_1 = 500,000.
TEST_F(WorkedDays, DayOne) {
    // sizing capital E_t (3.1)
    const HalfCompounding capital = half_compounded_capital(kStart, {});
    EXPECT_PRINTED(capital.capital, 500'000.00, 2);
    EXPECT_PRINTED(capital.account, 500'000.00, 2);
    EXPECT_PRINTED(capital.account - capital.capital, 0.00, 2);

    const Day d = day_one();
    ASSERT_EQ(d.book.size(), 4u);
    // u_i = M P / E_t, L / u, the TE cost term
    expect_printed(d.u, {0.416520, 0.099088, 0.067060, 0.024530}, 6, "u");
    const std::vector<double> cap = {4.801690, 20.184079, 29.824038, 81.532817};
    const std::vector<double> cap_whole = {4, 20, 29, 81};
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_PRINTED(2.0 / d.u[i], cap[i], 6);
        EXPECT_EQ(std::floor(2.0 / d.u[i]), cap_whole[i]);
    }
    ASSERT_DOUBLE_EQ(pm_->cost_manager_.calculate_costs("ZT", 1.0, 104.13).total_transaction_costs, 1.50);
    EXPECT_PRINTED(100.0 * 1.50 / d.day.num("capital"), 0.000300, 6);
    // equity slow rule (2.5): acts on no symbol
    EXPECT_EQ(d.ruled, (std::vector<double>{12.0, 8.0, 6.0, -4.0}));
    EXPECT_EQ(d.log.find("slow_rule_zeroed=[-]") != std::string::npos, true) << d.log;
    // N* (3)
    expect_printed(d.n_star, {12.004225, 4.205016, 6.990009, -7.839694}, 6, "N*");
    const auto n_star_u = times(d.n_star, d.u);
    expect_printed(n_star_u, {5.000000, 0.416667, 0.468750, -0.192308}, 6, "N* u");
    EXPECT_PRINTED(std::abs(n_star_u[0]) + std::abs(n_star_u[1]) + std::abs(n_star_u[2]) + std::abs(n_star_u[3]),
                   6.077724, 6);
    EXPECT_PRINTED(n_star_u[0] + n_star_u[1] + n_star_u[2] + n_star_u[3], 5.693109, 6);
    // deferral band (5.2): holds nothing
    EXPECT_EQ(d.band, (std::vector<int>{0, 0, 0, 0}));
    EXPECT_EQ(d.free, (std::vector<int>{1, 1, 1, 1}));
    // N*c (4) and x = N*c u
    expect_printed(d.capped, {4.801690, 4.205016, 6.990009, -7.839694}, 6, "N*c");
    EXPECT_EQ(d.cap_bound, (std::vector<int>{1, 0, 0, 0})) << "ZT capped";
    expect_printed(times(d.capped, d.u), {2.000000, 0.416667, 0.468750, -0.192308}, 6, "x");
    EXPECT_PRINTED(d.day.num("target_gross"), 3.077724, 6);
    // readings, capped (4); multipliers; trim re-read (6.4)
    expect_readings(d, {0.135000, 0.337500, 0.285000, 3.077724, 2.693109},
                    {3.333333, 2.666667, 2.807018, 2.599323, 2.227908},
                    {0.112200, 0.280500, 0.219914, 2.453322, 2.208022});
    // N~ = m_t N*c
    expect_printed(d.scaled, {4.801690, 4.205016, 6.990009, -7.839694}, 6, "N~");
    expect_printed(times(d.scaled, d.u), {2.000000, 0.416667, 0.468750, -0.192308}, 6, "x~");
    // held h
    EXPECT_EQ(d.held, (std::vector<double>{4.0, 1.0, 2.0, -3.0}));
    expect_printed(times(d.held, d.u), {1.666080, 0.099088, 0.134120, -0.073590}, 6, "h u");
    // sign close (5.2): none
    EXPECT_EQ(d.sign_closed, (std::vector<int>{0, 0, 0, 0}));
    EXPECT_TRUE(d.record.sign_closes.empty());
    // search y (5.2)
    EXPECT_EQ(d.search, (std::vector<double>{4.0, 5.0, 4.0, -6.0}));
    EXPECT_PRINTED(d.day.num("te"), 0.01942709, 8);
    EXPECT_EQ(d.day.text("passes"), "9");
    EXPECT_GT(5.0 * d.u[0], 2.0) << "ZT +1 inadmissible: 2.082600 > 2";
    EXPECT_PRINTED(5.0 * d.u[0], 2.082600, 6);
    // B_sigma (5.3)
    expect_printed(times(d.u, kSigma), {0.012496, 0.023781, 0.010730, 0.006378}, 6, "u sigma");
    EXPECT_PRINTED(d.day.num("B_sigma"), 0.023781, 6);
    EXPECT_EQ(d.day.text("B_symbol"), "MNQ");
    EXPECT_PRINTED(0.05 * 0.20, 0.010000, 6);
    // TE_h, a
    EXPECT_PRINTED(d.day.num("te_h"), 0.099903, 6);
    EXPECT_GT(d.day.num("te_h"), d.day.num("B_sigma"));
    EXPECT_PRINTED(d.day.num("a"), 0.761957, 6);
    // h + a (y - h)
    std::vector<double> moved;
    for (size_t i = 0; i < 4; ++i) moved.push_back(d.held[i] + d.day.num("a") * (d.search[i] - d.held[i]));
    expect_printed(moved, {4.000000, 4.047829, 3.523914, -5.285872}, 6, "h + a (y - h)");
    // rounded; cap clip
    EXPECT_EQ(d.pre_trim, (std::vector<double>{4.0, 4.0, 4.0, -5.0}));
    std::vector<double> weights;
    for (size_t i = 0; i < 4; ++i) weights.push_back(std::abs(d.pre_trim[i]) * d.u[i]);
    expect_printed(weights, {1.666080, 0.396352, 0.268240, 0.122650}, 6, "abs(n) u");
    EXPECT_EQ(d.clipped, (std::vector<int>{0, 0, 0, 0}));
    // fills (6.3)
    EXPECT_EQ(d.fills, (std::vector<double>{0.0, 3.0, 2.0, -2.0}));
    EXPECT_EQ(d.sign_fill, (std::vector<double>{0.0, 0.0, 0.0, 0.0}));
    EXPECT_EQ(d.rest_fill, (std::vector<double>{0.0, 3.0, 2.0, -2.0}));
    EXPECT_EQ(d.fill_contracts, 7);
    EXPECT_PRINTED(d.fill_cost, 10.50, 2);
    // stored (7)
    EXPECT_EQ(d.book, (std::vector<double>{4.0, 4.0, 4.0, -5.0}));
    EXPECT_PRINTED(d.record.risk_requested, 1.000000, 6);
    EXPECT_PRINTED(d.day.num("stored_gross"), 2.453322, 6);
    EXPECT_PRINTED(d.record.risk_scale, 0.797122, 6);
    EXPECT_PRINTED(d.stored_scale, 0.797122, 6);
    // risk_detail (7.3)
    expect_risk_detail(d, 500'000.00, 500'000.00);
}

// 9.2 Day 2: F = ZT +16, MNQ +2, MES +6, M2K -4 (M2K slow -5, -3); held = day 1's stored book
// ZT 4, MNQ 4, MES 4, M2K -5; E_2 = 500,000.
TEST_F(WorkedDays, DayTwo) {
    // sizing capital E_t (3.1): day 1's settled net of +6,000 is set aside
    const HalfCompounding capital = half_compounded_capital(kStart, {6'000.0});
    EXPECT_PRINTED(capital.capital, 500'000.00, 2);
    EXPECT_PRINTED(capital.account, 506'000.00, 2);
    EXPECT_PRINTED(capital.account - capital.capital, 6'000.00, 2);
    EXPECT_PRINTED(capital.cumulative, 6'000.0, 2);
    EXPECT_PRINTED(capital.peak, 6'000.0, 2);
    EXPECT_PRINTED(std::min(kStart, 500'000.0 + 6'000.0), 500'000.0, 2);

    ASSERT_EQ(day_one().book, (std::vector<double>{4.0, 4.0, 4.0, -5.0}));
    const Day d = day_two();
    ASSERT_EQ(d.book.size(), 4u);
    expect_printed(d.u, {0.416520, 0.099088, 0.067060, 0.024530}, 6, "u");
    EXPECT_PRINTED(100.0 * 1.50 / d.day.num("capital"), 0.000300, 6);
    // equity slow rule (2.5)
    EXPECT_EQ(d.ruled, (std::vector<double>{16.0, 2.0, 6.0, -4.0}));
    // N* (3)
    expect_printed(d.n_star, {16.005634, 1.051254, 6.990009, -7.839694}, 6, "N*");
    const auto n_star_u = times(d.n_star, d.u);
    expect_printed(n_star_u, {6.666667, 0.104167, 0.468750, -0.192308}, 6, "N* u");
    EXPECT_PRINTED(std::abs(n_star_u[0]) + std::abs(n_star_u[1]) + std::abs(n_star_u[2]) + std::abs(n_star_u[3]),
                   7.431891, 6);
    EXPECT_PRINTED(n_star_u[0] + n_star_u[1] + n_star_u[2] + n_star_u[3], 7.047276, 6);
    // deferral band (5.2)
    EXPECT_EQ(d.band, (std::vector<int>{0, 0, 0, 0}));
    // N*c (4)
    expect_printed(d.capped, {4.801690, 1.051254, 6.990009, -7.839694}, 6, "N*c");
    EXPECT_EQ(d.cap_bound, (std::vector<int>{1, 0, 0, 0}));
    expect_printed(times(d.capped, d.u), {2.000000, 0.104167, 0.468750, -0.192308}, 6, "x");
    EXPECT_PRINTED(d.day.num("target_gross"), 2.765224, 6);
    expect_readings(d, {0.080777, 0.201944, 0.210000, 2.765224, 2.380609},
                    {5.570860, 4.456688, 3.809524, 2.893074, 2.520364},
                    {0.088830, 0.222074, 0.202511, 2.378764, 2.084404});
    expect_printed(d.scaled, {4.801690, 1.051254, 6.990009, -7.839694}, 6, "N~");
    // held h
    EXPECT_EQ(d.held, (std::vector<double>{4.0, 4.0, 4.0, -5.0}));
    expect_printed(times(d.held, d.u), {1.666080, 0.396352, 0.268240, -0.122650}, 6, "h u");
    EXPECT_EQ(d.sign_closed, (std::vector<int>{0, 0, 0, 0}));
    // search y (5.2)
    EXPECT_EQ(d.search, (std::vector<double>{4.0, 2.0, 4.0, -7.0}));
    EXPECT_PRINTED(d.day.num("te"), 0.01751454, 8);
    EXPECT_EQ(d.day.text("passes"), "4");
    // B_sigma, TE_h, a
    EXPECT_PRINTED(d.day.num("B_sigma"), 0.023781, 6);
    EXPECT_EQ(d.day.text("B_symbol"), "MNQ");
    EXPECT_PRINTED(d.day.num("te_h"), 0.058272, 6);
    EXPECT_PRINTED(d.day.num("a"), 0.591891, 6);
    std::vector<double> moved;
    for (size_t i = 0; i < 4; ++i) moved.push_back(d.held[i] + d.day.num("a") * (d.search[i] - d.held[i]));
    expect_printed(moved, {4.000000, 2.816218, 4.000000, -6.183782}, 6, "h + a (y - h)");
    // rounded; cap clip
    EXPECT_EQ(d.pre_trim, (std::vector<double>{4.0, 3.0, 4.0, -6.0}));
    std::vector<double> weights;
    for (size_t i = 0; i < 4; ++i) weights.push_back(std::abs(d.pre_trim[i]) * d.u[i]);
    expect_printed(weights, {1.666080, 0.297264, 0.268240, 0.147180}, 6, "abs(n) u");
    EXPECT_EQ(d.clipped, (std::vector<int>{0, 0, 0, 0}));
    // fills (6.3)
    EXPECT_EQ(d.fills, (std::vector<double>{0.0, -1.0, 0.0, -1.0}));
    EXPECT_EQ(d.fill_contracts, 2);
    EXPECT_PRINTED(d.fill_cost, 3.00, 2);
    // stored (7)
    EXPECT_EQ(d.book, (std::vector<double>{4.0, 3.0, 4.0, -6.0}));
    EXPECT_PRINTED(d.record.risk_requested, 1.000000, 6);
    EXPECT_PRINTED(d.day.num("stored_gross"), 2.378764, 6);
    EXPECT_PRINTED(d.record.risk_scale, 0.860243, 6);
    EXPECT_PRINTED(d.stored_scale, 0.860243, 6);
    expect_risk_detail(d, 500'000.00, 506'000.00);
}

// 9.3 Day 3: F = ZT +16, MNQ -1 (slow -2, -1), MES -3 (slow -4, -2), M2K -4 (slow -6, +1); held =
// day 2's stored book ZT 4, MNQ 3, MES 4, M2K -6; E_3 = 475,500.
TEST_F(WorkedDays, DayThree) {
    // sizing capital E_t (3.1): closed form and recursion
    const HalfCompounding capital = half_compounded_capital(kStart, {6'000.0, -24'500.0});
    EXPECT_PRINTED(capital.cumulative, -18'500.0, 2);
    EXPECT_PRINTED(capital.peak, 6'000.0, 2);
    EXPECT_PRINTED(capital.peak - capital.cumulative, 24'500.0, 2);
    EXPECT_PRINTED(capital.capital, 475'500.00, 2);
    EXPECT_PRINTED(std::min(kStart, std::min(kStart, 500'000.0 + 6'000.0) - 24'500.0), 475'500.00, 2);
    EXPECT_PRINTED(capital.account, 481'500.00, 2);
    EXPECT_PRINTED(capital.account - capital.capital, 6'000.00, 2);
    // the case above the start: had day 2's net been +3,000
    EXPECT_PRINTED(half_compounded_capital(kStart, {6'000.0, 3'000.0}).capital, 500'000.00, 2);
    EXPECT_PRINTED(half_compounded_capital(kStart, {6'000.0, 3'000.0}).account - 500'000.0, 9'000.00, 2);

    ASSERT_EQ(day_one().book, (std::vector<double>{4.0, 4.0, 4.0, -5.0}));
    ASSERT_EQ(day_two().book, (std::vector<double>{4.0, 3.0, 4.0, -6.0}));
    const Day d = day_three();
    ASSERT_EQ(d.book.size(), 4u);
    // u_i, L / u, the TE cost term, E_t IDM w tau
    expect_printed(d.u, {0.437981, 0.104193, 0.070515, 0.025794}, 6, "u");
    const std::vector<double> cap = {4.566407, 19.195059, 28.362660, 77.537709};
    const std::vector<double> cap_whole = {4, 19, 28, 77};
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_PRINTED(2.0 / d.u[i], cap[i], 6);
        EXPECT_EQ(std::floor(2.0 / d.u[i]), cap_whole[i]);
    }
    EXPECT_PRINTED(100.0 * 1.50 / d.day.num("capital"), 0.000315, 6);
    EXPECT_PRINTED(d.day.num("capital") * 2.5 * 0.25 * 0.20, 59'437.5, 1);
    // equity slow rule (2.5): M2K -4 -> 0 (slow -6, +1); MNQ and MES stand (both slow speeds negative)
    EXPECT_EQ(d.ruled, (std::vector<double>{16.0, -1.0, -3.0, 0.0}));
    EXPECT_NE(d.log.find("slow_rule_zeroed=[M2K]"), std::string::npos) << d.log;
    // N* (3)
    expect_printed(d.n_star, {15.221358, -0.499871, -3.323749, 0.000000}, 6, "N*");
    const auto n_star_u = times(d.n_star, d.u);
    expect_printed(n_star_u, {6.666667, -0.052083, -0.234375, 0.000000}, 6, "N* u");
    EXPECT_PRINTED(std::abs(n_star_u[0]) + std::abs(n_star_u[1]) + std::abs(n_star_u[2]) + std::abs(n_star_u[3]),
                   6.953125, 6);
    EXPECT_PRINTED(n_star_u[0] + n_star_u[1] + n_star_u[2] + n_star_u[3], 6.380208, 6);
    double m2k_before_the_rule = 0.0;
    sizer_->calculate_position("M2K", -4.0, 2453.0, 0.260, &m2k_before_the_rule);
    EXPECT_PRINTED(m2k_before_the_rule, -7.455549, 6);
    // deferral band (5.2): MNQ HELD at +3 (abs(F) 1 < 2); MES free (abs(F) 3 >= 2); M2K no (F 0)
    EXPECT_EQ(d.band, (std::vector<int>{0, 1, 0, 0}));
    EXPECT_EQ(d.fixed, (std::vector<int>{0, 1, 0, 0}));
    EXPECT_EQ(d.free, (std::vector<int>{1, 0, 1, 1}));
    EXPECT_NE(d.log.find("band_holds=[MNQ]"), std::string::npos) << d.log;
    // N*c (4): ZT capped, MNQ a held row, MES, M2K 0
    EXPECT_PRINTED(d.capped[0], 4.566407, 6);
    EXPECT_EQ(d.cap_bound[0], 1);
    EXPECT_PRINTED(d.capped[2], -3.323749, 6);
    EXPECT_PRINTED(d.capped[3], 0.000000, 6);
    // x: N*c u on the free rows, h u on the held row
    expect_printed({d.capped[0] * d.u[0], d.held[1] * d.u[1], d.capped[2] * d.u[2], d.capped[3] * d.u[3]},
                   {2.000000, 0.312580, -0.234375, 0.000000}, 6, "x");
    EXPECT_PRINTED(d.day.num("target_gross"), 2.546955, 6);
    expect_readings(d, {0.068337, 0.170841, 0.172519, 2.546955, 2.078205},
                    {6.585059, 5.268047, 4.637162, 3.141005, 2.887106},
                    {0.067952, 0.169881, 0.161109, 2.193474, 1.935535});
    // N~ = m_t N*c on the free rows
    EXPECT_PRINTED(d.scaled[0], 4.566407, 6);
    EXPECT_PRINTED(d.scaled[2], -3.323749, 6);
    EXPECT_PRINTED(d.scaled[3], 0.000000, 6);
    // held h
    EXPECT_EQ(d.held, (std::vector<double>{4.0, 3.0, 4.0, -6.0}));
    expect_printed(times(d.held, d.u), {1.751924, 0.312580, 0.282061, -0.154763}, 6, "h u");
    // sign close (5.2): MES +4 to 0 (class SIGN); MNQ held by the band; M2K's zero forecast has no sign
    EXPECT_EQ(d.sign_closed, (std::vector<int>{0, 0, 1, 0}));
    ASSERT_EQ(d.record.sign_closes.count(kSleeve), 1u);
    ASSERT_EQ(d.record.sign_closes.at(kSleeve).size(), 1u);
    EXPECT_EQ(d.record.sign_closes.at(kSleeve).at("MES"), -4.0);
    EXPECT_NE(d.log.find("sign_closes=[MES]"), std::string::npos) << d.log;
    // search y (5.2), from [+4 +3 0 -6]
    EXPECT_EQ(d.search, (std::vector<double>{4.0, 3.0, -2.0, -2.0}));
    EXPECT_PRINTED(d.day.num("te"), 0.01382444, 8);
    EXPECT_EQ(d.day.text("passes"), "6");
    EXPECT_PRINTED(5.0 * d.u[0], 2.189905, 6);
    // B_sigma (5.3): the held row is out, ZT sets it
    expect_printed(times(d.u, kSigma), {0.013139, 0.025006, 0.011282, 0.006706}, 6, "u sigma");
    EXPECT_PRINTED(d.day.num("B_sigma"), 0.013139, 6);
    EXPECT_EQ(d.day.text("B_symbol"), "ZT");
    // TE_h, a
    EXPECT_PRINTED(d.day.num("te_h"), 0.016133, 6);
    EXPECT_GT(d.day.num("te_h"), d.day.num("B_sigma"));
    EXPECT_PRINTED(d.day.num("a"), 0.185567, 6);
    // h + a (y - h), h the closed book [+4 +3 0 -6]
    const std::vector<double> closed = {4.0, 3.0, 0.0, -6.0};
    std::vector<double> moved;
    for (size_t i = 0; i < 4; ++i) moved.push_back(closed[i] + d.day.num("a") * (d.search[i] - closed[i]));
    expect_printed(moved, {4.000000, 3.000000, -0.371135, -5.257730}, 6, "h + a (y - h)");
    // rounded; cap clip
    EXPECT_EQ(d.pre_trim, (std::vector<double>{4.0, 3.0, 0.0, -5.0}));
    std::vector<double> weights;
    for (size_t i = 0; i < 4; ++i) weights.push_back(std::abs(d.pre_trim[i]) * d.u[i]);
    expect_printed(weights, {1.751924, 0.312580, 0.000000, 0.128970}, 6, "abs(n) u");
    EXPECT_EQ(d.clipped, (std::vector<int>{0, 0, 0, 0}));
    // fills (6.3): MES SELL 4 is the SIGN close, M2K BUY 1
    EXPECT_EQ(d.fills, (std::vector<double>{0.0, 0.0, -4.0, 1.0}));
    EXPECT_EQ(d.sign_fill, (std::vector<double>{0.0, 0.0, -4.0, 0.0}));
    EXPECT_EQ(d.rest_fill, (std::vector<double>{0.0, 0.0, 0.0, 1.0}));
    EXPECT_EQ(d.fill_contracts, 5);
    EXPECT_PRINTED(d.fill_cost, 7.50, 2);
    // stored (7)
    EXPECT_EQ(d.book, (std::vector<double>{4.0, 3.0, 0.0, -5.0}));
    EXPECT_PRINTED(d.record.risk_requested, 1.000000, 6);
    EXPECT_PRINTED(d.day.num("stored_gross"), 2.193474, 6);
    EXPECT_PRINTED(d.record.risk_scale, 0.861214, 6);
    EXPECT_PRINTED(d.stored_scale, 0.861214, 6);
    // risk_detail (7.3)
    expect_risk_detail(d, 475'500.00, 481'500.00);
}
