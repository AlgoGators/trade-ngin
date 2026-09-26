#pragma once

// CM1: the transaction cost model and the P&L managers read a future's contract size and tick
// from its metadata row (metadata.contract_metadata through the InstrumentRegistry); nothing is
// hard-coded any more. A unit test that prices futures without a database registers the rows
// here: "Contract Size", "Tick Size" and "Minimum Price Fluctuation" of every futures row of the
// stage-3 session clone's metadata table (40 rows), verbatim.
//
// The registry is a process singleton with no public removal, so the rows stay registered for
// the rest of the process; ctest runs every test in its own process.

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"

namespace trade_ngin {
namespace testing {

inline void register_session_metadata_futures() {
    // symbol, Contract Size, Tick Size, Minimum Price Fluctuation
    static const std::vector<std::tuple<std::string, double, double, double>> rows = {
        {"6A", 100000, 0.0001, 10},      {"6B", 62500, 0.0001, 6.25},
        {"6C", 100000, 0.00005, 5},      {"6E", 125000, 0.00005, 6.25},
        {"6J", 12500000, 0.0000005, 6.25}, {"6L", 100000, 0.0001, 10},
        {"6M", 500000, 0.00001, 5},      {"6N", 100000, 0.00005, 5},
        {"6S", 125000, 0.00005, 6.25},   {"CL", 1000, 0.01, 10},
        {"ES", 50, 0.25, 12.5},          {"GC", 100, 0.10, 10},
        {"GF", 500, 0.025, 12.50},       {"HE", 400, 0.025, 10},
        {"HG", 25000, 0.0005, 12.5},     {"HO", 42000, 0.0001, 4.20},
        {"KE", 50, 0.25, 12.5},          {"LE", 400, 0.025, 10},
        {"M2K", 5, 0.1, 0.5},            {"MBT", 0.1, 5, 0.5},
        {"MES", 5, 0.25, 1.25},          {"MNQ", 2, 0.25, 0.5},
        {"MYM", 0.5, 1, 0.5},            {"NG", 10000, 0.001, 10},
        {"NQ", 20, 0.25, 5},             {"PL", 50, 0.1, 5},
        {"RB", 42000, 0.0001, 4.2},      {"RTY", 50, 0.1, 5},
        {"SI", 5000, 0.005, 25},         {"UB", 1000, 0.03125, 31.25},
        {"YM", 5, 1, 5},                 {"ZC", 50, 0.25, 12.5},
        {"ZF", 1000, 0.0078125, 7.8125}, {"ZL", 600, 0.01, 6},
        {"ZM", 100, 0.1, 10},            {"ZN", 1000, 0.015625, 15.625},
        {"ZR", 2000, 0.005, 10},         {"ZS", 50, 0.25, 12.5},
        {"ZT", 2000, 0.00390625, 7.8125}, {"ZW", 50, 0.25, 12.5},
    };
    auto& registry = InstrumentRegistry::instance();
    for (const auto& [symbol, contract_size, tick, tick_value] : rows) {
        FuturesSpec s;
        s.root_symbol = symbol;
        s.exchange = "CME";
        s.currency = "USD";
        s.multiplier = contract_size;
        s.tick_size = tick;
        s.commission_per_contract = 0.0;
        s.initial_margin = 0.0;
        s.maintenance_margin = 0.0;
        s.weight = 1.0;
        s.tick_value = tick_value;
        registry.register_instrument(symbol, std::make_shared<FuturesInstrument>(symbol, s));
    }
}

}  // namespace testing
}  // namespace trade_ngin
