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
 * Builds the MarketData from the risk window on every lap (on_bars) and runs
 * RiskManager::process_positions on the book (evaluate), printing the loop's
 * "Risk management result:" line exactly as the PortfolioManager did. It registers
 * no Logger component of its own: its RiskManager registers "RiskManager" in the
 * init list, at the point the PortfolioManager's constructor always built it.
 */
class CarverRiskModule final : public RiskModule {
public:
    /// Transitional: reads the PortfolioManager's risk window through a pointer; the PM still
    /// appends to and trims it before on_bars.
    CarverRiskModule(std::string id, RiskConfig config, const std::vector<Bar>* pm_window);

    const std::string& id() const override { return id_; }
    const std::string& type() const override { return type_; }
    std::set<RiskTerm> terms() const override;           ///< {COMPOSITION, MAGNITUDE}
    std::set<RiskAction> capabilities() const override;  ///< {SCALE}
    void on_bars(const std::vector<Bar>& bars, const RiskContext& ctx) override;
    Result<RiskDecision> evaluate(const std::unordered_map<std::string, Position>& book,
                                  const RiskContext& ctx) override;
    nlohmann::json describe() const override;  ///< {"id","type","terms","config"}

    /// SCALE iff r.risk_exceeded, with scale = r.recommended_scale bit for bit; else NONE with
    /// scale 1.0. metrics = r in both cases. Keyed on risk_exceeded, NOT on `scale != 1.0`: a NaN
    /// scale has risk_exceeded false, and SCALE(NaN) would throw in Decimal(double).
    static RiskDecision to_decision(const RiskResult& r, const std::string& module_id);

    const RiskManager& manager() const { return rm_; }
    const std::vector<Bar>& window() const { return *pm_window_; }

private:
    std::string id_;
    std::string type_{"carver"};
    RiskManager rm_;          ///< registers "RiskManager"; this class registers and logs nothing more
    MarketData market_data_;  ///< built by on_bars, read by evaluate on the same lap
    const std::vector<Bar>* pm_window_{nullptr};
};

}  // namespace trade_ngin
