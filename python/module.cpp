#include <pybind11/pybind11.h>

#include "bindings/bindings.hpp"

PYBIND11_MODULE(trade_ngin, m) {
    m.doc() = "Python bindings for trade_ngin";

    bind_core_types(m);
    bind_backtest_types(m);
    bind_strategy_types(m);
    bind_error_types(m);
    bind_database(m);
    bind_base_strategy(m);
    bind_backtest_api(m);
}
