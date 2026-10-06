#include "trade_ngin/portfolio/component_optimizer_evaluator.hpp"
#include "trade_ngin/optimization/annualized_sample_covariance.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace trade_ngin {
namespace {
using Evaluation = ComponentOptimizerEvaluation;

bool has_text(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return !std::isspace(c); });
}
std::string identity_text(const InstrumentIdentity& id) {
    return std::to_string(static_cast<int>(id.type)) + ":" + id.symbol;
}
Result<Evaluation> reject(ErrorCode code, const std::string& message) {
    return make_error<Evaluation>(code, message, "ComponentOptimizerEvaluator");
}
Result<Evaluation> forward(const TradeError& error) {
    return make_error<Evaluation>(error.code(), error.what(), error.component());
}
bool can_add(int64_t a, int64_t b) {
    return (b <= 0 || a <= std::numeric_limits<int64_t>::max() - b) &&
           (b >= 0 || a >= std::numeric_limits<int64_t>::min() - b);
}
bool valid_vector(const std::optional<std::vector<double>>& values, size_t n) {
    return !values || (values->size() == n &&
        std::all_of(values->begin(), values->end(),
                    [](double value) { return std::isfinite(value); }));
}

// Conservatively cover the solver's matrix products, rank-one update,
// linear penalty and buffer path using its maximum number of steps.
bool safe_solver_envelope(const ComponentOptimizerPreparedInputs& p,
                          const DynamicOptConfig& config, std::string& stage) {
    const size_t n = p.bindings.size();
    const long double limit = static_cast<long double>(std::numeric_limits<double>::max()) / 8.0L;
    const auto safe = [limit](long double x) { return std::isfinite(x) && x <= limit; };
    std::vector<long double> errors(n), buffer_errors(n);
    long double unscaled_cost_sum = 0.0L;
    for (size_t i = 0; i < n; ++i) {
        const long double steps = static_cast<long double>(config.max_iterations) *
                                  p.weights_per_increment[i];
        const long double prior = std::abs(static_cast<long double>(p.current_weights[i]));
        errors[i] = std::abs(static_cast<long double>(p.target_weights[i])) +
                    std::max(prior, steps);
        buffer_errors[i] = prior + steps;
        if (!safe(steps) || !safe(errors[i]) || !safe(buffer_errors[i])) {
            stage = identity_text(p.bindings[i].instrument) + ":weight_or_cost_bound";
            return false;
        }
        const long double unscaled_cost = buffer_errors[i] * p.cost_coefficients[i];
        unscaled_cost_sum += unscaled_cost;
        if (!safe(unscaled_cost) || !safe(unscaled_cost_sum)) {
            stage = identity_text(p.bindings[i].instrument) + ":unscaled_cost_bound";
            return false;
        }
        if (config.use_buffering) {
            const long double count = buffer_errors[i] / p.weights_per_increment[i];
            const long double margin = 1.0L +
                8.0L * std::numeric_limits<double>::epsilon() * config.max_iterations;
            if (!std::isfinite(count) ||
                count + margin >= std::numeric_limits<int>::max()) {
                stage = identity_text(p.bindings[i].instrument) + ":buffer_step_count";
                return false;
            }
        }
    }
    const long double scaled_cost_sum = unscaled_cost_sum * config.cost_penalty_scalar;
    if (!safe(scaled_cost_sum)) {
        stage = identity_text(p.bindings.back().instrument) + ":scaled_cost_bound";
        return false;
    }
    long double quadratic = 0.0L, buffer_quadratic = 0.0L;
    for (size_t i = 0; i < n; ++i) {
        long double row = 0.0L;
        for (size_t j = 0; j < n; ++j) {
            const long double cov = std::abs(static_cast<long double>(p.annualized_covariance[i][j]));
            row += cov * errors[j];
            quadratic += cov * errors[i] * errors[j];
            buffer_quadratic += cov * buffer_errors[i] * buffer_errors[j];
            if (!safe(row) || !safe(quadratic) || !safe(buffer_quadratic)) {
                stage = identity_text(p.bindings[i].instrument) + ":quadratic_bound";
                return false;
            }
        }
        const long double step = p.weights_per_increment[i];
        const long double rank_one = 2.0L * step * row + step * step *
            std::abs(static_cast<long double>(p.annualized_covariance[i][i]));
        if (!safe(rank_one) || !safe(quadratic + rank_one) ||
            !safe(std::sqrt(quadratic) + scaled_cost_sum)) {
            stage = identity_text(p.bindings[i].instrument) + ":rank_one_bound";
            return false;
        }
    }
    return true;
}
}  // namespace

ComponentOptimizerEvaluator::ComponentOptimizerEvaluator(DynamicOptConfig config)
    : optimizer_(std::move(config)) {}

Result<ComponentOptimizerEvaluation> ComponentOptimizerEvaluator::evaluate(
    const ComponentBookContext& context, const ComponentBookProposal& proposal,
    const ComponentOptimizerInputs& inputs) const {
    try {
        if (inputs.expected_portfolio_id != context.portfolio_id ||
            inputs.expected_date != context.date ||
            inputs.expected_portfolio_type != context.portfolio_type ||
            inputs.expected_revision != context.revision) {
            return reject(ErrorCode::INVALID_ARGUMENT, "optimizer_context_mismatch: supplied=" +
                inputs.expected_portfolio_id + ":" + inputs.expected_date + ":" +
                inputs.expected_portfolio_type + ":" + inputs.expected_revision + " context=" +
                context.portfolio_id + ":" + context.date + ":" +
                context.portfolio_type + ":" + context.revision);
        }
        if (!has_text(inputs.market_snapshot_id) || !has_text(inputs.capital_currency)) {
            return reject(ErrorCode::INVALID_ARGUMENT,
                          "optimizer_input_units: missing snapshot or capital currency");
        }
        auto overlay = overlay_component_book(context, proposal);
        if (overlay.is_error()) return forward(*overlay.error());
        const auto& book = overlay.value();
        if (book.instruments.empty()) {
            return reject(ErrorCode::INVALID_ARGUMENT, "optimizer_instrument_coverage: empty book");
        }
        const auto& config = optimizer_.get_config();
        if (!std::isfinite(config.capital) || config.capital <= 0.0 ||
            !std::isfinite(config.tau) || config.tau <= 0.0 ||
            !std::isfinite(config.cost_penalty_scalar) || config.cost_penalty_scalar < 0.0 ||
            !std::isfinite(config.convergence_threshold) || config.convergence_threshold < 0.0 ||
            !std::isfinite(config.buffer_size_factor) || config.buffer_size_factor < 0.0 ||
            !std::isfinite(config.asymmetric_risk_buffer) || config.max_iterations <= 0 ||
            config.max_iterations >= std::numeric_limits<int>::max() ||
            !std::isfinite(config.tau * config.buffer_size_factor)) {
            return reject(ErrorCode::INVALID_ARGUMENT, "optimizer_config_invalid: numerical value");
        }
        std::map<InstrumentIdentity, const ComponentOptimizerInstrument*> instruments;
        for (const auto& value : inputs.instruments) {
            const auto& id = value.instrument;
            if (!instruments.emplace(id, &value).second) {
                return reject(ErrorCode::INVALID_ARGUMENT,
                              "optimizer_instrument_coverage: duplicate " + identity_text(id));
            }
            if (value.mark_as_of != inputs.valuation_time ||
                !std::isfinite(value.mark) || value.mark <= 0.0 ||
                !std::isfinite(value.price_multiplier) || value.price_multiplier <= 0.0 ||
                value.calculation_increment.raw_value() <= 0 ||
                !std::isfinite(value.cash_cost_per_increment) || value.cash_cost_per_increment < 0.0 ||
                value.quote_currency != inputs.capital_currency ||
                value.cost_currency != inputs.capital_currency ||
                !has_text(value.increment_source_id) || !has_text(value.cost_source_id)) {
                return reject(ErrorCode::INVALID_ARGUMENT,
                              "optimizer_input_units: " + identity_text(id));
            }
        }
        if (instruments.size() != book.instruments.size()) {
            return reject(ErrorCode::INVALID_ARGUMENT,
                          "optimizer_instrument_coverage: count mismatch");
        }
        for (const auto& aggregate : book.instruments) {
            if (!instruments.contains(aggregate.instrument)) {
                return reject(ErrorCode::INVALID_ARGUMENT,
                              "optimizer_instrument_coverage: missing " +
                              identity_text(aggregate.instrument));
            }
        }
        const auto& grid = inputs.expected_observation_times;
        if (grid.size() < 21 || grid.back() > inputs.valuation_time) {
            return reject(ErrorCode::INVALID_ARGUMENT, "optimizer_history_grid: short or future");
        }
        for (size_t t = 1; t < grid.size(); ++t) {
            if (grid[t] <= grid[t - 1] || grid[t] > inputs.valuation_time) {
                return reject(ErrorCode::INVALID_ARGUMENT,
                              "optimizer_history_grid: unsorted, duplicate or future");
            }
        }
        std::map<Timestamp, size_t> grid_index;
        for (size_t t = 0; t < grid.size(); ++t) grid_index.emplace(grid[t], t);
        std::map<InstrumentIdentity, size_t> asset_index;
        for (size_t i = 0; i < book.instruments.size(); ++i) {
            asset_index.emplace(book.instruments[i].instrument, i);
        }
        std::vector<std::vector<double>> closes(book.instruments.size(),
                                                std::vector<double>(grid.size(), 0.0));
        std::vector<std::vector<bool>> seen(book.instruments.size(),
                                            std::vector<bool>(grid.size(), false));
        for (const auto& close : inputs.closes) {
            const auto asset = asset_index.find(close.instrument);
            const auto time = grid_index.find(close.timestamp);
            if (asset == asset_index.end() || time == grid_index.end() ||
                !std::isfinite(close.close) || close.close <= 0.0) {
                return reject(ErrorCode::INVALID_ARGUMENT,
                              "optimizer_history_grid: unknown or invalid close " +
                              identity_text(close.instrument));
            }
            if (seen[asset->second][time->second]) {
                return reject(ErrorCode::INVALID_ARGUMENT,
                              "optimizer_history_grid: duplicate close " +
                              identity_text(close.instrument));
            }
            seen[asset->second][time->second] = true;
            closes[asset->second][time->second] = close.close;
        }
        for (size_t i = 0; i < seen.size(); ++i) {
            if (std::find(seen[i].begin(), seen[i].end(), false) != seen[i].end()) {
                return reject(ErrorCode::INVALID_ARGUMENT,
                              "optimizer_history_grid: missing close " +
                              identity_text(book.instruments[i].instrument));
            }
        }
        ComponentOptimizerPreparedInputs prepared;
        const size_t n = book.instruments.size();
        std::map<InstrumentIdentity, int64_t> previous_raw;
        for (const auto& component : book.components) {
            if (!component.previous) continue;
            const int64_t prior = previous_raw[component.instrument];
            const int64_t added = component.previous->quantity.raw_value();
            if (!can_add(prior, added)) {
                return reject(ErrorCode::INVALID_DATA,
                              "optimizer_numeric_range: previous aggregate " +
                              identity_text(component.instrument));
            }
            previous_raw[component.instrument] = prior + added;
        }
        for (const auto& aggregate : book.instruments) {
            const auto& id = aggregate.instrument;
            const auto& value = *instruments.at(id);
            const Quantity previous = Quantity::from_raw(previous_raw[id]);
            prepared.bindings.push_back({id, aggregate.members, previous, aggregate.net_quantity});
            const double mark_value = value.mark * value.price_multiplier;
            const double unit_weight = mark_value / config.capital;
            const double prior = previous.to_double();
            const double target = aggregate.net_quantity.to_double();
            const double increment = value.calculation_increment.to_double();
            const double current_weight = prior * unit_weight;
            const double target_weight = target * unit_weight;
            const double step = increment * unit_weight;
            const double cost_fraction = value.cash_cost_per_increment / config.capital;
            const double coefficient = cost_fraction / step;
            if (!std::isfinite(mark_value) || mark_value <= 0.0 ||
                !std::isfinite(unit_weight) || unit_weight <= 0.0 ||
                !std::isfinite(step) || step <= 0.0 ||
                !std::isfinite(current_weight) || !std::isfinite(target_weight) ||
                (prior != 0.0 && current_weight == 0.0) ||
                (target != 0.0 && target_weight == 0.0) ||
                !std::isfinite(cost_fraction) ||
                (value.cash_cost_per_increment != 0.0 && cost_fraction == 0.0) ||
                !std::isfinite(coefficient) ||
                (cost_fraction != 0.0 && coefficient == 0.0)) {
                return reject(ErrorCode::INVALID_DATA,
                              "optimizer_numeric_range: weight or cost conversion " + identity_text(id));
            }
            prepared.current_weights.push_back(current_weight);
            prepared.target_weights.push_back(target_weight);
            prepared.weights_per_increment.push_back(step);
            prepared.cost_coefficients.push_back(coefficient);
        }
        std::vector<std::vector<double>> returns(grid.size() - 1, std::vector<double>(n, 0.0));
        for (size_t i = 0; i < n; ++i) {
            for (size_t t = 1; t < grid.size(); ++t) {
                const double change = closes[i][t] - closes[i][t - 1];
                const double daily = change / closes[i][t - 1];
                if (!std::isfinite(change) || !std::isfinite(daily)) {
                    return reject(ErrorCode::INVALID_DATA,
                                  "optimizer_numeric_range: daily return " +
                                  identity_text(book.instruments[i].instrument));
                }
                returns[t - 1][i] = daily;
            }
        }
        prepared.annualized_covariance = detail::annualized_sample_covariance(returns);
        for (size_t i = 0; i < n; ++i) {
            for (double cov : prepared.annualized_covariance[i]) {
                if (!std::isfinite(cov)) {
                    return reject(ErrorCode::INVALID_DATA,
                                  "optimizer_numeric_range: covariance " +
                                  identity_text(prepared.bindings[i].instrument));
                }
            }
        }
        std::string stage;
        if (!safe_solver_envelope(prepared, config, stage)) {
            return reject(ErrorCode::INVALID_DATA, "optimizer_numeric_range: " + stage);
        }
        OptimizationTrace trace;
        auto optimized = optimizer_.optimize(prepared.current_weights, prepared.target_weights,
            prepared.cost_coefficients, prepared.weights_per_increment,
            prepared.annualized_covariance, &trace);
        if (optimized.is_error()) return forward(*optimized.error());
        const auto& result = optimized.value();
        if (result.positions.size() != n ||
            !std::all_of(result.positions.begin(), result.positions.end(),
                         [](double x) { return std::isfinite(x); }) ||
            !std::isfinite(result.tracking_error) || !std::isfinite(result.cost_penalty) ||
            !valid_vector(trace.solver_positions, n) ||
            !valid_vector(trace.continuous_buffered_positions, n) ||
            !valid_vector(trace.rounded_buffered_positions, n)) {
            return reject(ErrorCode::INVALID_DATA, "optimizer_output_invalid: shape or finite value");
        }
        Evaluation output;
        output.evaluated_book = book;
        output.evaluated_proposal = proposal;
        output.evaluated_inputs = inputs;
        output.evaluated_config = config;
        output.prepared = std::move(prepared);
        output.optimization = result;
        output.trace = std::move(trace);
        std::sort(output.evaluated_proposal.quantities.begin(),
                  output.evaluated_proposal.quantities.end(),
                  [](const auto& a, const auto& b) { return a.key < b.key; });
        std::sort(output.evaluated_inputs.instruments.begin(),
                  output.evaluated_inputs.instruments.end(),
                  [](const auto& a, const auto& b) { return a.instrument < b.instrument; });
        std::sort(output.evaluated_inputs.closes.begin(),
                  output.evaluated_inputs.closes.end(), [](const auto& a, const auto& b) {
                      if (a.instrument == b.instrument) return a.timestamp < b.timestamp;
                      return a.instrument < b.instrument;
                  });
        return Result<Evaluation>(std::move(output));
    } catch (const std::exception& error) {
        return reject(ErrorCode::UNKNOWN_ERROR,
                      std::string("optimizer_adapter_failure: ") + error.what());
    }
}

}  // namespace trade_ngin
