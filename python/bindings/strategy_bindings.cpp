#include "bindings.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

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

void bind_trend_following_strategy(py::module_& m) {
    py::class_<TrendFollowingStrategy, BaseStrategy, std::shared_ptr<TrendFollowingStrategy>>(
        m, "TrendFollowingStrategy")
        .def(py::init<std::string, StrategyConfig, TrendFollowingConfig,
                      std::shared_ptr<PostgresDatabase>, std::shared_ptr<InstrumentRegistry>>(),
             py::arg("id"), py::arg("config"), py::arg("trend_config"), py::arg("db"),
             py::arg("registry") = nullptr);
}

void bind_trend_following_fast_strategy(py::module_& m) {
    // TODO add bindings for trend following fast strategy
}
