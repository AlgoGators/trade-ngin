#include "bindings.hpp"

#include <pybind11/pybind11.h>

#include "trade_ngin/api/backtest_api.hpp"

namespace py = pybind11;
using namespace trade_ngin;
using namespace trade_ngin::api;

void bind_backtest_api(py::module_& m) {
    py::class_<BacktestRunner>(m, "BacktestRunner")
        .def(py::init<>())
        .def("run_backtest", &BacktestRunner::run_backtest)
        .def("register_strategy", &BacktestRunner::register_strategy, py::arg("strategy_id"),
             py::arg("config"), py::arg("db"));
    // .def("register_strategy",
    //      py::overload_cast<const std::string&, std::shared_ptr<BaseStrategy>>(
    //          &BacktestRunner::register_strategy),
    //      py::arg("strategy_id"), py::arg("strategy"))
    // .def("register_strategy",
    //      py::overload_cast<const std::string&, const StrategyConfig&,
    //                        std::shared_ptr<PostgresDatabase>>(
    //          &BacktestRunner::register_strategy),
    //      py::arg("strategy_id"), py::arg("config"), py::arg("db"));
}
