#include "bindings.hpp"

#include <pybind11/chrono.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "trade_ngin/data/market_data_source.hpp"
#include "trade_ngin/data/postgres_database.hpp"

namespace py = pybind11;
using namespace trade_ngin;

/**
 * @brief Trampoline allowing a Python class to implement MarketDataSource
 *
 * Python overrides return plain lists rather than Result objects: the
 * Result caster is cast-only (C++ -> Python) and cannot load a Result back
 * out of Python. Exceptions raised in Python are caught here and converted
 * into the corresponding error Result.
 */
class PyMarketDataSource : public MarketDataSource {
public:
    using MarketDataSource::MarketDataSource;

    Result<std::vector<Bar>> load_bars(const std::vector<std::string>& symbols,
                                       const Timestamp& start_date, const Timestamp& end_date,
                                       AssetClass asset_class, DataFrequency freq) override {
        py::gil_scoped_acquire gil;

        py::function override = py::get_override(this, "load_bars");
        if (!override) {
            return make_error<std::vector<Bar>>(
                ErrorCode::NOT_INITIALIZED,
                "MarketDataSource subclass does not implement load_bars()", "PyMarketDataSource");
        }

        try {
            py::object result = override(symbols, start_date, end_date, asset_class, freq);
            return Result<std::vector<Bar>>(result.cast<std::vector<Bar>>());
        } catch (const py::error_already_set& e) {
            return make_error<std::vector<Bar>>(
                ErrorCode::MARKET_DATA_ERROR,
                std::string("Python load_bars() raised: ") + e.what(), "PyMarketDataSource");
        } catch (const py::cast_error&) {
            return make_error<std::vector<Bar>>(
                ErrorCode::INVALID_DATA, "Python load_bars() must return a list of Bar",
                "PyMarketDataSource");
        }
    }

    Result<std::vector<std::string>> get_symbols(AssetClass asset_class) override {
        py::gil_scoped_acquire gil;

        py::function override = py::get_override(this, "get_symbols");
        if (!override) {
            return make_error<std::vector<std::string>>(
                ErrorCode::NOT_INITIALIZED,
                "MarketDataSource subclass does not implement get_symbols()", "PyMarketDataSource");
        }

        try {
            py::object result = override(asset_class);
            return Result<std::vector<std::string>>(result.cast<std::vector<std::string>>());
        } catch (const py::error_already_set& e) {
            return make_error<std::vector<std::string>>(
                ErrorCode::MARKET_DATA_ERROR,
                std::string("Python get_symbols() raised: ") + e.what(), "PyMarketDataSource");
        } catch (const py::cast_error&) {
            return make_error<std::vector<std::string>>(
                ErrorCode::INVALID_DATA, "Python get_symbols() must return a list of str",
                "PyMarketDataSource");
        }
    }
};

void bind_data_source(py::module_& m) {
    py::class_<MarketDataSource, PyMarketDataSource, std::shared_ptr<MarketDataSource>>(
        m, "MarketDataSource",
        "Base class for custom market data providers. Subclass it and override:\n"
        "  load_bars(symbols, start_date, end_date, asset_class, freq) -> list[Bar]\n"
        "  get_symbols(asset_class) -> list[str]\n"
        "Both overrides return plain lists, not Result objects.")
        .def(py::init<>());

    py::class_<PostgresMarketDataSource, MarketDataSource,
               std::shared_ptr<PostgresMarketDataSource>>(
        m, "PostgresMarketDataSource", "MarketDataSource backed by the trade-ngin database.")
        .def(py::init<std::shared_ptr<PostgresDatabase>, std::string, size_t>(), py::arg("db"),
             py::arg("data_type") = "ohlcv", py::arg("batch_size") = 5);
}
