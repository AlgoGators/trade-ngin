#pragma once
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_core_types(py::module_& m);
void bind_error_types(py::module_& m);
void bind_strategy_types(py::module_& m);
