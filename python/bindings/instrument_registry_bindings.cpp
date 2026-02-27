#include "bindings.hpp"

#include <pybind11/pybind11.h>

#include "trade_ngin/instruments/instrument_registry.hpp"

namespace py = pybind11;
using namespace trade_ngin;

void bind_instrument_registry(py::module_& m) {
    py::class_<InstrumentRegistry, std::shared_ptr<InstrumentRegistry>>(m, "InstrumentRegistry");
}
