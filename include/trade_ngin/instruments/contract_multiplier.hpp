#pragma once

#include <optional>
#include <string>

namespace trade_ngin {

/**
 * @file contract_multiplier.hpp
 * @brief Contract size and price multiplier, which are not the same number.
 *
 * WHY THIS EXISTS
 * ---------------
 * Four places in this repository carried their own hardcoded table of
 * per-symbol constants, and they disagreed with each other:
 *
 *   src/core/email_sender.cpp        ZN 100000, ZC 5000, ZL 60000, LE 40000
 *   src/live/live_pnl_manager.cpp    ZN   1000, ZC 5000, ZL 60000, LE 40000
 *   src/backtest/backtest_pnl_manager.cpp
 *                                    ZN   1000, ZC   50, ZL   600, LE   400
 *   metadata.contract_metadata       "Contract Size", read by InstrumentRegistry
 *
 * They disagreed because they were answering two different questions under one
 * name. A futures contract has a CONTRACT SIZE -- the amount of the underlying
 * it delivers, 5,000 bushels of corn, $100,000 face of ten-year notes -- and a
 * PRICE MULTIPLIER, the currency value of one full point of the QUOTED price.
 *
 * For most contracts these are the same number, because the price is quoted per
 * unit of the underlying: crude is dollars per barrel on 1,000 barrels, gold
 * dollars per ounce on 100 ounces. The two coincide, and code that confuses
 * them is right by accident.
 *
 * It stops being right where the quote convention differs from the natural
 * unit, and there the error is a clean factor of 100:
 *
 *   Treasuries  quote as a PERCENTAGE OF PAR. ZN at 110.5 is 110.5% of
 *               $100,000 = $110,500. One point is $1,000, not $100,000.
 *   Grains      quote in CENTS per bushel. ZC at 450 is $4.50/bu on 5,000
 *               bushels = $22,500. One point (one cent) is $50, not $5,000.
 *   Livestock,  quote in CENTS per pound. LE at 185.5 is $1.855/lb on 40,000
 *   soybean oil pounds = $74,200. One point is $400, not $40,000.
 *
 * Forty ZN priced with the contract size read as a multiplier came to
 * $449,400,000 of exposure against a $264,000 book. That is the shape of this
 * bug when it reaches a screen.
 *
 * WHAT THIS MODULE PROVIDES
 * -------------------------
 * One table, with the derivation written down, and a resolver that turns a
 * reported contract size into a price multiplier. The resolver does not assume
 * which of the two quantities the metadata table holds -- it recognises either
 * and says which it saw, because that question is not settled and guessing it
 * wrong is a 100x error in the direction nobody notices until an exposure
 * figure looks absurd.
 */

/// One contract's two numbers, and the bridge between them.
///
/// Named for the quote convention rather than the contract, because
/// core/types.hpp already has a ContractSpec and it holds something else: the
/// instrument's identity, exchange and tick size, with a multiplier it takes on
/// trust from whoever built it.
struct QuotedContract {
    /// Underlying units per contract: bushels, ounces, dollars of face value.
    double contract_size = 0.0;

    /// Currency per (one unit of the underlying x one point of the quoted
    /// price). 1.0 when the price is quoted directly in the underlying's own
    /// unit, 0.01 when it is quoted in hundredths of it -- cents per bushel,
    /// cents per pound, or percent of par.
    double quote_scale = 1.0;

    /// Currency value of one full point of the quoted price. This is the number
    /// that belongs in `quantity * price * multiplier`.
    constexpr double price_multiplier() const { return contract_size * quote_scale; }
};

/// How a resolved multiplier was arrived at. Callers log this; it is the only
/// way to notice that the metadata table changed convention underneath us.
enum class MultiplierSource {
    /// The reported figure matched the known contract size, and was scaled.
    ScaledContractSize,
    /// The reported figure already was the point value. Used unchanged.
    AlreadyPointValue,
    /// Known symbol, but the reported figure matched neither. Used unchanged,
    /// and worth a warning: either the spec changed or the row is wrong.
    ReportedUnrecognised,
    /// No entry in the table. The reported figure is the only thing available.
    UnknownSymbol,
};

struct ResolvedMultiplier {
    double value = 1.0;
    MultiplierSource source = MultiplierSource::UnknownSymbol;
};

/**
 * @brief Reduce a traded symbol to its root.
 *
 * Strips the continuous-contract suffix data-ngin writes (`ES.v.0`, `ZC.c.1`)
 * and the month/year code of a dated contract (`ZCH5`). Returns the input
 * unchanged when neither applies.
 *
 * This replaces substring matching, which was how the previous tables did it
 * and which quietly priced the E-micro euro `M6E` as the full-size `6E`.
 */
std::string root_symbol(const std::string& symbol);

/// The known spec for a symbol, or nothing. Accepts suffixed symbols.
std::optional<QuotedContract> known_contract(const std::string& symbol);

/**
 * @brief The symbol this deployment actually prices `symbol` as.
 *
 * Usually the root itself. A handful of tickers are read as a different
 * contract -- ES as the micro E-mini, because InstrumentRegistry rewrites it
 * that way before every lookup, and a few micro tickers as their full-size
 * namesakes, inherited from the substring matching in the tables this module
 * replaces. See deployment_aliases() in the implementation: every entry there
 * is an unresolved factor of ten, preserved rather than silently changed.
 */
std::string deployment_symbol(const std::string& symbol);

/**
 * @brief Price multiplier for a symbol from the table alone.
 *
 * For use only where the InstrumentRegistry is unavailable. Nothing is invented
 * for an unknown symbol: the caller gets nothing and must decide what to do,
 * rather than silently multiplying by 1.0.
 */
std::optional<double> fallback_price_multiplier(const std::string& symbol);

/**
 * @brief Turn a reported "Contract Size" into a price multiplier.
 *
 * @param symbol Traded symbol, suffixed or not.
 * @param reported The `Contract Size` from `metadata.contract_metadata`.
 *
 * Recognises the reported figure as either a contract size or an already-scaled
 * point value, and reports which. Where the two coincide -- which is most
 * contracts -- both readings give the same answer and the distinction does not
 * arise.
 */
ResolvedMultiplier resolve_price_multiplier(const std::string& symbol, double reported);

/// Human-readable form of a MultiplierSource, for log lines. Not spelled
/// to_string: core/types.hpp puts one of those in namespace std for Decimal,
/// and an unqualified call near both is a coin toss a reader has to resolve.
const char* describe(MultiplierSource source);

}  // namespace trade_ngin
