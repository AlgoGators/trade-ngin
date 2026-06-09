#include "bindings.hpp"

#include "pystrategy.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <unordered_map>
#include <vector>

#include "trade_ngin/strategy/base_strategy.hpp"

namespace py = pybind11;
using namespace trade_ngin;

// TODO find some way so we don't need to repeat all this code
struct PyBaseStrategy : public PyStrategy {
    using PyStrategy::PyStrategy;  // Inherit constructors

    Result<void> on_data(const std::vector<Bar>& data) override {
        auto positions = generate_positions_from_data(data);
        for (const auto& pos : positions) {
            Result<void> result = update_position(pos.symbol, pos);
            if (result.is_error()) {
                return result;  // Return failure if any position update fails
            }
        }
        return Result<void>();  // Return success if all position updates succeed
    }

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
            py::get_override(static_cast<const PyStrategy*>(this), "initialize");
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
            py::get_override(static_cast<const PyStrategy*>(this), "get_price_history");
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

    std::vector<Position> generate_positions_from_data(
        const std::vector<Bar>& data) const override {
        py::gil_scoped_acquire gil;

        py::function override =
            py::get_override(static_cast<const PyStrategy*>(this), "generate_positions_from_data");
        if (!override) {
            WARN("No Python override found for generate_positions_from_data in Strategy " + id_ +
                 ", returning empty position list");
            return std::vector<Position>();
        }

        try {
            py::object result = override(data);

            if (result.is_none()) {
                INFO(
                    "Python generate_positions_from_data override returned None, treating as no "
                    "positions");
                return std::vector<Position>();
            }

            return result.cast<std::vector<Position>>();
        } catch (const py::error_already_set& e) {
            ERROR("Python generate_positions_from_data override failed: " + std::string(e.what()));
            return std::vector<Position>();
        }
    }
};

void bind_base_strategy(py::module_& m) {
    py::class_<PyStrategy, PyBaseStrategy, std::shared_ptr<PyStrategy>>(m, "BaseStrategy")
        .def(py::init<>())
        .def("initialize_from_context", &PyStrategy::initialize_from_context, py::arg("id"),
             py::arg("config"), py::arg("db"),
             py::arg("registry"))  // Internal initializer for Python subclasses to set up the
                                   // strategy from the context passed in by the backtest runner
        .def_property_readonly(
            "config",
            [](PyStrategy& self) -> const StrategyConfig& {
                return self.get_config();
            })  // Necessarily expose config from C++ side instead of Python side since we're
                // getting the configs from the config files. Also, currently read only which
                // should be sufficient, but potentially might want to add some setters in the
                // future if we want to allow dynamic config updates from Python side.
        .def_property_readonly(
            "registry",
            [](PyStrategy& self) -> std::shared_ptr<InstrumentRegistry> { return self.registry_; },
            py::return_value_policy::
                reference_internal)  // Expose registry to allow Python strategies to register
                                     // custom components like indicators or data sources
        .def("initialize", &PyStrategy::initialize)
        .def("on_signal", &PyStrategy::on_signal)
        .def("get_price_history", &PyStrategy::get_price_history)
        .def_property_readonly(
            "positions",
            [](const PyStrategy& self) -> const std::unordered_map<std::string, Position>& {
                return self.get_positions();
            },
            py::return_value_policy::reference_internal)
        .def("update_position", &PyStrategy::update_position, py::arg("symbol"),
             py::arg("position"))
        .def("generate_positions_from_data", &PyStrategy::generate_positions_from_data);
}
