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
        .def("register_strategy", [](BacktestRunner& self, const std::string& strategy_id,
                                     py::object py_class, py::dict py_config) {
            self.register_strategy(
                strategy_id,
                [py_class, py_config, strategy_id](
                    const StrategyContext& ctx,
                    const nlohmann::json& strategy_def) -> std::shared_ptr<BaseStrategy> {
                    py::gil_scoped_acquire gil;

                    // Construct Python object with no args
                    auto instance = py_class().cast<std::shared_ptr<BaseStrategy>>();

                    // Inject state
                    instance->initialize_from_context(strategy_id, ctx.base_strategy_config,
                                                      ctx.db);
                    return instance;
                });
        });
}
