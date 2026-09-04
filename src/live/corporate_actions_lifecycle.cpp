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
        case LifecycleOutcome::SPUN_OFF_CHILD_HELD:   return "SPUN_OFF_CHILD_HELD";
        case LifecycleOutcome::SPUN_OFF_CHILD_SOLD:   return "SPUN_OFF_CHILD_SOLD";
        case LifecycleOutcome::SKIPPED_NO_CHILD_PRICE: return "SKIPPED_NO_CHILD_PRICE";
    }
    return "SKIPPED_NO_POSITION";
}

SpinoffChildPolicy spinoff_child_policy_from_string(const std::string& s) {
    // Unknown text takes the DEFAULT, not HOLD: a typo in a config file must not strand an
    // unpriceable child position in the book (F-4). The runner logs which policy is in force.
    if (s == "hold") return SpinoffChildPolicy::HOLD;
    return SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE;
}

const char* spinoff_child_policy_to_string(SpinoffChildPolicy p) {
    switch (p) {
        case SpinoffChildPolicy::HOLD: return "hold";
        case SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE: return "liquidate_at_first_close";
    }
    return "liquidate_at_first_close";
}

std::vector<LifecycleAdjustment> CorporateActionsLifecycle::apply_spinoffs(
    std::unordered_map<std::string, Position>& positions,
    const std::vector<SpinoffEvent>& events,
    SpinoffChildPolicy policy) {
    std::vector<LifecycleAdjustment> log;
    log.reserve(events.size());

    for (const auto& ev : events) {
        // Names of the children as the EVENT states them, for the refusal messages: `adj`
        // carries deliveries, and a refusal delivers nothing.
        std::string requested;
        for (const auto& c : ev.children) {
            if (!requested.empty()) requested += ", ";
            requested += c.symbol + " x" + std::to_string(c.ratio);
        }

        LifecycleAdjustment adj;
        adj.symbol = ev.parent;
        adj.event_date = ev.ex_date;
        adj.vendor_label = "spinoff";
        adj.action_class = CorpActionClass::PRICE_RESTATING;
        adj.outcome = LifecycleOutcome::SKIPPED_NO_POSITION;
        adj.ratio_change = ev.parent_restatement_factor;

        auto it = positions.find(ev.parent);
        if (it == positions.end()) continue;  // not held; nothing to do, nothing to log

        Position& pos = it->second;
        const double qty = pos.quantity.as_double();
        const double basis = pos.average_price.as_double();
        adj.quantity_before = qty;
        adj.quantity_after = qty;          // a spinoff never changes the PARENT share count
        adj.avg_price_before = basis;
        adj.avg_price_after = basis;

        if (std::abs(qty) < 1e-9) {
            adj.outcome = LifecycleOutcome::SKIPPED_NO_POSITION;
            continue;
        }

        // F must be a real restatement and every r a real ratio. Neither is ever inferred
        // from magnitude -- both arrive from named columns -- so a bad value here is a data
        // fault, not a judgement call, and it must not be worked around.
        //
        // E2-F48: the test is `F > 1`, which is what this comment always said and what the
        // arithmetic below requires; the code tested `F > 0`. A distribution takes value OUT
        // of the parent, so the factor the price series steps by is strictly greater than 1
        // and `1 - 1/F` -- the fraction of the basis that leaves -- is strictly positive.
        // An F below 1 is a REVERSE SPLIT that fell on the spinoff's ex-date (DD 2019-06-03
        // with CTVA, HLT 2017-01-04, LDOS 2013-09-30). Accepting it made `1 - 1/F` negative
        // and handed the child a NEGATIVE cost basis, while suppressing a real split that
        // applied correctly before this path existed. The caller decomposes the bar and
        // sends the reverse split to class 1; this guard is the backstop.
        const double F = ev.parent_restatement_factor;
        bool terms_usable = (F > 1.0) && std::isfinite(F) && !ev.children.empty();
        for (const auto& c : ev.children) {
            if (!(c.ratio > 0.0) || !std::isfinite(c.ratio) || c.symbol.empty() ||
                c.symbol == ev.parent) {
                terms_usable = false;
            }
        }
        if (!terms_usable) {
            adj.outcome = LifecycleOutcome::SKIPPED_NO_CHILD_PRICE;
            WARN("Corp action SPINOFF " + ev.parent + " -> {" + requested + "} (" +
                 ev.ex_date + "): unusable terms (restatement factor " + std::to_string(F) +
                 ", " + std::to_string(ev.children.size()) +
                 " child/children) -- NOTHING applied. A distribution's factor is strictly "
                 "greater than 1; a factor at or below 1 is a reverse split on the same "
                 "ex-date and belongs to the class-1 path, not here (E2-F48). The parent is "
                 "left untouched and the caller must decide which of the bar's columns the "
                 "class-1 path still applies (E2-F31).");
            log.push_back(std::move(adj));
            continue;
        }

        // A child with no close cannot be delivered, sold, or marked -- and it cannot be
        // WEIGHTED either, so a missing close on ONE child makes the allocation across all of
        // them unknowable. Refusing whole is the only honest outcome: the alternative is to
        // invent a price from the parent's basis, which fabricates the one number a broker
        // statement would be compared against. (E2-F49: this was already all-or-nothing per
        // event; with several children it is now all-or-nothing across the set.)
        double fmv_total = 0.0;  // sum(r_i * P_i) -- the allocation denominator
        std::string unpriceable;
        for (const auto& c : ev.children) {
            if (!(c.first_close > 0.0) || !std::isfinite(c.first_close)) {
                if (!unpriceable.empty()) unpriceable += ", ";
                unpriceable += c.symbol;
                continue;
            }
            fmv_total += c.ratio * c.first_close;
        }
        if (!unpriceable.empty() || !(fmv_total > 0.0) || !std::isfinite(fmv_total)) {
            adj.outcome = LifecycleOutcome::SKIPPED_NO_CHILD_PRICE;
            WARN("Corp action SPINOFF " + ev.parent + " -> {" + requested + "} (" +
                 ev.ex_date + "): " +
                 (unpriceable.empty() ? std::string("the children carry no positive value")
                                      : unpriceable + " ha(s) no close on or after the "
                                                      "ex-date") +
                 ", so the distribution cannot be priced, allocated, delivered or sold. "
                 "NOTHING is applied and no dedup row is written -- the parent keeps " +
                 std::to_string(qty) + " shares at basis " + std::to_string(basis) +
                 ", which is a PRE-spinoff basis against a POST-spinoff price series, and "
                 "the children are missing from the book entirely. The caller must also "
                 "suppress the class-1 event (E2-F31). Load the child price series, or "
                 "reconcile this position by hand.");
            log.push_back(std::move(adj));
            continue;
        }

        // ---- the restatement ----
        const double parent_basis_after = basis > 0.0 ? basis / F : 0.0;
        // The value that LEFT the parent, per parent share, spread over the children it was
        // distributed into by relative fair market value. Exact complement of B/F, so total
        // basis is conserved over the parent AND every child together.
        const double pool_per_share = basis > 0.0 ? basis * (1.0 - 1.0 / F) : 0.0;

        std::vector<SpinoffChildDelivery> deliveries;
        deliveries.reserve(ev.children.size());
        bool allocation_sane = std::isfinite(parent_basis_after) && parent_basis_after >= 0.0;
        for (const auto& c : ev.children) {
            SpinoffChildDelivery d;
            d.symbol = c.symbol;
            d.ratio = c.ratio;
            d.first_close = c.first_close;
            // basis_i = pool * P_i / sum(r_j P_j); with one child this is pool / r exactly.
            d.avg_price = pool_per_share > 0.0 ? pool_per_share * c.first_close / fmv_total
                                               : 0.0;
            // BA-25: floor, but not through floating-point dust.
            //
            // `qty` is reloaded from `trading.positions.quantity`, which is numeric(20,6).
            // A retried reverse-split spinoff therefore arrives as 33.333333 rather than the
            // 33.33333333 the same-run path holds in memory, and 33.333333 x 1.8 is
            // 59.9999994: floor gives 59 whole shares plus a 0.9999994 "fraction", so the
            // holder is one share short and the book emits a cash-in-lieu SELL for very
            // nearly a whole share that no broker ever paid. The same-run path, working from
            // an unrounded quantity, delivers 60 and no CIL -- so the two paths disagreed
            // about the same event because of the storage precision between them.
            //
            // A real fractional entitlement is nowhere near an integer (100 x 0.33333 is
            // 33.333, a third of a share out), so rounding to the nearest share when the
            // exact figure is within 1e-6 of one cannot swallow a genuine fraction: 1e-6 of
            // a share is four orders of magnitude smaller than the smallest quantity the
            // column can even represent.
            const double exact = qty * c.ratio;
            const double nearest = std::round(exact);
            d.quantity = (std::abs(exact - nearest) < 1e-6) ? nearest : std::floor(exact);
            // Clamp: when the exact figure sat just BELOW an integer, `exact - quantity` is a
            // tiny negative, and a negative fraction would book a negative cash-in-lieu.
            d.fractional = std::max(0.0, exact - d.quantity);
            d.cash_in_lieu = d.fractional * c.first_close;
            // The fraction is sold at the child's first close against the CHILD's basis.
            // Those shares were child shares from the instant of distribution, never parent
            // shares.
            d.realized_delta = d.fractional * (c.first_close - d.avg_price);
            if (!std::isfinite(d.avg_price) || d.avg_price < 0.0) allocation_sane = false;
            deliveries.push_back(std::move(d));
        }

        // E2-F48: never a negative basis on any side. `F > 1` above makes this unreachable --
        // which is exactly why it is asserted rather than assumed. A negative child basis is
        // not a small error: under liquidate_at_first_close the child's whole first close
        // PLUS the negative basis is booked as realized gain on the parent's row, and the
        // number is persisted into trading.positions and live_results in one step.
        if (!allocation_sane) {
            adj.outcome = LifecycleOutcome::SKIPPED_NO_CHILD_PRICE;
            WARN("Corp action SPINOFF " + ev.parent + " -> {" + requested + "} (" +
                 ev.ex_date + "): the allocation produced a negative or non-finite basis "
                 "(parent " + std::to_string(parent_basis_after) + ", pool " +
                 std::to_string(pool_per_share) + ") from basis " + std::to_string(basis) +
                 ", factor " + std::to_string(F) +
                 ". NOTHING is applied -- a negative cost basis books the child's entire "
                 "first close, and more, as realized gain (E2-F48).");
            log.push_back(std::move(adj));
            continue;
        }

        if (basis > 0.0) pos.average_price = Decimal(parent_basis_after);
        adj.avg_price_after = parent_basis_after;
        adj.children = deliveries;

        double realized_total = 0.0;
        for (const auto& d : adj.children) realized_total += d.realized_delta;

        {
            std::string detail;
            for (const auto& d : adj.children) {
                if (!detail.empty()) detail += "; ";
                detail += std::to_string(d.ratio) + " x " + d.symbol + " -> " +
                          std::to_string(d.quantity) + " sh at basis " +
                          std::to_string(d.avg_price) + " (first close " +
                          std::to_string(d.first_close) + "), cash in lieu of " +
                          std::to_string(d.fractional) + " sh = " +
                          std::to_string(d.cash_in_lieu);
            }
            INFO("Corp action SPINOFF: " + ev.parent + " on " + ev.ex_date + " distributes " +
                 std::to_string(adj.children.size()) + " child/children per share. Parent qty " +
                 std::to_string(qty) + " unchanged, basis " + std::to_string(basis) + " -> " +
                 std::to_string(parent_basis_after) + " (factor " + std::to_string(F) +
                 "); " + detail + ". CIL realized " + std::to_string(realized_total) + ".");
        }

        if (policy == SpinoffChildPolicy::HOLD) {
            adj.outcome = LifecycleOutcome::SPUN_OFF_CHILD_HELD;
            for (const auto& d : adj.children) {
                if (!(d.quantity > 0.0)) continue;
                auto existing = positions.find(d.symbol);
                if (existing == positions.end()) {
                    Position child;
                    child.symbol = d.symbol;
                    child.quantity = Quantity(d.quantity);
                    child.average_price = Decimal(d.avg_price);
                    child.realized_pnl = Decimal(0.0);
                    child.unrealized_pnl = Decimal(0.0);
                    positions.emplace(d.symbol, std::move(child));
                } else {
                    // Already hold the child (it was in the universe and traded): merge at
                    // weighted-average cost, the same rule a rollover into a held acquirer
                    // uses.
                    Position& dest = existing->second;
                    const double q_dest = dest.quantity.as_double();
                    const double p_dest = dest.average_price.as_double();
                    const double q_sum = q_dest + d.quantity;
                    if (std::abs(q_sum) > 1e-9) {
                        dest.average_price =
                            Decimal((q_dest * p_dest + d.quantity * d.avg_price) / q_sum);
                    }
                    dest.quantity = Quantity(q_sum);
                }
            }
            adj.realized_delta = realized_total;
            pos.realized_pnl = Decimal(pos.realized_pnl.as_double() + realized_total);
            log.push_back(std::move(adj));
            continue;
        }

        // LIQUIDATE_AT_FIRST_CLOSE. Each child is received and sold in one step, so none of
        // them becomes an unpriceable holding the next run has to carry (F-4). The realized
        // figure is struck against that child's own allocated basis, so a child that opened
        // exactly at its allocated value books zero -- the distribution itself is not a P&L
        // event.
        adj.outcome = LifecycleOutcome::SPUN_OFF_CHILD_SOLD;
        std::string disposal_detail;
        for (auto& d : adj.children) {
            const double liquidation_realized = d.quantity * (d.first_close - d.avg_price);
            d.realized_delta += liquidation_realized;
            realized_total += liquidation_realized;
            if (!disposal_detail.empty()) disposal_detail += "; ";
            disposal_detail += std::to_string(d.quantity) + " " + d.symbol + " at " +
                               std::to_string(d.first_close) + " against basis " +
                               std::to_string(d.avg_price) + " (realized " +
                               std::to_string(liquidation_realized) + ")";
        }
        adj.realized_delta = realized_total;
        pos.realized_pnl = Decimal(pos.realized_pnl.as_double() + realized_total);

        INFO("Corp action SPINOFF child disposal: sold " + disposal_detail +
             "; policy=liquidate_at_first_close, so no unpriceable child is left in the "
             "book. Total realized booked on " + ev.parent + " for this event: " +
             std::to_string(adj.realized_delta) + ".");

        log.push_back(std::move(adj));
    }

    return log;
}

CorporateActionsLifecycle::RenameMap CorporateActionsLifecycle::build_rename_map(
    const std::vector<TickerAlias>& aliases) {
    RenameMap renames;
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
    for (auto& entry : renames) {
        std::sort(entry.second.begin(), entry.second.end());
    }
    return renames;
}

std::pair<std::string, std::string> CorporateActionsLifecycle::successor_at(
    const RenameMap& renames, const std::string& symbol, const std::string& date) {
    auto it = renames.find(symbol);
    if (it == renames.end()) return {};
    for (const auto& candidate : it->second) {
        if (date <= candidate.first) return candidate;
    }
    return {};
}

std::vector<std::string> CorporateActionsLifecycle::rename_chain(
    const RenameMap& renames, const std::string& symbol, const std::string& holding_start,
    const std::string& as_of_date) {
    std::vector<std::string> chain;
    if (holding_start.empty()) return chain;
    std::string sym = symbol;
    for (int hop = 0; hop < 8; ++hop) {
        auto [effective_until, successor] = successor_at(renames, sym, holding_start);
        if (successor.empty() || successor == sym) break;
        // The rename must also have already happened as of this run.
        if (!as_of_date.empty() && as_of_date <= effective_until) break;
        chain.push_back(successor);
        sym = successor;
    }
    return chain;
}

std::vector<LifecycleAdjustment> CorporateActionsLifecycle::apply_renames(
    std::unordered_map<std::string, Position>& positions,
    const std::vector<TickerAlias>& aliases,
    const std::string& as_of_date,
    const std::unordered_map<std::string, std::string>& position_inception) {

    std::vector<LifecycleAdjustment> log;

    // One rename map, shared with LiveDailyCycle::effective_universe so the universe
    // the run loads bars for and the re-keying done here cannot drift apart (E2-F34).
    const RenameMap renames = build_rename_map(aliases);
    if (renames.empty()) return log;

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

            auto [effective_until, successor] = successor_at(renames, sym, inception);
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
