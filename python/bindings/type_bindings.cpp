#include "bindings.hpp"

#include <pybind11/chrono.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl_bind.h>
#include <pybind11_json/pybind11_json.hpp>

#include "trade_ngin/backtest/backtest_types.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/strategy/types.hpp"

namespace py = pybind11;
using namespace trade_ngin;
using namespace trade_ngin::backtest;

namespace pybind11::detail {

// Custom type caster for Decimal to convert between C++ Decimal and Python's decimal.Decimal
template <>
struct type_caster<trade_ngin::Decimal> {
public:
    PYBIND11_TYPE_CASTER(trade_ngin::Decimal, _("Decimal"));

    bool load(handle src, bool) {
        std::string s = py::str(src);
        value = Decimal(s);
        return true;
    }

    static handle cast(const trade_ngin::Decimal& src, return_value_policy policy, handle parent) {
        py::object DecimalClass = py::module_::import("decimal").attr("Decimal");
        return DecimalClass(src.to_string()).release();
    }
};

}  // namespace pybind11::detail

PYBIND11_MAKE_OPAQUE(std::unordered_map<std::string, Position>);

// Bind types in trade_ngin/core/types.hpp
void bind_core_types(py::module_& m) {
    py::class_<Bar>(m, "Bar")
        .def(py::init<>())
        .def_readwrite("timestamp", &Bar::timestamp)
        .def_readwrite("open", &Bar::open)
        .def_readwrite("high", &Bar::high)
        .def_readwrite("low", &Bar::low)
        .def_readwrite("close", &Bar::close)
        .def_readwrite("volume", &Bar::volume)
        .def_readwrite("symbol", &Bar::symbol);

    py::class_<Position>(m, "Position")
        .def(py::init<>())
        .def_readwrite("symbol", &Position::symbol)
        .def_readwrite("quantity", &Position::quantity)
        .def_readwrite("avg_price", &Position::average_price)
        .def_readwrite("unrealized_pnl", &Position::unrealized_pnl)
        .def_readwrite("realized_pnl", &Position::realized_pnl)
        .def_readwrite("last_update", &Position::last_update);
    // TODO pybind chrono type - since Timestamp is system_clock, it converts automatically to
    // Python datetime

    using PositionMap = std::unordered_map<std::string, Position>;
    py::bind_map<PositionMap>(m, "PositionMap");

    py::class_<ExecutionReport>(m, "ExecutionReport")
        .def(py::init<>())
        .def_readwrite("order_id", &ExecutionReport::order_id)
        .def_readwrite("exec_id", &ExecutionReport::exec_id)
        .def_readwrite("symbol", &ExecutionReport::symbol)
        .def_readwrite("side", &ExecutionReport::side)
        .def_readwrite("quantity", &ExecutionReport::filled_quantity)
        .def_readwrite("price", &ExecutionReport::fill_price)
        .def_readwrite("timestamp", &ExecutionReport::fill_time)
        .def_readwrite("commissions_fees", &ExecutionReport::commissions_fees)
        .def_readwrite("implicit_price_impact", &ExecutionReport::implicit_price_impact)
        .def_readwrite("slippage_market_impact", &ExecutionReport::slippage_market_impact)
        .def_readwrite("total_transaction_costs", &ExecutionReport::total_transaction_costs)
        .def_readwrite("is_partial", &ExecutionReport::is_partial);
}

// Bind types in trade_ngin/backtest/backtest_types.hpp
void bind_backtest_types(py::module_& m) {
    py::class_<BacktestResults>(m, "BacktestResults")
        .def(py::init<>())
        .def("__str__",
             [](const BacktestResults& r) {
                 return "BacktestResults(total_return=" + std::to_string(r.total_return) +
                        ", volatility=" + std::to_string(r.volatility) +
                        ", sharpe_ratio=" + std::to_string(r.sharpe_ratio) +
                        ", sortino_ratio=" + std::to_string(r.sortino_ratio) +
                        ", max_drawdown=" + std::to_string(r.max_drawdown) +
                        ", calmar_ratio=" + std::to_string(r.calmar_ratio) +
                        ", total_trades=" + std::to_string(r.total_trades) +
                        ", win_rate=" + std::to_string(r.win_rate) +
                        ", profit_factor=" + std::to_string(r.profit_factor) +
                        ", avg_win=" + std::to_string(r.avg_win) +
                        ", avg_loss=" + std::to_string(r.avg_loss) +
                        ", max_win=" + std::to_string(r.max_win) +
                        ", max_loss=" + std::to_string(r.max_loss) +
                        ", avg_holding_period=" + std::to_string(r.avg_holding_period) +
                        ", var_95=" + std::to_string(r.var_95) +
                        ", cvar_95=" + std::to_string(r.cvar_95) +
                        ", beta=" + std::to_string(r.beta) +
                        ", correlation=" + std::to_string(r.correlation) +
                        ", downside_volatility=" + std::to_string(r.downside_volatility) + ")";
             })
        .def_readwrite("total_return", &BacktestResults::total_return)
        .def_readwrite("volatility", &BacktestResults::volatility)
        .def_readwrite("sharpe_ratio", &BacktestResults::sharpe_ratio)
        .def_readwrite("sortino_ratio", &BacktestResults::sortino_ratio)
        .def_readwrite("max_drawdown", &BacktestResults::max_drawdown)
        .def_readwrite("calmar_ratio", &BacktestResults::calmar_ratio)
        .def_readwrite("total_trades", &BacktestResults::total_trades)
        .def_readwrite("win_rate", &BacktestResults::win_rate)
        .def_readwrite("profit_factor", &BacktestResults::profit_factor)
        .def_readwrite("avg_win", &BacktestResults::avg_win)
        .def_readwrite("avg_loss", &BacktestResults::avg_loss)
        .def_readwrite("max_win", &BacktestResults::max_win)
        .def_readwrite("max_loss", &BacktestResults::max_loss)
        .def_readwrite("avg_holding_period", &BacktestResults::avg_holding_period)
        .def_readwrite("var_95", &BacktestResults::var_95)
        .def_readwrite("cvar_95", &BacktestResults::cvar_95)
        .def_readwrite("beta", &BacktestResults::beta)
        .def_readwrite("correlation", &BacktestResults::correlation)
        .def_readwrite("downside_volatility", &BacktestResults::downside_volatility);

    // TODO add bindings for other results
    // TODO bind to a dataframe?
}

// Bind types in trade_ngin/strategy/types.hpp
void bind_strategy_types(py::module_& m) {
    //     py::enum_<StrategyState>(m, "StrategyState")
    //         .value("Initialized", StrategyState::INITIALIZED)
    //         .value("Running", StrategyState::RUNNING)
    //         .value("Paused", StrategyState::PAUSED)
    //         .value("Stopped", StrategyState::STOPPED)
    //         .value("Error", StrategyState::ERROR)
    //         .export_values();
    //
    //     py::class_<StrategyMetadata>(m, "StrategyMetadata")
    //         .def(py::init<>())
    //         .def_readwrite("id", &StrategyMetadata::id)
    //         .def_readwrite("name", &StrategyMetadata::name)
    //         .def_readwrite("description", &StrategyMetadata::description)
    //         .def_readwrite("assets", &StrategyMetadata::assets)
    //         .def_readwrite("freqs", &StrategyMetadata::freqs)
    //         .def_readwrite("sharpe_ratio", &StrategyMetadata::sharpe_ratio)
    //         .def_readwrite("sortino_ratio", &StrategyMetadata::sortino_ratio)
    //         .def_readwrite("max_drawdown", &StrategyMetadata::max_drawdown)
    //         .def_readwrite("win_rate", &StrategyMetadata::win_rate);

    py::class_<StrategyConfig>(m, "StrategyConfig")
        .def(py::init<>())
        .def_readwrite("capital_allocation", &StrategyConfig::capital_allocation)
        .def_readwrite("max_leverage", &StrategyConfig::max_leverage)
        .def_readwrite("position_limits", &StrategyConfig::position_limits)
        .def_readwrite("max_drawdown", &StrategyConfig::max_drawdown)
        .def_readwrite("var_limit", &StrategyConfig::var_limit)
        .def_readwrite("correlation_limit", &StrategyConfig::correlation_limit)
        .def_readwrite("trading_params", &StrategyConfig::trading_params)
        .def_readwrite("costs", &StrategyConfig::costs)
        .def_readwrite("asset_classes", &StrategyConfig::asset_classes)
        .def_readwrite("frequencies", &StrategyConfig::frequencies)
        .def_readwrite("version", &StrategyConfig::version)
        .def("to_json", &StrategyConfig::to_json)
        .def("from_json", &StrategyConfig::from_json);

    //     py::class_<StrategyMetrics>(m, "StrategyMetrics")
    //         .def(py::init<>())
    //         .def_readwrite("unrealized_pnl", &StrategyMetrics::unrealized_pnl)
    //         .def_readwrite("realized_pnl", &StrategyMetrics::realized_pnl)
    //         .def_readwrite("total_pnl", &StrategyMetrics::total_pnl)
    //         .def_readwrite("sharpe_ratio", &StrategyMetrics::sharpe_ratio)
    //         .def_readwrite("sortino_ratio", &StrategyMetrics::sortino_ratio)
    //         .def_readwrite("max_drawdown", &StrategyMetrics::max_drawdown)
    //         .def_readwrite("win_rate", &StrategyMetrics::win_rate)
    //         .def_readwrite("profit_factor", &StrategyMetrics::profit_factor)
    //         .def_readwrite("total_trades", &StrategyMetrics::total_trades)
    //         .def_readwrite("avg_trade", &StrategyMetrics::avg_trade)
    //         .def_readwrite("avg_winner", &StrategyMetrics::avg_winner)
    //         .def_readwrite("avg_loser", &StrategyMetrics::avg_loser)
    //         .def_readwrite("max_winner", &StrategyMetrics::max_winner)
    //         .def_readwrite("max_loser", &StrategyMetrics::max_loser)
    //         .def_readwrite("avg_holding_period", &StrategyMetrics::avg_holding_period)
    //         .def_readwrite("turnover", &StrategyMetrics::turnover)
    //         .def_readwrite("volatility", &StrategyMetrics::volatility);
    //
    //     py::enum_<PnLAccountingMethod>(m, "PnLAccountingMethod")
    //         .value("RealizedOnly", PnLAccountingMethod::REALIZED_ONLY)
    //         .value("UnrealizedOnly", PnLAccountingMethod::UNREALIZED_ONLY)
    //         .value("Mixed", PnLAccountingMethod::MIXED)
    //         .export_values();
    //
    //     py::class_<PnLAccounting>(m, "PnLAccounting")
    //         .def(py::init<>())
    //         .def_readwrite("total_realized_pnl", &PnLAccounting::total_realized_pnl)
    //         .def_readwrite("total_unrealized_pnl", &PnLAccounting::total_unrealized_pnl)
    //         .def_readwrite("daily_realized_pnl", &PnLAccounting::daily_realized_pnl)
    //         .def_readwrite("daily_unrealized_pnl", &PnLAccounting::daily_unrealized_pnl)
    //         .def_readwrite("method", &PnLAccounting::method)
    //         .def("get_total_pnl", &PnLAccounting::get_total_pnl)
    //         .def("get_daily_pnl", &PnLAccounting::get_daily_pnl)
    //         .def("reset_daily", &PnLAccounting::reset_daily)
    //         .def("add_realized_pnl", &PnLAccounting::add_realized_pnl)
    //         .def("add_unrealized_pnl", &PnLAccounting::add_unrealized_pnl)
    //         .def("set_unrealized_pnl", &PnLAccounting::set_unrealized_pnl);
}

// Bind types in trade_ngin/core/error.hpp
// void bind_error_types(py::module_& m) {
//     py::class_<TradeError>(m, "TradeError")
//         .def(py::init<ErrorCode, std::string, std::string>(), py::arg("code"),
//         py::arg("message"),
//              py::arg("component") = "")
//         .def_property_readonly("code", &TradeError::code)
//         .def_property_readonly("message", &TradeError::what)
//         .def_property_readonly("component", &TradeError::component)
//         .def("to_string", &TradeError::to_string);
//
//     // Due to the use of templates, we only bind Result<void> here. Other Result<T> types can be
//     // added as needed.
//     py::class_<Result<void>>(m, "ResultVoid")
//         .def(py::init<>())
//         .def_property_readonly("is_ok", &Result<void>::is_ok)
//         .def_property_readonly("error", &Result<void>::error);
// }

// Bind example from existing trend following strategy in trade_ngin/strategy/trend_following.hpp
