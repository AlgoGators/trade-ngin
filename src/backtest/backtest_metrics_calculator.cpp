#include "trade_ngin/backtest/backtest_metrics_calculator.hpp"
#include "trade_ngin/backtest/backtest_engine.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace trade_ngin {

// ========== Return Calculations ==========

double BacktestMetricsCalculator::calculate_total_return(double start_value, double end_value) const {
    if (start_value <= 0.0) {
        return 0.0;
    }
    return (end_value - start_value) / start_value;
}

double BacktestMetricsCalculator::calculate_annualized_return(double total_return, int trading_days) const {
    if (trading_days <= 0) {
        return 0.0;
    }
    // Simple annualization: scale by 252/trading_days
    double annualization_factor = 252.0 / static_cast<double>(trading_days);
    return total_return * annualization_factor;
}

std::vector<double> BacktestMetricsCalculator::calculate_returns_from_equity(
    const std::vector<std::pair<Timestamp, double>>& equity_curve) const {
    std::vector<double> returns;
    if (equity_curve.size() < 2) {
        return returns;
    }

    returns.reserve(equity_curve.size() - 1);
    for (size_t i = 1; i < equity_curve.size(); ++i) {
        if (equity_curve[i - 1].second > 0.0) {
            double ret = (equity_curve[i].second - equity_curve[i - 1].second) /
                        equity_curve[i - 1].second;
            returns.push_back(ret);
        }
    }
    return returns;
}

// ========== Risk-Adjusted Return Metrics ==========

double BacktestMetricsCalculator::calculate_sharpe_ratio(
    const std::vector<double>& returns,
    int trading_days,
    double risk_free_rate) const {
    if (returns.empty() || trading_days <= 0) {
        return 0.0;
    }

    double mean_return = calculate_mean(returns);
    double volatility = calculate_volatility(returns);

    if (volatility <= 0.0) {
        return 0.0;
    }

    // Annualize mean return using actual trading days
    double annualization_factor = 252.0 / static_cast<double>(trading_days);
    double annualized_return = mean_return * annualization_factor;

    // Sharpe = (annualized return - risk free rate) / annualized volatility
    return (annualized_return - risk_free_rate) / volatility;
}

double BacktestMetricsCalculator::calculate_sortino_ratio(
    const std::vector<double>& returns,
    int trading_days,
    double minimum_acceptable_return) const {
    if (returns.empty() || trading_days <= 0) {
        return 0.0;
    }

    double mean_return = calculate_mean(returns);
    double downside_vol = calculate_downside_volatility(returns, minimum_acceptable_return);

    if (downside_vol <= 0.0) {
        // No negative returns - cap at reasonable value
        double annualization_factor = 252.0 / static_cast<double>(trading_days);
        return (mean_return * annualization_factor) >= 0 ? 999.0 : 0.0;
    }

    double annualization_factor = 252.0 / static_cast<double>(trading_days);
    return (mean_return * annualization_factor - minimum_acceptable_return) / downside_vol;
}

double BacktestMetricsCalculator::calculate_calmar_ratio(double total_return, double max_drawdown) const {
    if (max_drawdown <= 0.0) {
        return total_return >= 0 ? 999.0 : 0.0;
    }
    return total_return / max_drawdown;
}

// ========== Volatility Metrics ==========

double BacktestMetricsCalculator::calculate_volatility(const std::vector<double>& returns) const {
    if (returns.empty()) {
        return 0.0;
    }

    double mean_return = calculate_mean(returns);
    double sq_sum = std::inner_product(returns.begin(), returns.end(), returns.begin(), 0.0);
    double variance = sq_sum / returns.size() - mean_return * mean_return;

    // Annualize using sqrt(252) for daily volatility
    return std::sqrt(variance) * std::sqrt(252.0);
}

double BacktestMetricsCalculator::calculate_downside_volatility(
    const std::vector<double>& returns,
    double target) const {
    double downside_sum = 0.0;
    int downside_count = 0;

    for (double ret : returns) {
        if (ret < target) {
            double deviation = ret - target;
            downside_sum += deviation * deviation;
            downside_count++;
        }
    }

    if (downside_count <= 0) {
        return 0.0;
    }

    // Annualize using sqrt(252)
    return std::sqrt(downside_sum / downside_count) * std::sqrt(252.0);
}

// ========== Drawdown Metrics ==========

std::vector<std::pair<Timestamp, double>> BacktestMetricsCalculator::calculate_drawdowns(
    const std::vector<std::pair<Timestamp, double>>& equity_curve) const {
    std::vector<std::pair<Timestamp, double>> drawdowns;
    drawdowns.reserve(equity_curve.size());

    if (equity_curve.empty()) {
        return drawdowns;
    }

    double peak = equity_curve[0].second;

    for (const auto& [timestamp, equity] : equity_curve) {
        peak = std::max(peak, equity);
        double drawdown = equity < peak ? (peak - equity) / peak : 0.0;
        drawdowns.emplace_back(timestamp, drawdown);
    }

    return drawdowns;
}

double BacktestMetricsCalculator::calculate_max_drawdown(
    const std::vector<std::pair<Timestamp, double>>& equity_curve) const {
    auto drawdowns = calculate_drawdowns(equity_curve);
    if (drawdowns.empty()) {
        return 0.0;
    }

    auto max_it = std::max_element(drawdowns.begin(), drawdowns.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });
    return max_it->second;
}

// ========== Risk Metrics ==========

double BacktestMetricsCalculator::calculate_var_95(const std::vector<double>& returns) const {
    if (returns.empty()) {
        return 0.0;
    }

    std::vector<double> sorted_returns = returns;
    std::sort(sorted_returns.begin(), sorted_returns.end());

    size_t var_index = static_cast<size_t>(returns.size() * 0.05);
    if (var_index >= sorted_returns.size()) {
        var_index = sorted_returns.size() - 1;
    }

    return -sorted_returns[var_index];
}

double BacktestMetricsCalculator::calculate_cvar_95(const std::vector<double>& returns) const {
    if (returns.empty()) {
        return 0.0;
    }

    std::vector<double> sorted_returns = returns;
    std::sort(sorted_returns.begin(), sorted_returns.end());

    size_t var_index = static_cast<size_t>(returns.size() * 0.05);
    if (var_index == 0) {
        var_index = 1;  // Need at least one value
    }

    double cvar_sum = 0.0;
    for (size_t i = 0; i < var_index; ++i) {
        cvar_sum += sorted_returns[i];
    }

    return -cvar_sum / var_index;
}

std::unordered_map<std::string, double> BacktestMetricsCalculator::calculate_risk_metrics(
    const std::vector<double>& returns,
    int /* trading_days */) const {
    std::unordered_map<std::string, double> metrics;

    if (returns.empty()) {
        return metrics;
    }

    metrics["var_95"] = calculate_var_95(returns);
    metrics["cvar_95"] = calculate_cvar_95(returns);
    metrics["downside_volatility"] = calculate_downside_volatility(returns, 0.0);

    return metrics;
}

// ========== Trade Statistics ==========

BacktestMetricsCalculator::TradeStatistics BacktestMetricsCalculator::calculate_trade_statistics(
    const std::vector<ExecutionReport>& executions) const {
    TradeStatistics stats;

    std::unordered_map<std::string, double> positions;   // symbol -> net position
    std::unordered_map<std::string, double> avg_prices;  // symbol -> average entry price
    std::map<std::string, Timestamp> open_times;         // symbol -> first trade time
    std::vector<double> holding_periods;

    for (const auto& exec : executions) {
        const std::string& symbol = exec.symbol;
        double fill_price = static_cast<double>(exec.fill_price);
        double quantity = static_cast<double>(exec.filled_quantity);
        double commission = static_cast<double>(exec.commission);

        // Adjust quantity based on side
        double signed_qty = (exec.side == Side::BUY) ? quantity : -quantity;

        double current_pos = positions[symbol];
        double trade_pnl = -commission;

        if (current_pos == 0.0) {
            // Opening new position
            positions[symbol] = signed_qty;
            avg_prices[symbol] = fill_price;
            open_times[symbol] = exec.fill_time;
        } else if ((current_pos > 0 && signed_qty > 0) || (current_pos < 0 && signed_qty < 0)) {
            // Adding to existing position
            double total_value = current_pos * avg_prices[symbol] + signed_qty * fill_price;
            positions[symbol] = current_pos + signed_qty;
            if (positions[symbol] != 0.0) {
                avg_prices[symbol] = total_value / positions[symbol];
            }
        } else {
            // Reducing or closing position - realize P&L
            double close_qty = std::min(std::abs(signed_qty), std::abs(current_pos));
            trade_pnl += close_qty * (fill_price - avg_prices[symbol]) *
                        (current_pos > 0 ? 1.0 : -1.0);

            positions[symbol] = current_pos + signed_qty;
        }

        // Check if this is a position-closing trade
        bool is_closing_trade = std::abs(signed_qty) > 1e-6 && current_pos != 0.0 &&
            ((current_pos > 0 && signed_qty < 0) || (current_pos < 0 && signed_qty > 0));

        if (is_closing_trade) {
            stats.actual_trades.push_back(exec);

            if (trade_pnl > 0) {
                stats.total_profit += trade_pnl;
                stats.winning_trades++;
                stats.max_win = std::max(stats.max_win, trade_pnl);
            } else {
                stats.total_loss -= trade_pnl;  // total_loss is positive
                stats.max_loss = std::max(stats.max_loss, -trade_pnl);
            }

            // Calculate holding period
            auto it = open_times.find(symbol);
            if (it != open_times.end()) {
                auto duration = std::chrono::duration_cast<std::chrono::hours>(
                    exec.fill_time - it->second);
                double hours = static_cast<double>(duration.count());
                if (hours > 0) {
                    holding_periods.push_back(hours / 24.0);
                }
                open_times[symbol] = exec.fill_time;
            }
        }
    }

    stats.total_trades = static_cast<int>(stats.actual_trades.size());

    if (stats.total_trades > 0) {
        stats.win_rate = static_cast<double>(stats.winning_trades) / stats.total_trades;
        stats.avg_win = stats.winning_trades > 0 ?
            stats.total_profit / stats.winning_trades : 0.0;
        int losing_trades = stats.total_trades - stats.winning_trades;
        stats.avg_loss = losing_trades > 0 ?
            stats.total_loss / losing_trades : 0.0;
    }

    if (stats.total_loss > 0) {
        stats.profit_factor = stats.total_profit / stats.total_loss;
    } else if (stats.total_trades > 0 && stats.total_profit > 0) {
        stats.profit_factor = 999.0;
    }

    if (!holding_periods.empty()) {
        stats.avg_holding_period = std::accumulate(holding_periods.begin(),
            holding_periods.end(), 0.0) / holding_periods.size();
    }

    return stats;
}

// ========== Per-Symbol Analysis ==========

std::map<std::string, double> BacktestMetricsCalculator::calculate_symbol_pnl(
    const std::vector<ExecutionReport>& executions) const {
    std::unordered_map<std::string, double> positions;
    std::unordered_map<std::string, double> avg_prices;
    std::map<std::string, double> symbol_pnl_map;

    for (const auto& exec : executions) {
        const std::string& symbol = exec.symbol;
        double fill_price = static_cast<double>(exec.fill_price);
        double quantity = static_cast<double>(exec.filled_quantity);
        double commission = static_cast<double>(exec.commission);

        double signed_qty = (exec.side == Side::BUY) ? quantity : -quantity;

        double current_pos = positions[symbol];
        double trade_pnl = -commission;

        if (current_pos == 0.0) {
            positions[symbol] = signed_qty;
            avg_prices[symbol] = fill_price;
        } else if ((current_pos > 0 && signed_qty > 0) || (current_pos < 0 && signed_qty < 0)) {
            double total_value = current_pos * avg_prices[symbol] + signed_qty * fill_price;
            positions[symbol] = current_pos + signed_qty;
            if (positions[symbol] != 0.0) {
                avg_prices[symbol] = total_value / positions[symbol];
            }
        } else {
            double close_qty = std::min(std::abs(signed_qty), std::abs(current_pos));
            trade_pnl += close_qty * (fill_price - avg_prices[symbol]) *
                        (current_pos > 0 ? 1.0 : -1.0);
            positions[symbol] = current_pos + signed_qty;
        }

        symbol_pnl_map[symbol] += trade_pnl;
    }

    return symbol_pnl_map;
}

std::unordered_map<std::string, double> BacktestMetricsCalculator::calculate_monthly_returns(
    const std::vector<std::pair<Timestamp, double>>& equity_curve) const {
    std::unordered_map<std::string, double> monthly_returns;

    for (size_t i = 1; i < equity_curve.size(); ++i) {
        auto time_t = std::chrono::system_clock::to_time_t(equity_curve[i].first);
        std::tm tm;
        core::safe_localtime(&time_t, &tm);

        std::ostringstream month_key;
        month_key << std::setw(4) << (tm.tm_year + 1900) << "-"
                  << std::setw(2) << std::setfill('0') << (tm.tm_mon + 1);

        double period_return = (equity_curve[i].second - equity_curve[i - 1].second) /
                              equity_curve[i - 1].second;
        monthly_returns[month_key.str()] += period_return;
    }

    return monthly_returns;
}

// ========== Beta and Correlation ==========

std::pair<double, double> BacktestMetricsCalculator::calculate_beta_correlation(
    const std::vector<double>& returns) const {
    if (returns.size() <= 1) {
        return {0.0, 0.0};
    }

    double mean_return = calculate_mean(returns);

    // Simplified self-correlation calculation
    double covariance = 0.0;
    double variance_benchmark = 0.0;
    double variance_strategy = 0.0;

    for (size_t i = 1; i < returns.size(); ++i) {
        double prev_return = returns[i - 1];
        double curr_return = returns[i];
        covariance += (prev_return - mean_return) * (curr_return - mean_return);
        variance_benchmark += (prev_return - mean_return) * (prev_return - mean_return);
        variance_strategy += (curr_return - mean_return) * (curr_return - mean_return);
    }

    double beta = 0.0;
    double correlation = 0.0;

    if (variance_benchmark > 0) {
        beta = covariance / variance_benchmark;
        correlation = covariance / std::sqrt(variance_benchmark * variance_strategy);
    }

    return {beta, correlation};
}

// ========== Composite Calculation ==========

backtest::BacktestResults BacktestMetricsCalculator::calculate_all_metrics(
    const std::vector<std::pair<Timestamp, double>>& equity_curve,
    const std::vector<ExecutionReport>& executions,
    int warmup_days) const {
    backtest::BacktestResults results;

    if (equity_curve.empty()) {
        return results;
    }

    // Filter warmup period
    auto filtered_curve = filter_warmup_period(equity_curve, warmup_days);
    if (filtered_curve.empty()) {
        return results;
    }

    // Calculate returns from filtered data
    auto returns = calculate_returns_from_equity(filtered_curve);

    // Basic performance metrics
    results.total_return = calculate_total_return(
        filtered_curve.front().second, filtered_curve.back().second);

    int actual_trading_days = static_cast<int>(filtered_curve.size()) - 1;
    if (actual_trading_days <= 0) {
        actual_trading_days = 1;
    }

    // Volatility metrics
    results.volatility = calculate_volatility(returns);

    // Risk-adjusted metrics
    results.sharpe_ratio = calculate_sharpe_ratio(returns, actual_trading_days);
    results.sortino_ratio = calculate_sortino_ratio(returns, actual_trading_days);

    // Drawdown metrics
    auto drawdowns = calculate_drawdowns(filtered_curve);
    if (!drawdowns.empty()) {
        results.max_drawdown = std::max_element(drawdowns.begin(), drawdowns.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; })->second;
        results.drawdown_curve = drawdowns;
    }

    // Calmar ratio
    results.calmar_ratio = calculate_calmar_ratio(results.total_return, results.max_drawdown);

    // Risk metrics
    auto risk_metrics = calculate_risk_metrics(returns, actual_trading_days);
    results.var_95 = risk_metrics["var_95"];
    results.cvar_95 = risk_metrics["cvar_95"];
    results.downside_volatility = risk_metrics["downside_volatility"];

    // Beta and correlation
    auto [beta, correlation] = calculate_beta_correlation(returns);
    results.beta = beta;
    results.correlation = correlation;

    // Trade statistics
    auto trade_stats = calculate_trade_statistics(executions);
    results.total_trades = trade_stats.total_trades;
    results.win_rate = trade_stats.win_rate;
    results.profit_factor = trade_stats.profit_factor;
    results.avg_win = trade_stats.avg_win;
    results.avg_loss = trade_stats.avg_loss;
    results.max_win = trade_stats.max_win;
    results.max_loss = trade_stats.max_loss;
    results.avg_holding_period = trade_stats.avg_holding_period;
    results.actual_trades = trade_stats.actual_trades;

    // Per-symbol P&L
    auto symbol_pnl = calculate_symbol_pnl(executions);
    for (const auto& [symbol, pnl] : symbol_pnl) {
        results.symbol_pnl[symbol] = pnl;
    }

    // Monthly returns
    results.monthly_returns = calculate_monthly_returns(equity_curve);

    // Store warmup days
    results.warmup_days = warmup_days;

    return results;
}

// ========== Helper Methods ==========

double BacktestMetricsCalculator::calculate_mean(const std::vector<double>& values) const {
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double BacktestMetricsCalculator::calculate_std_dev(const std::vector<double>& values, double mean) const {
    if (values.empty()) {
        return 0.0;
    }

    double sq_sum = 0.0;
    for (double val : values) {
        sq_sum += (val - mean) * (val - mean);
    }
    return std::sqrt(sq_sum / values.size());
}

std::vector<std::pair<Timestamp, double>> BacktestMetricsCalculator::filter_warmup_period(
    const std::vector<std::pair<Timestamp, double>>& equity_curve,
    int warmup_days) const {
    if (warmup_days <= 0 || equity_curve.size() <= static_cast<size_t>(warmup_days)) {
        return equity_curve;
    }

    return std::vector<std::pair<Timestamp, double>>(
        equity_curve.begin() + warmup_days, equity_curve.end());
}

} // namespace trade_ngin
