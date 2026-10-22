// The live futures run and portfolio.json's two optional blocks (data/listing_dates.hpp,
// live/live_listing_guard.hpp). Listing dates: a live run never trades a predecessor contract, so
// with contracts declared it refuses, before anything is stored, a predecessor in its universe or
// in its stored Day T-1 book on or after the listing date, and a signal date before the listing
// date of a listed contract it runs. Relabels: a declared vendor relabel is read on every bar, on
// every id row and on the contract recorded on the stored Day T-1 row, in both runners alike.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/live/live_listing_guard.hpp"

using namespace trade_ngin;

namespace {

const std::vector<ListedContract> kPairs = {{"MES", "ES", "2019-05-06", 10.0},
                                            {"M2K", "RTY", "2019-05-06", 10.0}};

std::unordered_map<std::string, Position> book(
    const std::vector<std::pair<std::string, double>>& rows) {
    std::unordered_map<std::string, Position> out;
    for (const auto& [symbol, quantity] : rows) {
        Position p;
        p.symbol = symbol;
        p.quantity = Quantity(quantity);
        out[symbol] = p;
    }
    return out;
}

std::string read_source(const std::string& relative) {
    namespace fs = std::filesystem;
    // this source file's own tree first (tests/live/ -> the repository root), then the working directory
    const fs::path own = fs::path(__FILE__).parent_path().parent_path().parent_path();
    if (fs::exists(own / relative)) {
        std::ifstream in(own / relative);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) {
            std::ifstream in(dir / relative);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
        dir = dir.parent_path();
    }
    return {};
}

const char* const kLiveRunners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                                    "apps/strategies/live_portfolio.cpp"};

}  // namespace

TEST(LiveListingGuard, NothingDeclaredOrNothingFoundRefusesNothing) {
    EXPECT_TRUE(live_listing_refusals({}, {"ES.v.0", "MES.v.0"}, book({{"ES.v.0", 1.0}}), "2026-04-23").empty());
    EXPECT_TRUE(live_listing_refusals(kPairs, {"MES.v.0", "M2K.v.0", "ZN.v.0"},
                                      book({{"MES.v.0", 3.0}, {"ZN.v.0", -1.0}}), "2026-04-23")
                    .empty());
    // the listing date itself is inside the listed contract's window
    EXPECT_TRUE(live_listing_refusals(kPairs, {"MES.v.0", "M2K.v.0"}, book({}), "2019-05-06").empty());
}

TEST(LiveListingGuard, APredecessorInTheUniverseOnOrAfterTheListingDateRefuses) {
    for (const char* date : {"2019-05-06", "2026-04-23"}) {
        const auto lines = live_listing_refusals(kPairs, {"ES.v.0", "MES.v.0", "ZN.v.0"}, book({}), date);
        ASSERT_EQ(lines.size(), 1u) << date;
        EXPECT_EQ(lines[0],
                  std::string("LISTING_DATE_REFUSAL symbol ES.v.0 is in the run's universe on signal date ") +
                      date +
                      ", on or after the listing date 2019-05-06 of MES, the contract the book trades "
                      "from that date. Refusing to run: a predecessor contract is not traded on or after "
                      "its listed contract's listing date. Remove ES.v.0's rows from "
                      "futures_data.ohlcv_1d, or correct portfolio.json listing_dates.");
    }
    // each predecessor is named
    EXPECT_EQ(live_listing_refusals(kPairs, {"ES.v.0", "RTY.v.0", "MES.v.0"}, book({}), "2026-04-23").size(), 2u);
}

TEST(LiveListingGuard, APredecessorInTheUniverseBeforeTheListingDateIsNotRefusedForIt) {
    EXPECT_TRUE(live_listing_refusals(kPairs, {"ES.v.0", "ZN.v.0"}, book({}), "2019-05-05").empty());
}

TEST(LiveListingGuard, AStoredPredecessorPositionOnOrAfterTheListingDateRefuses) {
    const auto lines = live_listing_refusals(
        kPairs, {"MES.v.0", "M2K.v.0"}, book({{"ES.v.0", 1.0}, {"MES.v.0", 2.0}, {"RTY.v.0", 0.0}}),
        "2026-04-23");
    ASSERT_EQ(lines.size(), 1u) << "a stored row of quantity 0 is no position";
    EXPECT_EQ(lines[0],
              "LISTING_DATE_REFUSAL stored position ES.v.0 (quantity 1.000000, Day T-1 2026-04-23) is in "
              "a predecessor contract on or after the listing date 2019-05-06 of MES. Refusing to run: "
              "a predecessor contract is not held on or after its listed contract's listing date. "
              "Correct that trading.positions row, or correct portfolio.json listing_dates.");
    // a short is a position too
    EXPECT_EQ(live_listing_refusals(kPairs, {"MES.v.0"}, book({{"RTY.v.0", -2.0}}), "2019-05-06").size(), 1u);
}

TEST(LiveListingGuard, ASignalDateBeforeTheListingDateOfAListedContractRefuses) {
    const auto lines = live_listing_refusals(kPairs, {"MES.v.0", "ZN.v.0"}, book({{"ES.v.0", 1.0}}), "2019-05-05");
    ASSERT_EQ(lines.size(), 1u) << "before the listing date the stored predecessor is not what is refused";
    EXPECT_EQ(lines[0],
              "LISTING_DATE_REFUSAL signal date 2019-05-05 is before the listing date 2019-05-06 of "
              "MES.v.0, which is in the run's universe. Refusing to run: a live run does not trade the "
              "predecessor contract ES. Run a date on or after the listing date, or correct "
              "portfolio.json listing_dates.");
    // a run that does not carry the listed contract is not refused for it
    EXPECT_TRUE(live_listing_refusals(kPairs, {"ZN.v.0"}, book({}), "2019-05-05").empty());
}

// Both live runners: the blocks are installed and the guard judged where the symbol list is read,
// above the first write of the day, and the relabel is read on the bars, on both id loads and on
// the contract recorded on the stored rows. The twins carry the same text.
TEST(LiveListingGuard, BothLiveRunnersInstallTheBlocksAndRefuseBeforeTheFirstWrite) {
    std::vector<std::string> hunks;
    for (const char* runner : kLiveRunners) {
        const std::string src = read_source(runner);
        ASSERT_FALSE(src.empty()) << runner;
        const size_t relabels = src.find("ListingDates::instance().set_relabels(app_config.instrument_id_relabels);");
        const size_t contracts = src.find("ListingDates::instance().set(app_config.listing_dates);");
        const size_t guard = src.find("live_listing_refusals(");
        const size_t es_filter = src.find("s == \"ES.v.0\"");
        const size_t bars = src.find("db->get_market_data(symbols, start_date, end_date");
        const size_t first_write = src.find("db->store_live_run_metadata(");
        ASSERT_NE(relabels, std::string::npos) << runner;
        ASSERT_NE(contracts, std::string::npos) << runner;
        ASSERT_NE(guard, std::string::npos) << runner;
        ASSERT_NE(es_filter, std::string::npos) << runner;
        ASSERT_NE(bars, std::string::npos) << runner;
        ASSERT_NE(first_write, std::string::npos) << runner;
        EXPECT_LT(relabels, bars) << runner << ": the relabels are installed before any bar is loaded";
        EXPECT_LT(guard, es_filter) << runner << ": the guard reads the symbol list before any name filter";
        EXPECT_LT(guard, first_write) << runner << ": a refused run leaves no row";
        const size_t refusal_exit = src.find("if (!listing_refusals.empty()) {\n                    return 1;", guard);
        ASSERT_NE(refusal_exit, std::string::npos) << runner;
        EXPECT_LT(refusal_exit, first_write) << runner;

        // the stored book is read sleeve by sleeve: one sleeve's zero row cannot hide another's holding
        EXPECT_NE(src.find("combined_strategy_id, sleeve, portfolio_id, now - std::chrono::hours(24),\n"
                           "                        \"trading.positions\");\n"
                           "                    if (sleeve_book.is_error()) {"),
                  std::string::npos)
            << runner;
        EXPECT_NE(src.find("ListingDates::instance().apply_relabels(all_bars);"), std::string::npos) << runner;
        EXPECT_NE(src.find("for (const auto& line : ListingDates::instance().relabel_findings(all_bars)) WARN(line);\n"
                           "        ListingDates::instance().apply_relabels(all_bars);"),
                  std::string::npos)
            << runner << ": a relabel that matches nothing is reported before the bars are rewritten";
        EXPECT_NE(src.find("for (const auto& line : ListingDates::instance().relabel_findings(history)) WARN(line);\n"
                           "            ListingDates::instance().apply_relabels(history);"),
                  std::string::npos)
            << runner << ": and on the bars before the window";
        EXPECT_NE(src.find("ListingDates::instance().apply_relabels(history);"), std::string::npos) << runner;
        size_t id_reads = 0;
        for (size_t at = src.find("db->get_futures_instrument_ids("); at != std::string::npos;
             at = src.find("db->get_futures_instrument_ids(", at + 1)) {
            ++id_reads;
            const size_t wrap = src.rfind("ListingDates::instance().read_ids(", at);
            ASSERT_NE(wrap, std::string::npos) << runner;
            EXPECT_LT(at - wrap, 140u) << runner << ": an id load is read outside read_ids";
        }
        EXPECT_EQ(id_reads, 2u) << runner;
        EXPECT_NE(src.find("contract = ListingDates::instance().read_id(\n"
                           "                            symbol, now - std::chrono::hours(24), position.instrument_id);"),
                  std::string::npos)
            << runner << ": the contract recorded on the stored Day T-1 row";
        EXPECT_NE(src.find("held->second = ListingDates::instance().read_id(symbol, now, contract);"),
                  std::string::npos)
            << runner << ": the contract of today's stored ROLL legs";
        EXPECT_EQ(src.find("contract = position.instrument_id;"), std::string::npos) << runner;

        const size_t from = src.find("            // Declared instrument-id relabels and listing dates");
        const size_t to = src.find("            // Remove continuous contract variants (.c.0) and full-size ES");
        ASSERT_NE(from, std::string::npos) << runner;
        ASSERT_NE(to, std::string::npos) << runner;
        hunks.push_back(src.substr(from, to - from));
    }
    ASSERT_EQ(hunks.size(), 2u);
    EXPECT_EQ(hunks[0], hunks[1]) << "the twins differ";
}

// Both backtest runners: the blocks are installed where the symbol list is built, before the
// strategies read the registry; the full-size rewrite goes off with the contracts; the predecessors
// of the window are appended and one without a metadata row stops the run; the coordinator reads
// every id load through the one reader. The twins carry the same text.
TEST(ListingRunnerWiring, BothBacktestRunnersInstallTheBlocksAndTheCoordinatorReadsIdsThroughTheReader) {
    std::vector<std::string> hunks;
    for (const char* runner : {"apps/backtest/bt_portfolio_conservative.cpp", "apps/backtest/bt_portfolio.cpp"}) {
        const std::string src = read_source(runner);
        ASSERT_FALSE(src.empty()) << runner;
        const size_t from = src.find("            // Declared vendor id relabellings");
        const size_t to = src.find("            config.strategy_config.symbols = symbols;");
        ASSERT_NE(from, std::string::npos) << runner;
        ASSERT_NE(to, std::string::npos) << runner;
        ASSERT_LT(from, to) << runner;
        const std::string hunk = src.substr(from, to - from);
        for (const char* needed :
             {"ListingDates::instance().set_relabels(app_config.instrument_id_relabels);",
              "ListingDates::instance().set(app_config.listing_dates);",
              "ListingDates::instance().set_switch_rule(app_config.listing_switch_rule);",
              "InstrumentRegistry::instance().set_full_size_remap(false);",
              "ListingDates::instance().predecessor_symbols(\n                    symbols, config.strategy_config.start_date);",
              "const std::string wrong = ListingDates::ratio_error(",
              "if (!wrong.empty()) throw std::runtime_error(wrong);",
              "if (!InstrumentRegistry::instance().has_instrument(p)) {\n                        throw std::runtime_error(",
              "symbols.push_back(p);"}) {
            EXPECT_NE(hunk.find(needed), std::string::npos) << runner << " lacks: " << needed;
        }
        hunks.push_back(hunk);
    }
    ASSERT_EQ(hunks.size(), 2u);
    EXPECT_EQ(hunks[0], hunks[1]) << "the twins differ";

    const std::string coordinator = read_source("src/backtest/backtest_coordinator.cpp");
    ASSERT_FALSE(coordinator.empty());
    size_t id_reads = 0;
    for (size_t at = coordinator.find("pg->get_futures_instrument_ids("); at != std::string::npos;
         at = coordinator.find("pg->get_futures_instrument_ids(", at + 1)) {
        ++id_reads;
        const size_t wrap = coordinator.rfind("with_predecessor_ids(", at);
        ASSERT_NE(wrap, std::string::npos);
        EXPECT_LT(at - wrap, 120u) << "an id load is read outside the reader";
    }
    EXPECT_EQ(id_reads, 2u);
    EXPECT_NE(coordinator.find("return ListingDates::instance().read_ids(symbols, std::move(ids));"),
              std::string::npos);
}
