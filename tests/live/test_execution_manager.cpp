// Coverage for execution_manager.cpp. ExecutionManager owns a
// TransactionCostManager so we use a real one with a default config.
//
// Targets:
// - generate_daily_executions on adds, increases, decreases, and closes
// - generate_execution sets side correctly for buy/sell, populates IDs and
//   transaction-cost fields
// - generate_date_string formats YYYYMMDD
// - generate_exec_id encodes symbol/timestamp/sequence
// - update_market_data populates the prev_close map

#include <gtest/gtest.h>
#include <chrono>
#include <unordered_map>
#include <stdexcept>
#include "trade_ngin/live/execution_manager.hpp"

using namespace trade_ngin;

namespace {

Position make_position(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(avg_price);
    return p;
}

Timestamp at_local_date(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

}  // namespace

class ExecutionManagerTest : public ::testing::Test {};

TEST_F(ExecutionManagerTest, QtIdsSeparateStreamsWithoutChangingFillEconomics) {
    ExecutionManager em;
    const auto day=at_local_date(2026,9,22);
    auto system=em.generate_execution("ES",2,100,day,0);
    auto qt=em.generate_execution("ES",2,100,day,0,"qt");
    EXPECT_EQ(system.order_id,"DAILY_ES_20260922");
    EXPECT_EQ(qt.order_id,"QT_DAILY_ES_20260922");
    EXPECT_EQ(qt.exec_id,system.exec_id);
    EXPECT_EQ(qt.filled_quantity,system.filled_quantity);
    EXPECT_EQ(qt.fill_price,system.fill_price);
    EXPECT_EQ(qt.total_transaction_costs,system.total_transaction_costs);
    auto closed=em.generate_daily_executions({},{{"ES",make_position("ES",2,100)}},{{"ES",100}},day,"qt");
    ASSERT_TRUE(closed.is_ok()); ASSERT_EQ(closed.value().size(),1u);
    EXPECT_EQ(closed.value()[0].order_id,"QT_DAILY_ES_20260922");
    EXPECT_EQ(closed.value()[0].side,Side::SELL);
    auto opened=em.generate_daily_executions({{"ES",make_position("ES",2,100)}},{},{{"ES",100}},day,"qt");
    ASSERT_TRUE(opened.is_ok()); ASSERT_EQ(opened.value().size(),1u);
    EXPECT_EQ(opened.value()[0].order_id,"QT_DAILY_ES_20260922");
    EXPECT_TRUE(em.generate_daily_executions({}, {}, {},day,"unknown").is_error());
    EXPECT_THROW(em.generate_execution("ES",1,100,day,0,"unknown"),std::invalid_argument);
    EXPECT_THROW(em.generate_execution(std::string(48,'A'),1,100,day,0,"qt"),std::invalid_argument);
    EXPECT_TRUE(em.generate_daily_executions({{std::string(48,'A'),make_position(std::string(48,'A'),1,100)}},{},{},day,"qt").is_error());
}

transaction_cost::TransactionCostManager::Config evidence_config() {
    transaction_cost::TransactionCostManager::Config config;
    config.explicit_fee_per_contract = 2.75;
    config.spread_config.lambda = 0.4;
    config.spread_config.lookback_days = 2;
    config.impact_config.adv_lookback_days = 2;
    config.impact_config.min_adv = 150.5;
    config.impact_config.min_participation = 0.0;
    config.impact_config.max_participation = 0.08;
    return config;
}

void register_evidence_asset(ExecutionManager& manager, const std::string& symbol) {
    transaction_cost::AssetCostConfig asset;
    asset.symbol = symbol;
    asset.baseline_spread_ticks = 2.0;
    asset.min_spread_ticks = 0.5;
    asset.max_spread_ticks = 4.0;
    asset.spread_cost_multiplier = 0.3;
    asset.tick_size = 1.25;
    asset.point_value = 40.0;
    asset.max_impact_bps = 90.0;
    manager.get_transaction_cost_manager().register_asset_config(asset);
}

void expect_report_equal(const ExecutionReport& observed, const ExecutionReport& plain) {
    EXPECT_EQ(observed.order_id, plain.order_id);
    EXPECT_EQ(observed.exec_id, plain.exec_id);
    EXPECT_EQ(observed.symbol, plain.symbol);
    EXPECT_EQ(observed.side, plain.side);
    EXPECT_EQ(observed.filled_quantity, plain.filled_quantity);
    EXPECT_EQ(observed.fill_price, plain.fill_price);
    EXPECT_EQ(observed.fill_time, plain.fill_time);
    EXPECT_EQ(observed.commissions_fees, plain.commissions_fees);
    EXPECT_EQ(observed.implicit_price_impact, plain.implicit_price_impact);
    EXPECT_EQ(observed.slippage_market_impact, plain.slippage_market_impact);
    EXPECT_EQ(observed.total_transaction_costs, plain.total_transaction_costs);
    EXPECT_EQ(observed.is_partial, plain.is_partial);
}

void expect_call_cost(const ExecutionCallObservation& trace, double qty, double price) {
    EXPECT_EQ(trace.state, ExecutionCallState::returned);
    EXPECT_EQ(trace.cost.input_source, transaction_cost::CostInputSource::internally_tracked);
    EXPECT_EQ(trace.cost.quantity, qty);
    EXPECT_EQ(trace.cost.reference_price, price);
    EXPECT_EQ(trace.cost.retrieved_adv, 0.0);
    EXPECT_EQ(trace.cost.effective_adv, 100000.0);
    EXPECT_EQ(trace.cost.retrieved_volatility_multiplier, 1.0);
    EXPECT_EQ(trace.cost.effective_volatility_multiplier, 1.0);
    EXPECT_EQ(trace.cost.explicit_fee_per_contract, 2.75);
    EXPECT_EQ(trace.cost.point_value, 40.0);
    EXPECT_EQ(trace.cost.asset_lookup.path, transaction_cost::AssetLookupPath::exact_symbol);
    EXPECT_EQ(trace.cost.spread.baseline_spread_ticks, 2.0);
    EXPECT_EQ(trace.cost.spread.spread_cost_multiplier, 0.3);
    EXPECT_EQ(trace.cost.impact.min_adv, 150.5);
    EXPECT_EQ(trace.cost.impact.max_participation, 0.08);
    EXPECT_EQ(trace.cost.impact.selected_k_bps, 40.0);
    EXPECT_FALSE(trace.cost.adv_argument.has_value());
}

// ===== generate_daily_executions =====

TEST_F(ExecutionManagerTest, EmptyPositionsProducesNoExecutions) {
    ExecutionManager em;
    auto r = em.generate_daily_executions({}, {}, {}, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(ExecutionManagerTest, NewPositionGeneratesBuyExecution) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr{{"ES", make_position("ES", 5.0, 4500.0)}};
    std::unordered_map<std::string, Position> prev;
    std::unordered_map<std::string, double> prices{{"ES", 4500.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].side, Side::BUY);
    EXPECT_EQ(r.value()[0].symbol, "ES");
}

TEST_F(ExecutionManagerTest, ReducedPositionGeneratesSellExecution) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr{{"ES", make_position("ES", 3.0, 4500.0)}};
    std::unordered_map<std::string, Position> prev{{"ES", make_position("ES", 5.0, 4500.0)}};
    std::unordered_map<std::string, double> prices{{"ES", 4505.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].side, Side::SELL);
}

TEST_F(ExecutionManagerTest, UnchangedPositionProducesNoExecution) {
    ExecutionManager em;
    auto pos = make_position("ES", 5.0, 4500.0);
    std::unordered_map<std::string, Position> curr{{"ES", pos}};
    std::unordered_map<std::string, Position> prev{{"ES", pos}};
    std::unordered_map<std::string, double> prices{{"ES", 4500.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(ExecutionManagerTest, ClosedPositionGeneratesOppositeSideExecution) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr;
    std::unordered_map<std::string, Position> prev{{"ES", make_position("ES", 5.0, 4500.0)}};
    std::unordered_map<std::string, double> prices{{"ES", 4505.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].side, Side::SELL);  // Closing a long → SELL
}

TEST_F(ExecutionManagerTest, ClosedShortPositionGeneratesBuyExecution) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr;
    std::unordered_map<std::string, Position> prev{{"ES", make_position("ES", -3.0, 4500.0)}};
    std::unordered_map<std::string, double> prices{{"ES", 4505.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].side, Side::BUY);
}

TEST_F(ExecutionManagerTest, MissingMarketPriceFallsBackToAveragePrice) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr{{"ES", make_position("ES", 2.0, 4500.0)}};
    std::unordered_map<std::string, Position> prev;
    std::unordered_map<std::string, double> prices;  // no price for ES
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_DOUBLE_EQ(r.value()[0].fill_price.as_double(), 4500.0);
}

// ===== generate_execution =====

TEST_F(ExecutionManagerTest, GenerateExecutionPopulatesAllFields) {
    ExecutionManager em;
    auto ts = at_local_date(2026, 4, 28);
    auto exec = em.generate_execution("ES", 3.0, 4500.0, ts, 0);
    EXPECT_EQ(exec.symbol, "ES");
    EXPECT_EQ(exec.side, Side::BUY);
    EXPECT_DOUBLE_EQ(exec.filled_quantity.as_double(), 3.0);
    EXPECT_DOUBLE_EQ(exec.fill_price.as_double(), 4500.0);
    EXPECT_FALSE(exec.is_partial);
    EXPECT_NE(exec.exec_id.find("EXEC_ES_"), std::string::npos);
    EXPECT_NE(exec.order_id.find("DAILY_ES_"), std::string::npos);
}

TEST_F(ExecutionManagerTest, GenerateExecutionSequenceProducesDistinctIds) {
    ExecutionManager em;
    auto ts = at_local_date(2026, 4, 28);
    auto e0 = em.generate_execution("ES", 1.0, 4500.0, ts, 0);
    auto e1 = em.generate_execution("ES", 1.0, 4500.0, ts, 1);
    EXPECT_NE(e0.exec_id, e1.exec_id);
}

// ===== Static helpers =====

TEST_F(ExecutionManagerTest, GenerateDateStringHasYYYYMMDDFormat) {
    auto s = ExecutionManager::generate_date_string(at_local_date(2026, 4, 28));
    EXPECT_EQ(s.length(), 8u);
    EXPECT_NE(s.find("20260428"), std::string::npos);
}

TEST_F(ExecutionManagerTest, GenerateExecIdEncodesSymbolAndSequence) {
    auto s = ExecutionManager::generate_exec_id("ES", at_local_date(2026, 4, 28), 7);
    EXPECT_NE(s.find("EXEC_ES_"), std::string::npos);
    EXPECT_NE(s.find("_7"), std::string::npos);
}

// ===== update_market_data populates prev_close map =====

TEST_F(ExecutionManagerTest, UpdateMarketDataDoesNotThrow) {
    ExecutionManager em;
    EXPECT_NO_THROW(em.update_market_data("ES", 1000.0, 4500.0));
    EXPECT_NO_THROW(em.update_market_data("ES", 1100.0, 4510.0));
}

TEST_F(ExecutionManagerTest, DirectEvidenceCapturesRealBuySellAndZeroCostsWithoutChangingReports) {
    auto config = evidence_config();
    ExecutionManager observed(config), plain(config);
    register_evidence_asset(observed, "SYN");
    register_evidence_asset(plain, "SYN");
    const auto day = at_local_date(2026, 9, 24);
    ExecutionCallObservation trace;
    for (const double quantity : {4.0, -4.0, 0.0}) {
        auto actual = observed.generate_execution("SYN", quantity, 125.0, day, 3, "qt", &trace);
        auto baseline = plain.generate_execution("SYN", quantity, 125.0, day, 3, "qt");
        expect_report_equal(actual, baseline);
        expect_call_cost(trace, std::abs(quantity), 125.0);
        if (quantity != 0.0) {
            EXPECT_DOUBLE_EQ(actual.commissions_fees.as_double(), 11.0);
            EXPECT_NEAR(actual.implicit_price_impact.as_double(), 0.7531622776601684, 1e-8);
            EXPECT_NEAR(actual.slippage_market_impact.as_double(), 120.50596442562694, 1e-8);
            EXPECT_NEAR(actual.total_transaction_costs.as_double(), 131.50596442562694, 1e-8);
        } else {
            EXPECT_DOUBLE_EQ(actual.commissions_fees.as_double(), 0.0);
            EXPECT_DOUBLE_EQ(actual.slippage_market_impact.as_double(), 0.0);
            EXPECT_DOUBLE_EQ(actual.total_transaction_costs.as_double(), 0.0);
            EXPECT_EQ(actual.side, Side::SELL);
        }
    }
}

TEST_F(ExecutionManagerTest, DirectValidationAndThrowResetReusedEvidenceWithoutInventingCharge) {
    ExecutionManager observed(evidence_config()), plain(evidence_config());
    register_evidence_asset(observed, "SYN");
    register_evidence_asset(plain, "SYN");
    const auto day = at_local_date(2026, 9, 24);
    ExecutionCallObservation trace;
    observed.generate_execution("SYN", 1, 125, day, 0, "qt", &trace);
    EXPECT_TRUE(trace.cost.quantity.has_value());
    std::string actual_message, baseline_message;
    try { observed.generate_execution("SYN", 1, 125, day, 0, "bad", &trace); }
    catch (const std::invalid_argument& e) { actual_message = e.what(); }
    try { plain.generate_execution("SYN", 1, 125, day, 0, "bad"); }
    catch (const std::invalid_argument& e) { baseline_message = e.what(); }
    EXPECT_EQ(actual_message, baseline_message);
    EXPECT_EQ(trace.state, ExecutionCallState::rejected_stream);
    EXPECT_FALSE(trace.cost.quantity.has_value());
    const auto long_symbol = std::string(48, 'X');
    try { observed.generate_execution(long_symbol, 1, 125, day, 0, "qt", &trace); }
    catch (const std::invalid_argument& e) { actual_message = e.what(); }
    try { plain.generate_execution(long_symbol, 1, 125, day, 0, "qt"); }
    catch (const std::invalid_argument& e) { baseline_message = e.what(); }
    EXPECT_EQ(actual_message, baseline_message);
    EXPECT_EQ(trace.state, ExecutionCallState::rejected_id);
    EXPECT_FALSE(trace.cost.quantity.has_value());
}

TEST_F(ExecutionManagerTest, DailyAttemptsPairWithReportsAndPreservePriceBranch) {
    ExecutionManager observed(evidence_config()), plain(evidence_config());
    for (const auto& symbol : {"NEW", "CHANGED", "SHORT", "LONG"}) {
        register_evidence_asset(observed, symbol);
        register_evidence_asset(plain, symbol);
    }
    const auto day = at_local_date(2026, 9, 24);
    std::unordered_map<std::string, Position> current{
        {"NEW", make_position("NEW", 3, 101)},
        {"CHANGED", make_position("CHANGED", 7, 202)}};
    std::unordered_map<std::string, Position> previous{
        {"CHANGED", make_position("CHANGED", 5, 201)},
        {"SHORT", make_position("SHORT", -2, 303)},
        {"LONG", make_position("LONG", 2, 404)}};
    std::unordered_map<std::string, double> prices{{"NEW", 111}, {"LONG", 444}};
    DailyExecutionObservation trace;
    auto actual = observed.generate_daily_executions(current, previous, prices, day, "qt", &trace);
    auto baseline = plain.generate_daily_executions(current, previous, prices, day, "qt");
    ASSERT_TRUE(actual.is_ok()); ASSERT_TRUE(baseline.is_ok());
    ASSERT_EQ(actual.value().size(), 4u);
    ASSERT_EQ(trace.attempts.size(), actual.value().size());
    EXPECT_EQ(trace.state, DailyExecutionState::returned);
    EXPECT_FALSE(trace.error_code.has_value());
    for (size_t i = 0; i < trace.attempts.size(); ++i) {
        const auto& attempt = trace.attempts[i];
        expect_report_equal(actual.value()[i], baseline.value()[i]);
        EXPECT_TRUE(attempt.returned);
        EXPECT_EQ(attempt.sequence, i);
        EXPECT_EQ(attempt.symbol, actual.value()[i].symbol);
        EXPECT_EQ(attempt.execution.state, ExecutionCallState::returned);
        EXPECT_EQ(attempt.execution.cost.quantity, actual.value()[i].filled_quantity.as_double());
        EXPECT_EQ(attempt.execution.cost.reference_price, actual.value()[i].fill_price.as_double());
        EXPECT_DOUBLE_EQ(attempt.selected_price, actual.value()[i].fill_price.as_double());
        if (attempt.symbol == "NEW") {
            EXPECT_EQ(attempt.branch, DailyPositionBranch::current_position);
            EXPECT_EQ(attempt.price_source, ExecutionPriceSource::market_prices);
            EXPECT_DOUBLE_EQ(attempt.selected_price, 111);
        } else if (attempt.symbol == "CHANGED") {
            EXPECT_EQ(attempt.branch, DailyPositionBranch::current_position);
            EXPECT_EQ(attempt.price_source, ExecutionPriceSource::current_average_price);
            EXPECT_DOUBLE_EQ(attempt.selected_price, 202);
        } else if (attempt.symbol == "SHORT") {
            EXPECT_EQ(attempt.branch, DailyPositionBranch::removed_position);
            EXPECT_EQ(attempt.price_source, ExecutionPriceSource::previous_average_price);
            EXPECT_DOUBLE_EQ(attempt.selected_price, 303);
            EXPECT_EQ(actual.value()[i].side, Side::BUY);
        } else if (attempt.symbol == "LONG") {
            EXPECT_EQ(attempt.branch, DailyPositionBranch::removed_position);
            EXPECT_EQ(attempt.price_source, ExecutionPriceSource::market_prices);
            EXPECT_DOUBLE_EQ(attempt.selected_price, 444);
            EXPECT_EQ(actual.value()[i].side, Side::SELL);
        } else {
            ADD_FAILURE() << "Unexpected symbol: " << attempt.symbol;
        }
    }
}

TEST_F(ExecutionManagerTest, DailyReuseClearsSuccessForEmptyUnchangedAndUnsupportedBatches) {
    ExecutionManager manager(evidence_config()), plain(evidence_config());
    register_evidence_asset(manager, "SYN");
    register_evidence_asset(plain, "SYN");
    const auto day = at_local_date(2026, 9, 24);
    DailyExecutionObservation trace;
    auto first = manager.generate_daily_executions({{"SYN", make_position("SYN", 2, 125)}}, {}, {}, day, "system", &trace);
    auto first_plain = plain.generate_daily_executions({{"SYN", make_position("SYN", 2, 125)}}, {}, {}, day, "system");
    ASSERT_TRUE(first.is_ok()); ASSERT_TRUE(first_plain.is_ok());
    ASSERT_EQ(first.value().size(), 1u); ASSERT_EQ(first_plain.value().size(), 1u);
    expect_report_equal(first.value()[0], first_plain.value()[0]);
    ASSERT_EQ(trace.attempts.size(), 1u);
    auto empty = manager.generate_daily_executions({}, {}, {}, day, "system", &trace);
    ASSERT_TRUE(empty.is_ok()); EXPECT_TRUE(empty.value().empty());
    EXPECT_EQ(trace.state, DailyExecutionState::returned);
    EXPECT_TRUE(trace.attempts.empty());
    auto unchanged = manager.generate_daily_executions({{"SYN", make_position("SYN", 2, 125)}},
        {{"SYN", make_position("SYN", 2, 125)}}, {}, day, "system", &trace);
    ASSERT_TRUE(unchanged.is_ok()); EXPECT_TRUE(unchanged.value().empty());
    EXPECT_TRUE(trace.attempts.empty());
    auto rejected = manager.generate_daily_executions({}, {}, {}, day, "bad", &trace);
    ASSERT_TRUE(rejected.is_error());
    EXPECT_EQ(trace.state, DailyExecutionState::rejected_stream);
    EXPECT_EQ(trace.error_code, ErrorCode::INVALID_ARGUMENT);
    EXPECT_TRUE(trace.attempts.empty());
    auto repeated = manager.generate_daily_executions({{"SYN", make_position("SYN", 3, 125)}},
        {{"SYN", make_position("SYN", 2, 125)}}, {{"SYN", 130}}, day, "system", &trace);
    auto repeated_plain = plain.generate_daily_executions({{"SYN", make_position("SYN", 3, 125)}},
        {{"SYN", make_position("SYN", 2, 125)}}, {{"SYN", 130}}, day, "system");
    ASSERT_TRUE(repeated.is_ok()); ASSERT_EQ(repeated.value().size(), 1u);
    ASSERT_TRUE(repeated_plain.is_ok()); ASSERT_EQ(repeated_plain.value().size(), 1u);
    expect_report_equal(repeated.value()[0], repeated_plain.value()[0]);
    ASSERT_EQ(trace.attempts.size(), 1u);
    EXPECT_EQ(trace.attempts[0].symbol, "SYN");
    EXPECT_EQ(trace.attempts[0].sequence, 0u);
    EXPECT_EQ(trace.attempts[0].execution.cost.quantity, 1.0);
    EXPECT_EQ(trace.attempts[0].execution.cost.reference_price, 130.0);
    EXPECT_EQ(trace.attempts[0].price_source, ExecutionPriceSource::market_prices);
    EXPECT_EQ(trace.state, DailyExecutionState::returned);
    EXPECT_FALSE(trace.error_code.has_value());
}

TEST_F(ExecutionManagerTest, DailyLaterRemovedPositionErrorRetainsOnlyPartialAttemptEvidence) {
    ExecutionManager observed(evidence_config()), plain(evidence_config());
    register_evidence_asset(observed, "GOOD"); register_evidence_asset(plain, "GOOD");
    const auto day = at_local_date(2026, 9, 24);
    const auto bad = std::string(48, 'B');
    std::unordered_map<std::string, Position> current{{"GOOD", make_position("GOOD", 2, 125)}};
    std::unordered_map<std::string, Position> previous{{bad, make_position(bad, 1, 125)}};
    DailyExecutionObservation trace;
    auto actual = observed.generate_daily_executions(current, previous, {}, day, "qt", &trace);
    auto baseline = plain.generate_daily_executions(current, previous, {}, day, "qt");
    ASSERT_TRUE(actual.is_error()); ASSERT_TRUE(baseline.is_error());
    ASSERT_NE(actual.error(), nullptr); ASSERT_NE(baseline.error(), nullptr);
    EXPECT_EQ(actual.error()->code(), baseline.error()->code());
    EXPECT_EQ(std::string(actual.error()->what()), std::string(baseline.error()->what()));
    EXPECT_EQ(trace.state, DailyExecutionState::invalid_argument);
    EXPECT_EQ(trace.error_code, ErrorCode::INVALID_ARGUMENT);
    ASSERT_EQ(trace.attempts.size(), 2u);
    EXPECT_EQ(trace.attempts[0].symbol, "GOOD");
    EXPECT_EQ(trace.attempts[0].branch, DailyPositionBranch::current_position);
    EXPECT_EQ(trace.attempts[0].sequence, 0u);
    EXPECT_TRUE(trace.attempts[0].returned);
    expect_call_cost(trace.attempts[0].execution, 2, 125);
    EXPECT_EQ(trace.attempts[1].symbol, bad);
    EXPECT_EQ(trace.attempts[1].branch, DailyPositionBranch::removed_position);
    EXPECT_EQ(trace.attempts[1].sequence, 1u);
    EXPECT_FALSE(trace.attempts[1].returned);
    EXPECT_EQ(trace.attempts[1].execution.state, ExecutionCallState::rejected_id);
    EXPECT_FALSE(trace.attempts[1].execution.cost.quantity.has_value());
}

TEST_F(ExecutionManagerTest, UpdateEvidenceTracksActualPreviousCloseAndHistoryWithoutChangingCosts) {
    auto config = evidence_config();
    ExecutionManager observed(config), plain(config), another(config);
    for (auto* manager : {&observed, &plain, &another}) register_evidence_asset(*manager, "SYN");
    ExecutionMarketDataObservation trace;
    const auto day = at_local_date(2026, 9, 24);
    const double closes[] = {100, 0, 110, 120};
    const double volumes[] = {100, 200, 300, 400};
    const double expected_adv[] = {100, 150, 250, 350};
    for (size_t i = 0; i < 4; ++i) {
        observed.update_market_data("SYN", volumes[i], closes[i], &trace);
        plain.update_market_data("SYN", volumes[i], closes[i]);
        ASSERT_TRUE(trace.cost_model_reached);
        EXPECT_EQ(trace.previous_close_source, i == 0 ? PreviousCloseSource::initial_current_close
                                                        : PreviousCloseSource::stored_previous_close);
        EXPECT_EQ(trace.previous_close_forwarded, i == 0 ? 100 : closes[i - 1]);
        EXPECT_EQ(trace.market_data.volume.adv_lookback_days, 2u);
        if (closes[i] > 0 && (i == 0 || closes[i - 1] > 0)) {
            EXPECT_EQ(trace.market_data.log_returns.lookback_days, 2u);
        } else {
            EXPECT_FALSE(trace.market_data.log_returns.lookback_days.has_value());
        }
        EXPECT_DOUBLE_EQ(observed.get_transaction_cost_manager().get_adv("SYN"),
                         plain.get_transaction_cost_manager().get_adv("SYN"));
        ExecutionCallObservation charge_trace;
        auto charged = observed.generate_execution("SYN", 2, 125, day, i, "qt", &charge_trace);
        auto baseline = plain.generate_execution("SYN", 2, 125, day, i, "qt");
        expect_report_equal(charged, baseline);
        EXPECT_EQ(charge_trace.state, ExecutionCallState::returned);
        EXPECT_EQ(charge_trace.cost.retrieved_adv, expected_adv[i]);
        EXPECT_EQ(charge_trace.cost.effective_adv, expected_adv[i]);
        EXPECT_EQ(charge_trace.cost.retrieved_volatility_multiplier,
                  i == 3 ? 1.5 : 1.0);
        EXPECT_EQ(charge_trace.cost.volatility.lambda.has_value(), i == 3);
        if (i == 3) {
            EXPECT_EQ(charge_trace.cost.volatility.lambda, 0.4);
        }
        EXPECT_DOUBLE_EQ(observed.get_transaction_cost_manager().get_volatility_multiplier("SYN"),
                         plain.get_transaction_cost_manager().get_volatility_multiplier("SYN"));
    }
    another.update_market_data("SYN", 1000, 90, &trace);
    EXPECT_EQ(trace.previous_close_source, PreviousCloseSource::initial_current_close);
    EXPECT_EQ(trace.previous_close_forwarded, 90);
    EXPECT_EQ(trace.market_data.volume.adv_lookback_days, 2u);
    observed.update_market_data("SYN", 500, 130, &trace);
    EXPECT_EQ(trace.previous_close_source, PreviousCloseSource::stored_previous_close);
    EXPECT_EQ(trace.previous_close_forwarded, 120);
}
