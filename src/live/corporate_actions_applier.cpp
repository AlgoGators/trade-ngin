#include "trade_ngin/live/corporate_actions_applier.hpp"

#include <cmath>

#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

const char* CorporateActionsApplier::type_to_string(CorpActionType t) {
    switch (t) {
        case CorpActionType::SPLIT:     return "SPLIT";
        case CorpActionType::ADR_SPLIT: return "ADR_SPLIT";
        case CorpActionType::DIVIDEND:  return "DIVIDEND";
        case CorpActionType::UNKNOWN:   return "UNKNOWN";
    }
    return "UNKNOWN";
}

CorpActionType CorporateActionsApplier::type_from_type_string(const std::string& type_name) {
    if (type_name == "SPLIT") return CorpActionType::SPLIT;
    if (type_name == "ADR_SPLIT") return CorpActionType::ADR_SPLIT;
    if (type_name == "DIVIDEND") return CorpActionType::DIVIDEND;
    return CorpActionType::UNKNOWN;
}

CorpActionType CorporateActionsApplier::type_from_action_string(const std::string& action) {
    if (action == "split") return CorpActionType::SPLIT;
    if (action == "adrratiosplit") return CorpActionType::ADR_SPLIT;
    if (action == "dividend") return CorpActionType::DIVIDEND;
    return CorpActionType::UNKNOWN;
}

std::vector<PositionAdjustment> CorporateActionsApplier::apply(
    std::unordered_map<std::string, Position>& positions,
    const std::vector<CorpActionEvent>& events) {

    std::vector<PositionAdjustment> log;
    log.reserve(events.size());

    for (const auto& ev : events) {
        if (ev.type == CorpActionType::UNKNOWN) {
            WARN("CorporateActionsApplier: UNKNOWN event type for " + ev.symbol +
                 " on " + ev.ex_date + " -- skipping");
            continue;
        }

        auto it = positions.find(ev.symbol);
        if (it == positions.end()) {
            // No held position; nothing to adjust. Not an error.
            continue;
        }

        Position& pos = it->second;
        const double qty_before = pos.quantity.as_double();
        const double avg_before = pos.average_price.as_double();

        if (std::abs(qty_before) < 1e-9) {
            // Zero position; no adjustment needed.
            continue;
        }

        PositionAdjustment adj;
        adj.symbol = ev.symbol;
        adj.event_date = ev.ex_date;
        adj.type = ev.type;
        adj.quantity_before = qty_before;
        adj.avg_price_before = avg_before;
        adj.event_value = ev.value;
        adj.ratio_change = 1.0;

        switch (ev.type) {
            case CorpActionType::SPLIT:
            case CorpActionType::ADR_SPLIT: {
                if (ev.value <= 0.0 || !std::isfinite(ev.value)) {
                    WARN("CorporateActionsApplier: bad split factor " +
                         std::to_string(ev.value) + " for " + ev.symbol +
                         " on " + ev.ex_date + " -- skipping");
                    continue;
                }
                const double factor = ev.value;
                const double qty_after = qty_before * factor;
                const double avg_after = avg_before > 0.0 ? avg_before / factor : 0.0;
                pos.quantity = Quantity(qty_after);
                if (avg_before > 0.0) {
                    pos.average_price = Decimal(avg_after);
                }
                adj.quantity_after = qty_after;
                adj.avg_price_after = avg_after;
                adj.ratio_change = factor;
                break;
            }
            case CorpActionType::DIVIDEND: {
                // ratio_change = 1 + dividend_per_share / close[T-1]
                if (ev.close_at_ex_date <= 0.0 || !std::isfinite(ev.close_at_ex_date)) {
                    WARN("CorporateActionsApplier: dividend for " + ev.symbol +
                         " on " + ev.ex_date + " has invalid close[T-1]=" +
                         std::to_string(ev.close_at_ex_date) + " -- skipping");
                    continue;
                }
                if (ev.value <= 0.0 || !std::isfinite(ev.value)) {
                    WARN("CorporateActionsApplier: dividend for " + ev.symbol +
                         " on " + ev.ex_date + " has invalid amount=" +
                         std::to_string(ev.value) + " -- skipping");
                    continue;
                }
                const double ratio = 1.0 + ev.value / ev.close_at_ex_date;
                const double avg_after = avg_before > 0.0 ? avg_before / ratio : 0.0;
                // Dividend doesn't change quantity; only rescales avg_price into
                // the post-dividend adjusted-price frame (audit §1.15).
                if (avg_before > 0.0) {
                    pos.average_price = Decimal(avg_after);
                }
                // For the audit log's cash-flow figure, the basis is the
                // qty held on ex_date - 1 (ultrareview bug_021). When the
                // caller supplied that, record it; otherwise fall back to
                // the live position qty (same as before).
                //
                // Design note (ultrareview PR #39 follow-up): the `> 0`
                // sentinel uses zero, not -1 / NaN, because:
                //   (a) `CorpActionEvent::qty_at_ex_date` defaults to 0.0
                //       (legacy callers that don't populate it get the
                //       safe fallback to live qty without an API break),
                //   (b) a true `qty_at_ex_date == 0` would mean the holder
                //       owned zero shares on ex_date -- in which case the
                //       applier is correctly being asked to record a $0
                //       dividend basis whether we use the sentinel or the
                //       live qty (also zero), so the ambiguity is harmless,
                //   (c) negative qty (short positions during dividend) is
                //       not currently supported by the applier; the live
                //       app's lookup helper returns `position.quantity` so
                //       a short would surface as negative and be rejected
                //       by this check, falling back to live qty -- which is
                //       also the desired behavior for shorts (the short
                //       owes the dividend; we record |qty| via the abs in
                //       calculate_commission elsewhere).
                const bool have_ex_date_qty =
                    (ev.qty_at_ex_date > 0.0 && std::isfinite(ev.qty_at_ex_date));
                const double basis_qty = have_ex_date_qty ? ev.qty_at_ex_date : qty_before;

                // The fallback is deliberate (see above) but it must not be SILENT. It only
                // affects the audit cash-flow figure -- qty_held and total_cash on the
                // trading.corp_action_applied row -- and never the basis adjustment, which
                // is computed from the ratio above. But a dividend recorded against today's
                // share count instead of the ex-date holding gives a wrong total_cash with
                // nothing to indicate it, and the two differ precisely when they matter
                // most: during a catch-up over a period where position history is thin, or
                // right after a position was resized. Say so, and print both numbers so the
                // discrepancy is measurable rather than merely suspected.
                if (!have_ex_date_qty && std::abs(qty_before) > 1e-9) {
                    WARN("CorporateActionsApplier: dividend for " + ev.symbol + " (ex_date " +
                         ev.ex_date + "): no holding recorded at ex_date-1, so the cash-flow "
                         "basis falls back to today's quantity " + std::to_string(qty_before) +
                         ". qty_held and total_cash on the dedup row reflect that fallback, "
                         "not the ex-date holding. The basis adjustment is unaffected.");
                }
                adj.quantity_before = basis_qty;
                adj.quantity_after = basis_qty;  // unchanged
                adj.avg_price_after = avg_after;
                adj.ratio_change = ratio;
                break;
            }
            case CorpActionType::UNKNOWN:
                continue;  // handled above
        }

        log.push_back(std::move(adj));
    }

    return log;
}

}  // namespace trade_ngin
