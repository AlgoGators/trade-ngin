#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <set>

#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/apps/consumption_projection.hpp"
#include "trade_ngin/apps/run_consumption.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"
#include "../data/test_db_utils.hpp"

namespace trade_ngin {
namespace {
using Json = nlohmann::json;

const Json* find_node(const Json& document, std::string_view consumer, size_t ordinal = 0) {
    for (const auto& node : document.at("nodes")) {
        if (node.at("consumer") == consumer) {
            if (ordinal == 0) return &node;
            --ordinal;
        }
    }
    return nullptr;
}
const Json* find_read(const Json& node, std::string_view field) {
    for (const auto& read : node.at("reads")) if (read.at("field") == field) return &read;
    return nullptr;
}
void expect_unavailable(const Json& document, std::string_view reason) {
    ASSERT_EQ(document.at("status"), "unavailable");
    EXPECT_EQ(document.at("reason"), reason);
    EXPECT_TRUE(document.at("nodes").empty());
    for (auto it = document.at("coverage").begin(); it != document.at("coverage").end(); ++it) {
        EXPECT_EQ(it.value().at("status"), "unavailable");
        EXPECT_EQ(it.value().at("reason"), reason);
    }
}

void fill_cost(transaction_cost::CostChargeObservation& x) {
    x.input_source = transaction_cost::CostInputSource::internally_tracked;
    x.asset_lookup.path = transaction_cost::AssetLookupPath::exact_symbol;
    x.spread.baseline_spread_ticks = 1.0;
    x.spread.min_spread_ticks = 0.5;
    x.spread.max_spread_ticks = 2.0;
    x.spread.spread_cost_multiplier = 1.0;
    x.spread.tick_size = 0.25;
    x.volatility.lambda = 0.94;
    x.volatility.min_multiplier = 0.5;
    x.volatility.max_multiplier = 2.0;
    x.impact.min_adv = 100.0;
    x.impact.min_participation = 0.01;
    x.impact.max_participation = 0.1;
    x.impact.max_impact_bps = 50.0;
    x.explicit_fee_per_contract = 1.5;
    x.point_value = 50.0;
    x.quantity = 42.0;  // Deliberately private, never a public read.
    x.reference_price = 5000.0;
}

void fill_risk(RiskConfigConsumption& x) {
    x.var_limit = 0.1;
    x.jump_risk_limit = 0.2;
    x.max_correlation = 0.9;
    x.max_gross_leverage = 4.0;
    x.max_net_leverage = 2.0;
    x.confidence_level = 0.99;
    x.capital = Decimal::from_raw(123456789);
}

void fill_strategy(StrategyConsumptionTrace& x, const std::vector<std::string>& symbols) {
    x.profile = StrategyConsumptionProfile::Standard;
    x.history.max_history_size = 252;
    x.history.ema_windows = std::vector<std::pair<int, int>>{{2, 8}, {4, 16}, {8, 32},
        {16, 64}, {32, 128}, {64, 256}};
    x.volatility.vol_lookback_short = 25;
    x.volatility.max_history_size = 252;
    x.forecast.ema_windows = *x.history.ema_windows;
    x.forecast.vol_lookback_short = 25;
    x.forecast.fdm = std::vector<std::pair<int, double>>{{0, 1.0}, {1, 1.1}, {2, 1.2},
        {3, 1.3}, {4, 1.4}, {5, 1.5}, {6, 1.6}, {7, 1.7}};
    x.regime.vol_lookback_long = 100;
    x.sizing.capital_allocation = 100000.0;
    x.sizing.max_leverage = 4.0;
    x.sizing.idm = 1.2;
    x.sizing.risk_target = 0.2;
    x.sizing.fx_rate = 1.0;
    x.sizing.max_symbol_concentration = 0.3;
    x.buffering.use_position_buffering = true;
    x.buffering.weight = 0.5;
    x.buffering.capital_allocation = 100000.0;
    x.buffering.idm = 1.2;
    x.buffering.risk_target = 0.2;
    x.buffering.fx_rate = 1.0;
    x.buffering.carver_buffer_floor = 0.1;
    x.buffering.carver_buffer_position_factor = 0.2;
    x.base_risk.supported = true;
    x.base_risk.capital_allocation = 100000.0;
    x.base_risk.risk_max_leverage = Decimal::from_raw(400000000);
    x.base_risk.fallback_config_max_leverage = 4.0;
    x.base_risk.risk_max_drawdown = Decimal::from_raw(20000000);
    x.position_limits.supported = true;
    for (const auto& symbol : symbols) {
        x.sizing.symbol_limits[symbol] = {true, 10.0};
        x.buffering.symbol_limits[symbol] = {true, 10.0};
        x.base_risk.trading_multipliers[symbol] = {true, 1.0};
        x.position_limits.symbols[symbol] = {true, 100.0};
    }
}

RunConsumption synthetic_full_run(const std::vector<std::string>& symbols = {"ES"}) {
    RunConsumption run;
    run.controlled_selection = true;
    run.selection.emplace();
    run.selection->outcome = RunCallOutcome::ReturnedOk;
    SelectionRead selected;
    selected.name = "S0";
    selected.enabled_live_read = true;
    selected.enabled_live_present = true;
    selected.enabled_live_value = true;
    selected.allocation_read = true;
    selected.allocation_defaulted = false;
    selected.allocation_value = 1.0;
    selected.effective_allocation = 1.0;
    run.selection->payload.controlled_validation.push_back(selected);
    run.selection->payload.ordinary_selection.push_back(selected);
    run.selection->payload.controlled_sum = 1.0;
    run.selection->payload.ordinary_sum = 1.0;
    run.selection->payload.ordinary_normalized = false;
    run.primary_factory.emplace();
    run.primary_factory->outcome = RunCallOutcome::ReturnedOk;
    FactoryRead factory;
    factory.name = "S0";
    factory.type_defaulted = false;
    factory.profile = FactoryProfile::Standard;
    factory.effective_allocation = 1.0;
    factory.initial_capital_argument = 100000.0;
    factory.allocated_capital = 100000.0;
    factory.construction = SetupStage::Succeeded;
    factory.initialize = SetupStage::Succeeded;
    factory.start = SetupStage::Succeeded;
    run.primary_factory->payload.entries.push_back(factory);
    auto& registration = run.registrations.emplace_back();
    registration.identity = "S0";
    registration.call.outcome = RunCallOutcome::ReturnedOk;
    auto& r = registration.call.payload;
    r.outcome = PortfolioCallOutcome::ReturnedOk;
    r.initial_allocation = 1.0;
    r.min_allocation = 0.0;
    r.max_allocation = 1.0;
    r.total_allocation = 1.0;
    r.total_within_limit = true;
    r.requested_optimization = true;
    r.portfolio_optimization = true;
    r.requested_risk = true;
    r.portfolio_risk = true;
    r.stored_allocation = 1.0;
    r.stored_optimization = true;
    r.stored_risk = true;
    run.historical_days_reached = true;
    run.historical_days = 252;
    run.market_fetch.outcome = RunCallOutcome::ReturnedOk;
    run.arrow_conversion.outcome = RunCallOutcome::ReturnedOk;
    run.market_input_completed = true;
    run.history_loop = {true, true};
    for (const auto& symbol : symbols) {
        auto& update = run.history_updates.emplace_back();
        update.identity = symbol;
        update.call.outcome = RunCallOutcome::ReturnedOk;
        update.call.payload.cost_model_reached = true;
        update.call.payload.previous_close_source = PreviousCloseSource::initial_current_close;
        update.call.payload.market_data.volume.adv_lookback_days = 20;
        update.call.payload.market_data.log_returns.lookback_days = 30;
    }
    run.non_trading_decision_reached = true;
    run.skip_strategy_processing = false;
    run.preparation_stage = {true, true};
    run.primary_stage = {true, true};
    run.preparation.emplace();
    run.preparation->identity = "S0";
    run.preparation->call.outcome = RunCallOutcome::ReturnedOk;
    fill_strategy(run.preparation->call.payload, symbols);
    run.primary.emplace();
    run.primary->outcome = RunCallOutcome::ReturnedOk;
    run.primary->payload.outcome = PortfolioCallOutcome::ReturnedOk;
    run.primary->payload.skip_execution_generation = false;
    auto& primary_strategy = run.primary->payload.strategies.emplace_back();
    primary_strategy.strategy_id = "S0";
    primary_strategy.outcome = PortfolioCallOutcome::ReturnedOk;
    fill_strategy(primary_strategy.strategy, symbols);
    auto& pass = run.primary->payload.passes[0];
    run.primary->payload.pass_count = 1;
    pass.use_optimization = true;
    pass.use_risk_management = true;
    pass.optimization_helper = PortfolioCallOutcome::ReturnedOk;
    pass.optimization.total_capital = Decimal::from_raw(10000000000000LL);
    for (auto& map : pass.optimization.strategies) map["S0"] = {true, 1.0};
    auto& estimate = pass.optimization.estimates.emplace_back();
    estimate.symbol_index = 0;
    estimate.symbol = symbols.front();
    estimate.charge_call = PortfolioCallOutcome::ReturnedOk;
    fill_cost(estimate.charge);
    pass.optimization.optimizer_call = PortfolioCallOutcome::ReturnedOk;
    pass.optimization.optimizer.buffer_branch = OptimizationBufferBranch::Applied;
    auto& o = pass.optimization.optimizer.consumed_config;
    o.cost_penalty_scalar = 0.1;
    o.max_iterations = 100;
    o.convergence_threshold = 0.001;
    o.use_buffering = true;
    o.tau = 0.2;
    o.buffer_size_factor = 0.3;
    pass.risk_helper = PortfolioCallOutcome::ReturnedOk;
    pass.risk.source = PortfolioRiskManagerSource::Internal;
    pass.risk.lookback_period = 252;
    pass.risk.risk_call = PortfolioCallOutcome::ReturnedOk;
    fill_risk(pass.risk.risk);
    auto& strategy_charge = run.primary->payload.strategy_charges.emplace_back();
    strategy_charge.purpose = PortfolioChargePurpose::PerStrategy;
    strategy_charge.strategy_id = "S0";
    strategy_charge.symbol = symbols.front();
    strategy_charge.charge_call = PortfolioCallOutcome::ReturnedOk;
    fill_cost(strategy_charge.charge);
    auto& compatibility = run.primary->payload.compatibility_charges.emplace_back();
    compatibility.purpose = PortfolioChargePurpose::Compatibility;
    compatibility.symbol = symbols.front();
    compatibility.charge_call = PortfolioCallOutcome::ReturnedOk;
    fill_cost(compatibility.charge);
    run.execution_loop = {true, true};
    auto& batch = run.execution_batches.emplace_back();
    batch.identity = "S0";
    batch.call.outcome = RunCallOutcome::ReturnedOk;
    batch.call.payload.state = DailyExecutionState::returned;
    auto& attempt = batch.call.payload.attempts.emplace_back();
    attempt.symbol = symbols.front();
    attempt.sequence = 0;
    attempt.execution.state = ExecutionCallState::returned;
    attempt.returned = true;
    fill_cost(attempt.execution.cost);
    run.pnl_path_decision_reached = true;
    run.pnl_path_eligible = true;
    run.pnl_loop = {true, true};
    auto& pnl = run.pnl_finalizations.emplace_back();
    pnl.strategy = "S0";
    pnl.record_operands(true, 1.0, 100000.0, 100000.0);
    pnl.call.outcome = RunCallOutcome::ReturnedOk;
    run.diagnostics_reached = true;
    run.diagnostics_completed = true;
    run.snapshot_risk.emplace();
    run.snapshot_risk->outcome = RunCallOutcome::ReturnedOk;
    fill_risk(run.snapshot_risk->payload);
    run.benchmark_decision_reached = true;
    run.benchmark_mode = RunBenchmarkMode::Live;
    run.benchmark_state = RunBenchmarkState::Succeeded;
    return run;
}

RunConsumption synthetic_setup_prefix() {
    auto full = synthetic_full_run();
    RunConsumption prefix;
    prefix.controlled_selection = full.controlled_selection;
    prefix.selection = std::move(full.selection);
    prefix.primary_factory = std::move(full.primary_factory);
    prefix.registrations = std::move(full.registrations);
    return prefix;
}

void add_successful_setup_identity(RunConsumption& run, const std::string& name) {
    if (!run.primary_factory) {
        run.primary_factory.emplace();
        run.primary_factory->outcome = RunCallOutcome::ReturnedOk;
    }
    FactoryRead factory;
    factory.name = name;
    factory.construction = SetupStage::Succeeded;
    factory.initialize = SetupStage::Succeeded;
    factory.start = SetupStage::Succeeded;
    run.primary_factory->payload.entries.push_back(std::move(factory));
    auto& registration = run.registrations.emplace_back();
    registration.identity = name;
    registration.call.outcome = RunCallOutcome::ReturnedOk;
    registration.call.payload.outcome = PortfolioCallOutcome::ReturnedOk;
}

RunConsumption synthetic_typical_run() {
    std::vector<std::string> symbols;
    for (int i = 0; i < 40; ++i)
        symbols.push_back("F" + std::to_string(i / 10) + std::to_string(i % 10));
    auto run = synthetic_full_run(symbols);
    for (int strategy = 1; strategy < 3; ++strategy) {
        const std::string name = "S" + std::to_string(strategy);
        auto selection = run.selection->payload.ordinary_selection.front();
        selection.name = name;
        run.selection->payload.ordinary_selection.push_back(selection);
        run.selection->payload.controlled_validation.push_back(selection);
        auto factory = run.primary_factory->payload.entries.front();
        factory.name = name;
        run.primary_factory->payload.entries.push_back(factory);
        auto registration = run.registrations.front();
        registration.identity = name;
        run.registrations.push_back(registration);
        auto call = run.primary->payload.strategies.front();
        call.strategy_id = name;
        run.primary->payload.strategies.push_back(std::move(call));
        auto batch = run.execution_batches.front();
        batch.identity = name;
        run.execution_batches.push_back(std::move(batch));
    }
    for (auto* phase : {&run.selection->payload.ordinary_selection,
                        &run.selection->payload.controlled_validation})
        for (auto& entry : *phase) {
            entry.allocation_value = 1.0 / 3.0;
            entry.effective_allocation = 1.0 / 3.0;
        }
    for (auto& entry : run.primary_factory->payload.entries) {
        entry.effective_allocation = 1.0 / 3.0;
        entry.allocated_capital = 100000.0 / 3.0;
    }
    for (size_t i = 0; i < run.registrations.size(); ++i) {
        auto& r = run.registrations[i].call.payload;
        r.initial_allocation = 1.0 / 3.0;
        r.stored_allocation = 1.0 / 3.0;
        r.total_allocation = static_cast<double>(i + 1) / 3.0;
    }
    run.primary->payload.pass_count = 5;
    auto template_pass = run.primary->payload.passes[0];
    for (size_t pass_index = 0; pass_index < 5; ++pass_index) {
        auto& pass = run.primary->payload.passes[pass_index];
        pass = template_pass;
        for (auto& map : pass.optimization.strategies) {
            map["S0"] = {true, 1.0 / 3.0};
            for (int strategy = 1; strategy < 3; ++strategy)
                map["S" + std::to_string(strategy)] = {true, 1.0 / 3.0};
        }
        pass.optimization.estimates.clear();
        for (size_t i = 0; i < symbols.size(); ++i) {
            auto estimate = template_pass.optimization.estimates.front();
            estimate.symbol_index = i;
            estimate.symbol = symbols[i];
            pass.optimization.estimates.push_back(std::move(estimate));
        }
    }
    run.primary->payload.strategy_charges.clear();
    run.primary->payload.compatibility_charges.clear();
    for (int strategy = 0; strategy < 3; ++strategy) {
        for (const auto& symbol : symbols) {
            PortfolioExecutionCharge charge;
            charge.purpose = PortfolioChargePurpose::PerStrategy;
            charge.strategy_id = "S" + std::to_string(strategy);
            charge.symbol = symbol;
            charge.charge_call = PortfolioCallOutcome::ReturnedOk;
            fill_cost(charge.charge);
            run.primary->payload.strategy_charges.push_back(std::move(charge));
        }
    }
    for (const auto& symbol : symbols) {
        PortfolioExecutionCharge charge;
        charge.purpose = PortfolioChargePurpose::Compatibility;
        charge.symbol = symbol;
        charge.charge_call = PortfolioCallOutcome::ReturnedOk;
        fill_cost(charge.charge);
        run.primary->payload.compatibility_charges.push_back(std::move(charge));
    }
    for (auto& batch : run.execution_batches) {
        batch.call.payload.attempts.clear();
        for (size_t i = 0; i < symbols.size(); ++i) {
            DailyExecutionAttempt attempt;
            attempt.symbol = symbols[i];
            attempt.sequence = i;
            attempt.execution.state = ExecutionCallState::returned;
            attempt.returned = true;
            fill_cost(attempt.execution.cost);
            batch.call.payload.attempts.push_back(std::move(attempt));
        }
    }
    return run;
}

TEST(ConsumptionProjectionTest, CompiledDescriptorEqualsExactLocalAuthorityMirror) {
    std::ifstream source("contracts/consumption-v2-catalog.json", std::ios::binary);
    ASSERT_TRUE(source.good());
    const auto mirror = Json::parse(source);
    EXPECT_EQ(consumption_projection_catalog(), mirror);
    EXPECT_EQ(mirror.at("consumers").size(), 42u);
    EXPECT_EQ(mirror.at("fields").size(), 182u);
    EXPECT_EQ(mirror.at("metadata").size(), 40u);
}

TEST(ConsumptionProjectionTest, EnteredSelectorProjectsActualCall) {
    RunConsumption run;
    run.controlled_selection = false;
    run.selection.emplace();
    run.selection->outcome = RunCallOutcome::ReturnedOk;

    const auto projection = project_run_consumption(run);
    const auto& document = projection.document();
    ASSERT_EQ(document.at("status"), "partial");
    ASSERT_EQ(document.at("nodes").size(), 1u);
    EXPECT_EQ(document.at("nodes").at(0).at("consumer"), "setup.selector");
    EXPECT_EQ(document.at("nodes").at(0).at("outcome"), "returned_ok");
}

TEST(ConsumptionProjectionTest, ActualSelectorHelperPreservesParityAndReturnedReadOrigins) {
    const Json config = {{"A", {{"enabled_live", true}, {"default_allocation", 0.4}}},
                         {"B", {{"enabled_live", true}, {"default_allocation", 0.6}}}};
    RunConsumption run;
    run.selection.emplace();
    auto observed = invoke_run_result(&*run.selection, [&](SelectionConsumption* out) {
        return select_enabled_live_strategies(config, out);
    });
    auto plain = select_enabled_live_strategies(config);
    ASSERT_TRUE(observed.is_ok());
    ASSERT_TRUE(plain.is_ok());
    EXPECT_EQ(observed.value().names, plain.value().names);
    const auto document = project_run_consumption(run).document();
    const auto* entry = find_node(document, "setup.selection_entry");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->at("strategy"), "A");
    const auto* read = find_read(*entry, "setup.selection.enabled_live");
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->at("value"), true);
    EXPECT_EQ(read->at("origin"), "configured_strategy_leaf");
    EXPECT_EQ(document.at("coverage").at("setup").at("status"), "partial");
}

TEST(ConsumptionProjectionTest, UnenteredNativeDefaultsDoNotBecomeCalls) {
    RunConsumption run;
    run.selection.emplace();
    run.primary.emplace();
    expect_unavailable(project_run_consumption(run).document(), "instrumentation_missing");
}

TEST(ConsumptionProjectionTest, InvalidIdentityAndFiniteValueDiscardEntireTree) {
    RunConsumption bad_identity;
    bad_identity.selection.emplace();
    bad_identity.selection->outcome = RunCallOutcome::ReturnedOk;
    bad_identity.selection->payload.ordinary_selection.emplace_back().name = "bad name";
    expect_unavailable(project_run_consumption(bad_identity).document(), "invalid_observed_identity");

    RunConsumption bad_value;
    bad_value.selection.emplace();
    bad_value.selection->outcome = RunCallOutcome::ReturnedOk;
    bad_value.selection->payload.ordinary_sum = std::numeric_limits<double>::infinity();
    expect_unavailable(project_run_consumption(bad_value).document(), "invalid_observed_value");
}

TEST(ConsumptionProjectionTest, RawDecimalEndpointsAreExactStrings) {
    RunConsumption run;
    run.snapshot_risk.emplace();
    run.snapshot_risk->outcome = RunCallOutcome::ReturnedOk;
    run.snapshot_risk->payload.capital = Decimal::from_raw(std::numeric_limits<int64_t>::min());
    auto document = project_run_consumption(run).document();
    const auto* risk = find_node(document, "risk.diagnostics");
    ASSERT_NE(risk, nullptr);
    const auto* capital = find_read(*risk, "risk.capital");
    ASSERT_NE(capital, nullptr);
    EXPECT_EQ(capital->at("value"), "-92233720368.54775808");
    EXPECT_EQ(capital->at("value_type"), "fixed_decimal8");
    run.snapshot_risk->payload.capital = Decimal::from_raw(std::numeric_limits<int64_t>::max());
    document = project_run_consumption(run).document();
    risk = find_node(document, "risk.diagnostics");
    ASSERT_NE(risk, nullptr);
    capital = find_read(*risk, "risk.capital");
    ASSERT_NE(capital, nullptr);
    EXPECT_EQ(capital->at("value"), "92233720368.54775807");
}

TEST(ConsumptionProjectionTest, RepeatedHistoryCallsRemainDistinctAndOrdered) {
    RunConsumption run;
    for (int i = 0; i < 2; ++i) {
        auto& entry = run.history_updates.emplace_back();
        entry.identity = i == 0 ? "NQ" : "ES";
        entry.call.outcome = RunCallOutcome::ReturnedOk;
        entry.call.payload.cost_model_reached = true;
        entry.call.payload.market_data.volume.adv_lookback_days = 10 + i;
    }
    auto doc = project_run_consumption(run).document();
    const auto* first = find_node(doc, "execution.history_update", 0);
    const auto* second = find_node(doc, "execution.history_update", 1);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->at("symbol"), "NQ");
    EXPECT_EQ(second->at("symbol"), "ES");
    ASSERT_NE(find_node(doc, "cost.history", 0), nullptr);
    EXPECT_FALSE(find_node(doc, "cost.history", 0)->contains("outcome"));
}

TEST(ConsumptionProjectionTest, NativeContainerLimitIsInclusiveAndOneOverFailsClosed) {
    RunConsumption run;
    run.selection.emplace();
    run.selection->outcome = RunCallOutcome::ReturnedOk;
    run.selection->payload.ordinary_selection.resize(2048);
    for (auto& entry : run.selection->payload.ordinary_selection) entry.name = "A";
    auto at_limit = project_run_consumption(run).document();
    EXPECT_NE(at_limit.at("reason"), "capacity_exceeded");
    run.selection->payload.ordinary_selection.emplace_back().name = "A";
    expect_unavailable(project_run_consumption(run).document(), "capacity_exceeded");
}

transaction_cost::CostChargeObservation actual_equity_cost(double, bool, bool, bool);
Json project_equity_cost(const transaction_cost::CostChargeObservation&);

TEST(ConsumptionProjectionTest, FuturesAndActualEquityContextsProjectEveryCatalogConsumerFieldPair) {
    auto run = synthetic_full_run();
    const auto before_strategy = run.preparation->call.payload.sizing.capital_allocation;
    auto doc = project_run_consumption(run).document();
    ASSERT_EQ(doc.at("status"), "complete");
    EXPECT_EQ(doc.at("reason"), "none");
    std::set<std::pair<std::string, std::string>> actual;
    std::set<std::string> consumers;
    for (const auto& node : doc.at("nodes")) {
        consumers.insert(node.at("consumer").get<std::string>());
        for (const auto& read : node.at("reads"))
            actual.emplace(node.at("consumer").get<std::string>(), read.at("field").get<std::string>());
    }
    // Union genuine equity cost-reader observations with the legacy synthetic
    // futures context; do not add equity reads to the futures fixture itself.
    const auto equity = project_equity_cost(actual_equity_cost(-1000, true, true, true));
    ASSERT_EQ(equity.at("status"), "complete");
    for (const auto& node : equity.at("nodes"))
        for (const auto& read : node.at("reads"))
            actual.emplace(node.at("consumer").get<std::string>(), read.at("field").get<std::string>());
    std::ifstream source("contracts/consumption-v2-catalog.json", std::ios::binary);
    ASSERT_TRUE(source.good());
    const auto catalog = Json::parse(source);
    std::set<std::pair<std::string, std::string>> expected;
    for (const auto& field : catalog.at("fields"))
        expected.emplace(field.at("consumer").get<std::string>(), field.at("field").get<std::string>());
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(consumers.size(), catalog.at("consumers").size());
    EXPECT_EQ(run.preparation->call.payload.sizing.capital_allocation, before_strategy);
    EXPECT_EQ(doc.dump().find("reference_price"), std::string::npos);
    EXPECT_EQ(doc.dump().find("quantity"), std::string::npos);
}

TEST(ConsumptionProjectionTest, NativeInnerFailureOverridesOuterSuccess) {
    auto run = synthetic_full_run();
    run.primary->payload.passes[0].optimization.optimizer_call = PortfolioCallOutcome::ReturnedError;
    auto doc = project_run_consumption(run).document();
    EXPECT_EQ(doc.at("status"), "partial");
    EXPECT_EQ(doc.at("coverage").at("primary").at("reason"), "nonfatal_error");
    EXPECT_EQ(find_node(doc, "portfolio.primary")->at("outcome"), "returned_ok");
    EXPECT_EQ(find_node(doc, "optimizer.primary")->at("outcome"), "returned_error");
    run.primary->payload.passes[0].optimization.optimizer_call = PortfolioCallOutcome::InProgress;
    doc = project_run_consumption(run).document();
    EXPECT_EQ(doc.at("coverage").at("primary").at("reason"), "incomplete_call");
}

TEST(ConsumptionProjectionTest, NonTradingDecisionSkipsBothCalculationStages) {
    auto run = synthetic_full_run();
    run.skip_strategy_processing = true;
    run.preparation_skipped = true;
    run.primary_skipped = true;
    run.preparation.reset();
    run.primary.reset();
    run.preparation_stage = {};
    run.primary_stage = {};
    auto doc = project_run_consumption(run).document();
    EXPECT_EQ(doc.at("status"), "complete");
    EXPECT_EQ(doc.at("coverage").at("preparation").at("status"), "skipped");
    EXPECT_EQ(doc.at("coverage").at("primary").at("reason"), "non_trading_day");
    EXPECT_EQ(find_node(doc, "strategy.preparation"), nullptr);
    EXPECT_EQ(find_node(doc, "portfolio.primary"), nullptr);
    run.primary_skipped = false;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
}

TEST(ConsumptionProjectionTest, UnsupportedProfileAndMissingLoopRemainExplicit) {
    auto run = synthetic_full_run();
    run.preparation->call.payload = StrategyConsumptionTrace{};
    run.preparation->call.payload.profile = StrategyConsumptionProfile::Unsupported;
    run.history_loop.reached = false;
    run.history_loop.completed = false;
    run.history_updates.clear();
    auto doc = project_run_consumption(run).document();
    EXPECT_EQ(doc.at("status"), "partial");
    EXPECT_EQ(doc.at("coverage").at("preparation").at("reason"), "unsupported_consumer");
    EXPECT_EQ(doc.at("coverage").at("cost_history").at("reason"), "instrumentation_missing");
    const auto* preparation = find_node(doc, "strategy.preparation");
    ASSERT_NE(preparation, nullptr);
    for (const auto& node : doc.at("nodes")) {
        if (node.at("parent") == preparation->at("id"))
            ADD_FAILURE() << "Unsupported preparation gained child " << node.at("consumer");
    }
}

TEST(ConsumptionProjectionTest, ActualExecutionHelpersPreserveParityAcrossIndependentBooks) {
    RunConsumption first_run, second_run;
    ExecutionManager observed_first, plain_first, observed_second, plain_second;
    auto& update = first_run.history_updates.emplace_back();
    update.identity = "ES.v.0";
    invoke_run_value(&update.call, [&](ExecutionMarketDataObservation* out) {
        observed_first.update_market_data("ES.v.0", 1000.0, 5000.0, out);
    });
    plain_first.update_market_data("ES.v.0", 1000.0, 5000.0);
    EXPECT_EQ(update.call.outcome, RunCallOutcome::ReturnedOk);
    const auto day = std::chrono::system_clock::from_time_t(1'800'000'000);
    auto position = [](std::string symbol, double quantity, double price) {
        Position p;
        p.symbol = std::move(symbol);
        p.quantity = Decimal(quantity);
        p.average_price = Decimal(price);
        return p;
    };
    auto& first = first_run.execution_batches.emplace_back();
    first.identity = "S0";
    auto observed_reports = invoke_run_result(&first.call, [&](DailyExecutionObservation* out) {
        return observed_first.generate_daily_executions(
            {{"ES.v.0", position("ES.v.0", 2.0, 5000.0)}}, {},
            {{"ES.v.0", 5001.0}}, day, "system", out);
    });
    auto plain_reports = plain_first.generate_daily_executions(
        {{"ES.v.0", position("ES.v.0", 2.0, 5000.0)}}, {},
        {{"ES.v.0", 5001.0}}, day, "system");
    ASSERT_TRUE(observed_reports.is_ok());
    ASSERT_TRUE(plain_reports.is_ok());
    ASSERT_EQ(observed_reports.value().size(), plain_reports.value().size());
    EXPECT_EQ(observed_reports.value().at(0).order_id, plain_reports.value().at(0).order_id);
    auto& second = second_run.execution_batches.emplace_back();
    second.identity = "S1";
    auto observed_removed = invoke_run_result(&second.call, [&](DailyExecutionObservation* out) {
        return observed_second.generate_daily_executions(
            {}, {{"NQ.v.0", position("NQ.v.0", 3.0, 18000.0)}},
            {{"NQ.v.0", 18001.0}}, day, "system", out);
    });
    auto plain_removed = plain_second.generate_daily_executions(
        {}, {{"NQ.v.0", position("NQ.v.0", 3.0, 18000.0)}},
        {{"NQ.v.0", 18001.0}}, day, "system");
    ASSERT_TRUE(observed_removed.is_ok());
    ASSERT_TRUE(plain_removed.is_ok());
    EXPECT_EQ(observed_removed.value().at(0).order_id, plain_removed.value().at(0).order_id);
    // The ExecutionManager calls are real; the projection needs their caller's
    // successful registration context to make each independent book legal.
    add_successful_setup_identity(first_run, "S0");
    add_successful_setup_identity(second_run, "S1");
    const auto first_doc = project_run_consumption(first_run).document();
    const auto second_doc = project_run_consumption(second_run).document();
    ASSERT_NE(find_node(first_doc, "execution.batch"), nullptr);
    ASSERT_NE(find_node(first_doc, "execution.attempt"), nullptr);
    EXPECT_EQ(find_node(first_doc, "execution.attempt")->at("meta").at("branch"), "current_position");
    ASSERT_NE(find_node(first_doc, "cost.execution"), nullptr);
    EXPECT_FALSE(find_node(first_doc, "cost.execution")->contains("outcome"));
    ASSERT_NE(find_node(second_doc, "execution.attempt"), nullptr);
    EXPECT_EQ(find_node(second_doc, "execution.attempt")->at("meta").at("branch"), "removed_position");
    EXPECT_EQ(first_run.execution_batches.at(0).identity, "S0");
    EXPECT_EQ(second_run.execution_batches.at(0).identity, "S1");
}

TEST(ConsumptionProjectionTest, CompleteClaimRequiresRegisteredStrategyCoverage) {
    auto run = synthetic_full_run();
    run.primary->payload.strategies.clear();
    auto doc = project_run_consumption(run).document();
    EXPECT_NE(doc.at("coverage").at("primary").at("status"), "complete");
    run = synthetic_full_run();
    run.execution_batches.clear();
    doc = project_run_consumption(run).document();
    EXPECT_NE(doc.at("coverage").at("execution").at("status"), "complete");
}

TEST(ConsumptionProjectionTest, ConflictingSetupIdentitiesCannotProjectAsComplete) {
    auto run = synthetic_full_run();
    run.preparation->identity = "S1";
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run = synthetic_full_run();
    run.primary_factory->payload.entries.at(0).name = "S1";
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run = synthetic_full_run();
    run.registrations.push_back(run.registrations.front());
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
}

TEST(ConsumptionProjectionTest, EmptyReachedLoopsRetainCompleteCoverage) {
    auto run = synthetic_full_run();
    run.history_updates.clear();
    EXPECT_EQ(project_run_consumption(run).document().at("coverage").at("cost_history").at("status"), "complete");
    run.execution_batches.clear();
    EXPECT_NE(project_run_consumption(run).document().at("coverage").at("execution").at("status"), "complete");
}

TEST(ConsumptionProjectionTest, SyntheticTypicalFixtureMeasures480ChargesAndShortArrays) {
    auto run = synthetic_typical_run();
    const auto doc = project_run_consumption(run).document();
    ASSERT_NE(doc.at("status"), "unavailable");
    size_t cost_calls_and_scopes = 0, cost_reads = 0, history = 0, estimates = 0, attempts = 0;
    size_t history_arrays = 0, forecast_arrays = 0, all_reads = 0;
    for (const auto& node : doc.at("nodes")) {
        const std::string consumer = node.at("consumer");
        all_reads += node.at("reads").size();
        if (consumer == "execution.history_update") ++history;
        if (consumer == "portfolio.estimate") ++estimates;
        if (consumer == "execution.attempt") ++attempts;
        if (consumer == "cost.estimate" || consumer == "cost.strategy_execution" ||
            consumer == "cost.compatibility_execution" || consumer == "cost.execution") {
            ++cost_calls_and_scopes;
            cost_reads += node.at("reads").size();
        }
        if (consumer == "strategy.history") {
            ++history_arrays;
            const auto* ema = find_read(node, "strategy.history.ema_windows");
            ASSERT_NE(ema, nullptr);
            ASSERT_EQ(ema->at("value").size(), 6u);
            EXPECT_EQ(ema->at("value").at(0), Json::array({2, 8}));
            EXPECT_EQ(ema->at("value").at(5), Json::array({64, 256}));
        }
        if (consumer == "strategy.forecast") {
            ++forecast_arrays;
            const auto* ema = find_read(node, "strategy.forecast.ema_windows");
            const auto* fdm = find_read(node, "strategy.forecast.fdm");
            ASSERT_NE(ema, nullptr);
            ASSERT_NE(fdm, nullptr);
            EXPECT_EQ(ema->at("value").size(), 6u);
            ASSERT_EQ(fdm->at("value").size(), 8u);
            EXPECT_EQ(fdm->at("value").at(0), Json::array({0, 1.0}));
            EXPECT_EQ(fdm->at("value").at(7), Json::array({7, 1.7}));
        }
    }
    EXPECT_EQ(cost_calls_and_scopes, 480u);
    EXPECT_EQ(cost_reads, 6720u);
    EXPECT_EQ(history, 40u);
    EXPECT_EQ(estimates, 200u);
    EXPECT_EQ(attempts, 120u);
    EXPECT_EQ(history_arrays, 4u);
    EXPECT_EQ(forecast_arrays, 4u);
    EXPECT_LE(doc.at("nodes").size(), 1200u);
    EXPECT_LE(all_reads, 8511u);
    const auto compact = doc.dump();
    EXPECT_LT(compact.size() + 131072u, 2097152u);
    RecordProperty("fixture_name", "synthetic-3-strategy-40-symbol-5-pass-480-charge");
    RecordProperty("compact_bytes", static_cast<int>(compact.size()));
    RecordProperty("node_count", static_cast<int>(doc.at("nodes").size()));
    RecordProperty("read_count", static_cast<int>(all_reads));
    RecordProperty("consumption_json", compact);
}

TEST(ConsumptionProjectionTest, FirstOverCompactByteCapDiscardsWholeTree) {
    RunConsumption run;
    const std::string strategy(64, 'S'), symbol(64, 'Y');
    add_successful_setup_identity(run, strategy);
    run.primary.emplace();
    run.primary->outcome = RunCallOutcome::ReturnedOk;
    run.primary->payload.outcome = PortfolioCallOutcome::ReturnedOk;
    run.primary->payload.skip_execution_generation = false;
    PortfolioExecutionCharge charge;
    charge.purpose = PortfolioChargePurpose::PerStrategy;
    charge.strategy_id = strategy;
    charge.symbol = symbol;
    charge.charge_call = PortfolioCallOutcome::ReturnedOk;
    fill_cost(charge.charge);
    const double largest = std::numeric_limits<double>::max();
    auto& c = charge.charge;
    c.spread.baseline_spread_ticks = c.spread.min_spread_ticks = c.spread.max_spread_ticks = largest;
    c.spread.spread_cost_multiplier = c.spread.tick_size = largest;
    c.volatility.lambda = c.volatility.min_multiplier = c.volatility.max_multiplier = largest;
    c.impact.min_adv = c.impact.min_participation = c.impact.max_participation = c.impact.max_impact_bps = largest;
    c.explicit_fee_per_contract = c.point_value = largest;
    size_t low = 0, high = 1170;
    auto& charges = run.primary->payload.strategy_charges;
    while (low + 1 < high) {
        const size_t mid = low + (high - low) / 2;
        charges.assign(mid, charge);
        const auto doc = project_run_consumption(run).document();
        if (doc.at("status") == "unavailable") high = mid;
        else low = mid;
    }
    ASSERT_GT(high, 0u);
    charges.assign(low, charge);
    const auto just_under = project_run_consumption(run).document();
    ASSERT_NE(just_under.at("status"), "unavailable");
    EXPECT_LE(just_under.dump().size(), 2097152u);
    charges.assign(high, charge);
    const auto just_over = project_run_consumption(run).document();
    expect_unavailable(just_over, "capacity_exceeded");
    EXPECT_LE(high * 14, 16384u);
    EXPECT_LE(high + 1, 4096u);
    RecordProperty("last_fitting_charge_count", static_cast<int>(low));
    RecordProperty("first_over_charge_count", static_cast<int>(high));
    RecordProperty("last_fitting_compact_bytes", static_cast<int>(just_under.dump().size()));
}

TEST(ConsumptionProjectionTest, ReachedButUnfinishedNativeBatchCannotLookReturned) {
    auto run = synthetic_full_run();
    run.execution_batches.front().call.payload.state = DailyExecutionState::entered;
    auto doc = project_run_consumption(run).document();
    ASSERT_NE(find_node(doc, "execution.batch"), nullptr);
    EXPECT_EQ(find_node(doc, "execution.batch")->at("outcome"), "incomplete");
    EXPECT_EQ(doc.at("coverage").at("execution").at("reason"), "incomplete_call");
}

TEST(ConsumptionProjectionTest, ContradictoryNativeReadsDoNotDisappearIntoComplete) {
    auto run = synthetic_full_run();
    run.preparation->call.payload.profile = StrategyConsumptionProfile::Unsupported;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run = synthetic_full_run();
    run.history_updates.front().call.payload.cost_model_reached = false;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run = synthetic_full_run();
    run.registrations.front().call.payload.requested_optimization = false;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run = synthetic_full_run();
    run.primary->payload.passes[0].use_optimization = true;
    run.primary->payload.passes[0].optimization_helper = PortfolioCallOutcome::NotCalled;
    auto doc = project_run_consumption(run).document();
    EXPECT_NE(doc.at("coverage").at("primary").at("status"), "complete");
}

TEST(ConsumptionProjectionTest, InvalidEnumAndPairCountFailClosed) {
    auto run = synthetic_full_run();
    run.benchmark_mode = static_cast<RunBenchmarkMode>(777);
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run = synthetic_full_run();
    run.preparation->call.payload.forecast.fdm->resize(129, {1, 1.0});
    expect_unavailable(project_run_consumption(run).document(), "capacity_exceeded");
    run = synthetic_full_run();
    run.preparation->call.payload.forecast.fdm->resize(128, {1, 1.0});
    EXPECT_NE(project_run_consumption(run).document().at("reason"), "capacity_exceeded");
}

TEST(ConsumptionProjectionTest, StrategyAndSymbolIdentityLimitsAreExact) {
    RunConsumption strategies;
    strategies.selection.emplace();
    strategies.selection->outcome = RunCallOutcome::ReturnedOk;
    for (int i = 0; i < 32; ++i) {
        SelectionRead entry;
        entry.name = "S" + std::to_string(i);
        strategies.selection->payload.ordinary_selection.push_back(entry);
    }
    EXPECT_NE(project_run_consumption(strategies).document().at("reason"), "capacity_exceeded");
    strategies.selection->payload.ordinary_selection.emplace_back().name = "S32";
    expect_unavailable(project_run_consumption(strategies).document(), "capacity_exceeded");

    RunConsumption symbols;
    symbols.preparation.emplace();
    symbols.preparation->identity = "S0";
    symbols.preparation->call.outcome = RunCallOutcome::ReturnedOk;
    symbols.preparation->call.payload.profile = StrategyConsumptionProfile::Standard;
    add_successful_setup_identity(symbols, "S0");
    for (int i = 0; i < 1024; ++i)
        symbols.preparation->call.payload.sizing.symbol_limits["Y" + std::to_string(i)] = {true, 1.0};
    EXPECT_NE(project_run_consumption(symbols).document().at("status"), "unavailable");
    symbols.preparation->call.payload.sizing.symbol_limits["Y1024"] = {true, 1.0};
    expect_unavailable(project_run_consumption(symbols).document(), "capacity_exceeded");
}

TEST(ConsumptionProjectionTest, NodeLimitAccepts4096AndRejects4097) {
    RunConsumption run;
    run.history_updates.resize(2048);
    for (auto& entry : run.history_updates) {
        entry.identity = "ES";
        entry.call.outcome = RunCallOutcome::ReturnedOk;
        entry.call.payload.cost_model_reached = true;
        entry.call.payload.market_data.volume.adv_lookback_days = 1;
    }
    auto doc = project_run_consumption(run).document();
    ASSERT_NE(doc.at("status"), "unavailable");
    EXPECT_EQ(doc.at("nodes").size(), 4096u);
    run.market_fetch.outcome = RunCallOutcome::ReturnedOk;
    expect_unavailable(project_run_consumption(run).document(), "capacity_exceeded");
}

TEST(ConsumptionProjectionTest, Uint53AndIdentityLengthEndpoints) {
    RunConsumption run;
    run.preparation.emplace();
    run.preparation->identity = std::string(64, 'S');
    add_successful_setup_identity(run, run.preparation->identity);
    run.preparation->call.outcome = RunCallOutcome::ReturnedOk;
    run.preparation->call.payload.profile = StrategyConsumptionProfile::Standard;
    run.preparation->call.payload.history.max_history_size = 9007199254740991ULL;
    auto doc = project_run_consumption(run).document();
    ASSERT_NE(doc.at("status"), "unavailable");
    const auto* history = find_node(doc, "strategy.history");
    ASSERT_NE(history, nullptr);
    EXPECT_EQ(find_read(*history, "strategy.history.max_history_size")->at("value"), 9007199254740991ULL);
    run.preparation->call.payload.history.max_history_size = 9007199254740992ULL;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run.preparation->call.payload.history.max_history_size = 1;
    run.preparation->identity.push_back('S');
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_identity");
}

TEST(ConsumptionProjectionTest, PairAndPassPrefixLimitsAreExact) {
    RunConsumption pass_run;
    pass_run.primary.emplace();
    pass_run.primary->outcome = RunCallOutcome::ReturnedOk;
    pass_run.primary->payload.outcome = PortfolioCallOutcome::ReturnedOk;
    pass_run.primary->payload.pass_count = 5;
    EXPECT_NE(project_run_consumption(pass_run).document().at("reason"), "capacity_exceeded");
    pass_run.primary->payload.pass_count = 6;
    expect_unavailable(project_run_consumption(pass_run).document(), "capacity_exceeded");

    RunConsumption pairs;
    pairs.primary.emplace();
    pairs.primary->outcome = RunCallOutcome::ReturnedOk;
    pairs.primary->payload.outcome = PortfolioCallOutcome::ReturnedOk;
    const std::vector<std::pair<int, int>> ema(128, {1, 2});
    const std::vector<std::pair<int, double>> fdm(128, {1, 1.0});
    for (int i = 0; i < 21; ++i) {
        auto& call = pairs.primary->payload.strategies.emplace_back();
        call.strategy_id = "S" + std::to_string(i);
        add_successful_setup_identity(pairs, call.strategy_id);
        call.outcome = PortfolioCallOutcome::ReturnedOk;
        call.strategy.profile = StrategyConsumptionProfile::Standard;
        call.strategy.history.ema_windows = ema;
        call.strategy.forecast.ema_windows = ema;
        call.strategy.forecast.fdm = fdm;
    }
    pairs.preparation.emplace();
    pairs.preparation->identity = "S0";
    pairs.preparation->call.outcome = RunCallOutcome::ReturnedOk;
    pairs.preparation->call.payload.profile = StrategyConsumptionProfile::Standard;
    pairs.preparation->call.payload.history.ema_windows = ema;
    EXPECT_NE(project_run_consumption(pairs).document().at("status"), "unavailable");
    pairs.preparation->call.payload.forecast.fdm = std::vector<std::pair<int, double>>{{1, 1.0}};
    expect_unavailable(project_run_consumption(pairs).document(), "capacity_exceeded");
}

TEST(ConsumptionProjectionTest, TotalReadLimitAccepts16384AndRejects16385) {
    RunConsumption run;
    add_successful_setup_identity(run, "S");
    run.primary.emplace();
    run.primary->outcome = RunCallOutcome::ReturnedOk;
    run.primary->payload.outcome = PortfolioCallOutcome::ReturnedOk;
    PortfolioExecutionCharge charge;
    charge.purpose = PortfolioChargePurpose::PerStrategy;
    charge.strategy_id = "S";
    charge.symbol = "ES";
    charge.charge_call = PortfolioCallOutcome::ReturnedOk;
    fill_cost(charge.charge);
    run.primary->payload.strategy_charges.assign(1170, charge);
    run.snapshot_risk.emplace();
    run.snapshot_risk->outcome = RunCallOutcome::ReturnedOk;
    auto& risk = run.snapshot_risk->payload;
    risk.var_limit = 0.1;
    risk.jump_risk_limit = 0.2;
    risk.max_correlation = 0.9;
    risk.max_gross_leverage = 4.0;
    auto doc = project_run_consumption(run).document();
    ASSERT_NE(doc.at("status"), "unavailable") << doc.at("reason");
    size_t reads = 0;
    for (const auto& node : doc.at("nodes")) reads += node.at("reads").size();
    EXPECT_EQ(reads, 16384u);
    risk.max_net_leverage = 2.0;
    expect_unavailable(project_run_consumption(run).document(), "capacity_exceeded");
}

TEST(ConsumptionProjectionTest, OnlyUnorderedMapRowsSortByAsciiIdentity) {
    auto run = synthetic_full_run({"ZZ", "AA"});
    auto doc = project_run_consumption(run).document();
    ASSERT_NE(doc.at("status"), "unavailable");
    EXPECT_EQ(find_node(doc, "execution.history_update", 0)->at("symbol"), "ZZ");
    EXPECT_EQ(find_node(doc, "execution.history_update", 1)->at("symbol"), "AA");
    const auto* sizing = find_node(doc, "strategy.sizing");
    ASSERT_NE(sizing, nullptr);
    EXPECT_EQ(sizing->at("reads").at(6).at("symbol"), "AA");
    EXPECT_EQ(sizing->at("reads").at(8).at("symbol"), "ZZ");
}

TEST(ConsumptionProjectionTest, ActualFactoryStrategyAndPortfolioHelpersProjectWithoutChangingResults) {
    StateManager::reset_instance();
    auto db = std::make_shared<trade_ngin::testing::MockPostgresDatabase>("mock://projection");
    ASSERT_TRUE(db->connect().is_ok());
    StrategySelection selection;
    selection.names = {"TREND_A"};
    selection.allocations = {{"TREND_A", 0.5}};
    selection.configs = {{"TREND_A", {{"type", "TrendFollowingStrategy"}}}};
    StrategyConfig strategy_config;
    strategy_config.max_leverage = 10.0;
    strategy_config.max_drawdown = 0.5;
    StrategyDefaultsConfig defaults;
    auto build = [&](FactoryConsumption* output) {
        return build_strategy_instances(selection, strategy_config, 100000.0,
                                        defaults, std::nullopt, db, nullptr, output);
    };
    RunConsumption run;
    run.primary_factory.emplace();
    auto observed = invoke_run_value(&*run.primary_factory,
        [&](FactoryConsumption* output) { return build(output); });
    auto plain = build(nullptr);
    ASSERT_EQ(observed.size(), 1u);
    ASSERT_EQ(plain.size(), 1u);
    EXPECT_EQ(typeid(*observed[0]), typeid(*plain[0]));
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
    auto prewarm = invoke_run_result(&run.preparation->call,
        [&](StrategyConsumptionTrace* output) { return observed[0]->on_data(bars, output); });
    auto plain_prewarm = plain[0]->on_data(bars);
    EXPECT_EQ(prewarm.is_ok(), plain_prewarm.is_ok());
    PortfolioConfig config;
    config.total_capital = Decimal(100000.0);
    config.reserve_capital = Decimal(0.0);
    std::unique_ptr<PortfolioManager> primary, comparator;
    struct Cleanup {
        std::unique_ptr<PortfolioManager>& primary;
        std::unique_ptr<PortfolioManager>& comparator;
        bool subscribed = false;
        ~Cleanup() {
            if (subscribed) (void)MarketDataBus::instance().unsubscribe("PORTFOLIO_MANAGER");
            comparator.reset();
            primary.reset();
        }
    } cleanup{primary, comparator};
    primary = std::make_unique<PortfolioManager>(config, "PROJECTION_OBS");
    cleanup.subscribed = true;
    comparator = std::make_unique<PortfolioManager>(config, "PROJECTION_PLAIN");
    auto& registration = run.registrations.emplace_back();
    registration.identity = "TREND_A";
    auto added = invoke_run_result(&registration.call,
        [&](PortfolioRegistrationTrace* output) {
            return primary->add_strategy(observed[0], 0.5, false, false, output);
        });
    auto plain_added = comparator->add_strategy(plain[0], 0.5, false, false);
    EXPECT_EQ(added.is_ok(), plain_added.is_ok());
    ASSERT_TRUE(added.is_ok());
    run.primary.emplace();
    auto processed = invoke_run_result(&*run.primary,
        [&](PortfolioConsumptionTrace* output) {
            return primary->process_market_data(bars, false, std::nullopt, output);
        });
    auto plain_processed = comparator->process_market_data(bars, false, std::nullopt);
    EXPECT_EQ(processed.is_ok(), plain_processed.is_ok());
    ASSERT_TRUE(processed.is_ok());
    RunConsumption factory_only;
    factory_only.primary_factory = run.primary_factory;
    auto factory_doc = project_run_consumption(factory_only).document();
    EXPECT_NE(factory_doc.at("status"), "unavailable") << "factory: " << factory_doc.at("reason");
    RunConsumption preparation_only;
    preparation_only.primary_factory = run.primary_factory;
    preparation_only.registrations = run.registrations;
    preparation_only.preparation = run.preparation;
    auto preparation_doc = project_run_consumption(preparation_only).document();
    EXPECT_NE(preparation_doc.at("status"), "unavailable") << "preparation: " << preparation_doc.at("reason");
    RunConsumption registration_only;
    registration_only.primary_factory = run.primary_factory;
    registration_only.registrations = run.registrations;
    auto registration_doc = project_run_consumption(registration_only).document();
    EXPECT_NE(registration_doc.at("status"), "unavailable") << "registration: " << registration_doc.at("reason");
    RunConsumption primary_only;
    primary_only.primary_factory = run.primary_factory;
    primary_only.registrations = run.registrations;
    primary_only.primary = run.primary;
    auto primary_doc = project_run_consumption(primary_only).document();
    EXPECT_NE(primary_doc.at("status"), "unavailable") << "primary: " << primary_doc.at("reason");
    const auto doc = project_run_consumption(run).document();
    ASSERT_NE(doc.at("status"), "unavailable") << doc.at("reason");
    ASSERT_NE(find_node(doc, "setup.factory_entry"), nullptr);
    ASSERT_NE(find_node(doc, "strategy.preparation"), nullptr);
    ASSERT_NE(find_node(doc, "strategy.history"), nullptr);
    ASSERT_NE(find_node(doc, "portfolio.registration"), nullptr);
    ASSERT_NE(find_node(doc, "portfolio.primary"), nullptr);
    ASSERT_NE(find_node(doc, "strategy.primary"), nullptr);
    EXPECT_EQ(find_node(doc, "strategy.preparation")->at("strategy"), "TREND_A");
    EXPECT_EQ(primary->get_strategy_positions().size(), comparator->get_strategy_positions().size());
}

TEST(ConsumptionProjectionTest, ActualSnapshotRiskHelperProjectsOwnSettingsWithParity) {
    RiskConfig config;
    config.var_limit = 0.37;
    config.capital = Decimal::from_raw(12000000000000LL);
    RiskManager manager(config);
    MarketData market;
    market.ordered_symbols = {"ES.v.0"};
    market.symbol_indices = {{"ES.v.0", 0}};
    market.returns = {{0.01}, {-0.01}};
    market.covariance = {{0.0001}};
    Position position;
    position.symbol = "ES.v.0";
    position.quantity = Decimal(2);
    position.average_price = Decimal(5000);
    std::unordered_map<std::string, Position> positions{{"ES.v.0", position}};
    RunConsumption run;
    run.snapshot_risk.emplace();
    auto observed = invoke_run_result(&*run.snapshot_risk,
        [&](RiskConfigConsumption* output) {
            return manager.process_positions(positions, market, {}, output);
        });
    auto plain = manager.process_positions(positions, market);
    ASSERT_EQ(observed.is_ok(), plain.is_ok());
    ASSERT_TRUE(observed.is_ok());
    EXPECT_DOUBLE_EQ(observed.value().recommended_scale, plain.value().recommended_scale);
    const auto doc = project_run_consumption(run).document();
    const auto* risk = find_node(doc, "risk.diagnostics");
    ASSERT_NE(risk, nullptr);
    EXPECT_EQ(find_read(*risk, "risk.var_limit")->at("value"), 0.37);
    EXPECT_EQ(find_read(*risk, "risk.capital")->at("value"), "120000");
}

TEST(ConsumptionProjectionTest, DuplicateEntryScopesAndUnenteredCostReadsFailClosed) {
    auto run = synthetic_full_run();
    run.selection->payload.ordinary_selection.push_back(run.selection->payload.ordinary_selection.front());
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run = synthetic_full_run();
    run.primary_factory->payload.entries.push_back(run.primary_factory->payload.entries.front());
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run = synthetic_full_run();
    run.primary->payload.passes[0].optimization.estimates.front().charge_call = PortfolioCallOutcome::NotCalled;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run = synthetic_full_run();
    run.history_updates.front().call.payload.cost_model_reached = false;
    run.history_updates.front().call.payload.previous_close_source.reset();
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run = synthetic_full_run();
    run.execution_batches.front().call.payload.attempts.front().execution.state = ExecutionCallState::entered;
    run.execution_batches.front().call.payload.attempts.front().returned = false;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
}

TEST(ConsumptionProjectionTest, ReturnedHelpersCannotHideMissingNestedCalls) {
    auto run = synthetic_full_run();
    run.primary->payload.passes[0].optimization.optimizer_call = PortfolioCallOutcome::NotCalled;
    auto doc = project_run_consumption(run).document();
    EXPECT_NE(doc.at("coverage").at("primary").at("status"), "complete");
    run = synthetic_full_run();
    run.primary->payload.passes[0].risk.risk_call = PortfolioCallOutcome::NotCalled;
    doc = project_run_consumption(run).document();
    EXPECT_NE(doc.at("coverage").at("primary").at("status"), "complete");
}

TEST(ConsumptionProjectionTest, SelectionDefaultsNeedReturnedValues) {
    auto run = synthetic_full_run();
    auto& entry = run.selection->payload.ordinary_selection.front();
    entry.allocation_defaulted = true;
    entry.allocation_value.reset();
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run = synthetic_full_run();
    auto& enabled = run.selection->payload.ordinary_selection.front();
    enabled.enabled_live_defaulted = true;
    enabled.enabled_live_value.reset();
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
}

TEST(ConsumptionProjectionTest, FactorySuccessWithoutSuccessfulRegistrationIsPartial) {
    auto run = synthetic_setup_prefix();
    run.registrations.clear();
    auto doc = project_run_consumption(run).document();
    ASSERT_EQ(doc.at("status"), "partial");
    EXPECT_EQ(doc.at("coverage").at("setup").at("reason"), "instrumentation_missing");
    RecordProperty("consumption_json_factory_no_registration", doc.dump());

    run = synthetic_setup_prefix();
    run.registrations.front().call.outcome = RunCallOutcome::ReturnedError;
    run.registrations.front().call.payload = {};
    run.registrations.front().call.payload.outcome = PortfolioCallOutcome::ReturnedError;
    doc = project_run_consumption(run).document();
    ASSERT_EQ(doc.at("status"), "partial");
    EXPECT_EQ(doc.at("coverage").at("setup").at("reason"), "nonfatal_error");
    RecordProperty("consumption_json_factory_failed_registration", doc.dump());

    run = synthetic_full_run();
    run.registrations.clear();
    doc = project_run_consumption(run).document();
    expect_unavailable(doc, "invalid_observed_value");
    RecordProperty("consumption_json_factory_no_registration_contradiction", doc.dump());

    run = synthetic_full_run();
    run.registrations.front().call.outcome = RunCallOutcome::ReturnedError;
    run.registrations.front().call.payload = {};
    run.registrations.front().call.payload.outcome = PortfolioCallOutcome::ReturnedError;
    doc = project_run_consumption(run).document();
    expect_unavailable(doc, "invalid_observed_value");
    RecordProperty("consumption_json_factory_failed_registration_contradiction", doc.dump());

    run = synthetic_full_run();
    auto second = run.primary_factory->payload.entries.front();
    second.name = "S1";
    run.primary_factory->payload.entries.push_back(second);
    doc = project_run_consumption(run).document();
    ASSERT_EQ(doc.at("status"), "partial");
    EXPECT_EQ(doc.at("coverage").at("setup").at("reason"), "instrumentation_missing");
    RecordProperty("consumption_json_factory_registered_subset", doc.dump());

    run = synthetic_full_run();
    run.registrations.front().identity = "S1";
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");

    RunConsumption empty_book;
    empty_book.selection.emplace();
    empty_book.selection->outcome = RunCallOutcome::ReturnedOk;
    empty_book.primary_factory.emplace();
    empty_book.primary_factory->outcome = RunCallOutcome::ReturnedOk;
    doc = project_run_consumption(empty_book).document();
    EXPECT_EQ(doc.at("coverage").at("setup").at("status"), "complete");
}

TEST(ConsumptionProjectionTest, UnregisteredPreparationFailsClosed) {
    auto run = synthetic_setup_prefix();
    run.registrations.clear();
    run.preparation = synthetic_full_run().preparation;
    const auto doc = project_run_consumption(run).document();
    expect_unavailable(doc, "invalid_observed_value");
    RecordProperty("consumption_json_unregistered_preparation", doc.dump());
}

TEST(ConsumptionProjectionTest, UnregisteredPrimaryStrategyFailsClosed) {
    auto run = synthetic_setup_prefix();
    run.registrations.clear();
    run.primary.emplace();
    run.primary->outcome = RunCallOutcome::ReturnedOk;
    run.primary->payload.outcome = PortfolioCallOutcome::ReturnedOk;
    run.primary->payload.strategies.push_back(synthetic_full_run().primary->payload.strategies.front());
    const auto doc = project_run_consumption(run).document();
    expect_unavailable(doc, "invalid_observed_value");
    RecordProperty("consumption_json_unregistered_primary_strategy", doc.dump());
}

TEST(ConsumptionProjectionTest, UnregisteredOptimizationParticipantFailsClosed) {
    auto run = synthetic_setup_prefix();
    run.registrations.clear();
    run.primary.emplace();
    run.primary->outcome = RunCallOutcome::ReturnedOk;
    run.primary->payload.outcome = PortfolioCallOutcome::ReturnedOk;
    run.primary->payload.pass_count = 1;
    auto& pass = run.primary->payload.passes[0];
    pass.use_optimization = true;
    pass.optimization_helper = PortfolioCallOutcome::ReturnedOk;
    pass.optimization.strategies[0]["S0"] = {true, 1.0};
    const auto doc = project_run_consumption(run).document();
    expect_unavailable(doc, "invalid_observed_value");
    RecordProperty("consumption_json_unregistered_optimization_strategy", doc.dump());
}

TEST(ConsumptionProjectionTest, UnregisteredExecutionBatchFailsClosed) {
    auto run = synthetic_setup_prefix();
    run.registrations.clear();
    run.execution_batches.push_back(synthetic_full_run().execution_batches.front());
    const auto doc = project_run_consumption(run).document();
    expect_unavailable(doc, "invalid_observed_value");
    RecordProperty("consumption_json_unregistered_execution_batch", doc.dump());
}

TEST(ConsumptionProjectionTest, UnregisteredStrategyChargeFailsClosed) {
    auto run = synthetic_setup_prefix();
    run.registrations.clear();
    run.primary.emplace();
    run.primary->outcome = RunCallOutcome::ReturnedOk;
    run.primary->payload.outcome = PortfolioCallOutcome::ReturnedOk;
    run.primary->payload.skip_execution_generation = false;
    run.primary->payload.strategy_charges.push_back(synthetic_full_run().primary->payload.strategy_charges.front());
    const auto doc = project_run_consumption(run).document();
    expect_unavailable(doc, "invalid_observed_value");
    RecordProperty("consumption_json_unregistered_strategy_charge", doc.dump());
}

TEST(ConsumptionProjectionTest, UnenteredDownstreamPayloadDoesNotImplyWork) {
    auto run = synthetic_setup_prefix();
    run.registrations.clear();
    run.preparation = synthetic_full_run().preparation;
    run.preparation->call.outcome = RunCallOutcome::NotCalled;
    run.primary.emplace();
    run.primary->payload.strategies.push_back(synthetic_full_run().primary->payload.strategies.front());
    run.execution_batches.push_back(synthetic_full_run().execution_batches.front());
    run.execution_batches.front().call.outcome = RunCallOutcome::NotCalled;
    const auto doc = project_run_consumption(run).document();
    EXPECT_EQ(doc.at("status"), "partial");
    EXPECT_EQ(find_node(doc, "strategy.preparation"), nullptr);
    EXPECT_EQ(find_node(doc, "strategy.primary"), nullptr);
    EXPECT_EQ(find_node(doc, "execution.batch"), nullptr);
}

TEST(ConsumptionProjectionTest, LiveBenchmarkWithoutEnteredBranchIsNotComplete) {
    auto run = synthetic_full_run();
    run.benchmark_state = RunBenchmarkState::NotReached;
    auto doc = project_run_consumption(run).document();
    ASSERT_EQ(doc.at("status"), "partial");
    EXPECT_EQ(doc.at("coverage").at("control_flow").at("reason"), "instrumentation_missing");
    EXPECT_EQ(find_node(doc, "runner.benchmark"), nullptr);
    RecordProperty("consumption_json_live_benchmark_not_reached", doc.dump());

    run.benchmark_state = RunBenchmarkState::Deferred;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");

    run.benchmark_mode = RunBenchmarkMode::Deferred;
    doc = project_run_consumption(run).document();
    ASSERT_EQ(doc.at("coverage").at("control_flow").at("status"), "complete");
    const auto* deferred = find_node(doc, "runner.benchmark");
    ASSERT_NE(deferred, nullptr);
    EXPECT_EQ(deferred->at("meta").at("branch"), "not_reached");
    RecordProperty("consumption_json_deferred_benchmark", doc.dump());

    run.benchmark_mode = RunBenchmarkMode::Live;
    run.benchmark_state = RunBenchmarkState::InProgress;
    doc = project_run_consumption(run).document();
    EXPECT_EQ(doc.at("coverage").at("control_flow").at("reason"), "incomplete_call");
    EXPECT_EQ(find_node(doc, "runner.benchmark")->at("meta").at("branch"), "attempted");
    run.benchmark_state = RunBenchmarkState::Succeeded;
    doc = project_run_consumption(run).document();
    EXPECT_EQ(doc.at("coverage").at("control_flow").at("status"), "complete");
}

TEST(ConsumptionProjectionTest, MarketCompletionRequiresSuccessfulArrowReturn) {
    auto run = synthetic_full_run();
    run.arrow_conversion.outcome = RunCallOutcome::NotCalled;
    auto doc = project_run_consumption(run).document();
    ASSERT_EQ(doc.at("status"), "partial");
    EXPECT_EQ(doc.at("coverage").at("market_input").at("reason"), "instrumentation_missing");
    ASSERT_NE(find_node(doc, "runner.market_window"), nullptr);
    ASSERT_NE(find_node(doc, "runner.market_fetch"), nullptr);
    RecordProperty("consumption_json_arrow_not_called", doc.dump());

    run.arrow_conversion.outcome = RunCallOutcome::InProgress;
    doc = project_run_consumption(run).document();
    EXPECT_EQ(doc.at("coverage").at("market_input").at("reason"), "incomplete_call");
    run.arrow_conversion.outcome = RunCallOutcome::ReturnedError;
    doc = project_run_consumption(run).document();
    EXPECT_EQ(doc.at("coverage").at("market_input").at("reason"), "nonfatal_error");
    run.arrow_conversion.outcome = RunCallOutcome::Threw;
    doc = project_run_consumption(run).document();
    EXPECT_EQ(doc.at("coverage").at("market_input").at("reason"), "nonfatal_error");
    run.arrow_conversion.outcome = static_cast<RunCallOutcome>(99);
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
    run.arrow_conversion.outcome = RunCallOutcome::ReturnedOk;
    doc = project_run_consumption(run).document();
    EXPECT_EQ(doc.at("coverage").at("market_input").at("status"), "complete");
}

TEST(ConsumptionProjectionTest, SelectionPresenceAndDefaultMustMatchTheirPhase) {
    auto run = synthetic_full_run();
    auto& controlled = run.selection->payload.controlled_validation.front();
    controlled.enabled_live_present = false;
    controlled.enabled_live_defaulted = false;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");

    run = synthetic_full_run();
    auto& present = run.selection->payload.controlled_validation.front();
    present.enabled_live_defaulted = true;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");

    run = synthetic_full_run();
    auto& ordinary = run.selection->payload.ordinary_selection.front();
    ordinary.enabled_live_defaulted = false;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");

    run = synthetic_full_run();
    auto& absent = run.selection->payload.ordinary_selection.front();
    absent.enabled_live_present = false;
    absent.enabled_live_value = false;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");

    RunConsumption controlled_default;
    controlled_default.controlled_selection = true;
    controlled_default.selection.emplace();
    controlled_default.selection->outcome = RunCallOutcome::ReturnedError;
    SelectionRead defaulted;
    defaulted.name = "S0";
    defaulted.enabled_live_read = true;
    defaulted.enabled_live_present = false;
    defaulted.enabled_live_defaulted = true;
    defaulted.enabled_live_value = false;
    controlled_default.selection->payload.controlled_validation.push_back(defaulted);
    auto doc = project_run_consumption(controlled_default).document();
    const auto* controlled_entry = find_node(doc, "setup.selection_entry");
    ASSERT_NE(controlled_entry, nullptr);
    const auto* read = find_read(*controlled_entry, "setup.selection.enabled_live");
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->at("origin"), "code_default");
    RecordProperty("consumption_json_controlled_default", doc.dump());

    RunConsumption ordinary_absent;
    ordinary_absent.selection.emplace();
    ordinary_absent.selection->outcome = RunCallOutcome::ReturnedError;
    SelectionRead no_leaf;
    no_leaf.name = "S0";
    no_leaf.enabled_live_present = false;
    ordinary_absent.selection->payload.ordinary_selection.push_back(no_leaf);
    doc = project_run_consumption(ordinary_absent).document();
    const auto* ordinary_entry = find_node(doc, "setup.selection_entry");
    ASSERT_NE(ordinary_entry, nullptr);
    EXPECT_EQ(find_read(*ordinary_entry, "setup.selection.enabled_live"), nullptr);
    EXPECT_FALSE(ordinary_entry->at("meta").contains("enabled_live_defaulted"));
}

// Real cost-reader observations in a synthetic projection context, not proof
// that a legacy MODEL runner executed an equity portfolio.
const std::array<const char*, 4> equity_consumers = {
    "cost.estimate", "cost.strategy_execution", "cost.compatibility_execution", "cost.execution"};
const std::array<const char*, 10> equity_fields = {
    "cost.spread.tick_constrained", "cost.charge.commission_per_unit",
    "cost.charge.max_commission_pct", "cost.charge.max_commission_per_order",
    "cost.charge.min_commission_per_order", "cost.charge.apply_regulatory_fees",
    "cost.charge.sec_fee_per_million", "cost.charge.finra_taf_per_share",
    "cost.charge.finra_taf_cap_per_trade", "cost.charge.max_total_implicit_bps"};

transaction_cost::CostChargeObservation actual_equity_cost(
    double qty = -1000, bool fixed = true, bool fees = true, bool ticks = true) {
    transaction_cost::TransactionCostManager manager;
    auto config = transaction_cost::AssetCostConfigRegistry::get_equity_default_config();
    config.symbol = "EQ_OBS";
    config.commission_per_unit = 0.0035;
    config.min_commission_per_order = 0.35;
    config.max_commission_per_order = 98765;
    config.max_commission_pct = fixed ? -1 : 0.01;
    config.apply_regulatory_fees = fees;
    config.tick_constrained = ticks;
    config.sec_fee_per_million = 20.60;
    config.finra_taf_per_share = 0.000195;
    config.finra_taf_cap_per_trade = 9.79;
    config.max_total_implicit_bps = 123;
    manager.register_asset_config(config);
    transaction_cost::CostChargeObservation used;
    const auto observed = manager.calculate_costs(config.symbol, qty, 50, 1000000, 1, AssetType::EQUITY, &used);
    const auto plain = manager.calculate_costs(config.symbol, qty, 50, 1000000, 1, AssetType::EQUITY);
    EXPECT_DOUBLE_EQ(observed.commissions_fees, plain.commissions_fees);
    EXPECT_DOUBLE_EQ(observed.spread_price_impact, plain.spread_price_impact);
    EXPECT_DOUBLE_EQ(observed.market_impact_price_impact, plain.market_impact_price_impact);
    EXPECT_DOUBLE_EQ(observed.implicit_price_impact, plain.implicit_price_impact);
    EXPECT_DOUBLE_EQ(observed.slippage_market_impact, plain.slippage_market_impact);
    EXPECT_DOUBLE_EQ(observed.total_transaction_costs, plain.total_transaction_costs);
    return used;
}
Json project_equity_cost(const transaction_cost::CostChargeObservation& used) {
    auto run = synthetic_full_run({"EQ_OBS"});
    run.primary->payload.passes[0].optimization.estimates.front().charge = used;
    run.primary->payload.strategy_charges.front().charge = used;
    run.primary->payload.compatibility_charges.front().charge = used;
    run.execution_batches.front().call.payload.attempts.front().execution.cost = used;
    return project_run_consumption(run).document();
}

TEST(ConsumptionProjectionEquityTest, ActualSellRetainsTenEquityReadsInEveryCostConsumer) {
    const auto doc = project_equity_cost(actual_equity_cost());
    RecordProperty("actual_equity_cost_projection", doc.dump());
    ASSERT_EQ(doc.at("status"), "complete");
    const std::array<Json, 10> values = {true, 0.0035, -1, 98765, 0.35, true, 20.60, 0.000195, 9.79, 123};
    for (const auto* consumer : equity_consumers) {
        SCOPED_TRACE(consumer);
        const auto* node = find_node(doc, consumer);
        ASSERT_NE(node, nullptr);
        for (size_t i = 0; i < equity_fields.size(); ++i) {
            SCOPED_TRACE(equity_fields[i]);
            const auto* read = find_read(*node, equity_fields[i]);
            ASSERT_NE(read, nullptr);
            EXPECT_EQ(read->at("value"), values[i]);
            EXPECT_EQ(read->at("value_type"), i == 0 || i == 5 ? "bool" : "number");
            EXPECT_EQ(read->at("origin"), "runtime_effective");
        }
        EXPECT_EQ(find_read(*node, "cost.charge.explicit_fee_per_contract"), nullptr);
        EXPECT_EQ(find_read(*node, "cost.volatility.lambda"), nullptr);
    }
}

TEST(ConsumptionProjectionEquityTest, ActualBuyOmitsUnreadRatesAndFixedMaximum) {
    const auto doc = project_equity_cost(actual_equity_cost(1000, false));
    ASSERT_EQ(doc.at("status"), "complete");
    for (const auto* consumer : equity_consumers) {
        const auto* node = find_node(doc, consumer);
        ASSERT_NE(node, nullptr);
        const auto* enabled = find_read(*node, "cost.charge.apply_regulatory_fees");
        ASSERT_NE(enabled, nullptr);
        EXPECT_EQ(enabled->at("value"), true);
        const auto* maximum = find_read(*node, "cost.charge.max_commission_pct");
        ASSERT_NE(maximum, nullptr);
        EXPECT_EQ(maximum->at("value"), 0.01);
        for (const auto* absent : {"cost.charge.sec_fee_per_million", "cost.charge.finra_taf_per_share",
                                  "cost.charge.finra_taf_cap_per_trade", "cost.charge.max_commission_per_order"})
            EXPECT_EQ(find_read(*node, absent), nullptr);
    }
}

TEST(ConsumptionProjectionEquityTest, ActualFalseFlagsRemainReadsAndDisabledSellOmitsRates) {
    const auto doc = project_equity_cost(actual_equity_cost(-1000, false, false, false));
    ASSERT_EQ(doc.at("status"), "complete");
    for (const auto* consumer : equity_consumers) {
        const auto* node = find_node(doc, consumer);
        ASSERT_NE(node, nullptr);
        for (const auto* field : {"cost.spread.tick_constrained", "cost.charge.apply_regulatory_fees"}) {
            const auto* read = find_read(*node, field);
            ASSERT_NE(read, nullptr);
            EXPECT_EQ(read->at("value"), false);
            EXPECT_EQ(read->at("value_type"), "bool");
        }
        EXPECT_EQ(find_read(*node, "cost.charge.sec_fee_per_million"), nullptr);
    }
}

TEST(ConsumptionProjectionEquityTest, NewOnlyObservationCannotHideBehindUncalledConsumer) {
    auto run = synthetic_full_run();
    auto& charge = run.primary->payload.strategy_charges.front();
    charge.charge_call = PortfolioCallOutcome::NotCalled;
    charge.charge = {};
    charge.charge.apply_regulatory_fees = false;
    expect_unavailable(project_run_consumption(run).document(), "invalid_observed_value");
}

TEST(ConsumptionProjectionEquityTest, NonfiniteEquityReadRefusesEntireProjection) {
    auto used = actual_equity_cost();
    used.finra_taf_cap_per_trade = std::numeric_limits<double>::infinity();
    expect_unavailable(project_equity_cost(used), "invalid_observed_value");
}

TEST(ConsumptionProjectionEquityTest, FuturesLegacyReadsUnchangedAndEquityFieldsAbsent) {
    const auto doc = project_run_consumption(synthetic_full_run()).document();
    ASSERT_EQ(doc.at("status"), "complete");
    for (const auto* consumer : equity_consumers) {
        const auto* node = find_node(doc, consumer);
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->at("reads").size(), 14u);
        for (const auto* field : equity_fields) EXPECT_EQ(find_read(*node, field), nullptr);
        const auto* fee = find_read(*node, "cost.charge.explicit_fee_per_contract");
        ASSERT_NE(fee, nullptr);
        EXPECT_EQ(fee->at("value"), 1.5);
    }
    // Compare this complete RED document to GREEN independently of assertions.
    RecordProperty("futures_projection_before_after", doc.dump());
}


}  // namespace
}  // namespace trade_ngin
