#include "portfolio.hpp"
#include <stdexcept>
#include <unordered_map>
#include <algorithm>

Portfolio::Portfolio() : instruments_(nullptr), capital_(0.0), risk_object_(nullptr) {}

void Portfolio::set_instruments(std::vector<Instrument*>* instruments) {
    instruments_ = instruments;
}''

void Portfolio::set_weighted_strategies(const std::vector<std::pair<double, Strategy*>>& ws) {
    weighted_strategies_ = ws;
}

void Portfolio::set_capital(double c) {
    capital_ = c;
}

void Portfolio::set_risk_object(RiskMeasure* r) {
    risk_object_ = r;
}

void Portfolio::add_portfolio_rule(std::function<DataFrame(const Portfolio&)> rule) {
    portfolio_rules_.push_back(rule);
}

DataFrame Portfolio::multipliers() {
    if (!multipliers_.has_value()) {
        if (!instruments_ || instruments_->empty()) {
            throw std::runtime_error("No instruments in the portfolio");
        }
        std::unordered_map<std::string, std::vector<double>> m;
        for (auto* inst : *instruments_) {
            m[inst->name()] = {inst->multiplier()};
        }
        multipliers_ = DataFrame(m);
    }
    return multipliers_.value();
}

DataFrame Portfolio::prices() {
    if (!prices_.has_value()) {
        if (!instruments_ || instruments_->empty()) {
            throw std::runtime_error("No instruments set");
        }
        DataFrame df;
        for (auto* inst : *instruments_) {
            std::vector<double> p = inst->price();
            DataFrame single_col({{inst->name(), p}});
            if (df.empty()) df = single_col;
            else df = df.join(single_col);
        }
        prices_ = df;
    }
    return prices_.value();
}

DataFrame Portfolio::positions() {
    if (!positions_.has_value()) {
        if (weighted_strategies_.empty()) {
            throw std::runtime_error("No strategies set");
        }
        DataFrame combined;
        for (auto& w_s : weighted_strategies_) {
            double w = w_s.first;
            Strategy* s = w_s.second;
            DataFrame strat_pos = s->positions();

            std::unordered_map<std::string, std::vector<double>> row_map;
            for (auto& col : strat_pos.columns()) {
                row_map[col] = {w};
            }
            DataFrame weight_row(row_map);
            DataFrame weighted_pos = multiply_df_by_row(strat_pos, weight_row);

            if (combined.empty()) combined = weighted_pos;
            else combined = combined.add(weighted_pos);
        }

        auto pos_div = combined.div_row(multipliers());

        for (auto& rule : portfolio_rules_) {
            pos_div = rule(*this);
        }

        positions_ = pos_div;
    }
    return positions_.value();
}

DataFrame Portfolio::exposure() {
    DataFrame pos = positions();
    DataFrame prc = prices();
    DataFrame mult = multipliers();
    DataFrame tmp = multiply_dataframes(pos, prc);
    return tmp.mul_row(mult);
}

PnL Portfolio::getPnL() {
    return PnL(positions(), prices(), capital_, multipliers());
}

DataFrame Portfolio::multiply_df_by_row(const DataFrame& df, const DataFrame& row) {
    return df.mul_row(row);
}

DataFrame Portfolio::multiply_dataframes(const DataFrame& a, const DataFrame& b) {
    if (a.rows() != b.rows() || a.cols() != b.cols()) 
        throw std::runtime_error("Shape mismatch in multiply_dataframes");
    for (size_t i = 0; i < a.cols(); ++i) {
        if (a.columns()[i] != b.columns()[i])
            throw std::runtime_error("Column name mismatch in multiply_dataframes");
    }
    std::unordered_map<std::string, std::vector<double>> result_map;
    for (size_t c = 0; c < a.cols(); ++c) {
        const auto& col_name = a.columns()[c];
        auto ac = a.get_column(col_name);
        auto bc = b.get_column(col_name);
        std::vector<double> col(a.rows());
        for (size_t r = 0; r < a.rows(); ++r) {
            col[r] = ac[r] * bc[r];
        }
        result_map[col_name] = col;
    }
    return DataFrame(result_map);
}
