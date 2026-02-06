#include <pybind11/pybind11.h>
#include <pybind11_json/pybind11_json.hpp>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/strategy/types.hpp"

namespace py = pybind11;
using namespace trade_ngin;

// Bind types in trade_ngin/core/types.hpp
void bind_core_types(py::module_& m) {}

// Bind types in trade_ngin/core/error.hpp
// Due to the use of templates, we only bind Result<void> here. Other Result<T> types can be added
// as needed.
void bind_error_types(py::module_& m) {
    py::class_<TradeError>(m, "TradeError")
        .def(py::init<ErrorCode, std::string, std::string>(), py::arg("code"), py::arg("message"),
             py::arg("component") = "")
        .def_property_readonly("code", &TradeError::code)
        .def_property_readonly("message", &TradeError::what)
        .def_property_readonly("component", &TradeError::component)
        .def("to_string", &TradeError::to_string);

    py::class_<Result<void>>(m, "ResultVoid")
        .def(py::init<>())
        .def_property_readonly("is_ok", &Result<void>::is_ok)
        .def_property_readonly("error", &Result<void>::error);
}

// Bind types in trade_ngin/strategy/types.hpp
void bind_strategy_types(py::module_& m) {
    py::enum_<StrategyState>(m, "StrategyState")
        .value("Initialized", StrategyState::INITIALIZED)
        .value("Running", StrategyState::RUNNING)
        .value("Paused", StrategyState::PAUSED)
        .value("Stopped", StrategyState::STOPPED)
        .value("Error", StrategyState::ERROR)
        .export_values();

    py::class_<StrategyMetadata>(m, "StrategyMetadata")
        .def(py::init<>())
        .def_readwrite("id", &StrategyMetadata::id)
        .def_readwrite("name", &StrategyMetadata::name)
        .def_readwrite("description", &StrategyMetadata::description)
        .def_readwrite("assets", &StrategyMetadata::assets)
        .def_readwrite("freqs", &StrategyMetadata::freqs)
        .def_readwrite("sharpe_ratio", &StrategyMetadata::sharpe_ratio)
        .def_readwrite("sortino_ratio", &StrategyMetadata::sortino_ratio)
        .def_readwrite("max_drawdown", &StrategyMetadata::max_drawdown)
        .def_readwrite("win_rate", &StrategyMetadata::win_rate);

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

    py::class_<StrategyMetrics>(m, "StrategyMetrics")
        .def(py::init<>())
        .def_readwrite("unrealized_pnl", &StrategyMetrics::unrealized_pnl)
        .def_readwrite("realized_pnl", &StrategyMetrics::realized_pnl)
        .def_readwrite("total_pnl", &StrategyMetrics::total_pnl)
        .def_readwrite("sharpe_ratio", &StrategyMetrics::sharpe_ratio)
        .def_readwrite("sortino_ratio", &StrategyMetrics::sortino_ratio)
        .def_readwrite("max_drawdown", &StrategyMetrics::max_drawdown)
        .def_readwrite("win_rate", &StrategyMetrics::win_rate)
        .def_readwrite("profit_factor", &StrategyMetrics::profit_factor)
        .def_readwrite("total_trades", &StrategyMetrics::total_trades)
        .def_readwrite("avg_trade", &StrategyMetrics::avg_trade)
        .def_readwrite("avg_winner", &StrategyMetrics::avg_winner)
        .def_readwrite("avg_loser", &StrategyMetrics::avg_loser)
        .def_readwrite("max_winner", &StrategyMetrics::max_winner)
        .def_readwrite("max_loser", &StrategyMetrics::max_loser)
        .def_readwrite("avg_holding_period", &StrategyMetrics::avg_holding_period)
        .def_readwrite("turnover", &StrategyMetrics::turnover)
        .def_readwrite("volatility", &StrategyMetrics::volatility);

    py::enum_<PnLAccountingMethod>(m, "PnLAccountingMethod")
        .value("RealizedOnly", PnLAccountingMethod::REALIZED_ONLY)
        .value("UnrealizedOnly", PnLAccountingMethod::UNREALIZED_ONLY)
        .value("Mixed", PnLAccountingMethod::MIXED)
        .export_values();

    py::class_<PnLAccounting>(m, "PnLAccounting")
        .def(py::init<>())
        .def_readwrite("total_realized_pnl", &PnLAccounting::total_realized_pnl)
        .def_readwrite("total_unrealized_pnl", &PnLAccounting::total_unrealized_pnl)
        .def_readwrite("daily_realized_pnl", &PnLAccounting::daily_realized_pnl)
        .def_readwrite("daily_unrealized_pnl", &PnLAccounting::daily_unrealized_pnl)
        .def_readwrite("method", &PnLAccounting::method)
        .def("get_total_pnl", &PnLAccounting::get_total_pnl)
        .def("get_daily_pnl", &PnLAccounting::get_daily_pnl)
        .def("reset_daily", &PnLAccounting::reset_daily)
        .def("add_realized_pnl", &PnLAccounting::add_realized_pnl)
        .def("add_unrealized_pnl", &PnLAccounting::add_unrealized_pnl)
        .def("set_unrealized_pnl", &PnLAccounting::set_unrealized_pnl);
}
