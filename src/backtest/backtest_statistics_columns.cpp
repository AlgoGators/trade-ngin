#include "trade_ngin/backtest/backtest_statistics_columns.hpp"

#include <algorithm>

#include "trade_ngin/core/time_utils.hpp"

namespace trade_ngin {

namespace {

/// A percent figure of the live columns as the backtest's fraction.
std::optional<double> as_fraction(const std::optional<double>& percent) {
    return percent ? std::optional<double>(*percent / 100.0) : std::optional<double>();
}

}  // namespace

BacktestStatisticsColumns backtest_statistics_columns(const StatisticsSeries& series,
                                                      double sessions_per_year,
                                                      const std::string& base_date,
                                                      const std::string& through,
                                                      const BacktestStatisticsInputs& inputs) {
    const LiveStatisticsColumns live = live_statistics_columns(
        series, sessions_per_year, base_date, through, inputs.symbol_pnl, inputs.capitals,
        base_date);

    BacktestStatisticsColumns out;
    if (!series.returns_pct.empty()) {
        out.worst_day = *std::min_element(series.returns_pct.begin(), series.returns_pct.end()) /
                        100.0;
    }
    out.worst_day_date = live.worst_day_date;
    out.worst_day_symbol = live.worst_day_symbol;
    out.max_drawdown_sizing = as_fraction(live.max_drawdown_sizing);
    out.volatility_sizing = as_fraction(live.volatility_sizing);
    out.worst_day_sizing = as_fraction(live.worst_day_sizing);
    out.worst_day_sizing_date = live.worst_day_sizing_date;
    out.worst_day_sizing_symbol = live.worst_day_sizing_symbol;
    out.monthly_skew = live.monthly_skew;              // a ratio: no unit
    out.monthly_tail_ratio = live.monthly_tail_ratio;  // a ratio: no unit
    if (live.calendar_year_returns) {
        nlohmann::json years = *live.calendar_year_returns;
        for (auto& [year, cell] : years.items()) {
            (void)year;
            cell["return"] = cell["return"].get<double>() / 100.0;
        }
        out.calendar_year_returns = std::move(years);
    }
    out.losing_years = live.losing_years;
    out.sizing_returns = live.sizing_returns;
    out.complete_months = live.complete_months;
    return out;
}

std::vector<LiveResultsCell> backtest_statistics_cells(const BacktestStatisticsColumns& c) {
    auto number = [](const std::optional<double>& v) {
        return v ? live_results_number(*v) : std::optional<std::string>();
    };
    auto whole = [](const std::optional<int>& v) {
        return v ? std::optional<std::string>(std::to_string(*v)) : std::optional<std::string>();
    };
    return {
        {"worst_day", "numeric", number(c.worst_day)},
        {"worst_day_date", "date", c.worst_day_date},
        {"worst_day_symbol", "text", c.worst_day_symbol},
        {"max_drawdown_sizing", "numeric", number(c.max_drawdown_sizing)},
        {"volatility_sizing", "numeric", number(c.volatility_sizing)},
        {"worst_day_sizing", "numeric", number(c.worst_day_sizing)},
        {"worst_day_sizing_date", "date", c.worst_day_sizing_date},
        {"worst_day_sizing_symbol", "text", c.worst_day_sizing_symbol},
        {"monthly_skew", "numeric", number(c.monthly_skew)},
        {"monthly_tail_ratio", "numeric", number(c.monthly_tail_ratio)},
        {"calendar_year_returns", "jsonb",
         c.calendar_year_returns ? std::optional<std::string>(c.calendar_year_returns->dump())
                                 : std::optional<std::string>()},
        {"losing_years", "integer", whole(c.losing_years)},
        {"total_strategy_fills", "integer", whole(c.total_strategy_fills)},
    };
}

std::vector<LiveResultsCell> roll_split_summary_cells(const std::optional<double>& roll_bar_moves,
                                                      const std::optional<double>& roll_gap_values) {
    std::vector<LiveResultsCell> cells;
    if (roll_bar_moves) cells.push_back({"roll_bar_moves", "numeric", live_results_number(*roll_bar_moves)});
    if (roll_gap_values) cells.push_back({"roll_gap_values", "numeric", live_results_number(*roll_gap_values)});
    return cells;
}

std::string backtest_statistics_log_line(const BacktestStatisticsColumns& c) {
    auto number = [](const std::optional<double>& v) {
        const auto text = v ? live_results_number(*v) : std::optional<std::string>();
        return text ? *text : std::string("null");
    };
    auto whole = [](const std::optional<int>& v) {
        return v ? std::to_string(*v) : std::string("null");
    };
    auto text = [](const std::optional<std::string>& v) { return v ? *v : std::string("null"); };
    return "worst_day=" + number(c.worst_day) + " worst_day_date=" + text(c.worst_day_date) +
           " worst_day_symbol=" + text(c.worst_day_symbol) + " sizing_returns=" +
           std::to_string(c.sizing_returns) + " max_drawdown_sizing=" +
           number(c.max_drawdown_sizing) + " volatility_sizing=" + number(c.volatility_sizing) +
           " worst_day_sizing=" + number(c.worst_day_sizing) + " worst_day_sizing_date=" +
           text(c.worst_day_sizing_date) + " worst_day_sizing_symbol=" +
           text(c.worst_day_sizing_symbol) + " complete_months=" +
           std::to_string(c.complete_months) + " monthly_skew=" + number(c.monthly_skew) +
           " monthly_tail_ratio=" + number(c.monthly_tail_ratio) + " calendar_year_returns=" +
           (c.calendar_year_returns ? c.calendar_year_returns->dump() : std::string("null")) +
           " losing_years=" + whole(c.losing_years) + " total_strategy_fills=" +
           whole(c.total_strategy_fills);
}

std::vector<DatedCapital> stored_sizing_capitals(
    const std::vector<std::pair<Timestamp, double>>& curve,
    const std::vector<std::string>& risk_detail) {
    std::vector<DatedCapital> capitals;
    const size_t rows = std::min(curve.size(), risk_detail.size());
    for (size_t i = 0; i < rows; ++i) {
        if (risk_detail[i].empty()) continue;
        const nlohmann::json detail = nlohmann::json::parse(risk_detail[i], nullptr, false);
        if (!detail.is_object()) continue;
        const auto capital = detail.find("sizing_capital");
        if (capital == detail.end() || !capital->is_number()) continue;
        capitals.push_back({core::format_utc_date(curve[i].first), capital->get<double>()});
    }
    return capitals;
}

}  // namespace trade_ngin
