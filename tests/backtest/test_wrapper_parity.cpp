// tests/backtest/test_wrapper_parity.cpp
//
// The Python wrapper's path and the engine's usual path give the same trend-following backtest,
// to the last bit.
//
//   usual path:   a book's files loaded by ConfigLoader, the portfolio and sleeves assembled as
//                 apps/backtest/bt_portfolio.cpp assembles them, BacktestCoordinator on a database
//                 (here one that serves the test's rows).
//   wrapper path: BacktestRunner::initialize_with_config with BacktestRunConfig::config_base_path
//                 set, on a MarketDataSource holding the same bars.
//
// Both read the same bars, run the same sleeve with the same configuration, and every figure of
// the result must be EQUAL (not near): the equity curve, the fills, the positions and the
// statistics. The data has a vendor contract-id change, which the session classifier's id limb
// judges; the wrapper path reads the ids from the bars it was given, the usual path from the
// database.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "cost_basis_test_helpers.hpp"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include <nlohmann/json.hpp>

#include "trade_ngin/api/backtest_api.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/portfolio/loop_config.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/risk_module_config.hpp"
#include "trade_ngin/strategy/sleeve_config.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;
using trade_ngin::testing::cost_basis::Row;

namespace {

/// A double's bits: equal means the same number to the last bit, and two NaNs compare equal.
uint64_t bits(double x) {
    uint64_t b = 0;
    std::memcpy(&b, &x, sizeof(b));
    return b;
}

constexpr double kCapital = 1'000'000.0;
constexpr int kFirstDay = -500;  // the bars before the window are the estimators' history
constexpr int kWindowStart = 300;
constexpr int kLastDay = 700;

const std::vector<std::string> kSymbols = {"XA", "XB", "XC"};

/// Deterministic, platform-independent pseudo-random in [-1, 1) (no <random> distributions).
class Lcg {
public:
    explicit Lcg(uint64_t seed) : state_(seed) {}
    double next() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state_ >> 11) / static_cast<double>(1ULL << 52) - 1.0;
    }

private:
    uint64_t state_;
};

/// Three drifting series, a trend in each so the sleeve trades. XB's vendor id changes on day 450 (inside the window).
std::vector<Row> make_rows() {
    std::vector<Row> rows;
    Lcg rng(20261024);
    for (size_t s = 0; s < kSymbols.size(); ++s) {
        double price = 100.0 * static_cast<double>(s + 1);
        const double drift = (s == 1 ? -0.0004 : 0.0006);
        for (int d = kFirstDay; d <= kLastDay; ++d) {
            price *= 1.0 + drift + 0.01 * rng.next();
            Row row;
            row.symbol = kSymbols[s];
            row.day = d;
            row.close = price;
            row.volume = 100000.0 + 1000.0 * static_cast<double>((d + 1000) % 17);
            row.instrument_id = (s == 1 && d >= 450) ? "20000002" : "1000000" + std::to_string(s);
            rows.push_back(row);
        }
    }
    return rows;
}

using Sectors = std::vector<std::pair<std::string, std::string>>;

/// A database that serves the rows, for the session classifier their vendor ids, and for the
/// sleeve's instrument weights the contract metadata's sectors and the symbols that have bars.
class IdServingDb : public cost_basis::ServingDb {
public:
    using cost_basis::ServingDb::ServingDb;

    Sectors sectors;

    Result<std::shared_ptr<arrow::Table>> get_contract_metadata() const override {
        arrow::StringBuilder sector_builder, symbol_builder;
        for (const auto& [symbol, sector] : sectors) {
            ARROW_CHECK_OK(symbol_builder.Append(symbol));
            ARROW_CHECK_OK(sector_builder.Append(sector));
        }
        std::shared_ptr<arrow::Array> sector_array, symbol_array;
        ARROW_CHECK_OK(sector_builder.Finish(&sector_array));
        ARROW_CHECK_OK(symbol_builder.Finish(&symbol_array));
        if (sectors.empty()) {
            // a database whose metadata holds no sector column
            return Result<std::shared_ptr<arrow::Table>>(arrow::Table::Make(
                arrow::schema({arrow::field("Other", arrow::utf8())}), {symbol_array}));
        }
        return Result<std::shared_ptr<arrow::Table>>(arrow::Table::Make(
            arrow::schema({arrow::field("Sector", arrow::utf8()),
                           arrow::field("Databento Symbol", arrow::utf8())}),
            {sector_array, symbol_array}));
    }

    Result<std::vector<std::string>> get_symbols(AssetClass, DataFrequency,
                                                 const std::string&) override {
        return Result<std::vector<std::string>>(kSymbols);
    }

    Result<std::vector<market_data_utils::FuturesInstrumentId>> get_futures_instrument_ids(
        const std::vector<std::string>& symbols, const Timestamp& start_date,
        const Timestamp& end_date) override {
        std::vector<market_data_utils::FuturesInstrumentId> out;
        for (const auto& r : rows) {
            const auto ts = cost_basis::row_time(r);
            if (ts < start_date || ts > end_date) continue;
            if (std::find(symbols.begin(), symbols.end(), r.symbol) == symbols.end()) continue;
            out.push_back({r.symbol, core::format_utc_date(ts), r.instrument_id});
        }
        return Result<std::vector<market_data_utils::FuturesInstrumentId>>(out);
    }
};

/// The same rows as bars: converted by the code the database path converts with.
class RowsSource : public MarketDataSource {
public:
    RowsSource(const std::vector<Row>& rows, Sectors sectors) : sectors_(std::move(sectors)) {
        auto converted = DataConversionUtils::arrow_table_to_bars(cost_basis::to_table(rows));
        bars_ = converted.value();
    }

    Result<std::vector<Bar>> load_bars(const std::vector<std::string>& symbols,
                                       const Timestamp& start_date, const Timestamp& end_date,
                                       AssetClass, DataFrequency) override {
        std::vector<Bar> out;
        for (const auto& b : bars_) {
            if (b.timestamp < start_date || b.timestamp > end_date) continue;
            if (std::find(symbols.begin(), symbols.end(), b.symbol) == symbols.end()) continue;
            out.push_back(b);
        }
        return Result<std::vector<Bar>>(out);
    }

    Result<std::vector<std::string>> get_symbols(AssetClass) override {
        return Result<std::vector<std::string>>(kSymbols);
    }

    Result<Sectors> get_contract_sectors() override {
        if (sectors_.empty()) return MarketDataSource::get_contract_sectors();
        return Result<Sectors>(sectors_);
    }

private:
    Sectors sectors_;
    std::vector<Bar> bars_;
};

namespace fs = std::filesystem;

fs::path repo_config_template() {
    return fs::path(__FILE__).parent_path().parent_path().parent_path() / "config_template";
}

/// The shipped QT book, cut down to what a synthetic universe can run: no listing dates or vendor
/// relabels (those name real contracts), removals on this universe's names, the test's capital, and optionally a FAST sleeve beside
/// the standard one.
fs::path make_book(const std::string& tag, bool with_fast) {
    const fs::path base = fs::temp_directory_path() / ("trade_ngin_wrapper_parity_" + tag);
    fs::remove_all(base);
    fs::copy(repo_config_template(), base, fs::copy_options::recursive);
    const fs::path file = base / "portfolios" / "qt_conservative" / "portfolio.json";
    std::ifstream in(file);
    nlohmann::json j = nlohmann::json::parse(in);
    in.close();
    j.erase("listing_dates");
    j.erase("instrument_id_relabels");
    // trading rules removed by cost, on this universe's names, so the removal path runs
    // (the engine allows them on a book of exactly one trend sleeve only)
    if (with_fast) {
        j.erase("trading_rule_removals");
    } else {
        j["trading_rule_removals"] = {{"XA", {{2, 8}, {4, 16}}}, {"XC", {{2, 8}}}};
    }
    j["initial_capital"] = kCapital;
    j["starting_capital"] = kCapital;
    j["strategies"]["TREND_FOLLOWING"]["default_allocation"] = with_fast ? 0.7 : 1.0;
    if (with_fast) {
        nlohmann::json fast = j["strategies"]["TREND_FOLLOWING"];
        fast["type"] = "TrendFollowingFastStrategy";
        fast["default_allocation"] = 0.3;
        fast["config"]["risk_target"] = 0.25;
        fast["config"]["vol_lookback_short"] = 16;
        fast["config"]["ema_windows"] = {{2, 8}, {4, 16}, {8, 32}, {16, 64}};
        j["strategies"]["TREND_FOLLOWING_FAST"] = fast;  // sorts after the standard sleeve, which stays first
    }
    std::ofstream out(file);
    out << j.dump(2);
    out.close();
    // the loader reads an email.json for every book; nothing here sends
    std::ofstream email(base / "portfolios" / "qt_conservative" / "email.json");
    email << nlohmann::json{{"smtp_host", "smtp.test.com"}, {"smtp_port", 587},
                            {"username", "u"}, {"password", "p"},
                            {"from_email", "f@test.com"},
                            {"to_emails", nlohmann::json::array({"a@test.com"})}}
                 .dump(2);
    return base;
}

void register_symbols() {
    auto& registry = InstrumentRegistry::instance();
    for (const auto& symbol : kSymbols) {
        FuturesSpec spec;
        spec.root_symbol = symbol;
        spec.exchange = "CME";
        spec.currency = "USD";
        spec.multiplier = 50.0;
        spec.tick_size = 0.25;
        spec.commission_per_contract = 0.0;
        spec.initial_margin = 5000.0;
        spec.maintenance_margin = 4000.0;
        spec.weight = 1.0;
        spec.trading_hours = "09:30-16:00";
        registry.register_instrument(symbol, std::make_shared<FuturesInstrument>(symbol, spec));
    }
}

template <typename E>
Result<BacktestResults> fail(const E& e) {
    return make_error<BacktestResults>(e->code(), e->what(), "test");
}

/// The usual path: bt_portfolio's own steps, on the book's files.
Result<BacktestResults> run_usual(const fs::path& base, const std::vector<Row>& rows,
                                  const Sectors& sectors) {
    auto loaded = ConfigLoader::load(base, "qt_conservative");
    if (loaded.is_error()) return fail(loaded.error());
    AppConfig app = loaded.value();
    if (auto keys = ConfigLoader::require_loop_keys(app); keys.is_error()) {
        return fail(keys.error());
    }

    auto db = std::make_shared<IdServingDb>("mock://testdb");
    db->rows = rows;
    db->sectors = sectors;
    if (auto c = db->connect(); c.is_error()) return fail(c.error());

    const double capital = app.initial_capital;
    BacktestCoordinatorConfig cc;
    cc.initial_capital = capital;
    cc.use_optimization = app.use_optimization;
    cc.store_trade_details = false;
    cc.store_results = false;
    cc.portfolio_id = app.portfolio_id;
    cc.statistics_sessions_per_year = app.statistics.futures_sessions_per_year;
    auto registry = std::shared_ptr<InstrumentRegistry>(&InstrumentRegistry::instance(),
                                                        [](InstrumentRegistry*) {});
    BacktestCoordinator coordinator(db, registry.get(), cc);

    RiskConfig risk_config = app.risk_config;
    risk_config.capital = capital;
    DynamicOptConfig opt_config = app.opt_config;
    opt_config.capital = capital;

    PortfolioConfig portfolio_config;
    portfolio_config.total_capital = capital;
    portfolio_config.max_strategy_allocation = app.strategy_defaults.max_strategy_allocation;
    portfolio_config.min_strategy_allocation = app.strategy_defaults.min_strategy_allocation;
    portfolio_config.use_optimization = app.use_optimization;
    portfolio_config.covariance_history_prices = app.covariance_history_prices;
    portfolio_config.covariance_stale_dates = app.covariance_stale_dates;
    portfolio_config.risk_modules = app.risk_schema.portfolio;
    portfolio_config.sleeve_risk_modules = app.risk_schema.sleeves;
    portfolio_config.opt_config = opt_config;
    portfolio_config.risk_config = risk_config;
    apply_loop_config(app, portfolio_config);

    std::vector<std::string> names;
    std::map<std::string, double> allocations;
    std::map<std::string, nlohmann::json> defs;
    for (const auto& [id, def] : app.strategies_config.items()) {
        if (def.contains("enabled_backtest") && def["enabled_backtest"].get<bool>()) {
            names.push_back(id);
            allocations[id] = def.value("default_allocation", 0.5);
            defs[id] = def;
        }
    }
    double total = 0.0;
    for (const auto& [_, a] : allocations) total += a;
    for (auto& [_, a] : allocations) a /= total;

    StrategyConfig base_strategy;
    base_strategy.asset_classes = {AssetClass::FUTURES};
    base_strategy.frequencies = {DataFrequency::DAILY};
    base_strategy.max_drawdown = app.max_drawdown;
    base_strategy.max_leverage = loop_gross_leverage_limit(app);
    for (const auto& symbol : kSymbols) {
        base_strategy.position_limits[symbol] = app.execution.position_limit_backtest;
    }

    std::vector<std::shared_ptr<TrendFollowingStrategy>> strategies;
    for (const auto& id : names) {
        const auto& def = defs[id];
        const std::string type = def.value("type", "");
        base_strategy.capital_allocation = capital * allocations[id];
        TrendFollowingConfig trend = type == "TrendFollowingFastStrategy"
                                         ? fast_trend_following_config()
                                         : TrendFollowingConfig{};
        if (auto keys = read_required_sleeve_keys(id, def, trend); keys.is_error()) {
            return fail(keys.error());
        }
        if (def.contains("config")) {
            const auto& cfg = def["config"];
            if (cfg.contains("ema_windows")) {
                trend.ema_windows.clear();
                for (const auto& w : cfg["ema_windows"]) {
                    trend.ema_windows.push_back({w[0].get<int>(), w[1].get<int>()});
                }
            }
            trend.vol_lookback_long = cfg.value("vol_lookback_long", 252);
        }
        if (trend.fdm.empty()) trend.fdm = app.strategy_defaults.fdm;
        if (id == names.front()) {
            trend.equity_slow_symbols = app.equity_slow_rule.symbols;
            trend.equity_slow_pairs = app.equity_slow_rule.pairs;
            if (type != "TrendFollowingFastStrategy") hand_over_trading_rule_removals(app, trend);
            portfolio_config.overlay_sleeve = id;
            portfolio_config.overlay_tau = trend.risk_target;
        }
        auto strategy =
            std::make_shared<TrendFollowingStrategy>(id, base_strategy, trend, db, registry);
        if (auto r = strategy->initialize(); r.is_error()) return fail(r.error());
        if (auto r = strategy->start(); r.is_error()) return fail(r.error());
        strategies.push_back(strategy);
    }

    auto portfolio = std::make_shared<PortfolioManager>(portfolio_config);
    for (size_t i = 0; i < strategies.size(); ++i) {
        if (auto r = portfolio->add_strategy(strategies[i], allocations[names[i]],
                                             app.use_optimization);
            r.is_error()) {
            return fail(r.error());
        }
    }
    return coordinator.run_portfolio(portfolio, kSymbols, cost_basis::trading_day(kWindowStart),
                                     cost_basis::trading_day(kLastDay), AssetClass::FUTURES,
                                     DataFrequency::DAILY);
}

/// The wrapper's path: the argument-driven runner on a source, the book named by its directory.
Result<BacktestResults> run_wrapper(const fs::path& base, const std::vector<Row>& rows,
                                    const Sectors& sectors) {
    api::BacktestRunConfig cfg;
    cfg.symbols = kSymbols;
    cfg.start_date = cost_basis::trading_day(kWindowStart);
    cfg.end_date = cost_basis::trading_day(kLastDay);
    cfg.asset_class = AssetClass::FUTURES;
    cfg.data_freq = DataFrequency::DAILY;
    cfg.data_source = std::make_shared<RowsSource>(rows, sectors);
    cfg.config_base_path = base.string();

    api::BacktestRunner runner;
    runner.initialize_with_config("qt_conservative", cfg);
    return runner.run_backtest();
}

void expect_same_reports(const std::vector<ExecutionReport>& a, const std::vector<ExecutionReport>& b,
                         const char* what) {
    ASSERT_EQ(a.size(), b.size()) << what;
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].symbol, b[i].symbol) << what << " " << i;
        EXPECT_EQ(a[i].fill_time, b[i].fill_time) << what << " " << i;
        EXPECT_EQ(a[i].filled_quantity.raw_value(), b[i].filled_quantity.raw_value())
            << what << " " << i;
        EXPECT_EQ(a[i].fill_price.raw_value(), b[i].fill_price.raw_value()) << what << " " << i;
        EXPECT_EQ(a[i].commissions_fees.raw_value(), b[i].commissions_fees.raw_value()) << what << " " << i;
        EXPECT_EQ(a[i].total_transaction_costs.raw_value(), b[i].total_transaction_costs.raw_value())
            << what << " " << i;
    }
}

void expect_identical(const BacktestResults& usual, const BacktestResults& wrapper) {
    ASSERT_FALSE(usual.equity_curve.empty());
    ASSERT_EQ(usual.equity_curve.size(), wrapper.equity_curve.size());
    for (size_t i = 0; i < usual.equity_curve.size(); ++i) {
        EXPECT_EQ(usual.equity_curve[i].first, wrapper.equity_curve[i].first) << "day " << i;
        EXPECT_EQ(bits(usual.equity_curve[i].second), bits(wrapper.equity_curve[i].second))
            << "equity, day " << i;
    }
    ASSERT_EQ(usual.drawdown_curve.size(), wrapper.drawdown_curve.size());
    for (size_t i = 0; i < usual.drawdown_curve.size(); ++i) {
        EXPECT_EQ(bits(usual.drawdown_curve[i].second), bits(wrapper.drawdown_curve[i].second))
            << "drawdown, day " << i;
    }
    expect_same_reports(usual.executions, wrapper.executions, "execution");
    expect_same_reports(usual.actual_trades, wrapper.actual_trades, "actual trade");

    ASSERT_EQ(usual.positions.size(), wrapper.positions.size());
    for (size_t i = 0; i < usual.positions.size(); ++i) {
        EXPECT_EQ(usual.positions[i].symbol, wrapper.positions[i].symbol);
        EXPECT_EQ(usual.positions[i].quantity.raw_value(), wrapper.positions[i].quantity.raw_value());
        EXPECT_EQ(usual.positions[i].average_price.raw_value(),
                  wrapper.positions[i].average_price.raw_value());
    }

    EXPECT_EQ(bits(usual.total_return), bits(wrapper.total_return)) << "total_return";
    EXPECT_EQ(bits(usual.volatility), bits(wrapper.volatility)) << "volatility";
    EXPECT_EQ(bits(usual.sharpe_ratio), bits(wrapper.sharpe_ratio)) << "sharpe_ratio";
    EXPECT_EQ(bits(usual.sortino_ratio), bits(wrapper.sortino_ratio)) << "sortino_ratio";
    EXPECT_EQ(bits(usual.max_drawdown), bits(wrapper.max_drawdown)) << "max_drawdown";
    EXPECT_EQ(bits(usual.calmar_ratio), bits(wrapper.calmar_ratio)) << "calmar_ratio";
    EXPECT_EQ(usual.total_trades, wrapper.total_trades);
    EXPECT_EQ(bits(usual.win_rate), bits(wrapper.win_rate)) << "win_rate";
    EXPECT_EQ(bits(usual.transaction_costs), bits(wrapper.transaction_costs)) << "transaction_costs";
    EXPECT_EQ(bits(usual.roll_costs), bits(wrapper.roll_costs)) << "roll_costs";
    EXPECT_EQ(usual.total_roll_fills, wrapper.total_roll_fills);
    EXPECT_EQ(bits(usual.var_95), bits(wrapper.var_95)) << "var_95";
    EXPECT_EQ(bits(usual.cvar_95), bits(wrapper.cvar_95)) << "cvar_95";
    EXPECT_EQ(usual.warmup_days, wrapper.warmup_days);
    EXPECT_EQ(usual.statistics_days, wrapper.statistics_days);
    EXPECT_EQ(bits(usual.annualized_return), bits(wrapper.annualized_return)) << "annualized_return";
}

}  // namespace

class WrapperParityTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        register_symbols();
        rows_ = make_rows();
    }
    void TearDown() override {
        StateManager::reset_instance();
        TestBase::TearDown();
    }
    std::vector<Row> rows_;
    Sectors sectors_;  // none: the metadata holds no sector, as in the first tests
};

TEST_F(WrapperParityTest, TrendFollowingIsBitIdenticalOnTheDatabaseAndOnASource) {
    const auto base = make_book("one_sleeve", /*with_fast=*/false);
    auto usual = run_usual(base, rows_, sectors_);
    ASSERT_TRUE(usual.is_ok()) << usual.error()->what();
    auto wrapper = run_wrapper(base, rows_, sectors_);
    ASSERT_TRUE(wrapper.is_ok()) << wrapper.error()->what();
    EXPECT_GT(usual.value().total_trades, 0)
        << "the sleeve must trade for the comparison to mean anything";
    expect_identical(usual.value(), wrapper.value());
}

TEST_F(WrapperParityTest, TheStandardAndTheFastSleeveTogetherAreBitIdenticalToo) {
    const auto base = make_book("two_sleeves", /*with_fast=*/true);
    auto usual = run_usual(base, rows_, sectors_);
    ASSERT_TRUE(usual.is_ok()) << usual.error()->what();
    auto wrapper = run_wrapper(base, rows_, sectors_);
    ASSERT_TRUE(wrapper.is_ok()) << wrapper.error()->what();
    expect_identical(usual.value(), wrapper.value());
}

TEST_F(WrapperParityTest, BarsWithoutIdsStillMatchTheDatabaseOnTheSameRows) {
    // A source with no vendor ids (a CSV, a DataFrame) runs the id limb as unavailable; the
    // database on the same rows with the ids dropped gives the same run.
    for (auto& r : rows_) r.instrument_id.clear();
    const auto base = make_book("no_ids", false);
    auto usual = run_usual(base, rows_, sectors_);
    ASSERT_TRUE(usual.is_ok()) << usual.error()->what();
    auto wrapper = run_wrapper(base, rows_, sectors_);
    ASSERT_TRUE(wrapper.is_ok()) << wrapper.error()->what();
    expect_identical(usual.value(), wrapper.value());
}

TEST_F(WrapperParityTest, TheSectorWeightsOfASourceAreTheDatabasesWeights) {
    // Instrument weights are read from the contract metadata's sectors: a source gives the same
    // two columns, and the sleeve sizes on the same weights.
    sectors_ = {{"XA", "Equity"}, {"XB", "Equity"}, {"XC", "Energy"}};
    const auto base = make_book("sectors", /*with_fast=*/false);
    auto usual = run_usual(base, rows_, sectors_);
    ASSERT_TRUE(usual.is_ok()) << usual.error()->what();
    auto wrapper = run_wrapper(base, rows_, sectors_);
    ASSERT_TRUE(wrapper.is_ok()) << wrapper.error()->what();
    EXPECT_GT(usual.value().total_trades, 0);
    expect_identical(usual.value(), wrapper.value());
}

TEST_F(WrapperParityTest, ControlTheVendorIdsChangeTheRun) {
    // The comparison above would pass for any run the ids do not touch. Drop the ids from the
    // source alone, and the run must differ from the database's, which has them.
    const auto base = make_book("control", /*with_fast=*/false);
    auto usual = run_usual(base, rows_, sectors_);
    ASSERT_TRUE(usual.is_ok()) << usual.error()->what();
    auto stripped = rows_;
    for (auto& r : stripped) r.instrument_id.clear();
    auto wrapper = run_wrapper(base, stripped, sectors_);
    ASSERT_TRUE(wrapper.is_ok()) << wrapper.error()->what();
    bool differs = usual.value().equity_curve.size() != wrapper.value().equity_curve.size() ||
                   usual.value().total_trades != wrapper.value().total_trades;
    for (size_t i = 0; !differs && i < usual.value().equity_curve.size(); ++i) {
        differs = bits(usual.value().equity_curve[i].second) !=
                  bits(wrapper.value().equity_curve[i].second);
    }
    EXPECT_TRUE(differs) << "the id change on XB does not reach the run, so the id comparison "
                            "above says nothing";
}
