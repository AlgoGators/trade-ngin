#pragma once

// A sleeve for the one pass's portfolio-level tests (LOOP_SPEC sections 4 to 6): a BaseStrategy
// whose per-symbol rows the test writes directly, published through the same three calls the trend
// sleeve answers (get_target_positions, overlay_series, is_signalling). The portfolio manager sees
// nothing else of a sleeve, so a book built on this stub runs the real one pass.

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../risk/risk_module_test_helpers.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {
namespace testing {

inline Timestamp one_pass_day(int d) {
    return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d));  // 2026-01-01 + d days
}

inline Bar one_pass_bar(const std::string& symbol, int d, double close, double volume = 100000.0) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = one_pass_day(d);
    b.open = Decimal(close);
    b.high = Decimal(close * 1.01);
    b.low = Decimal(close * 0.99);
    b.close = Decimal(close);
    b.volume = volume;
    return b;
}

class OverlayStubStrategy : public BaseStrategy {
public:
    struct Row {
        double close{100.0};
        double multiplier{1.0};
        double optimal{0.0};    ///< N*, the sleeve's unrounded position
        double forecast{10.0};  ///< the sleeve's ruled forecast
        bool signalling{true};
        bool slow_rule_zeroed{false};
        std::vector<double> day;      ///< the overlay window's dates (whole day numbers)
        std::vector<double> returns;  ///< its adjusted percentage returns
        double jump_sigma_daily{0.0};  ///< the jump sigma as a daily standard deviation
        /// The optimiser's closes, when a test gives them itself (dates, raw closes; the adjusted
        /// level is the raw close). Empty: built from the window, one close per window date.
        std::vector<double> opt_day;
        std::vector<double> opt_close;
    };

    OverlayStubStrategy(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)) {
        metadata_.name = "Overlay Stub Strategy";
    }

    std::map<std::string, Row> rows;
    bool fail_on_data{false};

    /// `n` window dates ending at day number `last`, a small alternating return each, the same on
    /// every symbol that takes it (so the window is complete and the readings are finite).
    static void fill_window(Row& row, int n, double last = 20500.0, double scale = 0.01) {
        row.day.clear();
        row.returns.clear();
        for (int k = 0; k < n; ++k) {
            row.day.push_back(last - (n - 1 - k));
            row.returns.push_back(scale * ((k % 2 == 0) ? 1.0 : -0.9) * (1.0 + 0.1 * (k % 5)));
        }
    }

    Result<void> on_data(const std::vector<Bar>& data) override {
        (void)data;
        if (fail_on_data) {
            return make_error<void>(ErrorCode::STRATEGY_ERROR, "scripted failure", "OverlayStub");
        }
        return Result<void>();
    }

    std::unordered_map<std::string, Position> get_target_positions() const override {
        std::unordered_map<std::string, Position> out;
        for (const auto& [symbol, row] : rows) {
            Position p;
            p.symbol = symbol;
            // the listed quantity is a Decimal; a sleeve whose N* is not a number still lists the symbol
            p.quantity = Decimal(row.signalling && std::isfinite(row.optimal) ? row.optimal : 0.0);
            p.average_price = Decimal(row.close);
            p.last_update = one_pass_day(0);
            out[symbol] = p;
        }
        return out;
    }

    bool is_signalling(const std::string& symbol) const override {
        const auto it = rows.find(symbol);
        return it != rows.end() && it->second.signalling;
    }

    bool overlay_series(const std::string& symbol, OverlaySeries* out) const override {
        const auto it = rows.find(symbol);
        if (it == rows.end() || out == nullptr) return false;
        const Row& row = it->second;
        out->day = row.day;
        out->returns = row.returns;
        out->close = row.close;
        out->multiplier = row.multiplier;
        out->jump_sigma_daily = row.jump_sigma_daily;
        out->optimal_position = row.optimal;
        out->forecast = row.forecast;
        out->signalling = row.signalling;
        out->slow_rule_zeroed = row.slow_rule_zeroed;
        // The optimiser's closes: the window's dates with a level built from the returns.
        out->opt_day = row.day;
        out->opt_close.clear();
        out->opt_level.clear();
        double level = row.close;
        std::vector<double> levels(row.day.size(), row.close);
        for (size_t k = row.day.size(); k-- > 1;) {
            levels[k] = level;
            level = level / (1.0 + row.returns[k]);
        }
        if (!levels.empty()) levels[0] = level;
        out->opt_close = levels;
        out->opt_level = levels;
        if (!row.opt_day.empty()) {
            out->opt_day = row.opt_day;
            out->opt_close = row.opt_close;
            out->opt_level = row.opt_close;
        }
        return true;
    }
};

/// A book that names `overlay_sleeve` as its overlay sleeve: one carver module carrying the
/// overlay's limits (section 12's ratios and leverage limits), tau 0.20, cap 2, cost multiplier
/// 100, band 2, floor 0.05, trim 5.
inline PortfolioConfig one_pass_config(const std::string& overlay_sleeve, double capital = 500000.0) {
    PortfolioConfig pc{capital, 1.0, 0.0, /*optimization=*/true};
    pc.allow_fractional_positions = false;
    pc.overlay_sleeve = overlay_sleeve;
    pc.overlay_tau = 0.20;
    pc.opt_config.capital = capital;
    pc.opt_config.cost_penalty_scalar = 100.0;
    pc.risk_config.capital = capital;
    pc.risk_config.max_gross_leverage = 8.0;
    pc.risk_config.max_net_leverage = 6.0;
    RiskModuleConfig module = test_carver_module(pc.risk_config);
    auto& carver = std::get<CarverModuleConfig>(module.params);
    carver.r_max = 2.25;
    carver.r_jump_max = 4.5;
    carver.r_shock_max = 4.0;
    carver.per_name_cap = 2.0;
    carver.trim_max = 5;
    pc.risk_modules = {module};
    return pc;
}

inline std::shared_ptr<OverlayStubStrategy> make_overlay_stub(
    const std::string& id, double capital, std::shared_ptr<DatabaseInterface> db) {
    StrategyConfig sc;
    sc.capital_allocation = capital;
    sc.max_leverage = 10.0;
    sc.asset_classes = {AssetClass::FUTURES};
    sc.frequencies = {DataFrequency::DAILY};
    return std::make_shared<OverlayStubStrategy>(id, sc, std::move(db));
}

}  // namespace testing
}  // namespace trade_ngin
