#include <pybind11/pybind11.h>

namespace py = pybind11;

PYBIND11_MODULE(trade_ngin, m) {
    m.doc() = "Python bindings for trade_ngin";

    // bindings go here, e.g.
    // bind_process(m);
}
