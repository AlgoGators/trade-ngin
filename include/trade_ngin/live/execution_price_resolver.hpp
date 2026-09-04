#pragma once

#include "trade_ngin/core/types.hpp"
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace trade_ngin {

/**
 * ExecutionPriceResolver - supplies a REAL close for every symbol about to trade.
 *
 * A synthetic execution has to be priced at something. Before this existed the
 * answer, when the T-1 close was missing, was Position::average_price -- a cost
 * basis. That is a category error: the basis is what a position cost, not what it
 * is worth, and for a position opened today the basis is 0 until its fill has been
 * processed. Pricing a fill off it books the trade at zero, persists the zero as the
 * new basis, and reloads it the next session as a carried basis of zero.
 *
 * The fix is to widen the search rather than invent a number. A symbol with no T-1
 * close usually has a perfectly good older one -- a halt, a thin name that did not
 * print, an agricultural future with no Sunday session -- and that older close is a
 * real price the position genuinely traded at. This resolver finds it in the bars the
 * runner has already loaded, so it costs no extra query, and it reports how old the
 * price is so the caller can refuse one that has gone stale.
 *
 * Symbols it cannot price are returned as unpriced. The caller must NOT trade them:
 * no price means no execution, never a guessed one.
 */
class ExecutionPriceResolver {
public:
    /**
     * Default bound on how old a substituted close may be, in calendar days.
     *
     * Five days covers every ordinary gap in a US session calendar: a three-day
     * weekend (Fri -> Tue) plus one further holiday reaches Wednesday, which is the
     * longest run the NYSE/CME calendars produce. It also covers the agricultural
     * futures Sunday gap that the futures runners handle explicitly. Anything older
     * means the symbol is not merely between sessions -- it is halted, delisted, or
     * missing from the feed -- and a stale mark must not be presented as a live one.
     */
    static constexpr int kDefaultMaxStalenessDays = 5;

    /** A close that was actually observed, with the session it came from. */
    struct ResolvedClose {
        double price = 0.0;
        Timestamp as_of{};
    };

    /** What fill_missing() did, so the caller can log it and act on failures. */
    struct FillReport {
        /** Symbols priced from an older session: "SYM @ YYYY-MM-DD (N days stale)". */
        std::vector<std::string> widened;
        /** Symbols with no usable close at all. These must not be traded. */
        std::vector<std::string> unpriced;

        bool all_priced() const { return unpriced.empty(); }
    };

    /**
     * Most recent close at or before `as_of`, per symbol, from already-loaded bars.
     *
     * Bars after `as_of` are ignored: an execution priced at T-1 must not reach
     * forward into the session it is being generated for.
     */
    static std::unordered_map<std::string, ResolvedClose> latest_close_at_or_before(
        const std::vector<Bar>& bars,
        const Timestamp& as_of);

    /**
     * Fill gaps in `prices` for the symbols in `needed`, from `fallback`.
     *
     * Only symbols absent from `prices` are touched -- a present T-1 close always
     * wins, so the normal path is bit-identical to not calling this at all. A
     * fallback older than `max_staleness_days`, or a non-positive one, is refused and
     * the symbol is reported unpriced rather than being silently substituted.
     *
     * @param prices  [in,out] symbol -> close. Modified in place.
     * @param fallback  from latest_close_at_or_before().
     * @param needed  symbols that are about to trade and therefore must be priced.
     * @param as_of  the reference session, for computing staleness.
     * @param max_staleness_days  bound; see kDefaultMaxStalenessDays.
     */
    static FillReport fill_missing(
        std::unordered_map<std::string, double>& prices,
        const std::unordered_map<std::string, ResolvedClose>& fallback,
        const std::set<std::string>& needed,
        const Timestamp& as_of,
        int max_staleness_days = kDefaultMaxStalenessDays);

    /** Whole calendar days between two timestamps, floored at 0. */
    static long staleness_days(const Timestamp& as_of, const Timestamp& observed);
};

}  // namespace trade_ngin
