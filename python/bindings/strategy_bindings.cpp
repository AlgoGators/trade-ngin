#include "bindings.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "trade_ngin/strategy/base_strategy.hpp"

namespace py = pybind11;
using namespace trade_ngin;

struct PyBaseStrategy : public BaseStrategy {
    using BaseStrategy::BaseStrategy;  // Inherit constructors

    Result<void> on_data(const std::vector<Bar>& data) override {
        PYBIND11_OVERRIDE(Result<void>, BaseStrategy, on_data, data);
    };

    // Optional override for custom logging and logic over internal PnL logic
    Result<void> on_execution(const ExecutionReport& report) override {
        PYBIND11_OVERRIDE(Result<void>, BaseStrategy, on_execution, report);
    };

    // TODO probably not the best idea to expose this directly - the ideal interface would be to
    // expose a method that takes in everything you need for a position update and then calls
    // update_position internally after doing some validation and conversion. But for now this is
    // fine since it's just a simple wrapper around the C++ method and we can add more validation in
    // the future if needed.
    Result<void> update_position(const std::string& symbol, const Position& position) override {
        PYBIND11_OVERRIDE(Result<void>, BaseStrategy, update_position, symbol, position);
    };
};

void bind_base_strategy(py::module_& m) {
    py::class_<BaseStrategy, PyBaseStrategy, std::shared_ptr<BaseStrategy>>(m, "BaseStrategy")
        .def(py::init<std::string, StrategyConfig, std::shared_ptr<PostgresDatabase>>(),
             py::arg("id"), py::arg("config"), py::arg("db"))
        .def("on_data", &BaseStrategy::on_data)
        .def("on_execution", &BaseStrategy::on_execution)
        .def("update_position", &BaseStrategy::update_position, py::arg("symbol"),
             py::arg("position"));
    // TODO according to the strategy README, I need to expose .initialize()?
    // We can expose more that we need as we go on
}
