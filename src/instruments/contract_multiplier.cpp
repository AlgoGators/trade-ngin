// src/instruments/contract_multiplier.cpp
#include "trade_ngin/instruments/contract_multiplier.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace trade_ngin {

namespace {

// Quote-convention scales. Named rather than repeated as a bare 0.01 so that a
// reader can see WHY a contract is scaled, which is the part that was missing.
constexpr double PER_UNIT = 1.0;         // price quoted in the underlying's own unit
constexpr double CENTS = 0.01;           // price quoted in cents per unit
constexpr double PERCENT_OF_PAR = 0.01;  // price quoted as a percentage of face value

/// Root symbol -> contract size and quote convention.
///
/// Contract sizes are the CME/CBOT/NYMEX/COMEX contract specifications. The
/// scale is the quote convention, and the product of the two is the point
/// value -- the number that multiplies quantity * price.
const std::unordered_map<std::string, QuotedContract>& table() {
    static const std::unordered_map<std::string, QuotedContract> specs = {
        // --- Equity index: quoted in index points. size == multiplier. ------
        {"ES", {50.0, PER_UNIT}},   // E-mini S&P 500,      $50 x index
        {"MES", {5.0, PER_UNIT}},   // Micro E-mini S&P 500
        {"NQ", {20.0, PER_UNIT}},   // E-mini Nasdaq 100,   $20 x index
        {"MNQ", {2.0, PER_UNIT}},   // Micro E-mini Nasdaq
        {"YM", {5.0, PER_UNIT}},    // E-mini Dow,          $5 x index
        {"MYM", {0.5, PER_UNIT}},   // Micro E-mini Dow
        {"RTY", {50.0, PER_UNIT}},  // E-mini Russell 2000, $50 x index
        {"M2K", {5.0, PER_UNIT}},   // Micro E-mini Russell

        // --- Interest rates: quoted as a PERCENTAGE OF PAR. -----------------
        // Contract size is face value, so one point is 1% of it. This is the
        // group where reading contract size as a multiplier is 100x wrong.
        {"ZT", {200000.0, PERCENT_OF_PAR}},  // 2-Year T-Note  -> $2,000/pt
        {"ZF", {100000.0, PERCENT_OF_PAR}},  // 5-Year T-Note  -> $1,000/pt
        {"ZN", {100000.0, PERCENT_OF_PAR}},  // 10-Year T-Note -> $1,000/pt
        {"TN", {100000.0, PERCENT_OF_PAR}},  // Ultra 10-Year  -> $1,000/pt
        {"ZB", {100000.0, PERCENT_OF_PAR}},  // 30-Year T-Bond -> $1,000/pt
        {"UB", {100000.0, PERCENT_OF_PAR}},  // Ultra T-Bond   -> $1,000/pt

        // --- FX: quoted in USD per unit of the foreign currency. ------------
        // Size == multiplier. The micros are a TENTH of the full-size contract,
        // which the substring-matching tables this replaces got wrong: "M6E"
        // contains "6E", so the micro euro was priced as the full one.
        {"6A", {100000.0, PER_UNIT}},    // Australian dollar
        {"6B", {62500.0, PER_UNIT}},     // British pound
        {"6C", {100000.0, PER_UNIT}},    // Canadian dollar
        {"6E", {125000.0, PER_UNIT}},    // Euro
        {"6J", {12500000.0, PER_UNIT}},  // Japanese yen
        {"6M", {500000.0, PER_UNIT}},    // Mexican peso
        {"6N", {100000.0, PER_UNIT}},    // New Zealand dollar
        {"6S", {125000.0, PER_UNIT}},    // Swiss franc
        {"M6A", {10000.0, PER_UNIT}},    // E-micro Australian dollar
        {"M6B", {6250.0, PER_UNIT}},     // E-micro British pound
        {"M6E", {12500.0, PER_UNIT}},    // E-micro euro
        {"MSF", {12500.0, PER_UNIT}},    // E-micro Swiss franc

        // --- Metals: quoted per ounce or per pound. size == multiplier. -----
        {"GC", {100.0, PER_UNIT}},    // Gold,      $/oz on 100 oz
        {"MGC", {10.0, PER_UNIT}},    // Micro gold
        {"SI", {5000.0, PER_UNIT}},   // Silver,    $/oz on 5,000 oz
        {"SIL", {1000.0, PER_UNIT}},  // Micro silver
        {"HG", {25000.0, PER_UNIT}},  // Copper,    $/lb on 25,000 lb
        {"PL", {50.0, PER_UNIT}},     // Platinum,  $/oz on 50 oz
        {"PA", {100.0, PER_UNIT}},    // Palladium, $/oz on 100 oz

        // --- Energy: quoted per barrel, MMBtu or gallon. size == multiplier.
        {"CL", {1000.0, PER_UNIT}},   // WTI crude,     $/bbl on 1,000 bbl
        {"MCL", {100.0, PER_UNIT}},   // Micro crude
        {"BZ", {1000.0, PER_UNIT}},   // Brent crude
        {"NG", {10000.0, PER_UNIT}},  // Natural gas,   $/MMBtu on 10,000
        {"RB", {42000.0, PER_UNIT}},  // RBOB gasoline, $/gal on 42,000 gal
        {"HO", {42000.0, PER_UNIT}},  // Heating oil,   $/gal on 42,000 gal

        // --- Grains and oilseeds: mostly quoted in CENTS. -------------------
        {"ZC", {5000.0, CENTS}},   // Corn,        cents/bu on 5,000 bu -> $50/pt
        {"ZS", {5000.0, CENTS}},   // Soybeans,    cents/bu on 5,000 bu -> $50/pt
        {"ZW", {5000.0, CENTS}},   // Chicago wheat                     -> $50/pt
        {"KE", {5000.0, CENTS}},   // KC wheat                          -> $50/pt
        {"ZO", {5000.0, CENTS}},   // Oats                              -> $50/pt
        {"ZL", {60000.0, CENTS}},  // Soybean oil, cents/lb on 60,000 lb -> $600/pt
        // The two that are NOT quoted in cents, and so are their own multiplier:
        {"ZM", {100.0, PER_UNIT}},   // Soybean meal, $/short ton on 100 tons
        {"ZR", {2000.0, PER_UNIT}},  // Rough rice,   $/cwt on 2,000 cwt

        // --- Livestock: quoted in CENTS per pound. --------------------------
        {"LE", {40000.0, CENTS}},  // Live cattle,   40,000 lb -> $400/pt
        {"HE", {40000.0, CENTS}},  // Lean hogs,     40,000 lb -> $400/pt
        {"GF", {50000.0, CENTS}},  // Feeder cattle, 50,000 lb -> $500/pt

        // --- Volatility -----------------------------------------------------
        {"VX", {1000.0, PER_UNIT}},  // VIX, $1,000 x index
    };
    return specs;
}

/// Symbols this deployment prices as something other than their own contract.
///
/// These are NOT contract specifications, and that is the point of keeping them
/// in a separate map. They reproduce, exactly, what the two hardcoded tables
/// this module replaces already did, so that removing those tables changes the
/// arithmetic only where it was provably wrong.
///
/// The first group is deliberate elsewhere too: InstrumentRegistry::get_instrument
/// rewrites ES to MES, YM to MYM and NQ to MNQ before it looks anything up, so
/// this deployment reads a full-size equity-index ticker as the micro contract.
/// The rest come from the old tables' substring matching, where "M6E" contained
/// "6E" and was priced as the full-size euro.
///
/// EVERY ENTRY HERE IS A TEN-TIMES QUESTION and none of them is settled by
/// anything in this repository. If the fund trades full-size Russell contracts,
/// RTY is priced a tenth of what it should be; if it trades E-micro euro, M6E is
/// priced ten times too high. Deciding that needs the contract the fund actually
/// holds, which is not a thing this file can know -- so it preserves the
/// existing answer rather than quietly substituting a different one.
const std::unordered_map<std::string, std::string>& deployment_aliases() {
    static const std::unordered_map<std::string, std::string> aliases = {
        // Full-size equity-index tickers read as the micro contract, as
        // InstrumentRegistry already does for the first three.
        {"ES", "MES"},
        {"NQ", "MNQ"},
        {"YM", "MYM"},
        {"RTY", "M2K"},
        // Micro tickers read as the full-size contract, from substring matching.
        {"MGC", "GC"},
        {"MSF", "6S"},
        {"M6B", "6B"},
        {"M6E", "6E"},
        // Alternate vendor tickers for the same contract. These two are not in
        // question -- YK is soybeans and YW is wheat.
        {"YK", "ZS"},
        {"YW", "ZW"},
    };
    return aliases;
}

/// True when suffix is a futures month/year code: a month letter followed by
/// one or two year digits, as in H5 or Z24.
bool is_month_year_code(const std::string& suffix) {
    if (suffix.size() < 2 || suffix.size() > 3) {
        return false;
    }
    if (!std::isalpha(static_cast<unsigned char>(suffix[0]))) {
        return false;
    }
    for (std::size_t i = 1; i < suffix.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

/// Two figures agree if they match at a relative tolerance. Relative, because
/// these span 0.5 (micro Dow) to 12,500,000 (yen).
bool same_number(double a, double b) {
    const double scale = std::max(std::max(std::abs(a), std::abs(b)), 1.0);
    return std::abs(a - b) <= 1e-9 * scale;
}

}  // namespace

std::string root_symbol(const std::string& symbol) {
    std::string root = symbol;

    // Continuous-contract suffixes, as data-ngin writes them: ES.v.0, ZC.c.1.
    for (const char* marker : {".v.", ".c.", ".n."}) {
        auto pos = root.find(marker);
        if (pos != std::string::npos) {
            root = root.substr(0, pos);
        }
    }

    // A dated contract: strip a trailing month/year code, but only when what
    // remains is a root we actually know. ZCH5 -> ZC; MES keeps its S, because
    // ME is not in the table.
    if (table().count(root) == 0) {
        for (std::size_t take = 1; take < root.size(); ++take) {
            const std::string head = root.substr(0, take);
            if (table().count(head) > 0 && is_month_year_code(root.substr(take))) {
                return head;
            }
        }
    }

    return root;
}

std::optional<QuotedContract> known_contract(const std::string& symbol) {
    const auto& specs = table();
    auto it = specs.find(root_symbol(symbol));
    if (it == specs.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::string deployment_symbol(const std::string& symbol) {
    const auto& aliases = deployment_aliases();
    auto it = aliases.find(root_symbol(symbol));
    return it == aliases.end() ? root_symbol(symbol) : it->second;
}

std::optional<double> fallback_price_multiplier(const std::string& symbol) {
    // Aliases apply here and nowhere else. resolve_price_multiplier deals with a
    // metadata row that names its own symbol, so it must take that symbol
    // literally; this function is answering "what does this deployment price
    // that ticker as", which is a different question.
    auto spec = known_contract(deployment_symbol(symbol));
    if (!spec) {
        return std::nullopt;
    }
    return spec->price_multiplier();
}

ResolvedMultiplier resolve_price_multiplier(const std::string& symbol, double reported) {
    auto spec = known_contract(symbol);
    if (!spec) {
        return {reported, MultiplierSource::UnknownSymbol};
    }

    // Where the contract size and the point value coincide -- every contract
    // quoted in its own unit -- both branches return the same number, and which
    // one is taken first does not matter.
    if (same_number(reported, spec->contract_size)) {
        return {spec->price_multiplier(), MultiplierSource::ScaledContractSize};
    }
    if (same_number(reported, spec->price_multiplier())) {
        return {reported, MultiplierSource::AlreadyPointValue};
    }
    return {reported, MultiplierSource::ReportedUnrecognised};
}

const char* describe(MultiplierSource source) {
    switch (source) {
        case MultiplierSource::ScaledContractSize:
            return "contract size scaled by quote convention";
        case MultiplierSource::AlreadyPointValue:
            return "reported figure already a point value";
        case MultiplierSource::ReportedUnrecognised:
            return "reported figure matches neither known contract size nor point value";
        case MultiplierSource::UnknownSymbol:
            return "symbol not in the contract table";
    }
    return "unknown";
}

}  // namespace trade_ngin
