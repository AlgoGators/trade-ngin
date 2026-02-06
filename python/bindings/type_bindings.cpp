#include <pybind11/pybind11.h>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/strategy/types.hpp"

namespace py = pybind11;

void bind_core_types(py::module_& m) {}

void bind_error_types(py::module_& m) {
    py::class_<trade_ngin::TradeError>(m, "TradeError")
        .def(py::init<trade_ngin::ErrorCode, std::string, std::string>(), py::arg("code"),
             py::arg("message"), py::arg("component") = "")
        .def_property_readonly("code", &trade_ngin::TradeError::code)
        .def_property_readonly("message", &trade_ngin::TradeError::what)
        .def_property_readonly("component", &trade_ngin::TradeError::component)
        .def("to_string", &trade_ngin::TradeError::to_string);

    py::class_<trade_ngin::Result<void>>(m, "ResultVoid")
        .def(py::init<>())
        .def_property_readonly("is_ok", &trade_ngin::Result<void>::is_ok)
        .def_property_readonly("error", &trade_ngin::Result<void>::error);
}

void bind_strategy_types(py::module_& m) {
    py::class_<trade_ngin::StrategyConfig>(m, "StrategyConfig")
        .def(py::init<>())
        .def_readwrite("capital_allocation", &trade_ngin::StrategyConfig::capital_allocation)
        .def_readwrite("max_leverage", &trade_ngin::StrategyConfig::max_leverage)
        .def_readwrite("position_limits", &trade_ngin::StrategyConfig::position_limits)
        .def_readwrite("max_drawdown", &trade_ngin::StrategyConfig::max_drawdown)
        .def_readwrite("var_limit", &trade_ngin::StrategyConfig::var_limit)
        .def_readwrite("correlation_limit", &trade_ngin::StrategyConfig::correlation_limit)
        .def_readwrite("trading_params", &trade_ngin::StrategyConfig::trading_params)
        .def_readwrite("costs", &trade_ngin::StrategyConfig::costs)
        .def_readwrite("asset_classes", &trade_ngin::StrategyConfig::asset_classes)
        .def_readwrite("frequencies", &trade_ngin::StrategyConfig::frequencies)
        .def_readwrite("version", &trade_ngin::StrategyConfig::version);
}
