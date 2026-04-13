#pragma once

#include <pybind11/pybind11.h>

#include "trade_ngin/core/error.hpp"

namespace py = pybind11;

namespace pybind11::detail {

// Custom type caster for Result<T> to convert between C++ Result<T> and a regular Python object or
// raise an exception on error
template <typename T>
struct type_caster<trade_ngin::Result<T>> {
public:
    PYBIND11_TYPE_CASTER(trade_ngin::Result<T>, _("Result"));

    bool load(handle src, bool) {
        if (src.is_none()) {
            value = trade_ngin::Result<void>();
            return true;
        }
        return false;
    }

    static handle cast(trade_ngin::Result<T> src, return_value_policy policy, handle parent) {
        if (!src.is_ok()) {
            throw std::runtime_error(*(src.error()));
        }
        return make_caster<T>::cast(std::move(src.value()), policy, parent);
    }
};

// Void specialization for Result<void>
template <>
struct type_caster<trade_ngin::Result<void>> {
public:
    PYBIND11_TYPE_CASTER(trade_ngin::Result<void>, _("ResultVoid"));

    bool load(handle src, bool) {
        // We don't support loading Result<void> from Python, so just return false to indicate
        // failure
        return false;
    }

    static handle cast(trade_ngin::Result<void> src, return_value_policy policy, handle parent) {
        if (!src.is_ok()) {
            throw std::runtime_error(*(src.error()));
        }
        return py::none().release();
    }
};

}  // namespace pybind11::detail
