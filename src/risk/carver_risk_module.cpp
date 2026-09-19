// src/risk/carver_risk_module.cpp
#include "trade_ngin/risk/carver_risk_module.hpp"
#include <chrono>
#include <cstdint>
#include <set>
#include <unordered_map>
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

CarverRiskModule::CarverRiskModule(std::string id, RiskConfig config, int min_gate_dates)
    : id_(std::move(id)), rm_(std::move(config)), min_gate_dates_(min_gate_dates) {}

int CarverRiskModule::complete_dates_in_window() const {
    if (window_.empty()) return 0;
    // One pass: each symbol gets a bit, each date a mask of the symbols that printed on
    // it. A date is complete when its mask holds every symbol the window has seen.
    // Above 64 symbols the mask cannot hold them all, so the count falls back to the
    // exact set comparison rather than quietly reporting the wrong number.
    std::unordered_map<std::string, size_t> symbol_index;
    std::unordered_map<int64_t, uint64_t> mask_by_date;
    std::unordered_map<int64_t, std::set<std::string>> set_by_date;
    bool wide = false;
    for (const auto& bar : window_) {
        const auto key = std::chrono::duration_cast<std::chrono::seconds>(
                             bar.timestamp.time_since_epoch())
                             .count();
        auto [it, inserted] = symbol_index.emplace(bar.symbol, symbol_index.size());
        (void)inserted;
        if (it->second >= 64) wide = true;
        if (wide) {
            set_by_date[key].insert(bar.symbol);
        } else {
            mask_by_date[key] |= (uint64_t{1} << it->second);
        }
    }
    const size_t symbols = symbol_index.size();
    int complete = 0;
    if (wide) {
        // Rebuild the per-date sets from scratch: the masks collected before the 65th
        // symbol appeared are incomplete.
        set_by_date.clear();
        for (const auto& bar : window_) {
            const auto key = std::chrono::duration_cast<std::chrono::seconds>(
                                 bar.timestamp.time_since_epoch())
                                 .count();
            set_by_date[key].insert(bar.symbol);
        }
        for (const auto& [date, syms] : set_by_date) {
            (void)date;
            if (syms.size() == symbols) ++complete;
        }
        return complete;
    }
    const uint64_t all = symbols >= 64 ? ~uint64_t{0} : ((uint64_t{1} << symbols) - 1);
    for (const auto& [date, mask] : mask_by_date) {
        (void)date;
        if (mask == all) ++complete;
    }
    return complete;
}

std::set<RiskTerm> CarverRiskModule::terms() const {
    return {RiskTerm::COMPOSITION, RiskTerm::MAGNITUDE};
}

std::set<RiskAction> CarverRiskModule::capabilities() const {
    return {RiskAction::SCALE};
}

void CarverRiskModule::begin_rebalance(const RiskContext& ctx) {
    (void)ctx;
    appended_this_rebalance_ = false;
    applied_level_ = 1.0;
    last_requested_ = 1.0;
}

void CarverRiskModule::on_bars(const std::vector<Bar>& bars, const RiskContext& ctx) {
    (void)ctx;
    for (auto const& bar : bars) {
        window_.push_back(bar);
    }
    size_t lookback = rm_.get_config().lookback_period;
    if (window_.size() > lookback) {
        // keep only the last 'lookback' bars
        window_.erase(window_.begin(), window_.end() - static_cast<long>(lookback));
    }
    appended_this_rebalance_ = true;
    market_data_ = rm_.create_market_data(window_);
}

RiskDecision CarverRiskModule::to_decision(const RiskResult& r, const std::string& module_id) {
    RiskDecision d;
    d.module_id = module_id;
    if (r.risk_exceeded) {
        d.action = RiskAction::SCALE;
        d.scale = r.recommended_scale;
    } else {
        d.action = RiskAction::NONE;
        d.scale = 1.0;
    }
    d.metrics = r;
    return d;
}

Result<RiskDecision> CarverRiskModule::evaluate(
    const std::unordered_map<std::string, Position>& book, const RiskContext& ctx) {
    (void)ctx;
    auto result = rm_.process_positions(book, market_data_, {});
    if (result.is_error()) {
        return make_error<RiskDecision>(result.error()->code(), result.error()->what(),
                                        "RiskManager");
    }

    const auto& risk_result = result.value();
    INFO("Risk management result: risk_exceeded=" + std::to_string(risk_result.risk_exceeded) +
         ", scale=" + std::to_string(risk_result.recommended_scale) +
         ", portfolio_mult=" + std::to_string(risk_result.portfolio_multiplier) +
         ", jump_mult=" + std::to_string(risk_result.jump_multiplier) +
         ", correlation_mult=" + std::to_string(risk_result.correlation_multiplier) +
         ", leverage_mult=" + std::to_string(risk_result.leverage_multiplier));

    RiskDecision decision = to_decision(risk_result, id_);
    last_requested_ = decision.scale;
    // Blind: the same tests RiskManager's own early returns make, recorded as data only.
    bool mapped = false;
    for (const auto& [symbol, pos] : book) {
        (void)pos;
        if (market_data_.symbol_indices.count(symbol)) {
            mapped = true;
            break;
        }
    }
    // ... plus the two the schema names (HD, LEAD_RULINGS_C7 item 3): too few complete
    // dates to estimate anything from, and a book with no capital to divide by. With
    // lookback_unit "bars" a 252-BAR window is about 7 futures dates, so this flag is
    // true on most futures laps -- which is the finding T-4 made, stated rather than
    // hidden. Data only: it is not logged and not stored in T-6a (T-7 item 10 stores it).
    decision.blind = market_data_.returns.empty() || market_data_.covariance.empty() ||
                     market_data_.symbol_indices.empty() || market_data_.ordered_symbols.empty() ||
                     !mapped || complete_dates_in_window() < min_gate_dates_ ||
                     static_cast<double>(rm_.get_config().capital) <= 0.0;
    return Result<RiskDecision>(std::move(decision));
}

void CarverRiskModule::on_applied(const RiskApplied& applied, const RiskContext& ctx) {
    (void)ctx;
    if (applied.action == RiskAction::SCALE) {
        applied_level_ *= static_cast<double>(applied.factor);
    }
}

nlohmann::json CarverRiskModule::describe() const {
    nlohmann::json terms_json = nlohmann::json::array();
    for (auto t : terms()) terms_json.push_back(risk_term_name(t));
    return nlohmann::json{
        {"id", id_}, {"type", type_}, {"terms", terms_json}, {"config", rm_.get_config().to_json()}};
}

}  // namespace trade_ngin
