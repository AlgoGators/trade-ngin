// tests/instruments/test_contract_multiplier.cpp
//
// The contract size of a futures contract and the multiplier that turns its
// quoted price into money are two different numbers, and four tables in this
// repository used to disagree about which was which. These tests pin down the
// distinction so the next person to add a symbol has to state the convention
// rather than copy a number from whichever table they happened to open.

#include <gtest/gtest.h>

#include "trade_ngin/instruments/contract_multiplier.hpp"

using namespace trade_ngin;

namespace {

/// What this deployment prices the symbol as -- aliases applied.
double multiplier_of(const std::string& symbol) {
    auto value = fallback_price_multiplier(symbol);
    EXPECT_TRUE(value.has_value()) << symbol << " is not in the contract table";
    return value.value_or(0.0);
}

/// The contract's own specification, aliases not applied.
double spec_multiplier_of(const std::string& symbol) {
    auto spec = known_contract(symbol);
    EXPECT_TRUE(spec.has_value()) << symbol << " is not in the contract table";
    return spec ? spec->price_multiplier() : 0.0;
}

}  // namespace

// ---------------------------------------------------------------------------
// The contracts where size and multiplier coincide. Nothing to get wrong, but
// they are the majority and a regression here would be silent.
// ---------------------------------------------------------------------------

TEST(ContractMultiplier, EquityIndexIsQuotedInPoints) {
    EXPECT_DOUBLE_EQ(spec_multiplier_of("ES"), 50.0);
    EXPECT_DOUBLE_EQ(spec_multiplier_of("NQ"), 20.0);
    EXPECT_DOUBLE_EQ(spec_multiplier_of("YM"), 5.0);
    EXPECT_DOUBLE_EQ(spec_multiplier_of("RTY"), 50.0);
    EXPECT_DOUBLE_EQ(spec_multiplier_of("MES"), 5.0);
    EXPECT_DOUBLE_EQ(spec_multiplier_of("M2K"), 5.0);
}

TEST(ContractMultiplier, EnergyAndMetalsAreQuotedInTheirOwnUnit) {
    EXPECT_DOUBLE_EQ(multiplier_of("CL"), 1000.0);   // $/bbl on 1,000 barrels
    EXPECT_DOUBLE_EQ(multiplier_of("NG"), 10000.0);  // $/MMBtu on 10,000
    EXPECT_DOUBLE_EQ(multiplier_of("GC"), 100.0);    // $/oz on 100 ounces
    EXPECT_DOUBLE_EQ(multiplier_of("SI"), 5000.0);   // $/oz on 5,000 ounces
}

TEST(ContractMultiplier, ForeignExchangeIsQuotedInUsdPerUnit) {
    EXPECT_DOUBLE_EQ(multiplier_of("6E"), 125000.0);
    EXPECT_DOUBLE_EQ(multiplier_of("6B"), 62500.0);
    EXPECT_DOUBLE_EQ(spec_multiplier_of("M6E"), 12500.0);
}

// ---------------------------------------------------------------------------
// The contracts where they do not coincide. Every one of these was 100x wrong
// somewhere in the codebase.
// ---------------------------------------------------------------------------

TEST(ContractMultiplier, TreasuriesQuoteAsAPercentageOfPar) {
    // ZN at 110.5 is 110.5% of $100,000 of face value. One point is $1,000.
    // The contract size, $100,000, is not the multiplier -- reading it as one
    // priced forty contracts at $449,400,000 against a $264,000 book.
    EXPECT_DOUBLE_EQ(multiplier_of("ZN"), 1000.0);
    EXPECT_DOUBLE_EQ(multiplier_of("ZB"), 1000.0);
    EXPECT_DOUBLE_EQ(multiplier_of("ZF"), 1000.0);
    EXPECT_DOUBLE_EQ(multiplier_of("UB"), 1000.0);
    // The two-year is the exception within the exception: $200,000 of face.
    EXPECT_DOUBLE_EQ(multiplier_of("ZT"), 2000.0);
}

TEST(ContractMultiplier, GrainsQuoteInCentsPerBushel) {
    // Corn at 450 is $4.50 a bushel on 5,000 bushels = $22,500. One point --
    // one cent -- is $50.
    EXPECT_DOUBLE_EQ(multiplier_of("ZC"), 50.0);
    EXPECT_DOUBLE_EQ(multiplier_of("ZS"), 50.0);
    EXPECT_DOUBLE_EQ(multiplier_of("ZW"), 50.0);
    EXPECT_DOUBLE_EQ(multiplier_of("KE"), 50.0);
}

TEST(ContractMultiplier, SoybeanOilAndLivestockQuoteInCentsPerPound) {
    EXPECT_DOUBLE_EQ(multiplier_of("ZL"), 600.0);  // 60,000 lb
    EXPECT_DOUBLE_EQ(multiplier_of("LE"), 400.0);  // 40,000 lb
    EXPECT_DOUBLE_EQ(multiplier_of("HE"), 400.0);  // 40,000 lb
    EXPECT_DOUBLE_EQ(multiplier_of("GF"), 500.0);  // 50,000 lb
}

TEST(ContractMultiplier, TheGrainsThatAreNotQuotedInCentsAreNotScaled) {
    // Soybean meal is dollars per short ton and rough rice dollars per
    // hundredweight, so for these two the contract size IS the multiplier.
    // Scaling them by a blanket "grains are in cents" rule would be as wrong
    // in the other direction.
    EXPECT_DOUBLE_EQ(multiplier_of("ZM"), 100.0);
    EXPECT_DOUBLE_EQ(multiplier_of("ZR"), 2000.0);
}

// ---------------------------------------------------------------------------
// Symbol handling. The tables this replaces matched by substring, which is how
// the E-micro euro came to be priced as the full-size contract.
// ---------------------------------------------------------------------------

TEST(ContractMultiplier, TheTableStatesTheContractSpecificationNotTheDeploymentChoice) {
    // A micro contract is a tenth of its full-size namesake. The table says so
    // even where this deployment chooses to price them the same, because the
    // spec is a fact and the choice is a policy, and they belong in different
    // places.
    EXPECT_DOUBLE_EQ(spec_multiplier_of("M6E"), 12500.0);
    EXPECT_DOUBLE_EQ(spec_multiplier_of("M6B"), 6250.0);
    EXPECT_DOUBLE_EQ(spec_multiplier_of("MGC"), 10.0);
    EXPECT_DOUBLE_EQ(spec_multiplier_of("MNQ"), 2.0);
}

TEST(ContractMultiplier, DeploymentAliasesPreserveWhatTheOldTablesDid) {
    // Every one of these is an unresolved factor of ten, carried over unchanged
    // from the hardcoded tables this module replaces rather than silently
    // corrected. InstrumentRegistry::get_instrument already rewrites ES to MES
    // before every lookup, so the first four are the existing behaviour of the
    // live path, not an invention of this file.
    EXPECT_EQ(deployment_symbol("ES"), "MES");
    EXPECT_EQ(deployment_symbol("NQ"), "MNQ");
    EXPECT_EQ(deployment_symbol("YM"), "MYM");
    EXPECT_EQ(deployment_symbol("RTY"), "M2K");
    EXPECT_DOUBLE_EQ(multiplier_of("ES"), 5.0);
    EXPECT_DOUBLE_EQ(multiplier_of("NQ"), 2.0);
    EXPECT_DOUBLE_EQ(multiplier_of("YM"), 0.5);
    EXPECT_DOUBLE_EQ(multiplier_of("RTY"), 5.0);

    // And these came from substring matching: "M6E" contains "6E".
    EXPECT_DOUBLE_EQ(multiplier_of("M6E"), 125000.0);
    EXPECT_DOUBLE_EQ(multiplier_of("M6B"), 62500.0);
    EXPECT_DOUBLE_EQ(multiplier_of("MGC"), 100.0);
    EXPECT_DOUBLE_EQ(multiplier_of("MSF"), 125000.0);
}

TEST(ContractMultiplier, AlternateVendorTickersResolveToTheSameContract) {
    EXPECT_DOUBLE_EQ(multiplier_of("YK"), multiplier_of("ZS"));
    EXPECT_DOUBLE_EQ(multiplier_of("YW"), multiplier_of("ZW"));
}

TEST(ContractMultiplier, MicroContractsWithoutAnAliasKeepTheirOwnSpecification) {
    EXPECT_DOUBLE_EQ(multiplier_of("MES"), 5.0);
    EXPECT_DOUBLE_EQ(multiplier_of("MNQ"), 2.0);
    EXPECT_DOUBLE_EQ(multiplier_of("MCL"), 100.0);
    EXPECT_DOUBLE_EQ(multiplier_of("SIL"), 1000.0);
}

TEST(ContractMultiplier, ContinuousContractSuffixesAreStripped) {
    EXPECT_EQ(root_symbol("ES.v.0"), "ES");
    EXPECT_EQ(root_symbol("ZC.c.1"), "ZC");
    EXPECT_DOUBLE_EQ(multiplier_of("ZN.v.0"), 1000.0);
}

TEST(ContractMultiplier, DatedContractsReduceToTheirRoot) {
    EXPECT_EQ(root_symbol("ZCH5"), "ZC");
    EXPECT_EQ(root_symbol("ESZ24"), "ES");
    EXPECT_EQ(root_symbol("M6EZ4"), "M6E");
}

TEST(ContractMultiplier, ASymbolThatOnlyLooksLikeARootPlusMonthIsLeftAlone) {
    // MES is not ME plus a month code, and SIL is not SI plus one.
    EXPECT_EQ(root_symbol("MES"), "MES");
    EXPECT_EQ(root_symbol("SIL"), "SIL");
}

TEST(ContractMultiplier, AnUnknownSymbolYieldsNothingRatherThanOne) {
    // Returning 1.0 for an unrecognised symbol is how a notional of $4,850
    // gets reported for five S&P contracts. The caller has to decide.
    EXPECT_FALSE(fallback_price_multiplier("WHEAT").has_value());
    EXPECT_FALSE(fallback_price_multiplier("").has_value());
}

// ---------------------------------------------------------------------------
// Resolving a figure read from metadata.contract_metadata. Which of the two
// quantities that column holds is not settled, so the resolver recognises
// either and reports which it saw.
// ---------------------------------------------------------------------------

TEST(ResolvePriceMultiplier, AContractSizeIsScaledByTheQuoteConvention) {
    auto resolved = resolve_price_multiplier("ZN", 100000.0);
    EXPECT_DOUBLE_EQ(resolved.value, 1000.0);
    EXPECT_EQ(resolved.source, MultiplierSource::ScaledContractSize);
}

TEST(ResolvePriceMultiplier, AFigureThatIsAlreadyAPointValueIsLeftAlone) {
    // Double-scaling is the failure this branch exists to prevent: a table
    // already holding 1,000 for ZN must not become 10.
    auto resolved = resolve_price_multiplier("ZN", 1000.0);
    EXPECT_DOUBLE_EQ(resolved.value, 1000.0);
    EXPECT_EQ(resolved.source, MultiplierSource::AlreadyPointValue);
}

TEST(ResolvePriceMultiplier, WhereTheTwoCoincideEitherReadingGivesTheSameAnswer) {
    auto resolved = resolve_price_multiplier("ES", 50.0);
    EXPECT_DOUBLE_EQ(resolved.value, 50.0);
}

TEST(ResolvePriceMultiplier, AnUnrecognisedFigureIsUsedButFlagged) {
    // Neither the contract size nor the point value. The engine still has to
    // price the position, but somebody needs to look at that row.
    auto resolved = resolve_price_multiplier("ZN", 7.0);
    EXPECT_DOUBLE_EQ(resolved.value, 7.0);
    EXPECT_EQ(resolved.source, MultiplierSource::ReportedUnrecognised);
}

TEST(ResolvePriceMultiplier, AnUnknownSymbolPassesTheReportedFigureThrough) {
    auto resolved = resolve_price_multiplier("WHEAT", 5000.0);
    EXPECT_DOUBLE_EQ(resolved.value, 5000.0);
    EXPECT_EQ(resolved.source, MultiplierSource::UnknownSymbol);
}

// ---------------------------------------------------------------------------
// The property that makes the whole thing hang together.
// ---------------------------------------------------------------------------

TEST(ContractMultiplier, NotionalOfARealPositionIsPlausible) {
    // Forty ten-year notes at 110.5, the position that started this.
    const double notional = 40.0 * 110.5 * multiplier_of("ZN");
    EXPECT_NEAR(notional, 4'420'000.0, 1.0);

    // Ten corn at 450 cents.
    EXPECT_NEAR(10.0 * 450.0 * multiplier_of("ZC"), 225'000.0, 1.0);

    // Five full-size E-minis at 5,800, by the contract specification.
    EXPECT_NEAR(5.0 * 5800.0 * spec_multiplier_of("ES"), 1'450'000.0, 1.0);
}
