// src/optimization/one_pass.cpp
#include "trade_ngin/optimization/one_pass.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace trade_ngin {
namespace one_pass {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::size_t kMinReturns = 20;
constexpr double kMissingVariance = 0.01;
constexpr double kDaysPerYear = 365.25;

double sign(double v) {
    return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0);
}

// (n u - x)' Sigma (n u - x) for the error vector e
double quadratic(const Vector& e, const Matrix& covariance) {
    double total = 0.0;
    for (std::size_t i = 0; i < e.size(); ++i) {
        double row = 0.0;
        for (std::size_t j = 0; j < e.size(); ++j) row += covariance[i][j] * e[j];
        total += e[i] * row;
    }
    return total;
}

}  // namespace

std::pair<Vector, Mask> cap_target(const Vector& target, const Vector& u, double cap) {
    Vector out = target;
    Mask bound(target.size(), 0);
    for (std::size_t i = 0; i < target.size(); ++i) {
        const double limit = cap / u[i];
        if (std::abs(target[i]) > limit) {
            out[i] = sign(target[i]) * limit;
            bound[i] = 1;
        }
    }
    return {out, bound};
}

std::pair<Vector, Mask> clip_to_cap(const Vector& book, const Vector& u, double cap,
                                    const Mask& rows) {
    Vector out = book;
    Mask clipped(book.size(), 0);
    for (std::size_t i = 0; i < book.size(); ++i) {
        if (rows[i] && std::abs(book[i]) * u[i] > cap) {
            out[i] = sign(book[i]) * std::floor(cap / u[i]);
            clipped[i] = 1;
        }
    }
    return {out, clipped};
}

Covariance optimiser_covariance(const Matrix& closes, const Matrix& levels, const Vector& ordinals,
                                const Mask& stale) {
    Covariance out;
    const std::size_t dates = closes.size();
    const std::size_t n = stale.size();
    out.matrix.assign(n, Vector(n, 0.0));
    out.filled.assign(n, 0);
    std::vector<char> keep(n, 0);
    for (std::size_t i = 0; i < n; ++i) keep[i] = !stale[i];
    std::vector<std::size_t> count(n, 0);
    for (std::size_t d = 0; d < dates; ++d) {
        for (std::size_t i = 0; i < n; ++i) {
            if (!std::isnan(closes[d][i])) ++count[i];
        }
    }
    std::vector<std::size_t> idx;
    std::vector<std::size_t> rows;
    while (true) {
        idx.clear();
        for (std::size_t i = 0; i < n; ++i) {
            if (keep[i]) idx.push_back(i);
        }
        rows.clear();
        if (!idx.empty()) {
            for (std::size_t d = 0; d < dates; ++d) {
                bool all = true;
                for (std::size_t i : idx) {
                    if (std::isnan(closes[d][i])) {
                        all = false;
                        break;
                    }
                }
                if (all) rows.push_back(d);
            }
        }
        if ((rows.size() >= 1 && rows.size() - 1 >= kMinReturns) || idx.size() <= 1) break;
        // drop the participant with the fewest closes (the first of them on a tie)
        std::size_t drop = idx.front();
        for (std::size_t i : idx) {
            if (count[i] < count[drop]) drop = i;
        }
        keep[drop] = 0;
    }
    if (!idx.empty() && rows.size() >= 1 && rows.size() - 1 >= kMinReturns) {
        const std::size_t m = rows.size() - 1;  // returns
        const std::size_t k = idx.size();
        out.bars_per_year = static_cast<double>(m) /
                            ((ordinals[rows.back()] - ordinals[rows.front()]) / kDaysPerYear);
        Matrix ret(m, Vector(k, 0.0));
        for (std::size_t r = 0; r < m; ++r) {
            for (std::size_t a = 0; a < k; ++a) {
                const std::size_t i = idx[a];
                ret[r][a] = (levels[rows[r + 1]][i] - levels[rows[r]][i]) / closes[rows[r]][i];
            }
        }
        Vector mean(k, 0.0);
        for (std::size_t a = 0; a < k; ++a) {
            double sum = 0.0;
            for (std::size_t r = 0; r < m; ++r) sum += ret[r][a];
            mean[a] = sum / static_cast<double>(m);
        }
        for (std::size_t a = 0; a < k; ++a) {
            for (std::size_t b = a; b < k; ++b) {
                double sum = 0.0;
                for (std::size_t r = 0; r < m; ++r) {
                    sum += (ret[r][a] - mean[a]) * (ret[r][b] - mean[b]);
                }
                const double cov = sum / static_cast<double>(m - 1) * out.bars_per_year;
                out.matrix[idx[a]][idx[b]] = cov;
                out.matrix[idx[b]][idx[a]] = cov;
            }
        }
    } else {
        std::fill(keep.begin(), keep.end(), 0);
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!keep[i]) {
            out.filled[i] = 1;
            out.matrix[i][i] = kMissingVariance;
        }
    }
    return out;
}

double tracking_error(const Vector& n, const Vector& target_weights, const Matrix& covariance,
                      const Vector& u, const Vector& held, const Vector& cost, double cost_mult) {
    Vector e(n.size(), 0.0);
    double trade_cost = 0.0;
    for (std::size_t i = 0; i < n.size(); ++i) {
        e[i] = n[i] * u[i] - target_weights[i];
        trade_cost += std::abs(n[i] - held[i]) * cost[i];
    }
    return std::sqrt(std::max(quadratic(e, covariance), 0.0)) + cost_mult * trade_cost;
}

bool admissible(double n_new, double n_old, double side, double u, double cap) {
    if (std::abs(n_new) > std::abs(n_old) && std::abs(n_new) * u > cap) return false;
    return (side != 0.0 && n_new * side >= 0.0) ||
           (std::abs(n_new) < std::abs(n_old) && n_new * n_old >= 0.0);
}

long pass_cap(const Vector& target_weights, const Vector& held, const Vector& u, long max_iterations) {
    double total = 0.0;
    for (std::size_t i = 0; i < u.size(); ++i) {
        total += std::ceil(std::abs(target_weights[i] - held[i] * u[i]) / u[i]);
    }
    return std::max(max_iterations, static_cast<long>(2.0 * total + 1.0));
}

Search search(const Vector& target_weights, const Matrix& covariance, const Vector& u,
              const Vector& held, const Vector& cost, double cost_mult, double cap,
              double threshold, long max_iterations) {
    Search out;
    const std::size_t count = u.size();
    Vector n = held;
    Vector side(count, 0.0);
    for (std::size_t i = 0; i < count; ++i) side[i] = sign(target_weights[i]);
    const long limit = pass_cap(target_weights, held, u, max_iterations);
    double current = tracking_error(n, target_weights, covariance, u, held, cost, cost_mult);
    Vector e(count, 0.0), g(count, 0.0);
    while (true) {
        double q = 0.0;
        double base = 0.0;
        for (std::size_t i = 0; i < count; ++i) e[i] = n[i] * u[i] - target_weights[i];
        for (std::size_t i = 0; i < count; ++i) {
            double row = 0.0;
            for (std::size_t j = 0; j < count; ++j) row += covariance[i][j] * e[j];
            g[i] = row;
        }
        for (std::size_t i = 0; i < count; ++i) {
            q += e[i] * g[i];
            base += std::abs(n[i] - held[i]) * cost[i];
        }
        base *= cost_mult;
        double best = kInf;
        long best_index = -1;
        double best_step = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            for (double step : {1.0, -1.0}) {
                if (!admissible(n[i] + step, n[i], side[i], u[i], cap)) continue;
                const double te =
                    std::sqrt(std::max(
                        q + 2.0 * step * u[i] * g[i] + u[i] * u[i] * covariance[i][i], 0.0)) +
                    base +
                    cost_mult * (std::abs(n[i] + step - held[i]) - std::abs(n[i] - held[i])) * cost[i];
                if (te < best) {
                    best = te;
                    best_index = static_cast<long>(i);
                    best_step = step;
                }
            }
        }
        if (best_index < 0 || !(best < current - threshold)) break;
        if (out.passes >= limit) {
            out.pass_capped = true;
            break;
        }
        n[static_cast<std::size_t>(best_index)] += best_step;
        current = tracking_error(n, target_weights, covariance, u, held, cost, cost_mult);
        ++out.passes;
    }
    out.book = n;
    out.tracking_error = current;
    return out;
}

double round_half_away(double v) {
    return sign(v) * std::floor(std::abs(v) + 0.5);
}

std::pair<double, long> b_sigma(const Vector& u, const Matrix& covariance, const Mask& eligible,
                                double tau, double floor_ratio) {
    double best = -kInf;
    long index = -1;
    for (std::size_t i = 0; i < u.size(); ++i) {
        if (!eligible[i]) continue;
        const double g = u[i] * std::sqrt(std::max(covariance[i][i], 0.0));
        if (g > best) {
            best = g;
            index = static_cast<long>(i);
        }
    }
    if (index < 0 || !(best > floor_ratio * tau)) return {floor_ratio * tau, -1};
    return {best, index};
}

Mask book_eligible(const Vector& searched, const Vector& held) {
    Mask out(searched.size(), 0);
    for (std::size_t i = 0; i < searched.size(); ++i) out[i] = searched[i] != 0.0 || held[i] != 0.0;
    return out;
}

Buffered buffer(const Vector& searched, const Vector& held, const Vector& u,
                const Matrix& covariance, double b) {
    Buffered out;
    Vector delta(held.size(), 0.0);
    for (std::size_t i = 0; i < held.size(); ++i) delta[i] = (held[i] - searched[i]) * u[i];
    out.te_held = std::sqrt(std::max(quadratic(delta, covariance), 0.0));
    if (out.te_held <= b) {
        out.book = held;
        out.unrounded = held;
        return out;
    }
    out.a = (out.te_held - b) / out.te_held;
    out.traded = true;
    out.unrounded.assign(held.size(), 0.0);
    out.book.assign(held.size(), 0.0);
    bool same = true;
    for (std::size_t i = 0; i < held.size(); ++i) {
        out.unrounded[i] = held[i] + out.a * (searched[i] - held[i]);
        out.book[i] = round_half_away(out.unrounded[i]);
        same = same && out.book[i] == held[i];
    }
    out.returned_to_held = same;
    return out;
}

std::pair<Vector, Mask> forecast_close(const Vector& held, const Vector& forecast_sign) {
    Vector out = held;
    Mask closed(held.size(), 0);
    for (std::size_t i = 0; i < held.size(); ++i) {
        if (held[i] * forecast_sign[i] < 0.0) {
            out[i] = 0.0;
            closed[i] = 1;
        }
    }
    return {out, closed};
}

std::vector<std::pair<std::string, double>> over_limit(const overlay::Readings& readings,
                                                       const overlay::Limits& limits) {
    std::vector<std::pair<std::string, double>> out;
    auto check = [&](bool computed, const char* name, double reading, double limit) {
        const double x = std::abs(reading);
        if (computed && std::isfinite(x) && x > limit) out.emplace_back(name, x - limit);
    };
    check(readings.covariance_readings, "R", readings.risk, limits.risk);
    check(readings.covariance_readings, "R_jump", readings.jump, limits.jump);
    check(readings.covariance_readings, "R_shock", readings.shock, limits.shock);
    check(true, "L_g", readings.gross, limits.gross);
    check(true, "L_n", readings.net, limits.net);
    return out;
}

Trimmed trim(const Vector& book, const Mask& candidates,
             const std::function<overlay::Readings(const Vector&)>& read,
             const overlay::Limits& limits, int max_contracts) {
    Trimmed out;
    out.book = book;
    out.readings = read(out.book);
    auto value = [](const overlay::Readings& r, const std::string& term) {
        if (term == "R") return std::abs(r.risk);
        if (term == "R_jump") return std::abs(r.jump);
        if (term == "R_shock") return std::abs(r.shock);
        if (term == "L_g") return std::abs(r.gross);
        return std::abs(r.net);
    };
    while (true) {
        const auto over = over_limit(out.readings, limits);
        if (over.empty()) return out;
        if (static_cast<int>(out.removed.size()) >= max_contracts) {
            out.capped = true;
            return out;
        }
        const std::string term = over.front().first;
        double best = value(out.readings, term);
        long best_index = -1;
        overlay::Readings best_readings;
        for (std::size_t i = 0; i < out.book.size(); ++i) {
            if (!candidates[i] || out.book[i] == 0.0) continue;
            Vector trial = out.book;
            trial[i] -= sign(trial[i]);
            const overlay::Readings r = read(trial);
            if (value(r, term) < best) {
                best = value(r, term);
                best_index = static_cast<long>(i);
                best_readings = r;
            }
        }
        if (best_index < 0) return out;
        const std::size_t i = static_cast<std::size_t>(best_index);
        out.book[i] -= sign(out.book[i]);
        out.removed.emplace_back(i, term);
        out.readings = best_readings;
    }
}

namespace {

bool finite_readings(const overlay::Readings& r) {
    return std::isfinite(r.risk) && std::isfinite(r.jump) && std::isfinite(r.shock) &&
           std::isfinite(r.gross) && std::isfinite(r.net);
}

// The refused day: the held book on every row, nothing searched, trimmed or filled.
void refuse(const DayInputs& in, DayResult& out, const std::string& why, bool on_reread) {
    const std::size_t n = in.held.size();
    out.refusal = why;
    out.refusal_on_reread = on_reread;
    out.searched = false;
    out.scaled_target.assign(n, 0.0);
    out.sign_closed.assign(n, 0);
    out.clipped.assign(n, 0);
    out.search_book = in.held;
    out.pre_trim = in.held;
    out.book = in.held;
    out.trimmed.assign(n, 0.0);
    out.by_hold.assign(n, 0);
    out.by_hold_terms.clear();
    out.over_limit.clear();
    out.over_limit_excess_units = 0.0;
    out.sign_fill.assign(n, 0.0);
    out.rest_fill.assign(n, 0.0);
    out.target_gross = out.stored_gross = out.risk_scale = 0.0;
}

}  // namespace

DayResult rebalance(const DayInputs& in) {
    DayResult out;
    const std::size_t n = in.held.size();
    out.u.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) out.u[i] = in.multiplier[i] * in.close[i] / in.capital;
    bool inputs_finite = std::isfinite(in.capital) && in.capital > 0.0;
    for (std::size_t i = 0; i < n && inputs_finite; ++i) {
        inputs_finite = std::isfinite(out.u[i]) && out.u[i] > 0.0 && std::isfinite(in.held[i]) &&
                        std::isfinite(in.target[i]);
    }

    // The hold set: the engine's own (a non-SESSION bar, a withheld bar, a change bar) and the
    // deferral band (D39): a symbol the first sleeve signals whose held position is on the other
    // side of that sleeve's forecast while |F| < the band. Strict: at |F| = the band it is free.
    out.band.assign(n, 0);
    out.free.assign(n, 0);
    out.fixed.assign(n, 0);
    out.closeout.assign(n, 0);
    out.participant.assign(n, 0);
    Mask hold = in.hold;
    for (std::size_t i = 0; i < n; ++i) {
        const double f = in.first_forecast[i];
        if (in.first_signalling[i] && in.held[i] * f < 0.0 && std::abs(f) < in.sign_band) {
            out.band[i] = 1;
            hold[i] = 1;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        out.free[i] = in.signalling[i] && !hold[i];
        out.closeout[i] = in.has_bar[i] && !hold[i] && !in.signalling[i] && in.ever_signalled[i] &&
                          in.held[i] != 0.0;
        const bool hold_zero = hold[i] && in.signalling[i] && in.held[i] == 0.0;
        out.fixed[i] = !out.free[i] && !out.closeout[i] && (in.held[i] != 0.0 || hold_zero);
        out.participant[i] = out.free[i] || out.fixed[i] || out.closeout[i];
        if (out.participant[i]) out.participants.push_back(i);
    }

    // Section 4: the capped target, then the overlay over the participants.
    Vector target(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) target[i] = out.free[i] ? in.target[i] : 0.0;
    auto [capped, bound] = cap_target(target, out.u, in.cap);
    out.capped_target.assign(n, 0.0);
    out.cap_bound.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (!out.free[i]) continue;
        out.capped_target[i] = capped[i];
        out.cap_bound[i] = bound[i];
    }
    const std::size_t k = out.participants.size();
    Matrix part_returns(in.returns.size(), Vector(k, 0.0));
    for (std::size_t d = 0; d < in.returns.size(); ++d) {
        for (std::size_t a = 0; a < k; ++a) part_returns[d][a] = in.returns[d][out.participants[a]];
    }
    out.window = overlay::gate_window(part_returns, in.ordinals);
    Vector sigma_jump(k, 0.0);
    const double jump_factor = out.window.blind() ? 0.0 : std::sqrt(out.window.bars_per_year);
    for (std::size_t a = 0; a < k; ++a) {
        const double v = in.jump_sigma_daily[out.participants[a]] * jump_factor;
        sigma_jump[a] = std::isfinite(v) ? v : 0.0;
    }
    // the weights of a whole book over the participants: a free or fixed row at its quantity, a
    // close-out at zero
    auto weights_of = [&](const Vector& quantities) {
        Vector x(k, 0.0);
        for (std::size_t a = 0; a < k; ++a) {
            const std::size_t i = out.participants[a];
            x[a] = (out.free[i] || out.fixed[i]) ? quantities[i] * out.u[i] : 0.0;
        }
        return x;
    };
    auto read = [&](const Vector& quantities) {
        return overlay::readings(weights_of(quantities), out.window, sigma_jump);
    };
    Vector target_book(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        target_book[i] = out.free[i] ? out.capped_target[i] : (out.closeout[i] ? 0.0 : in.held[i]);
    }
    if (!inputs_finite) {
        refuse(in, out, "the sizing capital, a weight per contract, a held quantity or a target is "
                        "not a finite number", false);
        return out;
    }
    out.readings = read(target_book);
    out.multiplier = overlay::multiplier(out.readings, in.limits);
    if (!finite_readings(out.readings) || !std::isfinite(out.multiplier.m) ||
        out.multiplier.m < 0.0 || out.multiplier.m > 1.0) {
        refuse(in, out, "the overlay's readings of the capped target are not finite numbers", false);
        return out;
    }
    out.scaled_target.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        if (out.free[i]) out.scaled_target[i] = out.multiplier.m * out.capped_target[i];
    }

    // Section 5.2: the forecast-sign close (the sign of the uncapped N*), then the pass on the free
    // rows from the closed book.
    out.sign_closed.assign(n, 0);
    Vector closed_held = in.held;
    for (std::size_t i = 0; i < n; ++i) {
        if (out.free[i] && in.held[i] * sign(target[i]) < 0.0) {
            out.sign_closed[i] = 1;
            closed_held[i] = 0.0;
        }
    }
    Vector book(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) book[i] = out.closeout[i] ? 0.0 : closed_held[i];
    out.search_book = book;
    out.clipped.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (out.free[i]) out.free_rows.push_back(i);
    }
    const std::size_t f = out.free_rows.size();
    if (f > 0) {
        out.searched = true;
        // Sigma_opt over the free rows: their last closes, the stale rule in union dates of these
        // symbols (more than five union dates behind the newest last close).
        Matrix closes(in.opt_closes.size(), Vector(f, 0.0));
        Matrix levels(in.opt_closes.size(), Vector(f, 0.0));
        std::vector<long> last(f, -1);
        std::vector<long> union_count(in.opt_closes.size() + 1, 0);
        for (std::size_t d = 0; d < in.opt_closes.size(); ++d) {
            bool any = false;
            for (std::size_t a = 0; a < f; ++a) {
                closes[d][a] = in.opt_closes[d][out.free_rows[a]];
                levels[d][a] = in.opt_levels[d][out.free_rows[a]];
                if (!std::isnan(closes[d][a])) {
                    any = true;
                    last[a] = static_cast<long>(d);
                }
            }
            union_count[d + 1] = union_count[d] + (any ? 1 : 0);
        }
        long newest = -1;
        for (long l : last) newest = std::max(newest, l);
        out.stale.assign(f, 0);
        for (std::size_t a = 0; a < f; ++a) {
            // union dates after the symbol's last close, up to the newest last close
            const long behind = last[a] < 0 ? std::numeric_limits<long>::max()
                                            : union_count[static_cast<std::size_t>(newest) + 1] -
                                                  union_count[static_cast<std::size_t>(last[a]) + 1];
            out.stale[a] = behind > 5;
        }
        out.covariance = optimiser_covariance(closes, levels, in.opt_ordinals, out.stale);

        Vector uf(f), hf(f), xt(f), cost(f);
        for (std::size_t a = 0; a < f; ++a) {
            const std::size_t i = out.free_rows[a];
            uf[a] = out.u[i];
            hf[a] = closed_held[i];
            xt[a] = out.scaled_target[i] * out.u[i];
            cost[a] = in.cost[i] / in.capital;
        }
        const Search found = search(xt, out.covariance.matrix, uf, hf, cost, in.cost_multiplier,
                                    in.cap, 1e-6, in.max_iterations);
        out.search_te = found.tracking_error;
        out.passes = found.passes;
        out.pass_capped = found.pass_capped;
        const auto [b, b_index] = b_sigma(uf, out.covariance.matrix, book_eligible(found.book, hf),
                                          in.tau, in.b_sigma_floor);
        out.b_sigma = b;
        out.b_symbol = b_index < 0 ? -1 : static_cast<long>(out.free_rows[static_cast<std::size_t>(b_index)]);
        const Buffered buffered = buffer(found.book, hf, uf, out.covariance.matrix, b);
        out.te_held = buffered.te_held;
        out.a = buffered.a;
        out.traded = buffered.traded;
        out.returned_to_held = buffered.returned_to_held;
        const auto [clipped_book, clipped] = clip_to_cap(buffered.book, uf, in.cap, Mask(f, 1));
        for (std::size_t a = 0; a < f; ++a) {
            const std::size_t i = out.free_rows[a];
            out.search_book[i] = found.book[a];
            book[i] = clipped_book[a];
            out.clipped[i] = clipped[a];
        }
    }
    out.pre_trim = book;

    // Section 6.4: the stored book re-read by the overlay; over a limit, the trim on the free rows.
    Vector part_book(k, 0.0);
    Mask part_free(k, 0);
    for (std::size_t a = 0; a < k; ++a) {
        part_book[a] = book[out.participants[a]];
        part_free[a] = out.free[out.participants[a]];
    }
    auto read_participants = [&](const Vector& quantities) {
        Vector full = book;
        for (std::size_t a = 0; a < k; ++a) full[out.participants[a]] = quantities[a];
        return read(full);
    };
    const Trimmed trimmed = trim(part_book, part_free, read_participants, in.limits, in.trim_max);
    if (!finite_readings(trimmed.readings)) {
        refuse(in, out, "the overlay's re-read of the rounded book is not finite numbers", true);
        return out;
    }
    out.trimmed.assign(n, 0.0);
    for (std::size_t a = 0; a < k; ++a) book[out.participants[a]] = trimmed.book[a];
    for (const auto& [index, term] : trimmed.removed) out.trimmed[out.participants[index]] += 1.0;
    out.trim_capped = trimmed.capped;
    out.book = book;
    out.stored_readings = trimmed.readings;
    out.over_limit = over_limit(out.stored_readings, in.limits);
    double u_max = 0.0;
    for (std::size_t a = 0; a < k; ++a) {
        const std::size_t i = out.participants[a];
        if (book[i] != 0.0) u_max = std::max(u_max, out.u[i]);
    }
    for (const auto& [term, excess] : out.over_limit) {
        out.over_limit_excess_units =
            std::max(out.over_limit_excess_units, u_max > 0.0 ? excess / u_max : kInf);
    }

    // The held rows that keep a reading over on their own, and a held row beyond the cap.
    out.by_hold.assign(n, 0);
    {
        Vector held_only(n, 0.0);
        for (std::size_t i = 0; i < n; ++i) held_only[i] = out.fixed[i] ? in.held[i] : 0.0;
        const Vector x = weights_of(held_only);
        const overlay::Readings held_readings = overlay::readings(x, out.window, sigma_jump);
        for (const auto& [term, excess] : over_limit(held_readings, in.limits)) {
            (void)excess;
            out.by_hold_terms.push_back(term);
            const Mask named = overlay::contributors(x, out.window, sigma_jump, term);
            for (std::size_t a = 0; a < k; ++a) {
                if (named[a]) out.by_hold[out.participants[a]] = 1;
            }
        }
        bool cap_held = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (out.fixed[i] && std::abs(in.held[i]) * out.u[i] > in.cap) {
                cap_held = true;
                out.by_hold[i] = 1;
            }
        }
        if (cap_held) out.by_hold_terms.push_back("CAP");
    }

    // The fills: the sign close to flat, then the move from the closed book to the stored book.
    out.sign_fill.assign(n, 0.0);
    out.rest_fill.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        if (out.sign_closed[i]) out.sign_fill[i] = -in.held[i];
        out.rest_fill[i] = book[i] - closed_held[i];
    }

    // The measures: the delivered scale is the stored gross over the gross of the capped target the
    // overlay read (with the held rows), before m.
    for (std::size_t a = 0; a < k; ++a) {
        const std::size_t i = out.participants[a];
        out.target_gross += std::abs(target_book[i] * out.u[i]);
        out.stored_gross += std::abs(book[i] * out.u[i]);
    }
    out.risk_scale = out.target_gross > 0.0 ? out.stored_gross / out.target_gross : 0.0;
    return out;
}

}  // namespace one_pass
}  // namespace trade_ngin
