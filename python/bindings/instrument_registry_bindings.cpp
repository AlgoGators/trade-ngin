#include "bindings.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"

namespace py = pybind11;
using namespace trade_ngin;

void bind_instrument_registry(py::module_& m) {
    py::class_<Instrument, std::shared_ptr<Instrument>>(m, "Instrument")
        .def("get_symbol", &Instrument::get_symbol)
        .def("get_multiplier", &Instrument::get_multiplier)
        .def("get_tick_size", &Instrument::get_tick_size)
        .def("get_point_value", &Instrument::get_point_value);

    // ---- Specs ----
    // Default-constructible and field-writable so Python can describe an
    // instrument without going through the contract metadata table.
    py::class_<FuturesSpec>(m, "FuturesSpec")
        .def(py::init<>())
        .def_readwrite("root_symbol", &FuturesSpec::root_symbol)
        .def_readwrite("exchange", &FuturesSpec::exchange)
        .def_readwrite("currency", &FuturesSpec::currency)
        .def_readwrite("multiplier", &FuturesSpec::multiplier)
        .def_readwrite("tick_size", &FuturesSpec::tick_size)
        .def_readwrite("commission_per_contract", &FuturesSpec::commission_per_contract)
        .def_readwrite("initial_margin", &FuturesSpec::initial_margin)
        .def_readwrite("maintenance_margin", &FuturesSpec::maintenance_margin)
        .def_readwrite("weight", &FuturesSpec::weight)
        .def_readwrite("trading_hours", &FuturesSpec::trading_hours);

    py::class_<EquitySpec>(m, "EquitySpec")
        .def(py::init<>())
        .def_readwrite("exchange", &EquitySpec::exchange)
        .def_readwrite("currency", &EquitySpec::currency)
        .def_readwrite("lot_size", &EquitySpec::lot_size)
        .def_readwrite("tick_size", &EquitySpec::tick_size)
        .def_readwrite("commission_per_share", &EquitySpec::commission_per_share)
        .def_readwrite("is_etf", &EquitySpec::is_etf)
        .def_readwrite("is_marginable", &EquitySpec::is_marginable)
        .def_readwrite("margin_requirement", &EquitySpec::margin_requirement)
        .def_readwrite("sector", &EquitySpec::sector)
        .def_readwrite("industry", &EquitySpec::industry)
        .def_readwrite("trading_hours", &EquitySpec::trading_hours);

    // ---- Concrete instruments ----
    py::class_<FuturesInstrument, Instrument, std::shared_ptr<FuturesInstrument>>(
        m, "FuturesInstrument")
        .def(py::init<std::string, FuturesSpec>(), py::arg("symbol"), py::arg("spec"));

    py::class_<EquityInstrument, Instrument, std::shared_ptr<EquityInstrument>>(m,
                                                                                "EquityInstrument")
        .def(py::init<std::string, EquitySpec>(), py::arg("symbol"), py::arg("spec"));

    py::class_<InstrumentRegistry, std::shared_ptr<InstrumentRegistry>>(m, "InstrumentRegistry")
        .def_static("instance", &InstrumentRegistry::instance, py::return_value_policy::reference)
        .def("get_instrument", &InstrumentRegistry::get_instrument, py::arg("symbol"))
        .def("has_instrument", &InstrumentRegistry::has_instrument, py::arg("symbol"))
        .def("register_instrument", &InstrumentRegistry::register_instrument, py::arg("instrument"),
             "Register an instrument directly, bypassing the database.")
        .def("initialize_without_database", &InstrumentRegistry::initialize_without_database,
             "Initialize the registry for a database-free run.")
        .def("clear", &InstrumentRegistry::clear,
             "Drop all registered instruments. The registry is a process-wide "
             "singleton, so call this between runs.");
}
