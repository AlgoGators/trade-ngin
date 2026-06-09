#include "bindings.hpp"
#include "pystrategy.hpp"

#include <pybind11/pybind11.h>

#include "trade_ngin/api/backtest_api.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

namespace py = pybind11;
using namespace trade_ngin;
using namespace trade_ngin::api;

void bind_backtest_api(py::module_& m) {
    py::class_<BacktestRunner>(m, "BacktestRunner")
        .def(py::init<>())
        .def("run_backtest", &BacktestRunner::run_backtest, py::arg("portfolio"))
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
