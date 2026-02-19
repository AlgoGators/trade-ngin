#include "bindings.hpp"

#include <pybind11/pybind11.h>

#include "trade_ngin/strategy/base_strategy.hpp"

namespace py = pybind11;
using namespace trade_ngin;

struct PyBaseStrategy : public trade_ngin::BaseStrategy {
    using trade_ngin::BaseStrategy::BaseStrategy;  // Inherit constructors

    // The only required override
    Result<void> on_data(const std::vector<Bar>& data) override {
        PYBIND11_OVERRIDE(Result<void>, BaseStrategy, on_data, data);
    };
    // Optional override for custom logging and logic over internal PnL logic
    Result<void> on_execution(const ExecutionReport& report) override {
        PYBIND11_OVERRIDE(Result<void>, BaseStrategy, on_execution, report);
    };
};

void bind_base_strategy(py::module_& m) {
    py::class_<BaseStrategy, PyBaseStrategy, std::shared_ptr<BaseStrategy>>(m, "BaseStrategy")
        .def(py::init<std::string, StrategyConfig, std::shared_ptr<PostgresDatabase>>(),
             py::arg("id"), py::arg("config"), py::arg("db"))
        .def("on_data", &BaseStrategy::on_data)
        .def("on_execution", &BaseStrategy::on_execution);
}
