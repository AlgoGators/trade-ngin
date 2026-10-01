#include "trade_ngin/live/equity_sleeve_netting.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

namespace trade_ngin::live {
namespace {

constexpr double kQuantityTolerance = 1e-9;
constexpr double kPriceTolerance = 1e-9;

template <typename T>
Result<T> invalid(const std::string& reason) {
    return make_error<T>(ErrorCode::INVALID_ARGUMENT, reason,
                         "equity_sleeve_netting");
}

bool valid_owner(const std::string& value) {
    return !value.empty() && value.size() <= 100 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= 'A' && ch <= 'Z') ||
                      (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
           });
}

bool same_number(double left, double right, double tolerance) {
    return std::abs(left - right) <=
           tolerance * std::max({1.0, std::abs(left), std::abs(right)});
}

int direction(double quantity) {
    if (quantity > kQuantityTolerance) return 1;
    if (quantity < -kQuantityTolerance) return -1;
    return 0;
}

}  // namespace

Result<EquitySleeveNettingPlan> build_equity_sleeve_netting_plan(
    const std::vector<EquitySleevePositionBook>& sleeves) {
    if (sleeves.empty()) {
        return invalid<EquitySleeveNettingPlan>(
            "At least one live equity sleeve is required");
    }

    std::set<std::string> owners;
    std::map<std::string, double> account_quantities;
    std::map<std::string, double> account_prices;
    EquitySleeveNettingPlan plan;

    for (const auto& sleeve : sleeves) {
        if (!valid_owner(sleeve.strategy_name) ||
            !owners.insert(sleeve.strategy_name).second) {
            return invalid<EquitySleeveNettingPlan>(
                "Sleeve owner identity is invalid or duplicated");
        }

        std::set<std::string> symbols;
        for (const auto& [symbol, quantity] : sleeve.previous_quantities) {
            if (symbol.empty() || !std::isfinite(quantity)) {
                return invalid<EquitySleeveNettingPlan>(
                    "Previous sleeve positions contain invalid data");
            }
            symbols.insert(symbol);
        }
        for (const auto& [symbol, quantity] : sleeve.target_quantities) {
            if (symbol.empty() || !std::isfinite(quantity)) {
                return invalid<EquitySleeveNettingPlan>(
                    "Target sleeve positions contain invalid data");
            }
            symbols.insert(symbol);
        }
        for (const auto& [symbol, price] : sleeve.reference_prices) {
            if (symbol.empty() || !std::isfinite(price) || price <= 0.0) {
                return invalid<EquitySleeveNettingPlan>(
                    "Sleeve reference prices contain invalid data");
            }
        }

        for (const auto& symbol : symbols) {
            const auto previous_it = sleeve.previous_quantities.find(symbol);
            const auto target_it = sleeve.target_quantities.find(symbol);
            const double previous = previous_it == sleeve.previous_quantities.end()
                                        ? 0.0
                                        : previous_it->second;
            const double target = target_it == sleeve.target_quantities.end()
                                      ? 0.0
                                      : target_it->second;
            const double delta = target - previous;
            if (direction(delta) == 0) continue;

            const auto price_it = sleeve.reference_prices.find(symbol);
            if (price_it == sleeve.reference_prices.end()) {
                return invalid<EquitySleeveNettingPlan>(
                    "A nonzero sleeve delta is missing its reference price");
            }
            const auto account_price_it = account_prices.find(symbol);
            if (account_price_it != account_prices.end() &&
                !same_number(account_price_it->second, price_it->second,
                             kPriceTolerance)) {
                return invalid<EquitySleeveNettingPlan>(
                    "Sleeves disagree on the account reference price");
            }
            account_prices[symbol] = price_it->second;
            account_quantities[symbol] += delta;
            plan.logical_fills.push_back({sleeve.strategy_name, symbol, delta,
                                          price_it->second, 0.0, 0.0, 0.0});
        }
    }

    std::sort(plan.logical_fills.begin(), plan.logical_fills.end(),
              [](const auto& left, const auto& right) {
                  if (left.strategy_name != right.strategy_name) {
                      return left.strategy_name < right.strategy_name;
                  }
                  return left.symbol < right.symbol;
              });
    for (const auto& [symbol, quantity] : account_quantities) {
        if (!std::isfinite(quantity)) {
            return invalid<EquitySleeveNettingPlan>(
                "Account net quantity is not finite");
        }
        if (direction(quantity) != 0) {
            plan.account_orders.push_back(
                {symbol, quantity, account_prices.at(symbol)});
        }
    }
    return Result<EquitySleeveNettingPlan>(std::move(plan));
}

Result<std::vector<EquitySleeveLogicalFill>> reconcile_equity_sleeve_costs(
    const EquitySleeveNettingPlan& plan,
    const std::vector<EquitySleeveAsIfCost>& sleeve_costs,
    const std::vector<EquityAccountExecutionCost>& account_executions) {
    std::map<std::string, EquityAccountOrder> expected;
    for (const auto& order : plan.account_orders) {
        if (order.symbol.empty() || !std::isfinite(order.signed_quantity) ||
            direction(order.signed_quantity) == 0 ||
            !std::isfinite(order.reference_price) || order.reference_price <= 0.0 ||
            !expected.emplace(order.symbol, order).second) {
            return invalid<std::vector<EquitySleeveLogicalFill>>(
                "Netting plan contains an invalid account order");
        }
    }

    std::map<std::string, EquityAccountExecutionCost> actual;
    for (const auto& execution : account_executions) {
        if (execution.symbol.empty() || !std::isfinite(execution.signed_quantity) ||
            !std::isfinite(execution.total_transaction_costs) ||
            execution.total_transaction_costs < 0.0 ||
            !actual.emplace(execution.symbol, execution).second) {
            return invalid<std::vector<EquitySleeveLogicalFill>>(
                "Account executions contain invalid or duplicated data");
        }
    }
    if (actual.size() != expected.size()) {
        return invalid<std::vector<EquitySleeveLogicalFill>>(
            "Account executions do not cover the planned net orders");
    }
    for (const auto& [symbol, order] : expected) {
        const auto execution_it = actual.find(symbol);
        if (execution_it == actual.end() ||
            !same_number(execution_it->second.signed_quantity,
                         order.signed_quantity, kQuantityTolerance)) {
            return invalid<std::vector<EquitySleeveLogicalFill>>(
                "Account execution quantity does not match the net order");
        }
    }

    auto attributed = plan.logical_fills;
    std::map<std::pair<std::string, std::string>, std::size_t> logical_by_owner_symbol;
    for (std::size_t index = 0; index < attributed.size(); ++index) {
        auto& fill = attributed[index];
        fill.as_if_transaction_costs = 0.0;
        fill.netting_adjustment = 0.0;
        fill.net_transaction_costs = 0.0;
        if (!logical_by_owner_symbol
                 .emplace(std::make_pair(fill.strategy_name, fill.symbol), index)
                 .second) {
            return invalid<std::vector<EquitySleeveLogicalFill>>(
                "Netting plan duplicates an owner and symbol");
        }
    }
    if (sleeve_costs.size() != attributed.size()) {
        return invalid<std::vector<EquitySleeveLogicalFill>>(
            "As-if costs do not cover every logical sleeve fill");
    }
    std::set<std::pair<std::string, std::string>> seen_costs;
    for (const auto& cost : sleeve_costs) {
        const auto key = std::make_pair(cost.strategy_name, cost.symbol);
        const auto fill_it = logical_by_owner_symbol.find(key);
        if (fill_it == logical_by_owner_symbol.end() ||
            !seen_costs.insert(key).second ||
            !std::isfinite(cost.signed_quantity) ||
            !std::isfinite(cost.as_if_transaction_costs) ||
            cost.as_if_transaction_costs < 0.0 ||
            !same_number(cost.signed_quantity,
                         attributed[fill_it->second].signed_quantity,
                         kQuantityTolerance)) {
            return invalid<std::vector<EquitySleeveLogicalFill>>(
                "As-if sleeve costs do not match the logical fills");
        }
        attributed[fill_it->second].as_if_transaction_costs =
            cost.as_if_transaction_costs;
    }

    std::map<std::string, std::vector<std::size_t>> fills_by_symbol;
    for (std::size_t index = 0; index < attributed.size(); ++index) {
        fills_by_symbol[attributed[index].symbol].push_back(index);
    }
    for (const auto& [symbol, indices] : fills_by_symbol) {
        double gross_as_if_cost = 0.0;
        for (const auto index : indices) {
            gross_as_if_cost += attributed[index].as_if_transaction_costs;
        }
        if (!std::isfinite(gross_as_if_cost)) {
            return invalid<std::vector<EquitySleeveLogicalFill>>(
                "Gross as-if sleeve cost is not finite");
        }
        const auto account_it = actual.find(symbol);
        const double account_cost = account_it == actual.end()
                                        ? 0.0
                                        : account_it->second.total_transaction_costs;
        if (gross_as_if_cost == 0.0) {
            if (account_cost != 0.0) {
                return invalid<std::vector<EquitySleeveLogicalFill>>(
                    "A positive account cost cannot be allocated over zero as-if cost");
            }
            continue;
        }

        const double credit_total = gross_as_if_cost - account_cost;
        double allocated_adjustment = 0.0;
        for (std::size_t offset = 0; offset < indices.size(); ++offset) {
            auto& fill = attributed[indices[offset]];
            if (offset + 1 == indices.size()) {
                fill.netting_adjustment = credit_total - allocated_adjustment;
            } else {
                fill.netting_adjustment =
                    credit_total * fill.as_if_transaction_costs / gross_as_if_cost;
                allocated_adjustment += fill.netting_adjustment;
            }
            fill.net_transaction_costs =
                fill.as_if_transaction_costs - fill.netting_adjustment;
            if (std::abs(fill.net_transaction_costs) <= kQuantityTolerance) {
                fill.net_transaction_costs = 0.0;
            }
        }
    }
    return Result<std::vector<EquitySleeveLogicalFill>>(std::move(attributed));
}

}  // namespace trade_ngin::live
