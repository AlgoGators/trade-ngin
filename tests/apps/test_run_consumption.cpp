#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <chrono>
#include <stdexcept>

#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/apps/run_consumption.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "../data/test_db_utils.hpp"

using namespace trade_ngin;

TEST(RunConsumptionTest, ActualSelectorsKeepTheirDistinctReachedReadsAndExactResults) {
    const nlohmann::json config = {
        {"A", {{"enabled_live", true}, {"default_allocation", 0.4}}},
        {"B", {{"enabled_live", true}, {"default_allocation", 0.6}}},
        {"C", {{"enabled_live", false}}}};
    RunInvocation<SelectionConsumption> call;
    auto controlled = invoke_run_result(&call, [&](SelectionConsumption* output) {
        return select_controlled_live_strategies(config, output);
    });
    auto plain_controlled = select_controlled_live_strategies(config);
    ASSERT_TRUE(controlled.is_ok());
    ASSERT_TRUE(plain_controlled.is_ok());
    EXPECT_EQ(controlled.value().names, plain_controlled.value().names);
    EXPECT_EQ(controlled.value().allocations, plain_controlled.value().allocations);
    EXPECT_EQ(controlled.value().configs, plain_controlled.value().configs);
    EXPECT_EQ(call.outcome, RunCallOutcome::ReturnedOk);
    ASSERT_EQ(call.payload.controlled_validation.size(), 3u);
    ASSERT_EQ(call.payload.ordinary_selection.size(), 3u);
    EXPECT_DOUBLE_EQ(*call.payload.controlled_sum, 1.0);
    EXPECT_DOUBLE_EQ(*call.payload.ordinary_selection[0].effective_allocation, 0.4);

    auto ordinary = invoke_run_result(&call, [&](SelectionConsumption* output) {
        return select_enabled_live_strategies(config, output);
    });
    auto plain_ordinary = select_enabled_live_strategies(config);
    ASSERT_TRUE(ordinary.is_ok());
    ASSERT_TRUE(plain_ordinary.is_ok());
    EXPECT_EQ(ordinary.value().names, plain_ordinary.value().names);
    EXPECT_EQ(ordinary.value().allocations, plain_ordinary.value().allocations);
    EXPECT_EQ(call.outcome, RunCallOutcome::ReturnedOk);
    EXPECT_TRUE(call.payload.controlled_validation.empty());
    EXPECT_FALSE(call.payload.controlled_sum.has_value());
    ASSERT_EQ(call.payload.ordinary_selection.size(), 3u);
}

TEST(RunConsumptionTest, ActualSelectorErrorAndThrownReaderKeepOriginalSemantics) {
    RunInvocation<SelectionConsumption> call;
    const nlohmann::json invalid = {{"A", {{"enabled_live", true},
                                               {"default_allocation", 0.4}}}};
    auto result = invoke_run_result(&call, [&](SelectionConsumption* output) {
        return select_controlled_live_strategies(invalid, output);
    });
    auto plain = select_controlled_live_strategies(invalid);
    ASSERT_TRUE(result.is_error());
    ASSERT_TRUE(plain.is_error());
    EXPECT_EQ(result.error()->code(), plain.error()->code());
    EXPECT_STREQ(result.error()->what(), plain.error()->what());
    EXPECT_EQ(call.outcome, RunCallOutcome::ReturnedError);
    ASSERT_EQ(call.payload.controlled_validation.size(), 1u);

    int invokes = 0;
    EXPECT_THROW(invoke_run_result(&call, [&](SelectionConsumption* output) {
        ++invokes;
        return select_enabled_live_strategies(
            {{"A", {{"enabled_live", "not-a-bool"}}}}, output);
    }), nlohmann::json::type_error);
    EXPECT_EQ(invokes, 1);
    EXPECT_EQ(call.outcome, RunCallOutcome::Threw);
    ASSERT_EQ(call.payload.ordinary_selection.size(), 1u);
    EXPECT_TRUE(call.payload.controlled_validation.empty());
}

namespace {
Position position(std::string symbol, double quantity, double price) {
    Position value;
    value.symbol = std::move(symbol);
    value.quantity = Decimal(quantity);
    value.average_price = Decimal(price);
    return value;
}

void same_report(const ExecutionReport& observed, const ExecutionReport& plain) {
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
}  // namespace

TEST(RunConsumptionTest, ActualHistoryAndTwoDailyBatchesKeepOrderAndCompleteReports) {
    RunConsumption run_a;
    RunConsumption run_b;
    ExecutionManager observed_a;
    ExecutionManager plain_a;
    ExecutionManager observed_b;
    ExecutionManager plain_b;
    const auto day = std::chrono::system_clock::from_time_t(1'800'000'000);
    for (int i = 0; i < 2; ++i) {
        auto* history = run_a.append_symbol(run_a.history_updates, "ES.v.0");
        ASSERT_NE(history, nullptr);
        invoke_run_value(&history->call, [&](ExecutionMarketDataObservation* output) {
            observed_a.update_market_data("ES.v.0", 1000.0 + i, 5000.0 + i, output);
        });
        plain_a.update_market_data("ES.v.0", 1000.0 + i, 5000.0 + i);
        EXPECT_EQ(history->call.outcome, RunCallOutcome::ReturnedOk);
        EXPECT_EQ(history->identity, "ES.v.0");
        EXPECT_TRUE(history->call.payload.cost_model_reached);
        EXPECT_TRUE(history->call.payload.previous_close_source.has_value());
    }
    ASSERT_EQ(run_a.history_updates.size(), 2u);
    EXPECT_EQ(run_a.history_updates[0].call.payload.previous_close_source,
              PreviousCloseSource::initial_current_close);
    EXPECT_EQ(run_a.history_updates[1].call.payload.previous_close_source,
              PreviousCloseSource::stored_previous_close);

    auto* first = run_a.append_strategy(run_a.execution_batches, "TREND_A");
    ASSERT_NE(first, nullptr);
    auto observed_first = invoke_run_result(&first->call, [&](DailyExecutionObservation* output) {
        return observed_a.generate_daily_executions(
            {{"ES.v.0", position("ES.v.0", 2.0, 5000.0)}}, {},
            {{"ES.v.0", 5001.0}}, day, "system", output);
    });
    auto plain_first = plain_a.generate_daily_executions(
        {{"ES.v.0", position("ES.v.0", 2.0, 5000.0)}}, {},
        {{"ES.v.0", 5001.0}}, day, "system");
    ASSERT_TRUE(observed_first.is_ok());
    ASSERT_TRUE(plain_first.is_ok());
    ASSERT_EQ(observed_first.value().size(), 1u);
    ASSERT_EQ(plain_first.value().size(), 1u);
    same_report(observed_first.value()[0], plain_first.value()[0]);
    EXPECT_EQ(observed_first.value()[0].order_id, "DAILY_ES.v.0_20270115");
    EXPECT_EQ(first->call.outcome, RunCallOutcome::ReturnedOk);
    ASSERT_EQ(first->call.payload.attempts.size(), 1u);
    EXPECT_EQ(first->call.payload.attempts[0].symbol, "ES.v.0");
    EXPECT_TRUE(first->call.payload.attempts[0].returned);
    EXPECT_TRUE(first->call.payload.attempts[0].execution.cost.explicit_fee_per_contract.has_value());

    auto* next = run_a.append_strategy(run_a.execution_batches, "TREND_NEXT");
    ASSERT_NE(next, nullptr);
    auto observed_next = invoke_run_result(&next->call, [&](DailyExecutionObservation* output) {
        return observed_a.generate_daily_executions(
            {{"CL.v.0", position("CL.v.0", 1.0, 80.0)}}, {},
            {{"CL.v.0", 81.0}}, day, "system", output);
    });
    auto plain_next = plain_a.generate_daily_executions(
        {{"CL.v.0", position("CL.v.0", 1.0, 80.0)}}, {},
        {{"CL.v.0", 81.0}}, day, "system");
    ASSERT_TRUE(observed_next.is_ok());
    ASSERT_TRUE(plain_next.is_ok());
    ASSERT_EQ(observed_next.value().size(), 1u);
    same_report(observed_next.value()[0], plain_next.value()[0]);
    ASSERT_EQ(run_a.execution_batches.size(), 2u);
    EXPECT_EQ(run_a.execution_batches[0].identity, "TREND_A");
    EXPECT_EQ(run_a.execution_batches[1].identity, "TREND_NEXT");
    ASSERT_EQ(run_a.execution_batches[1].call.payload.attempts.size(), 1u);
    EXPECT_EQ(run_a.execution_batches[1].call.payload.attempts[0].symbol, "CL.v.0");

    auto* second = run_b.append_strategy(run_b.execution_batches, "TREND_B");
    ASSERT_NE(second, nullptr);
    auto observed_second = invoke_run_result(&second->call, [&](DailyExecutionObservation* output) {
        return observed_b.generate_daily_executions(
            {}, {{"NQ.v.0", position("NQ.v.0", 3.0, 18000.0)}},
            {{"NQ.v.0", 18001.0}}, day, "system", output);
    });
    auto plain_second = plain_b.generate_daily_executions(
        {}, {{"NQ.v.0", position("NQ.v.0", 3.0, 18000.0)}},
        {{"NQ.v.0", 18001.0}}, day, "system");
    ASSERT_TRUE(observed_second.is_ok());
    ASSERT_TRUE(plain_second.is_ok());
    ASSERT_EQ(observed_second.value().size(), 1u);
    ASSERT_EQ(plain_second.value().size(), 1u);
    same_report(observed_second.value()[0], plain_second.value()[0]);
    EXPECT_EQ(second->call.outcome, RunCallOutcome::ReturnedOk);
    ASSERT_EQ(second->call.payload.attempts.size(), 1u);
    EXPECT_EQ(second->call.payload.attempts[0].branch, DailyPositionBranch::removed_position);
    EXPECT_TRUE(run_b.history_updates.empty());
    EXPECT_TRUE(run_a.execution_batches[0].identity != run_b.execution_batches[0].identity);
}

TEST(RunConsumptionTest, OuterAdmissionFailsClosedAndRunsActualReaderWithNullOutput) {
    RunConsumption strategies;
    for (int i = 0; i < 32; ++i) {
        EXPECT_TRUE(strategies.admit_strategy("S" + std::to_string(i)));
    }
    EXPECT_TRUE(strategies.admit_strategy("S0"));
    EXPECT_FALSE(strategies.admit_strategy("S32"));
    EXPECT_EQ(strategies.failure, RunEvidenceFailure::CapacityExceeded);
    EXPECT_FALSE(strategies.admit_strategy("bad name"));
    EXPECT_EQ(strategies.failure, RunEvidenceFailure::CapacityExceeded);

    RunConsumption invalid;
    EXPECT_FALSE(invalid.admit_symbol("bad symbol"));
    EXPECT_EQ(invalid.failure, RunEvidenceFailure::InvalidObservedIdentity);
    EXPECT_FALSE(invalid.admit_symbol("ES"));
    RunConsumption symbols;
    for (int i = 0; i < 1024; ++i) {
        EXPECT_TRUE(symbols.admit_symbol("Y" + std::to_string(i)));
    }
    EXPECT_TRUE(symbols.admit_symbol("Y0"));
    EXPECT_FALSE(symbols.admit_symbol("Y1024"));
    EXPECT_EQ(symbols.failure, RunEvidenceFailure::CapacityExceeded);

    RunConsumption entries;
    for (size_t i = 0; i < 2048; ++i) {
        ASSERT_NE(entries.append_symbol(entries.history_updates, "ES.v.0"), nullptr);
    }
    EXPECT_EQ(entries.history_updates.size(), 2048u);
    auto* overflow = entries.append_symbol(entries.history_updates, "ES.v.0");
    EXPECT_EQ(overflow, nullptr);
    EXPECT_EQ(entries.failure, RunEvidenceFailure::CapacityExceeded);
    ExecutionManager manager;
    int invokes = 0;
    invoke_run_value(overflow ? &overflow->call : nullptr,
                     [&](ExecutionMarketDataObservation* output) {
                         ++invokes;
                         EXPECT_EQ(output, nullptr);
                         manager.update_market_data("ES.v.0", 1200.0, 5010.0, output);
                     });
    EXPECT_EQ(invokes, 1);
    EXPECT_EQ(entries.history_updates.size(), 2048u);
}

TEST(RunConsumptionTest, ActualSelectorNativeIdentityFailureDropsOffendingPayload) {
    RunConsumption invalid;
    invalid.selection.emplace();
    const nlohmann::json bad_name = {
        {"bad name", {{"enabled_live", true}, {"default_allocation", 1.0}}}};
    auto selected = invoke_run_result(&invalid.selection.value(),
        [&](SelectionConsumption* output) {
            return select_enabled_live_strategies(bad_name, output);
        });
    auto plain = select_enabled_live_strategies(bad_name);
    ASSERT_TRUE(selected.is_ok());
    ASSERT_TRUE(plain.is_ok());
    EXPECT_EQ(selected.value().names, plain.value().names);
    EXPECT_FALSE(invalid.observe_native_limits());
    EXPECT_EQ(invalid.failure, RunEvidenceFailure::InvalidObservedIdentity);
    EXPECT_FALSE(invalid.selection.has_value());
    int invoked = 0;
    auto after = invoke_run_result(static_cast<RunInvocation<SelectionConsumption>*>(nullptr),
        [&](SelectionConsumption* output) {
            ++invoked;
            EXPECT_EQ(output, nullptr);
            return select_enabled_live_strategies(
                {{"A", {{"enabled_live", true}}}}, output);
        });
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(after.value().names, (std::vector<std::string>{"A"}));
    EXPECT_EQ(invoked, 1);

    RunConsumption capacity;
    capacity.selection.emplace();
    nlohmann::json many = nlohmann::json::object();
    for (int i = 0; i < 33; ++i) {
        many["S" + std::to_string(i)] = {{"enabled_live", false}};
    }
    auto no_selection = invoke_run_result(&capacity.selection.value(),
        [&](SelectionConsumption* output) {
            return select_enabled_live_strategies(many, output);
        });
    ASSERT_TRUE(no_selection.is_error());
    EXPECT_FALSE(capacity.observe_native_limits());
    EXPECT_EQ(capacity.failure, RunEvidenceFailure::CapacityExceeded);
    EXPECT_FALSE(capacity.selection.has_value());
    RunConsumption lengths;
    EXPECT_FALSE(lengths.admit_strategy(std::string(65, 'A')));
    EXPECT_EQ(lengths.failure, RunEvidenceFailure::InvalidObservedIdentity);
    RunConsumption symbol;
    EXPECT_TRUE(symbol.admit_symbol("ES/FUT.v:0-1"));
    EXPECT_FALSE(symbol.admit_strategy("ES/FUT"));
    EXPECT_EQ(symbol.failure, RunEvidenceFailure::InvalidObservedIdentity);
}

TEST(RunConsumptionTest, EmptyLoopsSkippedStagesAndBenchmarkFailureStayDistinct) {
    RunConsumption run;
    run.history_loop.reached = true;
    run.history_loop.completed = true;
    run.execution_loop.reached = true;
    run.execution_loop.completed = true;
    EXPECT_TRUE(run.history_updates.empty());
    EXPECT_TRUE(run.execution_batches.empty());
    run.observe_non_trading_decision(true);
    EXPECT_TRUE(run.non_trading_decision_reached);
    EXPECT_TRUE(run.skip_strategy_processing);
    EXPECT_TRUE(run.preparation_skipped);
    EXPECT_TRUE(run.primary_skipped);
    EXPECT_FALSE(run.preparation.has_value());
    EXPECT_FALSE(run.primary.has_value());

    run.observe_benchmark_mode("deferred");
    EXPECT_EQ(run.benchmark_mode, RunBenchmarkMode::Deferred);
    EXPECT_EQ(run.benchmark_state, RunBenchmarkState::Deferred);
    run.observe_benchmark_mode("live");
    EXPECT_EQ(run.benchmark_mode, RunBenchmarkMode::Live);
    EXPECT_EQ(run.benchmark_state, RunBenchmarkState::InProgress);
    run.note_benchmark_error();
    run.note_benchmark_success();
    EXPECT_EQ(run.benchmark_state, RunBenchmarkState::NonfatalError);
    run.observe_benchmark_mode("unexpected-secret-text");
    EXPECT_EQ(run.benchmark_mode, RunBenchmarkMode::Unsupported);
    EXPECT_EQ(run.benchmark_state, RunBenchmarkState::Deferred);

    RunConsumption other;
    other.observe_non_trading_decision(false);
    EXPECT_FALSE(other.preparation_skipped);
    EXPECT_FALSE(other.primary_skipped);
    other.observe_benchmark_mode("live");
    other.note_benchmark_success();
    EXPECT_EQ(other.benchmark_state, RunBenchmarkState::Succeeded);
    EXPECT_EQ(run.failure, RunEvidenceFailure::None);
}

TEST(RunConsumptionTest, PnlOperandsDistinguishAllocationHitFromFallback) {
    RunConsumption run;
    auto* hit = run.append_pnl("TREND_A");
    ASSERT_NE(hit, nullptr);
    hit->record_operands(true, 0.25, 100000.0, 25000.0);
    EXPECT_TRUE(hit->allocation_found);
    EXPECT_DOUBLE_EQ(hit->allocation, 0.25);
    EXPECT_DOUBLE_EQ(hit->initial_capital, 100000.0);
    EXPECT_DOUBLE_EQ(hit->strategy_capital, 25000.0);
    auto* fallback = run.append_pnl("TREND_B");
    ASSERT_NE(fallback, nullptr);
    fallback->record_operands(false, 1.0, 100000.0, 100000.0);
    EXPECT_FALSE(fallback->allocation_found);
    EXPECT_DOUBLE_EQ(fallback->allocation, 1.0);
    EXPECT_DOUBLE_EQ(fallback->strategy_capital, 100000.0);
}

TEST(RunConsumptionTest, ActualDailyErrorAndBoundedThrowPreserveOutcomes) {
    ExecutionManager manager;
    RunConsumption run;
    auto* batch = run.append_strategy(run.execution_batches, "TREND_A");
    ASSERT_NE(batch, nullptr);
    auto error = invoke_run_result(&batch->call, [&](DailyExecutionObservation* output) {
        return manager.generate_daily_executions({}, {}, {},
            std::chrono::system_clock::from_time_t(1'800'000'000), "unsupported", output);
    });
    auto plain = manager.generate_daily_executions({}, {}, {},
        std::chrono::system_clock::from_time_t(1'800'000'000), "unsupported");
    ASSERT_TRUE(error.is_error());
    ASSERT_TRUE(plain.is_error());
    EXPECT_EQ(error.error()->code(), plain.error()->code());
    EXPECT_STREQ(error.error()->what(), plain.error()->what());
    EXPECT_EQ(batch->call.outcome, RunCallOutcome::ReturnedError);
    EXPECT_EQ(batch->call.payload.state, DailyExecutionState::rejected_stream);

    RunInvocation<ExecutionMarketDataObservation> thrown;
    int count = 0;
    try {
        invoke_run_value(&thrown, [&](ExecutionMarketDataObservation* output) -> int {
            ++count;
            EXPECT_NE(output, nullptr);
            throw std::runtime_error("bounded-reader-throw");
        });
        FAIL() << "expected original exception";
    } catch (const std::runtime_error& exception) {
        EXPECT_STREQ(exception.what(), "bounded-reader-throw");
    }
    EXPECT_EQ(count, 1);
    EXPECT_EQ(thrown.outcome, RunCallOutcome::Threw);
}

TEST(RunConsumptionTest, SnapshotRiskUsesItsOwnActualReaderAndEmptyPriceDefault) {
    RiskConfig config;
    config.var_limit = 0.37;
    config.jump_risk_limit = 0.12;
    config.max_correlation = 0.8;
    config.max_gross_leverage = 3.0;
    config.max_net_leverage = 2.0;
    config.confidence_level = 0.975;
    config.capital = Decimal(120000.0);
    RiskManager snapshot(config);
    MarketData market;
    market.ordered_symbols = {"ES.v.0"};
    market.symbol_indices = {{"ES.v.0", 0}};
    market.returns = {{0.01}, {-0.01}};
    market.covariance = {{0.0001}};
    const std::unordered_map<std::string, Position> positions = {
        {"ES.v.0", position("ES.v.0", 2.0, 5000.0)}};
    RunConsumption run;
    run.snapshot_risk.emplace();
    auto observed = invoke_run_result(&run.snapshot_risk.value(),
        [&](RiskConfigConsumption* output) {
            return snapshot.process_positions(positions, market, {}, output);
        });
    auto plain = snapshot.process_positions(positions, market);
    ASSERT_TRUE(observed.is_ok());
    ASSERT_TRUE(plain.is_ok());
    EXPECT_EQ(run.snapshot_risk->outcome, RunCallOutcome::ReturnedOk);
    EXPECT_EQ(run.snapshot_risk->payload.var_limit, 0.37);
    EXPECT_EQ(run.snapshot_risk->payload.jump_risk_limit, 0.12);
    EXPECT_EQ(run.snapshot_risk->payload.confidence_level, 0.975);
    EXPECT_EQ(run.snapshot_risk->payload.capital, Decimal(120000.0));
    EXPECT_DOUBLE_EQ(observed.value().recommended_scale, plain.value().recommended_scale);
    EXPECT_DOUBLE_EQ(observed.value().portfolio_var, plain.value().portfolio_var);
    EXPECT_DOUBLE_EQ(observed.value().gross_leverage, plain.value().gross_leverage);
    EXPECT_DOUBLE_EQ(observed.value().net_leverage, plain.value().net_leverage);
}

TEST(RunConsumptionTest, ActualFactoryStrategyRegistrationAndPrimaryReaderStaySeparateFromBenchmark) {
    StateManager::reset_instance();
    auto db = std::make_shared<trade_ngin::testing::MockPostgresDatabase>("mock://run-consumption");
    ASSERT_TRUE(db->connect().is_ok());
    StrategySelection selection;
    selection.names = {"TREND_A"};
    selection.allocations = {{"TREND_A", 0.5}};
    selection.configs = {{"TREND_A", {{"type", "TrendFollowingStrategy"}}}};
    StrategyConfig strategy_config;
    strategy_config.max_leverage = 10.0;
    strategy_config.max_drawdown = 0.5;
    StrategyDefaultsConfig defaults;
    auto build = [&](FactoryConsumption* output = nullptr) {
        return build_strategy_instances(selection, strategy_config, 100000.0,
                                        defaults, std::nullopt, db, nullptr, output);
    };
    RunConsumption run;
    run.primary_factory.emplace();
    auto observed = invoke_run_value(&run.primary_factory.value(),
                                     [&](FactoryConsumption* output) { return build(output); });
    ASSERT_EQ(observed.size(), 1u);
    EXPECT_EQ(run.primary_factory->outcome, RunCallOutcome::ReturnedOk);
    ASSERT_EQ(run.primary_factory->payload.entries.size(), 1u);
    EXPECT_EQ(run.primary_factory->payload.entries[0].name, "TREND_A");
    EXPECT_EQ(run.primary_factory->payload.entries[0].profile, FactoryProfile::Standard);
    EXPECT_DOUBLE_EQ(*run.primary_factory->payload.entries[0].allocated_capital, 50000.0);
    auto benchmark = build();
    ASSERT_EQ(benchmark.size(), 1u);
    EXPECT_EQ(run.primary_factory->payload.entries.size(), 1u);
    EXPECT_EQ(typeid(*observed[0]), typeid(*benchmark[0]));
    EXPECT_EQ(observed[0]->get_metadata().id, benchmark[0]->get_metadata().id);

    std::vector<Bar> bars;
    const auto start = std::chrono::system_clock::from_time_t(1'700'000'000);
    for (int i = 0; i < 300; ++i) {
        Bar bar;
        bar.symbol = "ES.v.0";
        bar.timestamp = start + std::chrono::hours(24 * i);
        bar.open = bar.close = Decimal(100.0 + i * 0.01);
        bar.high = Decimal(101.0 + i * 0.01);
        bar.low = Decimal(99.0 + i * 0.01);
        bar.volume = 100000.0;
        bars.push_back(bar);
    }
    run.preparation.emplace();
    run.preparation->identity = "TREND_A";
    auto observed_prewarm = invoke_run_result(&run.preparation->call,
        [&](StrategyConsumptionTrace* output) { return observed[0]->on_data(bars, output); });
    auto plain_prewarm = benchmark[0]->on_data(bars);
    EXPECT_EQ(observed_prewarm.is_ok(), plain_prewarm.is_ok());
    EXPECT_EQ(run.preparation->call.outcome,
              observed_prewarm.is_ok() ? RunCallOutcome::ReturnedOk : RunCallOutcome::ReturnedError);
    EXPECT_EQ(run.preparation->call.payload.profile, StrategyConsumptionProfile::Standard);
    EXPECT_TRUE(run.preparation->call.payload.history.max_history_size.has_value());

    PortfolioConfig config;
    config.total_capital = Decimal(100000.0);
    config.reserve_capital = Decimal(0.0);
    std::unique_ptr<PortfolioManager> primary;
    std::unique_ptr<PortfolioManager> plain;
    struct PortfolioBusCleanup {
        std::unique_ptr<PortfolioManager>& primary;
        std::unique_ptr<PortfolioManager>& plain;
        bool subscribed = false;

        bool release() {
            // PortfolioManager registers a raw-this callback under this fixed bus key.
            // Deactivate it before either manager can be destroyed, even on ASSERT return.
            const bool ok = !subscribed ||
                MarketDataBus::instance().unsubscribe("PORTFOLIO_MANAGER").is_ok();
            subscribed = false;
            plain.reset();
            primary.reset();
            return ok;
        }

        ~PortfolioBusCleanup() { (void)release(); }
    } cleanup{primary, plain};
    primary = std::make_unique<PortfolioManager>(config, "RUN_PRIMARY_OBS");
    cleanup.subscribed = true;
    plain = std::make_unique<PortfolioManager>(config, "RUN_PRIMARY_PLAIN");
    auto* registration = run.append_strategy(run.registrations, "TREND_A");
    ASSERT_NE(registration, nullptr);
    auto add_observed = invoke_run_result(&registration->call,
        [&](PortfolioRegistrationTrace* output) {
            return primary->add_strategy(observed[0], 0.5, false, false, output);
        });
    auto add_plain = plain->add_strategy(benchmark[0], 0.5, false, false);
    EXPECT_EQ(add_observed.is_ok(), add_plain.is_ok());
    ASSERT_TRUE(add_observed.is_ok());
    EXPECT_EQ(registration->call.outcome, RunCallOutcome::ReturnedOk);
    EXPECT_EQ(registration->call.payload.stored_allocation, 0.5);
    run.primary.emplace();
    auto processed = invoke_run_result(&run.primary.value(),
        [&](PortfolioConsumptionTrace* output) {
            return primary->process_market_data(bars, false, std::nullopt, output);
        });
    auto plain_processed = plain->process_market_data(bars, false, std::nullopt);
    EXPECT_EQ(processed.is_ok(), plain_processed.is_ok());
    ASSERT_TRUE(processed.is_ok());
    EXPECT_EQ(run.primary->outcome, RunCallOutcome::ReturnedOk);
    EXPECT_EQ(run.primary->payload.skip_execution_generation, false);
    EXPECT_GE(run.primary->payload.pass_count, 1u);
    ASSERT_EQ(run.primary->payload.strategies.size(), 1u);
    EXPECT_EQ(run.primary->payload.strategies[0].strategy_id, "TREND_A");
    const auto observed_positions = primary->get_strategy_positions();
    const auto plain_positions = plain->get_strategy_positions();
    ASSERT_EQ(observed_positions.size(), 1u);
    ASSERT_EQ(plain_positions.size(), 1u);
    ASSERT_EQ(observed_positions.count("TREND_A"), 1u);
    ASSERT_EQ(plain_positions.count("TREND_A"), 1u);
    const auto& observed_symbols = observed_positions.at("TREND_A");
    const auto& plain_symbols = plain_positions.at("TREND_A");
    ASSERT_EQ(observed_symbols.size(), 1u);
    ASSERT_EQ(plain_symbols.size(), 1u);
    ASSERT_EQ(observed_symbols.count("ES.v.0"), 1u);
    ASSERT_EQ(plain_symbols.count("ES.v.0"), 1u);
    const auto& value = observed_symbols.at("ES.v.0");
    const auto& comparison = plain_symbols.at("ES.v.0");
    EXPECT_EQ(value.symbol, "ES.v.0");
    EXPECT_EQ(value.quantity, Decimal(0.0));
    EXPECT_EQ(value.average_price, Decimal(102.99));
    EXPECT_EQ(value.unrealized_pnl, Decimal(0.0));
    EXPECT_EQ(value.realized_pnl, Decimal(0.0));
    EXPECT_EQ(value.last_update, bars.back().timestamp);
    EXPECT_EQ(value.symbol, comparison.symbol);
    EXPECT_EQ(value.quantity, comparison.quantity);
    EXPECT_EQ(value.average_price, comparison.average_price);
    EXPECT_EQ(value.unrealized_pnl, comparison.unrealized_pnl);
    EXPECT_EQ(value.realized_pnl, comparison.realized_pnl);
    EXPECT_EQ(value.last_update, comparison.last_update);
    EXPECT_TRUE(cleanup.release());
    observed[0]->stop();
    benchmark[0]->stop();
    db->disconnect();
    StateManager::reset_instance();
}
