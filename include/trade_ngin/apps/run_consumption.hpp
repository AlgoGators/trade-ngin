#pragma once

#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "trade_ngin/apps/setup_consumption.hpp"
#include "trade_ngin/live/execution_consumption.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/risk_manager.hpp"

namespace trade_ngin {

enum class RunCallOutcome { NotCalled, InProgress, ReturnedOk, ReturnedError, Threw };
enum class RunEvidenceFailure { None, CapacityExceeded, InvalidObservedIdentity };
enum class RunBenchmarkMode { Deferred, Live, Unsupported };
enum class RunBenchmarkState { NotReached, Deferred, InProgress, Succeeded, NonfatalError };

template <typename Payload>
struct RunInvocation {
    RunCallOutcome outcome{RunCallOutcome::NotCalled};
    Payload payload{};
};

template <typename Payload>
struct RunNamedInvocation {
    std::string identity;
    RunInvocation<Payload> call;
};

class RunUnwindGuard {
public:
    explicit RunUnwindGuard(RunCallOutcome& outcome) noexcept
        : outcome_(outcome), uncaught_(std::uncaught_exceptions()) {}
    ~RunUnwindGuard() noexcept {
        if (std::uncaught_exceptions() > uncaught_) outcome_ = RunCallOutcome::Threw;
    }

private:
    RunCallOutcome& outcome_;
    int uncaught_;
};

template <typename Payload, typename Callable>
auto invoke_run_result(RunInvocation<Payload>* call, Callable&& reader) {
    if (call == nullptr) return std::forward<Callable>(reader)(nullptr);
    call->payload = Payload{};
    call->outcome = RunCallOutcome::InProgress;
    RunUnwindGuard guard(call->outcome);
    auto result = std::forward<Callable>(reader)(&call->payload);
    call->outcome = result.is_error() ? RunCallOutcome::ReturnedError
                                      : RunCallOutcome::ReturnedOk;
    return result;
}

template <typename Payload, typename Callable>
auto invoke_run_value(RunInvocation<Payload>* call, Callable&& reader) {
    using Value = std::invoke_result_t<Callable, Payload*>;
    if (call == nullptr) {
        if constexpr (std::is_void_v<Value>) {
            std::forward<Callable>(reader)(nullptr);
            return;
        } else {
            return std::forward<Callable>(reader)(nullptr);
        }
    }
    call->payload = Payload{};
    call->outcome = RunCallOutcome::InProgress;
    RunUnwindGuard guard(call->outcome);
    if constexpr (std::is_void_v<Value>) {
        std::forward<Callable>(reader)(&call->payload);
        call->outcome = RunCallOutcome::ReturnedOk;
    } else {
        auto result = std::forward<Callable>(reader)(&call->payload);
        call->outcome = RunCallOutcome::ReturnedOk;
        return result;
    }
}

struct RunLoopBoundary {
    bool reached{false};
    bool completed{false};
};

struct RunEmptyObservation {};

struct RunPnlFinalization {
    std::string strategy;
    bool allocation_found{false};
    double allocation{1.0};
    double initial_capital{0.0};
    double strategy_capital{0.0};
    RunInvocation<RunEmptyObservation> call;

    void record_operands(bool found, double effective_allocation,
                         double capital_operand, double forwarded_capital) {
        allocation_found = found;
        allocation = effective_allocation;
        initial_capital = capital_operand;
        strategy_capital = forwarded_capital;
    }
};

struct RunConsumption {
    RunEvidenceFailure failure{RunEvidenceFailure::None};
    bool controlled_selection{false};
    std::optional<RunInvocation<SelectionConsumption>> selection;
    std::optional<RunInvocation<FactoryConsumption>> primary_factory;
    std::vector<RunNamedInvocation<PortfolioRegistrationTrace>> registrations;
    int historical_days{0};
    bool historical_days_reached{false};
    RunInvocation<RunEmptyObservation> market_fetch;
    RunInvocation<RunEmptyObservation> arrow_conversion;
    bool market_input_completed{false};
    RunLoopBoundary history_loop;
    std::vector<RunNamedInvocation<ExecutionMarketDataObservation>> history_updates;
    bool non_trading_decision_reached{false};
    bool skip_strategy_processing{false};
    bool preparation_skipped{false};
    bool primary_skipped{false};
    RunLoopBoundary preparation_stage;
    RunLoopBoundary primary_stage;
    std::optional<RunNamedInvocation<StrategyConsumptionTrace>> preparation;
    std::optional<RunInvocation<PortfolioConsumptionTrace>> primary;
    RunLoopBoundary pnl_loop;
    bool pnl_path_decision_reached{false};
    bool pnl_path_eligible{false};
    std::vector<RunPnlFinalization> pnl_finalizations;
    RunLoopBoundary execution_loop;
    std::vector<RunNamedInvocation<DailyExecutionObservation>> execution_batches;
    RunBenchmarkMode benchmark_mode{RunBenchmarkMode::Deferred};
    bool benchmark_decision_reached{false};
    RunBenchmarkState benchmark_state{RunBenchmarkState::NotReached};
    bool diagnostics_reached{false};
    bool diagnostics_completed{false};
    std::optional<RunInvocation<RiskConfigConsumption>> snapshot_risk;

    bool admit_strategy(std::string_view identity);
    bool admit_symbol(std::string_view identity);
    bool admit_entry(size_t count);
    bool collecting() const noexcept { return failure == RunEvidenceFailure::None; }
    bool observe_native_limits();
    void discard_failed_evidence();
    void observe_benchmark_mode(std::string_view mode);
    void observe_non_trading_decision(bool skip);
    void note_benchmark_error();
    void note_benchmark_success();

    RunPnlFinalization* append_pnl(std::string_view strategy) {
        if (!admit_entry(pnl_finalizations.size()) || !admit_strategy(strategy)) return nullptr;
        pnl_finalizations.emplace_back();
        pnl_finalizations.back().strategy = strategy;
        return &pnl_finalizations.back();
    }

    template <typename Payload>
    RunNamedInvocation<Payload>* append_strategy(
        std::vector<RunNamedInvocation<Payload>>& entries, std::string_view identity) {
        if (!admit_entry(entries.size()) || !admit_strategy(identity)) return nullptr;
        entries.emplace_back();
        entries.back().identity = identity;
        return &entries.back();
    }

    template <typename Payload>
    RunNamedInvocation<Payload>* append_symbol(
        std::vector<RunNamedInvocation<Payload>>& entries, std::string_view identity) {
        if (!admit_entry(entries.size()) || !admit_symbol(identity)) return nullptr;
        entries.emplace_back();
        entries.back().identity = identity;
        return &entries.back();
    }

private:
    std::unordered_set<std::string> strategies_;
    std::unordered_set<std::string> symbols_;
};

inline bool valid_run_identity(std::string_view identity, bool symbol) noexcept {
    if (identity.empty() || identity.size() > 64) return false;
    const auto alnum = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9');
    };
    if (!alnum(identity.front())) return false;
    for (char c : identity.substr(1)) {
        if (!alnum(c) && c != '_' && c != '.' && c != ':' && c != '-' &&
            !(symbol && c == '/')) return false;
    }
    return true;
}

inline bool RunConsumption::admit_strategy(std::string_view identity) {
    if (failure != RunEvidenceFailure::None) return false;
    if (!valid_run_identity(identity, false)) {
        failure = RunEvidenceFailure::InvalidObservedIdentity;
        return false;
    }
    for (const auto& existing : strategies_) {
        if (existing == identity) return true;
    }
    if (strategies_.size() >= 32) {
        failure = RunEvidenceFailure::CapacityExceeded;
        return false;
    }
    strategies_.emplace(identity);
    return true;
}

inline bool RunConsumption::admit_symbol(std::string_view identity) {
    if (failure != RunEvidenceFailure::None) return false;
    if (!valid_run_identity(identity, true)) {
        failure = RunEvidenceFailure::InvalidObservedIdentity;
        return false;
    }
    for (const auto& existing : symbols_) {
        if (existing == identity) return true;
    }
    if (symbols_.size() >= 1024) {
        failure = RunEvidenceFailure::CapacityExceeded;
        return false;
    }
    symbols_.emplace(identity);
    return true;
}

inline bool RunConsumption::admit_entry(size_t count) {
    if (failure != RunEvidenceFailure::None) return false;
    if (count >= 2048) {
        failure = RunEvidenceFailure::CapacityExceeded;
        return false;
    }
    return true;
}
inline bool RunConsumption::observe_native_limits() {
    if (!collecting()) return false;
    struct Cleanup {
        RunConsumption& run;
        ~Cleanup() { run.discard_failed_evidence(); }
    } cleanup{*this};
    const auto count_ok = [this](size_t count) {
        if (count <= 2048) return true;
        failure = RunEvidenceFailure::CapacityExceeded;
        return false;
    };
    const auto symbol_map_ok = [this, &count_ok](const auto& values) {
        if (!count_ok(values.size())) return false;
        for (const auto& [symbol, value] : values) {
            (void)value;
            if (!admit_symbol(symbol)) return false;
        }
        return true;
    };
    const auto strategy_ok = [&symbol_map_ok](const StrategyConsumptionTrace& trace) {
        return symbol_map_ok(trace.sizing.symbol_limits) &&
               symbol_map_ok(trace.buffering.symbol_limits) &&
               symbol_map_ok(trace.base_risk.trading_multipliers) &&
               symbol_map_ok(trace.position_limits.symbols);
    };
    if (selection) {
        const auto& reads = selection->payload;
        if (!count_ok(reads.controlled_validation.size()) ||
            !count_ok(reads.ordinary_selection.size())) return false;
        for (const auto& read : reads.controlled_validation)
            if (!admit_strategy(read.name)) return false;
        for (const auto& read : reads.ordinary_selection)
            if (!admit_strategy(read.name)) return false;
    }
    if (primary_factory) {
        if (!count_ok(primary_factory->payload.entries.size())) return false;
        for (const auto& entry : primary_factory->payload.entries)
            if (!admit_strategy(entry.name)) return false;
    }
    if (!count_ok(registrations.size()) || !count_ok(history_updates.size()) ||
        !count_ok(pnl_finalizations.size()) || !count_ok(execution_batches.size())) return false;
    if (preparation && !strategy_ok(preparation->call.payload)) return false;
    if (primary) {
        const auto& trace = primary->payload;
        if (!count_ok(trace.strategies.size()) ||
            !count_ok(trace.strategy_charges.size()) ||
            !count_ok(trace.compatibility_charges.size())) return false;
        for (const auto& invocation : trace.strategies) {
            if (!admit_strategy(invocation.strategy_id) ||
                !strategy_ok(invocation.strategy)) return false;
        }
        for (const auto& charge : trace.strategy_charges) {
            if (!admit_strategy(charge.strategy_id) || !admit_symbol(charge.symbol)) return false;
        }
        for (const auto& charge : trace.compatibility_charges)
            if (!admit_symbol(charge.symbol)) return false;
        for (size_t i = 0; i < trace.pass_count && i < trace.passes.size(); ++i) {
            const auto& optimization = trace.passes[i].optimization;
            if (!count_ok(optimization.estimates.size())) return false;
            for (const auto& estimate : optimization.estimates)
                if (!admit_symbol(estimate.symbol)) return false;
            for (const auto& map : optimization.strategies) {
                if (!count_ok(map.size())) return false;
                for (const auto& [strategy, value] : map) {
                    (void)value;
                    if (!admit_strategy(strategy)) return false;
                }
            }
        }
    }
    for (const auto& batch : execution_batches) {
        if (!count_ok(batch.call.payload.attempts.size())) return false;
        for (const auto& attempt : batch.call.payload.attempts)
            if (!admit_symbol(attempt.symbol)) return false;
    }
    return true;
}

inline void RunConsumption::discard_failed_evidence() {
    if (collecting()) return;
    selection.reset();
    primary_factory.reset();
    registrations.clear();
    history_updates.clear();
    preparation.reset();
    primary.reset();
    pnl_finalizations.clear();
    execution_batches.clear();
    snapshot_risk.reset();
    strategies_.clear();
    symbols_.clear();
}
inline void RunConsumption::observe_benchmark_mode(std::string_view mode) {
    benchmark_decision_reached = true;
    benchmark_mode = mode == "live" ? RunBenchmarkMode::Live
                    : mode == "deferred" ? RunBenchmarkMode::Deferred
                                         : RunBenchmarkMode::Unsupported;
    benchmark_state = benchmark_mode == RunBenchmarkMode::Live
                          ? RunBenchmarkState::InProgress : RunBenchmarkState::Deferred;
}

inline void RunConsumption::observe_non_trading_decision(bool skip) {
    non_trading_decision_reached = true;
    skip_strategy_processing = skip;
    preparation_skipped = skip;
    primary_skipped = skip;
}

inline void RunConsumption::note_benchmark_error() {
    benchmark_state = RunBenchmarkState::NonfatalError;
}

inline void RunConsumption::note_benchmark_success() {
    if (benchmark_state == RunBenchmarkState::InProgress)
        benchmark_state = RunBenchmarkState::Succeeded;
}

}  // namespace trade_ngin
