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

                    // Hold the Python wrapper alive explicitly
                    py::object py_instance = py_class();
                    // Inject state
                    py_instance.attr("initialize_from_context")(strategy_id,
                                                                ctx.base_strategy_config, ctx.db);

                    // Extract raw C++ pointer while the Python wrapper is still alive.
                    BaseStrategy* raw = py_instance.cast<BaseStrategy*>();

                    // Keep the Python wrapper alive for the entire lifetime of the returned
                    // shared_ptr. py::get_override works by looking up the Python wrapper in
                    // pybind11's registered_instances map; if the wrapper is GC'd the entry is
                    // removed and every subsequent get_override call returns empty. The aliasing
                    // constructor shares ownership with py_owner (keeping the py::object alive)
                    // while the pointer seen by callers is the BaseStrategy*.  The GIL must be
                    // held when the py::object is finally released.
                    auto py_owner = std::shared_ptr<py::object>(
                        new py::object(std::move(py_instance)), [](py::object* obj) {
                            py::gil_scoped_acquire destroy_gil;
                            delete obj;
                        });
                    return std::shared_ptr<BaseStrategy>(py_owner, raw);
                });
        });
}
