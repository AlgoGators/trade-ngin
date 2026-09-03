#include "trade_ngin/live/corporate_actions_lifecycle.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

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
    const std::string& as_of_date,
    const std::unordered_map<std::string, std::string>& position_inception) {

    std::vector<LifecycleAdjustment> log;

    // Group by historical ticker, ascending by effective_until. ISO YYYY-MM-DD
    // compares lexicographically, so a plain sort orders the eras.
    //
    // An alias with no effective_until cannot be era-bounded, and class 2 fails
    // narrow: it is dropped rather than applied unconditionally. That is the same
    // rule the dedup mirror uses, and it matters because an unbounded alias is
    // exactly the shape that re-keys a currently-trading ticker.
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> renames;
    for (const auto& a : aliases) {
        if (a.historical_ticker.empty() || a.current_symbol.empty()) continue;
        if (a.historical_ticker == a.current_symbol) continue;
        if (a.effective_until.empty()) {
            WARN("Corp action SERIES_CONTINUITY: alias " + a.historical_ticker + " -> " +
                 a.current_symbol + " has no effective_until; cannot era-bound it, skipping");
            continue;
        }
        renames[a.historical_ticker].emplace_back(a.effective_until, a.current_symbol);
    }
    if (renames.empty()) return log;
    for (auto& entry : renames) {
        std::sort(entry.second.begin(), entry.second.end());
    }

    // The successor a ticker had at `date`: the first rename on or after it.
    // A date later than every rename maps NOWHERE -- the ticker belongs to
    // whoever holds it now, not to the old company.
    //
    // The compare is inclusive on purpose. effective_until conventions differ by
    // a day between curated rows (day-after) and backfilled rows (source date),
    // so on the boundary day `<=` errs toward NOT applying a backfilled rename.
    // That is the safe direction: the rename is simply retried next run.
    auto successor_at = [&renames](const std::string& sym, const std::string& date)
        -> std::pair<std::string, std::string> {  // (effective_until, current_symbol)
        auto it = renames.find(sym);
        if (it == renames.end()) return {};
        for (const auto& candidate : it->second) {
            if (date <= candidate.first) return candidate;
        }
        return {};
    };

    // Snapshot the keys: the loop below erases from and inserts into `positions`.
    std::vector<std::string> held;
    held.reserve(positions.size());
    for (const auto& entry : positions) held.push_back(entry.first);
    std::sort(held.begin(), held.end());  // deterministic order across runs

    for (const auto& original : held) {
        if (renames.find(original) == renames.end()) continue;  // nothing maps this ticker

        auto inception_it = position_inception.find(original);
        if (inception_it == position_inception.end() || inception_it->second.empty()) {
            // Should not happen -- inceptions come from the same positions table --
            // but without one there is no era to test, and guessing re-keys a live
            // holding onto a dead symbol.
            WARN("Corp action SERIES_CONTINUITY: no inception date for held symbol " +
                 original + "; skipping its renames this run (fail-narrow)");
            continue;
        }
        const std::string& inception = inception_it->second;

        // Follow the chain (A->B->C) at the SAME inception, bounded so a cyclic
        // alias map cannot spin.
        std::string sym = original;
        for (int hop = 0; hop < 8; ++hop) {
            auto old_it = positions.find(sym);
            if (old_it == positions.end()) break;  // already merged away

            auto [effective_until, successor] = successor_at(sym, inception);
            if (successor.empty() || successor == sym) break;

            // The rename must also have already happened as of this run.
            if (!as_of_date.empty() && as_of_date <= effective_until) break;

            LifecycleAdjustment adj;
            adj.symbol = sym;
            adj.event_date = effective_until;
            adj.vendor_label = "tickerchangeto";
            adj.action_class = CorpActionClass::SERIES_CONTINUITY;
            adj.outcome = LifecycleOutcome::RENAMED;
            adj.quantity_before = old_it->second.quantity.as_double();
            adj.contra_ticker = successor;

            auto new_it = positions.find(successor);
            if (new_it == positions.end()) {
                // Simple re-key: same holding, new name. Cost basis untouched --
                // a rename is not an economic event.
                Position moved = old_it->second;
                moved.symbol = successor;
                positions.erase(old_it);
                positions.emplace(successor, std::move(moved));
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

            INFO("Corp action SERIES_CONTINUITY: " + sym + " -> " + successor +
                 " (effective_until " + effective_until + ", position inception " + inception +
                 "), qty " + std::to_string(adj.quantity_before) + " -> " +
                 std::to_string(adj.quantity_after));
            log.push_back(std::move(adj));
            sym = successor;
        }
    }

    return log;
}

std::vector<LifecycleAdjustment> CorporateActionsLifecycle::apply_terminations(
    std::unordered_map<std::string, Position>& positions,
    const std::vector<TerminationEvent>& events,
    const std::unordered_map<std::string, double>& final_closes,
    const std::string& feed_last_date) {

    // The date the WARNs quote. Measured by the caller; the compiled-in constant
    // is only the fallback for a caller that could not read the table.
    const std::string feed_through =
        feed_last_date.empty() ? std::string(kCorpActionTableFrozenAfter) : feed_last_date;

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

        // Stock-for-stock rollover is DISABLED. Two independent reasons, both measured
        // against equities_data.corporate_action (E2-F9):
        //
        // 1. `contraticker` carries the sentinel "N/A" on 546,662 rows -- the dominant
        //    value in the table. An `!empty()` test passes it, and the successor position
        //    was then keyed on a symbol literally named "N/A", which fails symbol
        //    validation and aborts the entire run. Any delisting of a held name hit this.
        //
        // 2. `ev.ratio` comes from corporate_action.value, which is NOT a share exchange
        //    ratio. On acquisition rows it averages 2,051 and reaches 133,846.7 -- deal
        //    values in millions. A real stock-for-stock ratio is ~0.1-3.0. Applying it
        //    would have turned 40 DFS shares into 40 x 50343.3 = 2,013,732 COF shares at a
        //    basis of $0.0039, against a real and currently-trading symbol, with nothing to
        //    catch it. Reason 1 was masking reason 2: fixing the sentinel alone converts a
        //    loud abort into a silent 50,000x position.
        //
        // There is no trustworthy ratio column in this feed, so CONVERTED_TO_CONTRA cannot
        // be made safe with the data that exists. Every termination therefore exits at the
        // last real close, which is conservative, correct, and reconcilable with a broker:
        // the position was held, the symbol stopped trading, the holder is out at the last
        // price. Re-enabling this requires a verified deal-terms source, not a code change.
        // Same root cause as NEW-5 (spinoff child-share receipt), which stays data-blocked.
        const bool contra_is_usable =
            !ev.contra_ticker.empty() && ev.contra_ticker != "N/A" &&
            ev.contra_ticker != ev.symbol &&
            std::all_of(ev.contra_ticker.begin(), ev.contra_ticker.end(),
                        [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '-'; });

        // The handler still performs the rollover when it is handed genuine terms -- that
        // capability is deliberately preserved so a revived deal-terms feed activates it
        // with no code change (tests/live/corp_actions/test_effect_classes.cpp:324).
        // The protection lives at the CALLER: the runner must not manufacture `has_terms`
        // and `ratio` out of corporate_action.value, which is a deal value, not a ratio.
        // Trust is a question of provenance, not of magnitude -- a plausibility band cannot
        // separate a real 0.5 ratio from a $0.6m deal value, so the judgement belongs where
        // the source is known.
        const bool terms_usable = ev.has_terms && contra_is_usable &&
                                  ev.ratio > 0.0 && std::isfinite(ev.ratio);

        if (!terms_usable && ev.has_terms && !contra_is_usable) {
            WARN("Termination for " + ev.symbol + " (" + ev.event_date +
                 "): successor '" + ev.contra_ticker +
                 "' is not a usable ticker (sentinel or malformed) -- exiting at the final "
                 "close rather than rolling into it (E2-F9).");
        }

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
                 " on " + ev.event_date +
                 ": no deal terms (corporate_action feed last row " + feed_through +
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
             " on " + ev.event_date +
             ": deal terms unavailable (corporate_action feed last row " + feed_through +
             ") -- exiting " + std::to_string(qty_before) + " shares at final close " +
             std::to_string(exit_price) + " (realized " + std::to_string(realized_delta) +
             "). Correct for a cash deal; a stock-for-stock deal would instead roll into "
             "the successor once terms are available.");
        log.push_back(std::move(adj));
    }

    return log;
}

}  // namespace trade_ngin
