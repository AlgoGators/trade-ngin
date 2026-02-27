#include "bindings.hpp"

#include <pybind11/pybind11.h>

#include "trade_ngin/api/backtest_api.hpp"

// TODO temp
#include "trade_ngin/strategy/trend_following.hpp"

namespace py = pybind11;
using namespace trade_ngin;
using namespace trade_ngin::api;

void bind_backtest_api(py::module_& m) {
    py::class_<BacktestRunner>(m, "BacktestRunner")
        .def(py::init<>())
        .def("run_backtest", &BacktestRunner::run_backtest)
        // TODO VERY TEMPORARY STOPGAP MEASURE
        // .def("register_strategy", [](BacktestRunner& self, const std::string& strategy_id,
        //                              py::object py_class, py::dict py_config) {
        .def("register_strategy", [](BacktestRunner& self, const std::string& strategy_id,
                                     py::object py_class, TrendFollowingConfig py_config) {
            self.register_strategy(
                strategy_id,
                [py_class, py_config, strategy_id](
                    const StrategyContext& ctx,
                    const nlohmann::json& strategy_def) -> std::shared_ptr<BaseStrategy> {
                    py::gil_scoped_acquire gil;
                    py::object py_instance = py_class(strategy_id, ctx.base_strategy_config,
                                                      py_config, ctx.db, ctx.registry);

                    return py_instance.cast<std::shared_ptr<BaseStrategy>>();
                });
        });
}
