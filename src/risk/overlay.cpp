// src/risk/overlay.cpp
#include "trade_ngin/risk/overlay.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace trade_ngin {
namespace overlay {

const char* GateWindow::name(Mode mode) {
    switch (mode) {
        case Mode::kComplete:
            return "complete";
        case Mode::kZeroFill:
            return "zerofill";
        case Mode::kBlind:
            break;
    }
    return "blind";
}

GateWindow gate_window(const std::vector<std::vector<double>>& returns,
                       const std::vector<double>& ordinals) {
    GateWindow out;
    const std::size_t participants = returns.empty() ? 0 : returns.front().size();
    out.in_r.assign(participants, 0);
    out.in_shock.assign(participants, 0);
    out.no_return.assign(participants, 1);
    out.short_history.assign(participants, 0);
    out.shock_sigma.assign(participants, 0.0);
    if (participants == 0 || returns.size() != ordinals.size()) return out;

    // The window: the last kWindowDates rows on which any participant has a return.
    std::vector<std::size_t> rows;
    for (std::size_t d = returns.size(); d-- > 0 && rows.size() < kWindowDates;) {
        bool any = false;
        for (double r : returns[d]) {
            if (!std::isnan(r)) {
                any = true;
                break;
            }
        }
        if (any) rows.push_back(d);
    }
    std::reverse(rows.begin(), rows.end());
    out.window_dates = rows.size();
    if (rows.empty()) return out;
    out.first_ordinal = ordinals[rows.front()];
    out.last_ordinal = ordinals[rows.back()];

    std::vector<std::size_t> count(participants, 0);
    for (std::size_t d : rows) {
        for (std::size_t i = 0; i < participants; ++i) {
            if (!std::isnan(returns[d][i])) ++count[i];
        }
    }
    std::vector<std::size_t> core;
    for (std::size_t i = 0; i < participants; ++i) {
        out.no_return[i] = count[i] == 0;
        out.short_history[i] = count[i] > 0 && count[i] < kParticipantReturnsFloor;
        if (!out.no_return[i] && !out.short_history[i]) {
            out.in_r[i] = 1;
            core.push_back(i);
        }
    }
    if (core.empty()) {
        std::fill(out.in_r.begin(), out.in_r.end(), 0);
        return out;
    }

    std::vector<std::size_t> complete;
    for (std::size_t d : rows) {
        bool all = true;
        for (std::size_t i : core) {
            if (std::isnan(returns[d][i])) {
                all = false;
                break;
            }
        }
        if (all) complete.push_back(d);
    }
    out.complete_dates = complete.size();
    if (complete.size() < kBlindBelow) return out;  // BLIND: in_r stays, no covariance reading

    const double span = ordinals[complete.back()] - ordinals[complete.front()];
    out.bars_per_year = static_cast<double>(complete.size() - 1) / (span / kDaysPerYear);

    // The sample covariance of the complete dates, or of every window row with the missing
    // returns zero-filled.
    const bool use_complete = complete.size() >= kCompleteDatesFloor;
    out.mode = use_complete ? GateWindow::Mode::kComplete : GateWindow::Mode::kZeroFill;
    const std::vector<std::size_t>& sample = use_complete ? complete : rows;
    const std::size_t n = sample.size();
    const std::size_t k = core.size();
    auto value = [&](std::size_t d, std::size_t i) {
        const double r = returns[d][i];
        return std::isnan(r) ? 0.0 : r;
    };
    std::vector<double> mean(k, 0.0);
    for (std::size_t a = 0; a < k; ++a) {
        double sum = 0.0;
        for (std::size_t d : sample) sum += value(d, core[a]);
        mean[a] = sum / static_cast<double>(n);
    }
    out.covariance.assign(k, std::vector<double>(k, 0.0));
    for (std::size_t a = 0; a < k; ++a) {
        for (std::size_t b = a; b < k; ++b) {
            double sum = 0.0;
            for (std::size_t d : sample) {
                sum += (value(d, core[a]) - mean[a]) * (value(d, core[b]) - mean[b]);
            }
            const double cov = sum / static_cast<double>(n - 1) * out.bars_per_year;
            out.covariance[a][b] = cov;
            out.covariance[b][a] = cov;
        }
    }
    for (std::size_t a = 0; a < k; ++a) {
        out.shock_sigma[core[a]] = std::sqrt(std::max(out.covariance[a][a], 0.0));
        out.in_shock[core[a]] = 1;
    }
    // A short-history participant shocks on its own window sigma (at least two returns).
    for (std::size_t i = 0; i < participants; ++i) {
        if (!out.short_history[i] || count[i] < 2) continue;
        double sum = 0.0;
        for (std::size_t d : rows) {
            if (!std::isnan(returns[d][i])) sum += returns[d][i];
        }
        const double own_mean = sum / static_cast<double>(count[i]);
        double squares = 0.0;
        for (std::size_t d : rows) {
            if (!std::isnan(returns[d][i])) {
                squares += (returns[d][i] - own_mean) * (returns[d][i] - own_mean);
            }
        }
        out.shock_sigma[i] =
            std::sqrt(squares / static_cast<double>(count[i] - 1) * out.bars_per_year);
        out.in_shock[i] = 1;
    }
    return out;
}

Readings readings(const std::vector<double>& weights, const GateWindow& window,
                  const std::vector<double>& sigma_jump) {
    Readings out;
    for (double x : weights) {
        out.gross += std::abs(x);
        out.net += x;
    }
    if (window.blind()) return out;
    out.covariance_readings = true;
    std::vector<std::size_t> core;
    for (std::size_t i = 0; i < window.in_r.size(); ++i) {
        if (window.in_r[i]) core.push_back(i);
    }
    const std::size_t k = core.size();
    std::vector<double> sigma(k, 0.0);
    for (std::size_t a = 0; a < k; ++a) sigma[a] = std::sqrt(std::max(window.covariance[a][a], 0.0));
    double risk = 0.0, jump = 0.0;
    for (std::size_t a = 0; a < k; ++a) {
        for (std::size_t b = 0; b < k; ++b) {
            const double xa = weights[core[a]], xb = weights[core[b]];
            risk += xa * window.covariance[a][b] * xb;
            // the window's correlation on the two jump sigmas
            double rho = a == b ? 1.0 : 0.0;
            if (a != b && sigma[a] * sigma[b] > 0.0) {
                rho = window.covariance[a][b] / (sigma[a] * sigma[b]);
            }
            jump += xa * rho * sigma_jump[core[a]] * sigma_jump[core[b]] * xb;
        }
    }
    out.risk = std::sqrt(std::max(risk, 0.0));
    out.jump = std::sqrt(std::max(jump, 0.0));
    for (std::size_t i = 0; i < window.in_shock.size(); ++i) {
        if (window.in_shock[i]) out.shock += std::abs(weights[i]) * window.shock_sigma[i];
    }
    return out;
}

Multiplier multiplier(const Readings& readings, const Limits& limits) {
    Multiplier out;
    auto term = [](bool computed, double reading, double limit) {
        const double x = std::abs(reading);
        return computed && std::isfinite(x) && x > 0.0 ? std::min(1.0, limit / x) : 1.0;
    };
    out.risk = term(readings.covariance_readings, readings.risk, limits.risk);
    out.jump = term(readings.covariance_readings, readings.jump, limits.jump);
    out.shock = term(readings.covariance_readings, readings.shock, limits.shock);
    out.gross = term(true, readings.gross, limits.gross);
    out.net = term(true, readings.net, limits.net);
    const std::pair<const char*, double> terms[] = {{"R", out.risk},
                                                    {"R_jump", out.jump},
                                                    {"R_shock", out.shock},
                                                    {"L_g", out.gross},
                                                    {"L_n", out.net}};
    for (const auto& [name, value] : terms) out.m = std::min(out.m, value);
    if (out.m < 1.0) {
        for (const auto& [name, value] : terms) {
            if (value == out.m) {
                out.binding = name;
                break;
            }
        }
    }
    return out;
}

double percentile(std::vector<double> values, double pct) {
    if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    const double pos = (pct / 100.0) * static_cast<double>(values.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = std::min(lo + 1, values.size() - 1);
    return values[lo] + (pos - static_cast<double>(lo)) * (values[hi] - values[lo]);
}

Inputs build_inputs(double tau, const std::vector<std::string>& symbols,
                    const std::vector<ParticipantSeries>& series) {
    Inputs out;
    out.tau = tau;
    out.symbols = symbols;
    const std::size_t n = symbols.size();
    out.close.assign(n, 0.0);
    out.multiplier.assign(n, 1.0);
    out.jump_sigma_daily.assign(n, 0.0);
    out.has_series.assign(n, 0);
    for (std::size_t i = 0; i < n && i < series.size(); ++i) {
        const ParticipantSeries& s = series[i];
        if (!s.present || s.day == nullptr || s.returns == nullptr) continue;
        out.has_series[i] = 1;
        out.close[i] = s.close;
        out.multiplier[i] = s.multiplier;
        out.jump_sigma_daily[i] = s.jump_sigma_daily;
        out.ordinals.insert(out.ordinals.end(), s.day->begin(), s.day->end());
    }
    std::sort(out.ordinals.begin(), out.ordinals.end());
    out.ordinals.erase(std::unique(out.ordinals.begin(), out.ordinals.end()), out.ordinals.end());
    out.returns.assign(out.ordinals.size(),
                       std::vector<double>(n, std::numeric_limits<double>::quiet_NaN()));
    for (std::size_t i = 0; i < n && i < series.size(); ++i) {
        if (!out.has_series[i]) continue;
        const auto& day = *series[i].day;
        const auto& ret = *series[i].returns;
        for (std::size_t k = 0; k < day.size() && k < ret.size(); ++k) {
            const auto row = std::lower_bound(out.ordinals.begin(), out.ordinals.end(), day[k]) -
                             out.ordinals.begin();
            out.returns[static_cast<std::size_t>(row)][i] = ret[k];
        }
    }
    return out;
}

Evaluation evaluate(const Inputs& inputs, const std::vector<std::pair<std::string, double>>& book,
                    double capital, const LimitRatios& ratios) {
    Evaluation out;
    const std::size_t n = inputs.symbols.size();
    out.weights.assign(n, 0.0);
    for (const auto& [symbol, quantity] : book) {
        if (quantity == 0.0) continue;
        const auto at = std::lower_bound(inputs.symbols.begin(), inputs.symbols.end(), symbol);
        const std::size_t i = static_cast<std::size_t>(at - inputs.symbols.begin());
        if (at == inputs.symbols.end() || *at != symbol || !inputs.has_series[i] ||
            !(capital > 0.0)) {
            out.outside.push_back(symbol);
            continue;
        }
        out.weights[i] = quantity * inputs.multiplier[i] * inputs.close[i] / capital;
    }
    std::sort(out.outside.begin(), out.outside.end());
    out.window = gate_window(inputs.returns, inputs.ordinals);
    out.sigma_jump.assign(n, 0.0);
    const double factor = out.window.blind() ? 0.0 : std::sqrt(out.window.bars_per_year);
    for (std::size_t i = 0; i < n; ++i) out.sigma_jump[i] = inputs.jump_sigma_daily[i] * factor;
    out.readings = readings(out.weights, out.window, out.sigma_jump);
    out.limits.risk = ratios.risk * inputs.tau;
    out.limits.jump = ratios.jump * inputs.tau;
    out.limits.shock = ratios.shock * inputs.tau;
    out.limits.gross = ratios.gross;
    out.limits.net = ratios.net;
    out.multiplier = multiplier(out.readings, out.limits);
    return out;
}

}  // namespace overlay
}  // namespace trade_ngin
