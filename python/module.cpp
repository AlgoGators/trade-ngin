#include <pybind11/pybind11.h>

PYBIND11_MODULE(trade_ngin, m) {
    m.doc() = "Python bindings for trade_ngin";

    // bind_core_types(m);
    // bind_error_types(m);
    // bind_strategy_types(m);
}
