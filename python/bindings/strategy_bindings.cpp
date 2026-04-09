#include "bindings.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl_bind.h>

#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_fast.hpp"

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
    // Result<void> update_position(const std::string& symbol, const Position& position) override {
    //     PYBIND11_OVERRIDE(Result<void>, BaseStrategy, update_position, symbol, position);
    // };

    // TODO custom override to tell C++ part that the strategy has been initialized from Python side
    // TODO pass to python to initialize
    // "Initialization" that would normally be done in the constructor but we're unable to do
    // that here so we do it in the initialize method - maybe can use write new constructors later
    Result<void> initialize() override {
        py::gil_scoped_acquire gil;

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

        // Optional Python extension
        if (py::get_override(this, "initialize")) {
            INFO("Override FOUND");
        } else {
            WARN("Override NOT found");
        }

        if (py::function override = py::get_override(this, "initialize")) {
            INFO("Running Python override for initialize in Strategy " + id_);
            auto result = override().cast<Result<void>>();
            if (result.is_error()) {
                return result;
            }
        }

        INFO("Initialization successful for Python Strategy " + id_);

        return Result<void>();
    }
};

void bind_base_strategy(py::module_& m) {
    using PositionMap = std::unordered_map<std::string, Position>;

    py::class_<BaseStrategy, PyBaseStrategy, std::shared_ptr<BaseStrategy>>(m, "BaseStrategy")
        .def(py::init<>())
        .def("initialize_from_context", &BaseStrategy::initialize_from_context, py::arg("id"),
             py::arg("config"),
             py::arg("db"))  // Hidden internal initializer for Python subclasses to set up the
                             // strategy from the context passed in by the backtest runner
        .def("config",
             [](BaseStrategy& self) -> const StrategyConfig& {
                 return self.get_config();
             })  // Necessarily expose config from C++ side instead of Python side since we're
                 // getting the configs from the config files. Also, currently read only which
                 // should be sufficient, but potentially might want to add some setters in the
                 // future if we want to allow dynamic config updates from Python side.
        .def("initialize", &BaseStrategy::initialize)
        .def("on_data", &BaseStrategy::on_data)
        .def("on_execution", &BaseStrategy::on_execution)
        .def_property_readonly(
            "positions",
            [](const BaseStrategy& self) -> const PositionMap& { return self.get_positions(); },
            py::return_value_policy::reference_internal)
        .def("update_position", &BaseStrategy::update_position, py::arg("symbol"),
             py::arg("position"));
    // TODO We can expose more that we need as we go on
}

void bind_trend_following_config(py::module_& m) {
    py::class_<TrendFollowingConfig>(m, "TrendFollowingConfig")
        .def(py::init<>())
        .def_readwrite("weight", &TrendFollowingConfig::weight)
        .def_readwrite("risk_target", &TrendFollowingConfig::risk_target)
        .def_readwrite("fx_rate", &TrendFollowingConfig::fx_rate)
        .def_readwrite("idm", &TrendFollowingConfig::idm)
        .def_readwrite("max_symbol_concentration", &TrendFollowingConfig::max_symbol_concentration)
        .def_readwrite("use_position_buffering", &TrendFollowingConfig::use_position_buffering)
        .def_readwrite("ema_windows", &TrendFollowingConfig::ema_windows)
        .def_readwrite("vol_lookback_short", &TrendFollowingConfig::vol_lookback_short)
        .def_readwrite("vol_lookback_long", &TrendFollowingConfig::vol_lookback_long)
        .def_readwrite("fdm", &TrendFollowingConfig::fdm)
        .def("to_dict", [](const TrendFollowingConfig& cfg) {
            py::dict d;
            d["weight"] = cfg.weight;
            d["risk_target"] = cfg.risk_target;
            d["fx_rate"] = cfg.fx_rate;
            d["idm"] = cfg.idm;
            d["max_symbol_concentration"] = cfg.max_symbol_concentration;
            d["use_position_buffering"] = cfg.use_position_buffering;
            d["ema_windows"] = cfg.ema_windows;
            d["vol_lookback_short"] = cfg.vol_lookback_short;
            d["vol_lookback_long"] = cfg.vol_lookback_long;
            d["fdm"] = cfg.fdm;
            return d;
        });
}

// There's a bit of a problem with binding what we already have because we're going from C++ to
// Python back to C++ back to Python back to C++...
// It's fine though
void bind_trend_following_strategy(py::module_& m) {
    py::class_<TrendFollowingStrategy, BaseStrategy, std::shared_ptr<TrendFollowingStrategy>>(
        m, "TrendFollowingStrategy")
        .def(
            "__init__",
            [](TrendFollowingStrategy& self, const std::string& id,
               const StrategyConfig& base_config, py::dict trend_config,
               std::shared_ptr<PostgresDatabase> db, std::shared_ptr<InstrumentRegistry> registry) {
                // Convert py::dict -> TrendFollowingConfig
                TrendFollowingConfig cfg;
                cfg.weight = trend_config["weight"].cast<double>();
                cfg.risk_target = trend_config["risk_target"].cast<double>();
                cfg.fx_rate = trend_config["fx_rate"].cast<double>();
                cfg.idm = trend_config["idm"].cast<double>();
                cfg.max_symbol_concentration =
                    trend_config["max_symbol_concentration"].cast<double>();
                cfg.use_position_buffering = trend_config["use_position_buffering"].cast<bool>();
                cfg.ema_windows =
                    trend_config["ema_windows"].cast<std::vector<std::pair<int, int>>>();
                cfg.vol_lookback_short = trend_config["vol_lookback_short"].cast<int>();
                cfg.vol_lookback_long = trend_config["vol_lookback_long"].cast<int>();
                cfg.fdm = trend_config["fdm"].cast<std::vector<std::pair<int, double>>>();

                // Placement new to call the real constructor - highkey wizard magic
                new (&self) TrendFollowingStrategy(id, base_config, cfg, db, registry);
            },
            py::arg("id"), py::arg("config"), py::arg("trend_config"), py::arg("db"),
            py::arg("registry") = nullptr);
}

void bind_trend_following_fast_config(py::module_& m) {
    py::class_<TrendFollowingFastConfig>(m, "TrendFollowingFastConfig")
        .def(py::init<>())
        .def_readwrite("weight", &TrendFollowingFastConfig::weight)
        .def_readwrite("risk_target", &TrendFollowingFastConfig::risk_target)
        .def_readwrite("fx_rate", &TrendFollowingFastConfig::fx_rate)
        .def_readwrite("idm", &TrendFollowingFastConfig::idm)
        .def_readwrite("max_symbol_concentration",
                       &TrendFollowingFastConfig::max_symbol_concentration)
        .def_readwrite("use_position_buffering", &TrendFollowingFastConfig::use_position_buffering)
        .def_readwrite("ema_windows", &TrendFollowingFastConfig::ema_windows)
        .def_readwrite("vol_lookback_short", &TrendFollowingFastConfig::vol_lookback_short)
        .def_readwrite("vol_lookback_long", &TrendFollowingFastConfig::vol_lookback_long)
        .def_readwrite("fdm", &TrendFollowingFastConfig::fdm)
        .def("to_dict", [](const TrendFollowingFastConfig& cfg) {
            py::dict d;
            d["weight"] = cfg.weight;
            d["risk_target"] = cfg.risk_target;
            d["fx_rate"] = cfg.fx_rate;
            d["idm"] = cfg.idm;
            d["max_symbol_concentration"] = cfg.max_symbol_concentration;
            d["use_position_buffering"] = cfg.use_position_buffering;
            d["ema_windows"] = cfg.ema_windows;
            d["vol_lookback_short"] = cfg.vol_lookback_short;
            d["vol_lookback_long"] = cfg.vol_lookback_long;
            d["fdm"] = cfg.fdm;
            return d;
        });
}

void bind_trend_following_fast_strategy(py::module_& m) {
    py::class_<TrendFollowingFastStrategy, BaseStrategy,
               std::shared_ptr<TrendFollowingFastStrategy>>(m, "TrendFollowingFastStrategy")
        .def(
            "__init__",
            [](TrendFollowingFastStrategy& self, const std::string& id,
               const StrategyConfig& base_config, py::dict trend_config,
               std::shared_ptr<PostgresDatabase> db, std::shared_ptr<InstrumentRegistry> registry) {
                // Convert py::dict -> TrendFollowingFastConfig
                TrendFollowingFastConfig cfg;
                cfg.weight = trend_config["weight"].cast<double>();
                cfg.risk_target = trend_config["risk_target"].cast<double>();
                cfg.fx_rate = trend_config["fx_rate"].cast<double>();
                cfg.idm = trend_config["idm"].cast<double>();
                cfg.max_symbol_concentration =
                    trend_config["max_symbol_concentration"].cast<double>();
                cfg.use_position_buffering = trend_config["use_position_buffering"].cast<bool>();
                cfg.ema_windows =
                    trend_config["ema_windows"].cast<std::vector<std::pair<int, int>>>();
                cfg.vol_lookback_short = trend_config["vol_lookback_short"].cast<int>();
                cfg.vol_lookback_long = trend_config["vol_lookback_long"].cast<int>();
                cfg.fdm = trend_config["fdm"].cast<std::vector<std::pair<int, double>>>();

                // Placement new to call the real constructor - highkey wizard magic
                new (&self) TrendFollowingFastStrategy(id, base_config, cfg, db, registry);
            },
            py::arg("id"), py::arg("config"), py::arg("trend_config"), py::arg("db"),
            py::arg("registry") = nullptr);
}
