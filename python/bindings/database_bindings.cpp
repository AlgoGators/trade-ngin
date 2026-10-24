#include "bindings.hpp"

#include <pybind11/pybind11.h>

#include "trade_ngin/data/postgres_database.hpp"

namespace py = pybind11;
using namespace trade_ngin;

void bind_database(py::module_& m) {
    py::class_<PostgresDatabase, std::shared_ptr<PostgresDatabase>>(m, "PostgresDatabase")
        .def(py::init<const std::string>())
        .def("connect", &PostgresDatabase::connect)
        .def("disconnect", &PostgresDatabase::disconnect);
}
