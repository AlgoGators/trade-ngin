#pragma once
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include "dataframe.hpp"
#include "instrument.hpp"
#include "strategy.hpp"
#include "risk_measure.hpp"

class Portfolio {
public:
    Portfolio();

    void set_instruments(std::vector<Instrument*>* instruments);
    void set_weighted_strategies(const std::vector<std::pair<double, Strategy*>>& ws);
    void set_capital(double c);
    void set_risk_object(RiskMeasure* r);
    void add_portfolio_rule(std::function<DataFrame(const Portfolio&)> rule);

    DataFrame multipliers();
    DataFrame prices();
    DataFrame positions();
    DataFrame exposure();
    PnL getPnL();

private:
    std::vector<Instrument*>* instruments_;
    std::vector<std::pair<double, Strategy*>> weighted_strategies_;
    double capital_;
    RiskMeasure* risk_object_;
    std::vector<std::function<DataFrame(const Portfolio&)>> portfolio_rules_;

    std::optional<DataFrame> multipliers_;
    std::optional<DataFrame> prices_;
    std::optional<DataFrame> positions_;

    DataFrame multiply_df_by_row(const DataFrame& df, const DataFrame& row);
    DataFrame multiply_dataframes(const DataFrame& a, const DataFrame& b);
};
