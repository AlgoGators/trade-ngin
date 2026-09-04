#include "trade_ngin/live/corporate_actions_lifecycle.hpp"

#include <cmath>

#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

const char* CorporateActionsLifecycle::outcome_to_string(LifecycleOutcome o) {
    switch (o) {
        case LifecycleOutcome::EXITED_AT_FINAL_CLOSE: return "EXITED_AT_FINAL_CLOSE";
        case LifecycleOutcome::CONVERTED_TO_CONTRA:   return "CONVERTED_TO_CONTRA";
        case LifecycleOutcome::RENAMED:               return "RENAMED";
        case LifecycleOutcome::SKIPPED_NO_POSITION:   return "SKIPPED_NO_POSITION";
        case LifecycleOutcome::SKIPPED_NO_PRICE:      return "SKIPPED_NO_PRICE";
    }
    return "SKIPPED_NO_POSITION";
}

std::vector<LifecycleAdjustment> CorporateActionsLifecycle::apply_renames(
    std::unordered_map<std::string, Position>& positions,
    const std::vector<TickerAlias>& aliases,
    const std::string& as_of_date) {

    std::vector<LifecycleAdjustment> log;

    for (const auto& alias : aliases) {
        if (alias.historical_ticker.empty() || alias.current_symbol.empty()) continue;
        if (alias.historical_ticker == alias.current_symbol) continue;

        // The rename is only in effect once we are past effective_until (the
        // last date the old ticker was valid). An empty effective_until means
        // open-ended: treat the rename as already in force.
        if (!alias.effective_until.empty() && !as_of_date.empty() &&
            as_of_date <= alias.effective_until) {
            continue;  // YYYY-MM-DD sorts lexicographically
        }

        auto old_it = positions.find(alias.historical_ticker);
        if (old_it == positions.end()) continue;  // not held under the old key

        LifecycleAdjustment adj;
        adj.symbol = alias.historical_ticker;
        adj.event_date = alias.effective_until;
        adj.vendor_label = "tickerchangeto";
        adj.action_class = CorpActionClass::SERIES_CONTINUITY;
        adj.outcome = LifecycleOutcome::RENAMED;
        adj.quantity_before = old_it->second.quantity.as_double();
        adj.contra_ticker = alias.current_symbol;

        auto new_it = positions.find(alias.current_symbol);
        if (new_it == positions.end()) {
            // Simple re-key: same holding, new name. Cost basis untouched --
            // a rename is not an economic event.
            Position moved = old_it->second;
            moved.symbol = alias.current_symbol;
            positions.erase(old_it);
            positions.emplace(alias.current_symbol, std::move(moved));
            adj.quantity_after = adj.quantity_before;
        } else {
            // Both keys present: the same holding recorded twice across the
            // rename boundary. Merge into the current symbol with a
            // quantity-weighted average cost so neither leg's basis is lost.
            Position& dest = new_it->second;
            const double q_old = adj.quantity_before;
            const double q_new = dest.quantity.as_double();
            const double p_old = old_it->second.average_price.as_double();
            const double p_new = dest.average_price.as_double();
            const double q_sum = q_old + q_new;

            if (std::abs(q_sum) > 1e-9) {
                dest.average_price = Decimal((q_old * p_old + q_new * p_new) / q_sum);
            }
            dest.quantity = Quantity(q_sum);
            dest.realized_pnl = Decimal(dest.realized_pnl.as_double() +
                                        old_it->second.realized_pnl.as_double());
            positions.erase(old_it);
            adj.quantity_after = q_sum;
        }

        INFO("Corp action SERIES_CONTINUITY: " + alias.historical_ticker + " -> " +
             alias.current_symbol + " (effective_until " +
             (alias.effective_until.empty() ? std::string("open") : alias.effective_until) +
             "), qty " + std::to_string(adj.quantity_before) + " -> " +
             std::to_string(adj.quantity_after));
        log.push_back(std::move(adj));
    }

    return log;
}

std::vector<LifecycleAdjustment> CorporateActionsLifecycle::apply_terminations(
    std::unordered_map<std::string, Position>& positions,
    const std::vector<TerminationEvent>& events,
    const std::unordered_map<std::string, double>& final_closes) {

    std::vector<LifecycleAdjustment> log;
    log.reserve(events.size());

    for (const auto& ev : events) {
        LifecycleAdjustment adj;
        adj.symbol = ev.symbol;
        adj.event_date = ev.event_date;
        adj.vendor_label = ev.vendor_label;
        adj.action_class = CorpActionClass::TERMINATION;
        adj.contra_ticker = ev.contra_ticker;

        auto it = positions.find(ev.symbol);
        if (it == positions.end()) {
            adj.outcome = LifecycleOutcome::SKIPPED_NO_POSITION;
            continue;  // not held; nothing to do and nothing worth logging
        }

        Position& pos = it->second;
        const double qty_before = pos.quantity.as_double();
        adj.quantity_before = qty_before;

        if (std::abs(qty_before) < 1e-9) {
            adj.outcome = LifecycleOutcome::SKIPPED_NO_POSITION;
            continue;
        }

        const bool terms_usable = ev.has_terms && !ev.contra_ticker.empty() &&
                                  ev.ratio > 0.0 && std::isfinite(ev.ratio);

        if (terms_usable) {
            // Stock-for-stock: roll into the successor. Cost basis divides by
            // the same ratio the share count multiplies by, so total basis --
            // and therefore P&L at the moment of conversion -- is unchanged.
            const double qty_after = qty_before * ev.ratio;
            const double avg_before = pos.average_price.as_double();
            const double avg_after = avg_before > 0.0 ? avg_before / ev.ratio : 0.0;

            Position rolled = pos;
            rolled.symbol = ev.contra_ticker;
            rolled.quantity = Quantity(qty_after);
            if (avg_before > 0.0) rolled.average_price = Decimal(avg_after);

            positions.erase(it);
            auto existing = positions.find(ev.contra_ticker);
            if (existing == positions.end()) {
                positions.emplace(ev.contra_ticker, std::move(rolled));
            } else {
                // Already hold the acquirer: merge at weighted-average cost.
                Position& dest = existing->second;
                const double q_dest = dest.quantity.as_double();
                const double p_dest = dest.average_price.as_double();
                const double q_sum = q_dest + qty_after;
                if (std::abs(q_sum) > 1e-9) {
                    dest.average_price =
                        Decimal((q_dest * p_dest + qty_after * avg_after) / q_sum);
                }
                dest.quantity = Quantity(q_sum);
                dest.realized_pnl =
                    Decimal(dest.realized_pnl.as_double() + rolled.realized_pnl.as_double());
            }

            adj.outcome = LifecycleOutcome::CONVERTED_TO_CONTRA;
            adj.quantity_after = qty_after;
            INFO("Corp action TERMINATION/" + ev.vendor_label + ": " + ev.symbol +
                 " on " + ev.event_date + " converted to " + std::to_string(ev.ratio) +
                 " x " + ev.contra_ticker + ", qty " + std::to_string(qty_before) +
                 " -> " + std::to_string(qty_after));
            log.push_back(std::move(adj));
            continue;
        }

        // No usable terms. Exit at the final close -- exactly right for cash
        // deals, an approximation of the post-event path for stock deals.
        auto price_it = final_closes.find(ev.symbol);
        if (price_it == final_closes.end() || !(price_it->second > 0.0) ||
            !std::isfinite(price_it->second)) {
            adj.outcome = LifecycleOutcome::SKIPPED_NO_PRICE;
            WARN("Corp action TERMINATION/" + ev.vendor_label + " for " + ev.symbol +
                 " on " + ev.event_date + ": no deal terms (corporate_action feed frozen "
                 "after " + std::string(kCorpActionTableFrozenAfter) +
                 ") AND no final close available -- position left untouched, "
                 "operator review required");
            log.push_back(std::move(adj));
            continue;
        }

        const double exit_price = price_it->second;
        const double avg_price = pos.average_price.as_double();
        const double realized_delta = (exit_price - avg_price) * qty_before;

        pos.realized_pnl = Decimal(pos.realized_pnl.as_double() + realized_delta);
        pos.unrealized_pnl = Decimal(0.0);
        pos.quantity = Quantity(0.0);

        adj.outcome = LifecycleOutcome::EXITED_AT_FINAL_CLOSE;
        adj.quantity_after = 0.0;
        adj.exit_price = exit_price;
        adj.realized_delta = realized_delta;

        WARN("Corp action TERMINATION/" + ev.vendor_label + " for " + ev.symbol +
             " on " + ev.event_date + ": deal terms unavailable (corporate_action feed "
             "frozen after " + std::string(kCorpActionTableFrozenAfter) +
             ") -- exiting " + std::to_string(qty_before) + " shares at final close " +
             std::to_string(exit_price) + " (realized " + std::to_string(realized_delta) +
             "). Correct for a cash deal; a stock-for-stock deal would instead roll into "
             "the successor once terms are available.");
        log.push_back(std::move(adj));
    }

    return log;
}

}  // namespace trade_ngin
