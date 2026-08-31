#include "bindings.hpp"
#include "pystrategy.hpp"

#include <pybind11/chrono.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "trade_ngin/api/backtest_api.hpp"
#include "trade_ngin/data/market_data_source.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

namespace py = pybind11;
using namespace trade_ngin;
using namespace trade_ngin::api;

void bind_backtest_api(py::module_& m) {
    py::class_<StrategySpec>(m, "StrategySpec")
        .def(py::init<>())
        .def(py::init([](std::string strategy_id, double allocation, py::object config) {
                 StrategySpec spec;
                 spec.strategy_id = std::move(strategy_id);
                 spec.allocation = allocation;
                 if (!config.is_none()) {
                     // Round-trip through JSON text so any plain Python dict works.
                     auto dumped = py::module_::import("json").attr("dumps")(config);
                     spec.config = nlohmann::json::parse(dumped.cast<std::string>());
                 }
                 return spec;
             }),
             py::arg("strategy_id"), py::arg("allocation") = 1.0,
             py::arg("config") = py::none())
        .def_readwrite("strategy_id", &StrategySpec::strategy_id)
        .def_readwrite("allocation", &StrategySpec::allocation);

    py::class_<BacktestRunConfig>(m, "BacktestRunConfig",
                                  "Self-contained backtest description. Supplying one makes the "
                                  "run argument-driven: no config files, no database.")
        .def(py::init<>())
        .def_readwrite("symbols", &BacktestRunConfig::symbols)
        .def_readwrite("start_date", &BacktestRunConfig::start_date)
        .def_readwrite("end_date", &BacktestRunConfig::end_date)
        .def_readwrite("asset_class", &BacktestRunConfig::asset_class)
        .def_readwrite("data_freq", &BacktestRunConfig::data_freq)
        .def_property(
            "data_source",
            [](const BacktestRunConfig& self) { return self.data_source; },
            [](BacktestRunConfig& self, py::object source) {
                if (source.is_none()) {
                    self.data_source.reset();
                    return;
                }

                // Keep the Python wrapper alive for as long as the C++ side
                // holds the source. py::get_override looks the instance up in
                // pybind11's registered_instances map; if the wrapper is
                // collected the entry disappears and every override lookup
                // silently returns empty. The aliasing constructor shares
                // ownership with the py::object while exposing the
                // MarketDataSource* to callers. The GIL must be held when the
                // py::object is finally released.
                auto* raw = source.cast<MarketDataSource*>();
                auto owner = std::shared_ptr<py::object>(
                    new py::object(std::move(source)), [](py::object* obj) {
                        py::gil_scoped_acquire destroy_gil;
                        delete obj;
                    });
                self.data_source = std::shared_ptr<MarketDataSource>(owner, raw);
            })
        .def_readwrite("initial_capital", &BacktestRunConfig::initial_capital)
        .def_readwrite("reserve_capital_pct", &BacktestRunConfig::reserve_capital_pct)
        .def_readwrite("max_drawdown", &BacktestRunConfig::max_drawdown)
        .def_readwrite("max_leverage", &BacktestRunConfig::max_leverage)
        .def_readwrite("position_limit", &BacktestRunConfig::position_limit)
        .def_readwrite("max_strategy_allocation", &BacktestRunConfig::max_strategy_allocation)
        .def_readwrite("min_strategy_allocation", &BacktestRunConfig::min_strategy_allocation)
        .def_readwrite("use_risk_management", &BacktestRunConfig::use_risk_management)
        .def_readwrite("use_optimization", &BacktestRunConfig::use_optimization)
        .def_readwrite("strategies", &BacktestRunConfig::strategies)
        .def_readwrite("portfolio_id", &BacktestRunConfig::portfolio_id)
        .def_readwrite("store_results", &BacktestRunConfig::store_results)
        .def_readwrite("export_csv", &BacktestRunConfig::export_csv)
        .def_readwrite("csv_output_path", &BacktestRunConfig::csv_output_path);

    py::class_<BacktestRunner>(m, "BacktestRunner")
        .def(py::init<>())
        .def("initialize", &BacktestRunner::initialize, py::arg("portfolio_name"))
        .def("initialize_with_config", &BacktestRunner::initialize_with_config,
             py::arg("portfolio_name"), py::arg("config"),
             "Initialize a fully argument-driven run (no config files, no database).")
        .def(
            "set_data_source",
            [](BacktestRunner& self, py::object source) {
                // Same wrapper-lifetime handling as BacktestRunConfig.data_source.
                auto* raw = source.cast<MarketDataSource*>();
                auto owner = std::shared_ptr<py::object>(
                    new py::object(std::move(source)), [](py::object* obj) {
                        py::gil_scoped_acquire destroy_gil;
                        delete obj;
                    });
                self.set_data_source(std::shared_ptr<MarketDataSource>(owner, raw));
            },
            py::arg("source"))
        .def("run_backtest", &BacktestRunner::run_backtest)
        .def(
            "register_strategy",
            [](BacktestRunner& self, const std::string& strategy_id, py::object py_class) {
                self.register_strategy(
                    strategy_id,
                    [py_class, strategy_id](
                        const StrategyContext& ctx,
                        const nlohmann::json& strategy_defaults) -> std::shared_ptr<BaseStrategy> {
                        py::gil_scoped_acquire gil;
                        (void)strategy_defaults;  // Currently unused, but could be passed to the
                                                  // Python class if needed

                        // Hold the Python wrapper alive explicitly
                        py::object py_instance = py_class();
                        // Inject state
                        py_instance.attr("initialize_from_context")(
                            strategy_id, ctx.base_strategy_config, ctx.db, ctx.registry);

                        // Extract raw C++ pointer while the Python wrapper is
                        // still alive.
                        PyStrategy* raw = py_instance.cast<PyStrategy*>();
                        BaseStrategy* base = static_cast<BaseStrategy*>(raw);

                        // Keep the Python wrapper alive for the entire lifetime
                        // of the returned shared_ptr. py::get_override works by
                        // looking up the Python wrapper in pybind11's
                        // registered_instances map; if the wrapper is GC'd the
                        // entry is removed and every subsequent get_override
                        // call returns empty. The aliasing constructor shares
                        // ownership with py_owner (keeping the py::object alive)
                        // while the pointer seen by callers is the
                        // BaseStrategy*.  The GIL must be held when the
                        // py::object is finally released.
                        auto py_owner = std::shared_ptr<py::object>(
                            new py::object(std::move(py_instance)), [](py::object* obj) {
                                py::gil_scoped_acquire destroy_gil;
                                delete obj;
                            });
                        return std::shared_ptr<BaseStrategy>(py_owner, base);
                    });
            },
            py::arg("strategy_id"), py::arg("strategy"))
        .def("register_trend_following_strategy",  // Ideally we wouldn't need to do this separately
                                                   // but this is the quickest and best solution
                                                   // right now IMO
             [](BacktestRunner& self) {
                 self.register_strategy(
                     "TREND_FOLLOWING",  // TODO Eventually allow multiple registrations with
                                         // different configs and names
                     [](const StrategyContext& ctx,
                        const nlohmann::json& strategy_def) -> std::shared_ptr<BaseStrategy> {
                         TrendFollowingConfig trend_config;
                         if (strategy_def.contains("config")) {
                             const auto& cfg = strategy_def["config"];
                             trend_config.weight = cfg.value("weight", 0.03);
                             trend_config.risk_target =
                                 cfg.value("risk_target", 0.15);  // Conservative default
                             trend_config.idm = cfg.value("idm", 2.5);
                             trend_config.max_symbol_concentration =
                                 cfg.value("max_symbol_concentration", 0.15);
                             trend_config.use_position_buffering =
                                 cfg.value("use_position_buffering", true);
                             trend_config.carver_buffer_floor =
                                 cfg.value("carver_buffer_floor",
                                           0.5);  // Unfortunately cannot use app_config
                                                  // defaults here so we'll just have to
                                                  // hard-code them
                                                  // TODO consider allowing app_config defaults to
                                                  // be injected here in the future
                             trend_config.carver_buffer_position_factor =
                                 cfg.value("carver_buffer_position_factor",
                                           0.0);  // Hard-coded default
                             if (cfg.contains("ema_windows")) {
                                 trend_config.ema_windows.clear();
                                 for (const auto& window : cfg["ema_windows"]) {
                                     trend_config.ema_windows.push_back(
                                         {window[0].get<int>(), window[1].get<int>()});
                                 }
                             }
                             trend_config.vol_lookback_short = cfg.value("vol_lookback_short", 32);
                             trend_config.vol_lookback_long = cfg.value("vol_lookback_long", 252);
                         }
                         // Set FDM from strategy_defaults
                         if (trend_config.fdm.empty()) {
                             trend_config.fdm = {{1, 1.0},  {2, 1.03}, {3, 1.08}, {4, 1.13},
                                                 {5, 1.19}, {6, 1.26}};  // Hard-coded default
                         }

                         // Construct the strategy with context and config
                         return std::make_shared<TrendFollowingStrategy>(
                             "TREND_FOLLOWING", ctx.base_strategy_config, trend_config, ctx.db,
                             ctx.registry);
                     });
             });
}
