#pragma once

#include <pybind11/pybind11.h>

#include "result_caster.hpp"  // Warns that it's unused but required here for the TU that includes this header to properly convert Result<T> to Python exceptions

namespace py = pybind11;

// In order of when they should be bound, since some types depend on others
void bind_core_types(py::module_& m);
void bind_backtest_types(py::module_& m);
void bind_strategy_types(py::module_& m);
// void bind_error_types(py::module_& m);

void bind_database(py::module_& m);
void bind_instrument_registry(py::module_& m);

void bind_base_strategy(py::module_& m);
void bind_trend_following_config(py::module_& m);
void bind_trend_following_strategy(py::module_& m);
void bind_trend_following_fast_strategy(py::module_& m);

void bind_backtest_api(py::module_& m);
