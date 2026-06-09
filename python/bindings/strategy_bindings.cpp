#include "bindings.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <unordered_map>
#include <vector>

#include "trade_ngin/strategy/base_strategy.hpp"

namespace py = pybind11;
using namespace trade_ngin;

// TODO allow conversion from Result<void> somewhere so we don't need manual control of override for
// each function
struct PyBaseStrategy : public BaseStrategy {
    using BaseStrategy::BaseStrategy;  // Inherit constructors

    Result<void> on_data(const std::vector<Bar>& data) override {
        py::gil_scoped_acquire gil;

        py::function override = py::get_override(static_cast<const BaseStrategy*>(this), "on_data");

        if (!override) {
            WARN("No Python override found for on_data in Strategy " + id_ +
                 ", using base implementation");
            return BaseStrategy::on_data(data);
        }

        try {
            py::object result = override(data);

            if (result.is_none()) {
                INFO("Python on_data override returned None, treating as success");
                return Result<void>();
            }

            return result.cast<Result<void>>();
        } catch (const py::error_already_set& e) {
            return make_error<void>(ErrorCode::NOT_INITIALIZED,
                                    std::string("Python on_data override failed: ") + e.what(),
                                    "BaseStrategy");
        }
    }

    // Optional override for custom logging and logic over internal PnL logic
    Result<void> on_execution(const ExecutionReport& report) override {
        PYBIND11_OVERRIDE(Result<void>, BaseStrategy, on_execution, report);
    };

    // TODO custom override to tell C++ part that the strategy has been initialized from Python side
    // "Initialization" that would normally be done in the constructor but we're unable to do
    // that here so we do it in the initialize method - maybe can use write new constructors later
    Result<void> initialize() override {
        py::gil_scoped_acquire gil;

        // std::cout << py::str(py::cast(this).get_type()) << std::endl;
        DEBUG("Type of self in Python initialize override: " +
              std::string(py::str(py::type::of(py::cast(this)))));

        // Always run base initialization
        auto base_result = BaseStrategy::initialize();
        if (base_result.is_error()) {
            return base_result;
        }

        INFO("Base initialization successful for Python Strategy " + id_);

        // Required C++ setup
        Logger::register_component("Python Strategy");

        metadata_.id = id_;
        metadata_.name = "Python Strategy";
        metadata_.description = "Implementation of strategy defined in Python";

        py::function override =
            py::get_override(static_cast<const BaseStrategy*>(this), "initialize");
        if (!override) {
            WARN("No Python override found for initialize in Strategy " + id_);
            return Result<void>();
        }

        try {
            py::object result = override();

            // Python returns None → success
            if (result.is_none()) {
                INFO("Python initialize override returned None, treating as success");
                return Result<void>();
            }

            return result.cast<Result<void>>();
        } catch (const py::error_already_set& e) {
            return make_error<void>(ErrorCode::NOT_INITIALIZED,
                                    std::string("Python initialization failed: ") + e.what(),
                                    "BaseStrategy");
        }
    }

    std::unordered_map<std::string, std::vector<double>> get_price_history() const override {
        py::gil_scoped_acquire gil;

        py::function override =
            py::get_override(static_cast<const BaseStrategy*>(this), "get_price_history");
        if (!override) {
            WARN("No Python override found for get_price_history in Strategy " + id_ +
                 ", using base implementation");
            return BaseStrategy::get_price_history();
        }

        try {
            py::object result = override();

            if (result.is_none()) {
                INFO("Python get_price_history override returned None, treating as empty history");
                return std::unordered_map<std::string, std::vector<double>>();
            }

            return result.cast<std::unordered_map<std::string, std::vector<double>>>();
        } catch (const py::error_already_set& e) {
            ERROR("Python get_price_history override failed: " + std::string(e.what()));
            return std::unordered_map<std::string, std::vector<double>>();
        }
    }
};

void bind_base_strategy(py::module_& m) {
    py::class_<BaseStrategy, PyBaseStrategy, std::shared_ptr<BaseStrategy>>(m, "BaseStrategy")
        .def(py::init<>())
        .def("initialize_from_context", &BaseStrategy::initialize_from_context, py::arg("id"),
             py::arg("config"),
             py::arg("db"))  // Internal initializer for Python subclasses to set up the
                             // strategy from the context passed in by the backtest runner
        .def_property_readonly(
            "config",
            [](BaseStrategy& self) -> const StrategyConfig& {
                return self.get_config();
            })  // Necessarily expose config from C++ side instead of Python side since we're
                // getting the configs from the config files. Also, currently read only which
                // should be sufficient, but potentially might want to add some setters in the
                // future if we want to allow dynamic config updates from Python side.
        .def("initialize", &BaseStrategy::initialize)
        .def("on_data", &BaseStrategy::on_data)
        .def("on_execution", &BaseStrategy::on_execution)
        .def("on_signal", &BaseStrategy::on_signal)
        .def("get_price_history", &BaseStrategy::get_price_history)
        .def_property_readonly(
            "positions",
            [](const BaseStrategy& self) -> const std::unordered_map<std::string, Position>& {
                return self.get_positions();
            },
            py::return_value_policy::reference_internal)
        .def("update_position", &PyStrategy::update_position, py::arg("symbol"),
             py::arg("position"))
        .def("generate_positions_from_data", &PyStrategy::generate_positions_from_data);
}
