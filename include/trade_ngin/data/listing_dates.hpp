// include/trade_ngin/data/listing_dates.hpp
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/market_data_utils.hpp"

namespace trade_ngin {

/**
 * @brief One contract with a listing date and the contract the book trades before it
 *
 * portfolio.json's optional "listing_dates" block (off when absent):
 *   "listing_dates": {"contracts": [{"symbol": "MES", "listed": "2019-05-06", "before": "ES", "ratio": 10}]}
 * `symbol` and `before` are roots ("MES" for "MES.v.0"); `listed` is a UTC date.
 */
struct ListedContract {
    std::string symbol;  ///< the contract that lists on `listed` (the micro)
    std::string before;  ///< the contract traded before it (the full-size predecessor)
    std::string listed;  ///< YYYY-MM-DD
    double ratio{0.0};   ///< listed contracts per predecessor contract of the same exposure (10 for a micro)
};

/// One conversion a cycle's signal feed makes due: both contracts printed, the listed one on or
/// after its listing date. Whether a position is held, and whether both bars are sessions, is the
/// caller's to read.
struct ListingConversion {
    std::string from;  ///< the predecessor's symbol ("ES.v.0")
    std::string to;    ///< the listed contract's symbol ("MES.v.0")
    double ratio{0.0};
    double from_close{0.0};
    double to_close{0.0};
};

/// The cost of one conversion leg, as the cost model prices a fill.
struct ListingLegCost {
    double commissions_fees{0.0};
    double implicit_price_impact{0.0};
    double slippage_market_impact{0.0};
    double total_transaction_costs{0.0};
};

/// A vendor instrument-id change that is NOT a roll: from `date` the vendor labels the SAME contract
/// of `symbol` (a root) with id `to` where it had used `from`. portfolio.json's optional block:
///   "instrument_id_relabels": [{"symbol": "MES", "date": "2026-02-22", "from": "42140878", "to": "42003800"}]
/// Every bar of the symbol dated on or after `date` that carries `to` is read as carrying `from`, at
/// the loader, so no consumer sees a change: no change bar, no hold, no ROLL legs or costs, the
/// day's move booked as on any day, and the adjusted series keeps that day's return.
struct InstrumentIdRelabel {
    std::string symbol;
    std::string date;  ///< YYYY-MM-DD, the first bar that carries the new id
    std::string from;
    std::string to;
};

/// How the book moves from the predecessor to the listed contract on the switch rebalance.
///   kCloseReenter  the predecessor is closed by the engine's close-out and the pass opens the
///                  listed contract from a held position of zero
///   kConvert       q predecessor contracts become exactly ratio x q listed contracts, and the pass
///                  starts from that held position
///   kOpenAtTarget  the predecessor is closed and the listed contract is opened at its unrounded
///                  target (bounded by the per-name cap) rounded to the nearest whole contract, a
///                  half away from zero; the pass starts from that held position
///   kCarryToTarget a pair whose predecessor is HELD is switched as kOpenAtTarget (one exit of the
///                  predecessor, one entry of the listed contract at its rounded target); a pair with
///                  nothing held is left to the pass, as kConvert leaves it
enum class ListingSwitchRule { kCloseReenter, kConvert, kOpenAtTarget, kCarryToTarget };
const char* to_string(ListingSwitchRule rule);
/// "close_reenter", "convert", "open_at_target", "carry_to_target"; false on anything else.
bool parse_listing_switch_rule(const std::string& text, ListingSwitchRule* out);

/// One sleeve's switch: the fill that closes the predecessor (signed; 0 when none is held), the
/// fill in the listed contract (signed; 0 when none), and the listed contract's held quantity after.
struct ListingSwitch {
    double close_from{0.0};
    double trade_to{0.0};
    double new_to{0.0};
};
/// `target_to` is the listed contract's unrounded target in contracts and `cap_to` the per-name cap
/// in contracts (0 = none); both are read by kOpenAtTarget and kCarryToTarget only. kCloseReenter plans nothing.
/// `in_deferral_band`: the held predecessor is on the other side of a forecast weaker than the
/// deferral band (LOOP_SPEC 5.2, D39), a holding the pass would HOLD. The two target rules then
/// carry it as kConvert does (the pass holds the carried position) instead of trading it to a
/// target the band says not to trade to yet.
ListingSwitch plan_listing_switch(ListingSwitchRule rule, double ratio, double held_from,
                                  double held_to, double target_to, double cap_to,
                                  bool in_deferral_band = false);

/// The fills of a planned switch: the predecessor's close at its close, then the listed contract's
/// fill at its close, each priced by `cost_of(symbol, signed quantity, price)`; a zero move makes
/// no fill and is not priced. Type STRATEGY (they are trades of the book: the trade statistics
/// close the one and open the other); told apart from the pass's fills by their ids ("LC-..." the
/// close, "LO-..." the listed contract's). Throws std::invalid_argument on a close that is not positive.
std::vector<ExecutionReport> make_listing_switch_fills(
    const ListingConversion& conversion, const ListingSwitch& plan, const Timestamp& fill_time,
    const std::string& id_close, const std::string& id_open,
    const std::function<ListingLegCost(const std::string&, double, double)>& cost_of);

/**
 * @brief The tradeable windows of contracts that share one price history
 *
 * The vendor stores the predecessor's bars under the later contract's symbol (the E-mini's bars as
 * "MES.v.0"). With contracts set, a backtest whose window starts before a listing date runs the
 * predecessor as a symbol of its own on those same bars: the predecessor may hold a position only
 * on signal bars dated before the listing date, the listed contract only on signal bars dated on
 * or after it. Both keep the whole history. With no contract set (the default) every method is the
 * identity and no caller's behaviour changes.
 */
class ListingDates {
public:
    static ListingDates& instance();

    /// Replaces the contracts. An empty list switches the rule off. Throws std::invalid_argument on
    /// an entry with an empty root, a date that is not YYYY-MM-DD, a ratio that is not a positive
    /// whole number, or a root named twice.
    void set(const std::vector<ListedContract>& contracts);
    /// set()'s checks alone: throws std::invalid_argument, changes nothing.
    static void validate(const std::vector<ListedContract>& contracts);
    void clear();
    /// The switch rule (kOpenAtTarget unless set). clear() does not change it.
    void set_switch_rule(ListingSwitchRule rule);
    ListingSwitchRule switch_rule() const;
    bool enabled() const;
    std::vector<ListedContract> contracts() const;

    /// May the symbol hold a position on a signal bar of this instant? A predecessor: only before
    /// its listing date. A listed contract: only on or after it. Any other symbol: always.
    bool tradeable(const std::string& symbol, const Timestamp& bar_time) const;

    /// The ratio against the two contracts' sizes: empty when one `before` contract is exactly
    /// `ratio` listed contracts (before_multiplier == ratio x listed_multiplier), else the line to
    /// refuse the run with. The ratio is read by rule convert and by the deferral-band carry, so a
    /// wrong one would carry the wrong exposure.
    static std::string ratio_error(const ListedContract& contract, double before_multiplier,
                                   double listed_multiplier);

    /// Is the symbol ("ES.v.0" or "ES") a predecessor of a listed contract?
    bool is_predecessor(const std::string& symbol) const;

    /// The root a symbol's instrument weight and per-root rules are read under: the listed contract
    /// for a predecessor ("ES" -> "MES"), the root itself otherwise.
    std::string pair_root(const std::string& root) const;

    /// The predecessor symbols a run over `symbols` adds for a window starting at `window_start`:
    /// for every listed contract among `symbols` whose listing date is after the window's start, its
    /// predecessor with the listed symbol's suffix ("MES.v.0" -> "ES.v.0"), in the contracts' order.
    std::vector<std::string> predecessor_symbols(const std::vector<std::string>& symbols,
                                                 const Timestamp& window_start) const;

    /// `symbols` without the predecessors (they have no stored rows of their own), order kept.
    std::vector<std::string> stored_symbols(const std::vector<std::string>& symbols) const;

    /// For every predecessor in `symbols`: a copy of each row stored under its listed contract's
    /// symbol, appended under the predecessor's symbol. Rows already there are not touched.
    void add_predecessor_bars(const std::vector<std::string>& symbols, std::vector<Bar>& bars) const;
    void add_predecessor_ids(const std::vector<std::string>& symbols,
                             std::vector<market_data_utils::FuturesInstrumentId>& ids) const;

    /// Replaces the declared relabellings (empty: none). Throws std::invalid_argument on an entry
    /// with an empty field or a date that is not YYYY-MM-DD. Independent of the contracts.
    void set_relabels(const std::vector<InstrumentIdRelabel>& relabels);
    static void validate_relabels(const std::vector<InstrumentIdRelabel>& relabels);
    /// The id a bar of `symbol` at `bar_time` is read with: `from` where a relabel names its id as
    /// `to` on or after the relabel's date, the id itself otherwise.
    std::string read_id(const std::string& symbol, const Timestamp& bar_time,
                        const std::string& instrument_id) const;
    /// read_id applied to every bar / id row (the row's date for an id row). No-ops with no relabel.
    void apply_relabels(std::vector<Bar>& bars) const;
    void apply_relabels(std::vector<market_data_utils::FuturesInstrumentId>& ids) const;
    bool has_relabels() const;

    /// The instrument-id rows of a load as every consumer reads them, backtest and live: the declared
    /// relabels applied, then every predecessor in `symbols` given the rows of its listed contract.
    /// The identity with nothing declared; an error is passed through.
    Result<std::vector<market_data_utils::FuturesInstrumentId>> read_ids(
        const std::vector<std::string>& symbols,
        Result<std::vector<market_data_utils::FuturesInstrumentId>> ids) const;

    /// The conversions this signal feed makes due (see ListingConversion), in the contracts' order.
    std::vector<ListingConversion> conversions_due(const std::vector<Bar>& signal_feed) const;

    /// "MES.v.0" -> "MES"
    static std::string root_of(const std::string& symbol);

private:
    struct Entry {
        ListedContract contract;
        Timestamp listed_at;  ///< 00:00 UTC of the listing date
    };
    ListingDates() = default;
    static std::vector<Entry> build(const std::vector<ListedContract>& contracts);
    /// predecessor symbol -> listed symbol, for the predecessors named in `symbols`
    std::vector<std::pair<std::string, std::string>> pairs_in(
        const std::vector<std::string>& symbols) const;

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    struct RelabelEntry {
        InstrumentIdRelabel relabel;
        Timestamp from_time;
    };
    static std::vector<RelabelEntry> build_relabels(const std::vector<InstrumentIdRelabel>& relabels);
    std::vector<RelabelEntry> relabels_;
    ListingSwitchRule rule_{ListingSwitchRule::kOpenAtTarget};
};

}  // namespace trade_ngin
