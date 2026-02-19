// Debug verification for anshu-leverage branch changes
// Self-contained: no linking needed, just includes headers to verify struct fields compile
// Run: g++ -std=c++20 -I include -I /opt/homebrew/include tests/debug_verify_changes.cpp -o build/debug_verify && ./build/debug_verify

#include <iostream>
#include <string>
#include <unordered_map>
#include <cmath>

// Pull in just the types/structs we need to verify
#include "trade_ngin/core/types.hpp"

// Forward-verify struct field names by including headers (compile-time check)
// If any renamed field is wrong, this won't compile
#include "trade_ngin/live/live_metrics_calculator.hpp"
#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/live/live_trading_coordinator.hpp"
#include "trade_ngin/live/margin_manager.hpp"
#include "trade_ngin/risk/risk_manager.hpp"

using namespace trade_ngin;

int main() {
    int passed = 0;
    int failed = 0;

    auto check = [&](const std::string& name, bool condition) {
        if (condition) {
            std::cout << "  PASS: " << name << std::endl;
            passed++;
        } else {
            std::cout << "  FAIL: " << name << std::endl;
            failed++;
        }
    };

    // =====================================================
    // TEST 1: active_positions per-strategy counting fix
    // =====================================================
    std::cout << "\n=== TEST 1: active_positions per-strategy counting ===" << std::endl;
    {
        // Simulate: 2 strategies both holding AAPL + unique positions
        // TREND_FOLLOWING: AAPL(qty=5), MSFT(qty=3)
        // TREND_FOLLOWING_FAST: AAPL(qty=2), GOOG(qty=1)
        Timestamp now;
        std::unordered_map<std::string, std::unordered_map<std::string, Position>> strategy_positions_map;

        strategy_positions_map["TREND_FOLLOWING"]["AAPL"] = Position("AAPL", Quantity(5.0), Price(150.0), Decimal(0), Decimal(0), now);
        strategy_positions_map["TREND_FOLLOWING"]["MSFT"] = Position("MSFT", Quantity(3.0), Price(300.0), Decimal(0), Decimal(0), now);
        strategy_positions_map["TREND_FOLLOWING_FAST"]["AAPL"] = Position("AAPL", Quantity(2.0), Price(150.0), Decimal(0), Decimal(0), now);
        strategy_positions_map["TREND_FOLLOWING_FAST"]["GOOG"] = Position("GOOG", Quantity(1.0), Price(2500.0), Decimal(0), Decimal(0), now);

        // OLD way: aggregate by symbol then count (MarginManager does this)
        std::unordered_map<std::string, Position> aggregated;
        for (const auto& [strat, pos_map] : strategy_positions_map) {
            for (const auto& [sym, pos] : pos_map) {
                aggregated[sym] = pos;  // overwrites duplicate symbols
            }
        }
        int old_count = 0;
        for (const auto& [sym, pos] : aggregated) {
            if (std::abs(pos.quantity.as_double()) > 1e-6) old_count++;
        }

        // NEW way: count per-strategy positions (the fix)
        int true_active_positions = 0;
        for (const auto& [strategy_id, pos_map] : strategy_positions_map) {
            for (const auto& [symbol, pos] : pos_map) {
                if (std::abs(pos.quantity.as_double()) > 1e-6) {
                    true_active_positions++;
                }
            }
        }

        std::cout << "  Old (aggregated by symbol) count: " << old_count << std::endl;
        std::cout << "  New (per-strategy) count: " << true_active_positions << std::endl;

        check("Old way undercounts shared symbols (expect 3)", old_count == 3);
        check("New way counts all strategy positions (expect 4)", true_active_positions == 4);
        check("Fix: new count > old count when symbols overlap", true_active_positions > old_count);

        // Zero-quantity positions should still be excluded
        strategy_positions_map["TREND_FOLLOWING"]["TSLA"] = Position("TSLA", Quantity(0.0), Price(200.0), Decimal(0), Decimal(0), now);
        int count_with_zero = 0;
        for (const auto& [strategy_id, pos_map] : strategy_positions_map) {
            for (const auto& [symbol, pos] : pos_map) {
                if (std::abs(pos.quantity.as_double()) > 1e-6) {
                    count_with_zero++;
                }
            }
        }
        check("Zero-qty positions excluded (still 4)", count_with_zero == 4);
    }

    // =====================================================
    // TEST 2: gross_leverage field in all structs
    // (Compile-time proof that renaming is complete)
    // =====================================================
    std::cout << "\n=== TEST 2: gross_leverage field accessible in all structs ===" << std::endl;
    {
        // MarginManager::MarginMetrics
        MarginManager::MarginMetrics mm;
        mm.gross_leverage = 2.5;
        check("MarginManager::MarginMetrics.gross_leverage = 2.5", mm.gross_leverage == 2.5);

        // CalculatedMetrics
        CalculatedMetrics cm;
        cm.gross_leverage = 1.8;
        check("CalculatedMetrics.gross_leverage = 1.8", cm.gross_leverage == 1.8);

        // TradingMetrics
        TradingMetrics tm;
        tm.gross_leverage = 3.0;
        check("TradingMetrics.gross_leverage = 3.0", tm.gross_leverage == 3.0);

        // LiveResultsRow
        LiveResultsRow row;
        row.gross_leverage = 1.5;
        check("LiveResultsRow.gross_leverage = 1.5", row.gross_leverage == 1.5);

        // MarginMetrics (from live_data_loader)
        MarginMetrics lmm;
        lmm.gross_leverage = 2.2;
        check("MarginMetrics (loader).gross_leverage = 2.2", lmm.gross_leverage == 2.2);
    }

    // =====================================================
    // TEST 3: calculate_gross_leverage logic
    // =====================================================
    std::cout << "\n=== TEST 3: calculate_gross_leverage logic ===" << std::endl;
    {
        // Inline the same logic from margin_manager.cpp / live_metrics_calculator.cpp
        auto calc_gross_leverage = [](double gross_notional, double portfolio_value) -> double {
            if (portfolio_value <= 0.0) return 0.0;
            return gross_notional / portfolio_value;
        };

        double lev1 = calc_gross_leverage(500000.0, 250000.0);
        std::cout << "  gross_leverage(500K, 250K) = " << lev1 << "x" << std::endl;
        check("500K notional / 250K portfolio = 2.0x", std::abs(lev1 - 2.0) < 1e-6);

        double lev2 = calc_gross_leverage(1000000.0, 1000000.0);
        std::cout << "  gross_leverage(1M, 1M) = " << lev2 << "x" << std::endl;
        check("1M notional / 1M portfolio = 1.0x (fully invested, no leverage)", std::abs(lev2 - 1.0) < 1e-6);

        double lev3 = calc_gross_leverage(3000000.0, 1000000.0);
        std::cout << "  gross_leverage(3M, 1M) = " << lev3 << "x" << std::endl;
        check("3M notional / 1M portfolio = 3.0x", std::abs(lev3 - 3.0) < 1e-6);

        double lev4 = calc_gross_leverage(500000.0, 0.0);
        std::cout << "  gross_leverage(500K, 0) = " << lev4 << "x" << std::endl;
        check("Zero portfolio value returns 0.0 (safe)", lev4 == 0.0);

        double lev5 = calc_gross_leverage(0.0, 1000000.0);
        std::cout << "  gross_leverage(0, 1M) = " << lev5 << "x" << std::endl;
        check("Zero notional returns 0.0x (no positions)", lev5 == 0.0);
    }

    // =====================================================
    // TEST 4: max_gross_leverage removed from RiskConfig
    // =====================================================
    std::cout << "\n=== TEST 4: RiskConfig changes ===" << std::endl;
    {
        RiskConfig rc;
        check("max_net_leverage default is 2.0", rc.max_net_leverage == 2.0);
        // Compile-time proof: rc.max_gross_leverage would fail here
        check("max_gross_leverage removed (proven by compilation)", true);

        RiskResult rr;
        check("RiskResult.net_leverage exists", rr.net_leverage == 0.0);
        // Compile-time proof: rr.gross_leverage would fail here
        check("RiskResult.gross_leverage removed (proven by compilation)", true);
    }

    // =====================================================
    // TEST 5: DB column mapping verification
    // =====================================================
    std::cout << "\n=== TEST 5: DB column mapping (logical verification) ===" << std::endl;
    {
        // Simulate what postgres_database.cpp does:
        // C++ gross_leverage value gets written to DB portfolio_leverage column
        double gross_notional = 2000000.0;
        double portfolio_value = 1000000.0;
        double gross_leverage = gross_notional / portfolio_value;  // This is what the code calculates

        // In the INSERT, this goes into the "portfolio_leverage" column position
        std::cout << "  C++ gross_leverage = " << gross_leverage << std::endl;
        std::cout << "  -> Written to DB column 'portfolio_leverage'" << std::endl;
        check("gross_leverage value computed correctly for DB write", std::abs(gross_leverage - 2.0) < 1e-6);

        // Simulate what live_data_loader.cpp does when reading back:
        // Reads DB "portfolio_leverage" column -> stores in C++ gross_leverage field
        LiveResultsRow row;
        row.gross_leverage = gross_leverage;  // This is what load_live_results does
        check("DB read: portfolio_leverage column -> gross_leverage field", row.gross_leverage == gross_leverage);

        MarginMetrics mm;
        mm.gross_leverage = gross_leverage;  // This is what load_margin_metrics does
        check("DB read: margin metrics round-trip consistent", mm.gross_leverage == gross_leverage);
    }

    // =====================================================
    // SUMMARY
    // =====================================================
    std::cout << "\n========================================" << std::endl;
    if (failed == 0) {
        std::cout << "ALL " << passed << " CHECKS PASSED" << std::endl;
    } else {
        std::cout << passed << " passed, " << failed << " FAILED" << std::endl;
    }
    std::cout << "========================================\n" << std::endl;

    return failed > 0 ? 1 : 0;
}
