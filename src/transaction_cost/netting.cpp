// K3, the netting adjustment (T-7b-2 8b). See include/trade_ngin/transaction_cost/netting.hpp.

#include "trade_ngin/transaction_cost/netting.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <sstream>

namespace trade_ngin {
namespace transaction_cost {

namespace {

// |Q| below this is no account order (quantities are whole contracts or shares).
constexpr double kZeroQuantity = 1e-9;

const char* status_name(NettingStatus s) {
    switch (s) {
        case NettingStatus::SINGLE:
            return "SINGLE";
        case NettingStatus::FULL_CROSS:
            return "FULL_CROSS";
        case NettingStatus::NETTED:
            return "NETTED";
        case NettingStatus::MIXED_PRICES:
            return "MIXED_PRICES";
        case NettingStatus::ZERO_COST:
            return "ZERO_COST";
    }
    return "?";
}

std::string num(double v) {
    std::ostringstream ss;
    ss.precision(10);
    ss << v;
    return ss.str();
}

}  // namespace

NettingOutcome net_symbol_day(const std::vector<NettingLeg>& legs, const AccountCostFn& cost_of_q) {
    NettingOutcome out;
    out.adjustment.assign(legs.size(), Decimal());
    for (const auto& l : legs) {
        out.account_quantity += l.signed_quantity;
        out.sum_own_cost += l.own_cost;
    }
    if (legs.size() < 2) {
        out.status = NettingStatus::SINGLE;
        out.account_cost = out.sum_own_cost;
        return out;
    }
    for (const auto& l : legs) {
        if (l.fill_price != legs.front().fill_price) {
            out.status = NettingStatus::MIXED_PRICES;
            return out;
        }
    }

    // Q = 0: the account sends no order and pays nothing. C(0) = 0 by definition; the cost model
    // is not called, so no per-order minimum can apply. Each row is credited its own cost exactly.
    if (std::abs(out.account_quantity) < kZeroQuantity) {
        out.account_quantity = 0.0;
        out.status = NettingStatus::FULL_CROSS;
        for (size_t i = 0; i < legs.size(); ++i) out.adjustment[i] = legs[i].own_cost;
        out.credit_total = out.sum_own_cost;
        return out;
    }

    out.account_cost = Decimal(cost_of_q(out.account_quantity, legs.front().fill_price));
    const int64_t weight_total = out.sum_own_cost.raw_value();
    if (weight_total <= 0) {
        out.status = NettingStatus::ZERO_COST;
        return out;
    }
    out.status = NettingStatus::NETTED;
    const int64_t credit = out.sum_own_cost.raw_value() - out.account_cost.raw_value();
    out.credit_total = Decimal::from_raw(credit);

    // credit_total split pro rata to C(q_i) in whole 1e-8 units: the floor of each exact quota,
    // then one unit to each of the largest remainders (a tie to the smaller sleeve name) until the
    // parts sum to |credit_total|; the sign is applied after, as S2's split does for a short book.
    using u128 = unsigned __int128;
    const u128 magnitude = static_cast<u128>(credit < 0 ? -static_cast<__int128>(credit)
                                                        : static_cast<__int128>(credit));
    const u128 total = static_cast<u128>(weight_total);
    std::vector<int64_t> part(legs.size(), 0);
    std::vector<u128> remainder(legs.size(), 0);
    u128 assigned = 0;
    for (size_t i = 0; i < legs.size(); ++i) {
        const u128 w = static_cast<u128>(std::max<int64_t>(legs[i].own_cost.raw_value(), 0));
        const u128 exact = magnitude * w;
        part[i] = static_cast<int64_t>(exact / total);
        remainder[i] = exact % total;
        assigned += exact / total;
    }
    std::vector<size_t> order(legs.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (remainder[a] != remainder[b]) return remainder[a] > remainder[b];
        return legs[a].sleeve < legs[b].sleeve;
    });
    const auto extra = static_cast<size_t>(magnitude - assigned);  // fewer than the legs
    for (size_t k = 0; k < extra; ++k) part[order[k % order.size()]] += 1;
    for (size_t i = 0; i < legs.size(); ++i)
        out.adjustment[i] = Decimal::from_raw(credit < 0 ? -part[i] : part[i]);
    return out;
}

NettingReport apply_netting_adjustments(
    std::vector<SleeveExecution>& rows,
    const std::function<double(const std::string&, double, double)>& cost_of) {
    NettingReport rep;
    std::map<std::string, std::vector<size_t>> by_symbol;  // ordered: a deterministic log order
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].report) continue;
        rows[i].report->netting_adjustment = Decimal();
        by_symbol[rows[i].report->symbol].push_back(i);
    }
    for (const auto& [symbol, idx] : by_symbol) {
        if (idx.size() < 2) continue;
        std::vector<NettingLeg> legs;
        legs.reserve(idx.size());
        for (size_t i : idx) {
            const auto& r = *rows[i].report;
            const double q = static_cast<double>(r.filled_quantity);
            legs.push_back({rows[i].sleeve, r.side == Side::SELL ? -q : q,
                            static_cast<double>(r.fill_price), r.total_transaction_costs});
        }
        const std::string sym = symbol;
        const NettingOutcome out =
            net_symbol_day(legs, [&](double q, double px) { return cost_of(sym, q, px); });
        for (size_t k = 0; k < idx.size(); ++k)
            rows[idx[k]].report->netting_adjustment = out.adjustment[k];

        std::ostringstream line;
        line << (out.status == NettingStatus::MIXED_PRICES ? "NETTING_MIXED_PRICES" : "NETTING")
             << " sym=" << symbol << " status=" << status_name(out.status)
             << " Q=" << num(out.account_quantity) << " C(Q)=" << out.account_cost.to_string()
             << " sum_C(q_i)=" << out.sum_own_cost.to_string()
             << " credit_total=" << out.credit_total.to_string() << " rows:";
        for (size_t k = 0; k < idx.size(); ++k) {
            const auto& r = *rows[idx[k]].report;
            line << " " << legs[k].sleeve << " q=" << num(legs[k].signed_quantity)
                 << " px=" << num(legs[k].fill_price)
                 << " C=" << r.total_transaction_costs.to_string()
                 << " adj=" << out.adjustment[k].to_string()
                 << " net=" << (r.total_transaction_costs - out.adjustment[k]).to_string();
        }
        if (out.status == NettingStatus::MIXED_PRICES) {
            line << " (rows at different fill prices are not netted)";
            rep.warn_lines.push_back(line.str());
        } else {
            rep.info_lines.push_back(line.str());
            ++rep.symbol_days_netted;
        }
    }
    return rep;
}

double add_net_costs(double running, const std::vector<ExecutionReport>& fills, size_t from) {
    for (size_t i = from; i < fills.size(); ++i) running += static_cast<double>(net_cost(fills[i]));
    return running;
}

}  // namespace transaction_cost
}  // namespace trade_ngin
