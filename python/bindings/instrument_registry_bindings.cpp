#include "bindings.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/instruments/instrument.hpp"

namespace py = pybind11;
using namespace trade_ngin;

void bind_instrument_registry(py::module_& m) {
    py::class_<Instrument, std::shared_ptr<Instrument>>(m, "Instrument")
        .def("get_symbol", &Instrument::get_symbol)
        .def("get_multiplier", &Instrument::get_multiplier)
        .def("get_tick_size", &Instrument::get_tick_size)
        .def("get_point_value", &Instrument::get_point_value);

    py::class_<InstrumentRegistry, std::shared_ptr<InstrumentRegistry>>(m, "InstrumentRegistry")
        .def_static("instance", &InstrumentRegistry::instance, py::return_value_policy::reference)
        .def("get_instrument", &InstrumentRegistry::get_instrument, py::arg("symbol"))
        .def("has_instrument", &InstrumentRegistry::has_instrument, py::arg("symbol"));
}

