// include/trade_ngin/risk/carver_risk_module.hpp
#pragma once

#include <string>
#include <vector>
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/risk/risk_module.hpp"

namespace trade_ngin {

/**
 * @brief The Carver risk gate as a RiskModule: wraps the unchanged RiskManager.
 *
 * Owns the risk window (the last lookback_period bars appended; it was the
 * PortfolioManager's risk_history_). On every lap on_bars appends that lap's bars,
 * trims the window and builds the MarketData from it; evaluate then runs
 * RiskManager::process_positions on the book, printing the loop's
 * "Risk management result:" line exactly as the PortfolioManager did. It registers
 * no Logger component of its own: its RiskManager registers "RiskManager" in the
 * init list, at the point the PortfolioManager's constructor always built it.
 */
class CarverRiskModule final : public RiskModule {
public:
    CarverRiskModule(std::string id, RiskConfig config);

    const std::string& id() const override { return id_; }
    const std::string& type() const override { return type_; }
    std::set<RiskTerm> terms() const override;           ///< {COMPOSITION, MAGNITUDE}
    std::set<RiskAction> capabilities() const override;  ///< {SCALE}
    /// Resets the per-rebalance state (the appended flag, the applied level and the last
    /// request). Never clears the window: it spans rebalances.
    void begin_rebalance(const RiskContext& ctx) override;
    /// Appends this lap's bars to the window, trims it to lookback_period and builds the
    /// MarketData. Runs on every lap, including a lap whose book is empty.
    void on_bars(const std::vector<Bar>& bars, const RiskContext& ctx) override;
    Result<RiskDecision> evaluate(const std::unordered_map<std::string, Position>& book,
                                  const RiskContext& ctx) override;
    nlohmann::json describe() const override;  ///< {"id","type","terms","config"}

    /// SCALE iff r.risk_exceeded, with scale = r.recommended_scale bit for bit; else NONE with
    /// scale 1.0. metrics = r in both cases. Keyed on risk_exceeded, NOT on `scale != 1.0`: a NaN
    /// scale has risk_exceeded false, and SCALE(NaN) would throw in Decimal(double).
    static RiskDecision to_decision(const RiskResult& r, const std::string& module_id);

    const RiskManager& manager() const { return rm_; }
    const std::vector<Bar>& window() const { return window_; }
    bool appended_this_rebalance() const { return appended_this_rebalance_; }
    double applied_level() const { return applied_level_; }
    double last_requested() const { return last_requested_; }

private:
    std::string id_;
    std::string type_{"carver"};
    RiskManager rm_;          ///< registers "RiskManager"; this class registers and logs nothing more
    MarketData market_data_;  ///< built by on_bars, read by evaluate on the same lap
    std::vector<Bar> window_;  ///< was PortfolioManager::risk_history_
    // Per-rebalance state, reset by begin_rebalance. READ BY NOTHING yet: the risk loop still
    // appends the same bars on every lap and applies the gate's scale as a per-lap rate. The
    // once-per-rebalance append and the level cut that will read these land in T-6b (commit 9).
    bool appended_this_rebalance_{false};  ///< set by on_bars
    double applied_level_{1.0};            ///< product of the factors applied this rebalance
    double last_requested_{1.0};           ///< the scale evaluate last requested this rebalance
};

}  // namespace trade_ngin
