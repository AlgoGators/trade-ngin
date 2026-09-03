// Coverage for execution_manager.cpp. ExecutionManager owns a
// TransactionCostManager so we use a real one with a default config.
//
// Targets:
// - generate_daily_executions on adds, increases, decreases, and closes
// - generate_execution sets side correctly for buy/sell, populates IDs and
//   transaction-cost fields
// - generate_date_string formats YYYYMMDD
// - generate_exec_id encodes symbol/timestamp/sequence
// - update_market_data populates the prev_close map

#include <gtest/gtest.h>
#include <chrono>
#include <unordered_map>
#include "trade_ngin/live/execution_manager.hpp"

using namespace trade_ngin;

namespace {

Position make_position(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(avg_price);
    return p;
}

Timestamp at_local_date(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

}  // namespace

class ExecutionManagerTest : public ::testing::Test {};

// ===== generate_daily_executions =====

TEST_F(ExecutionManagerTest, EmptyPositionsProducesNoExecutions) {
    ExecutionManager em;
    auto r = em.generate_daily_executions({}, {}, {}, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(ExecutionManagerTest, NewPositionGeneratesBuyExecution) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr{{"ES", make_position("ES", 5.0, 4500.0)}};
    std::unordered_map<std::string, Position> prev;
    std::unordered_map<std::string, double> prices{{"ES", 4500.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].side, Side::BUY);
    EXPECT_EQ(r.value()[0].symbol, "ES");
}

TEST_F(ExecutionManagerTest, ReducedPositionGeneratesSellExecution) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr{{"ES", make_position("ES", 3.0, 4500.0)}};
    std::unordered_map<std::string, Position> prev{{"ES", make_position("ES", 5.0, 4500.0)}};
    std::unordered_map<std::string, double> prices{{"ES", 4505.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].side, Side::SELL);
}

TEST_F(ExecutionManagerTest, UnchangedPositionProducesNoExecution) {
    ExecutionManager em;
    auto pos = make_position("ES", 5.0, 4500.0);
    std::unordered_map<std::string, Position> curr{{"ES", pos}};
    std::unordered_map<std::string, Position> prev{{"ES", pos}};
    std::unordered_map<std::string, double> prices{{"ES", 4500.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(ExecutionManagerTest, ClosedPositionGeneratesOppositeSideExecution) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr;
    std::unordered_map<std::string, Position> prev{{"ES", make_position("ES", 5.0, 4500.0)}};
    std::unordered_map<std::string, double> prices{{"ES", 4505.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].side, Side::SELL);  // Closing a long → SELL
}

TEST_F(ExecutionManagerTest, ClosedShortPositionGeneratesBuyExecution) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr;
    std::unordered_map<std::string, Position> prev{{"ES", make_position("ES", -3.0, 4500.0)}};
    std::unordered_map<std::string, double> prices{{"ES", 4505.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].side, Side::BUY);
}

// This test used to be MissingMarketPriceFallsBackToAveragePrice and asserted that a
// symbol with no market price was filled at its average_price. That fallback was the
// bug: average_price is a COST BASIS, and for a position opened today it is 0 until
// the fill being priced has been processed -- so the fallback booked fills at zero,
// persisted the zero as the new basis, and reloaded it the next session as a carried
// basis of zero. The old assertion passed only because the fixture handed it a
// pre-set basis of 4500.0, which production does not have for a new position.
//
// A symbol we cannot price is a symbol we must not trade.
TEST_F(ExecutionManagerTest, MissingMarketPriceSkipsTheExecutionRatherThanPricingItFromBasis) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr{{"ES", make_position("ES", 2.0, 4500.0)}};
    std::unordered_map<std::string, Position> prev;
    std::unordered_map<std::string, double> prices;  // no price for ES
    std::vector<std::string> unpriced;
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now(),
                                          PricingPolicy::STRICT, &unpriced);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty()) << "an unpriceable symbol must not generate an execution";
    ASSERT_EQ(unpriced.size(), 1u);
    EXPECT_EQ(unpriced[0], "ES");
}

// A brand-new position has no basis at all. This is the production shape the old
// fallback actually met, and the one that made the zero self-sustaining.
TEST_F(ExecutionManagerTest, MissingMarketPriceOnAZeroBasisPositionDoesNotFillAtZero) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr{{"ES", make_position("ES", 2.0, 0.0)}};
    std::unordered_map<std::string, Position> prev;
    std::unordered_map<std::string, double> prices;
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now(),
                                          PricingPolicy::STRICT);
    ASSERT_TRUE(r.is_ok());
    for (const auto& e : r.value()) {
        EXPECT_GT(e.fill_price.as_double(), 0.0) << "fill booked at a non-positive price";
    }
    EXPECT_TRUE(r.value().empty());
}

// A price that is present but non-positive is not a price either.
TEST_F(ExecutionManagerTest, NonPositiveMarketPriceIsRefused) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr{{"ES", make_position("ES", 2.0, 4500.0)}};
    std::unordered_map<std::string, Position> prev;
    std::unordered_map<std::string, double> prices{{"ES", 0.0}};
    std::vector<std::string> unpriced;
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now(),
                                          PricingPolicy::STRICT, &unpriced);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
    ASSERT_EQ(unpriced.size(), 1u);
    EXPECT_EQ(unpriced[0], "ES");
}

// A close-out with no price must not book a fill at the position's own basis, which
// would silently report zero realized PnL on the exit.
TEST_F(ExecutionManagerTest, MissingMarketPriceOnCloseOutSkipsRatherThanUsingBasis) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr;
    std::unordered_map<std::string, Position> prev{{"ES", make_position("ES", 3.0, 4500.0)}};
    std::unordered_map<std::string, double> prices;  // no price for ES
    std::vector<std::string> unpriced;
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now(),
                                          PricingPolicy::STRICT, &unpriced);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
    ASSERT_EQ(unpriced.size(), 1u);
    EXPECT_EQ(unpriced[0], "ES");
}

// ===== generate_execution =====

TEST_F(ExecutionManagerTest, GenerateExecutionPopulatesAllFields) {
    ExecutionManager em;
    auto ts = at_local_date(2026, 4, 28);
    auto exec = em.generate_execution("ES", 3.0, 4500.0, ts, 0);
    EXPECT_EQ(exec.symbol, "ES");
    EXPECT_EQ(exec.side, Side::BUY);
    EXPECT_DOUBLE_EQ(exec.filled_quantity.as_double(), 3.0);
    EXPECT_DOUBLE_EQ(exec.fill_price.as_double(), 4500.0);
    EXPECT_FALSE(exec.is_partial);
    EXPECT_NE(exec.exec_id.find("EXEC_ES_"), std::string::npos);
    EXPECT_NE(exec.order_id.find("DAILY_ES_"), std::string::npos);
}

TEST_F(ExecutionManagerTest, GenerateExecutionSequenceProducesDistinctIds) {
    ExecutionManager em;
    auto ts = at_local_date(2026, 4, 28);
    auto e0 = em.generate_execution("ES", 1.0, 4500.0, ts, 0);
    auto e1 = em.generate_execution("ES", 1.0, 4500.0, ts, 1);
    EXPECT_NE(e0.exec_id, e1.exec_id);
}

// ===== Static helpers =====

TEST_F(ExecutionManagerTest, GenerateDateStringHasYYYYMMDDFormat) {
    auto s = ExecutionManager::generate_date_string(at_local_date(2026, 4, 28));
    EXPECT_EQ(s.length(), 8u);
    EXPECT_NE(s.find("20260428"), std::string::npos);
}

TEST_F(ExecutionManagerTest, GenerateExecIdEncodesSymbolAndSequence) {
    auto s = ExecutionManager::generate_exec_id("ES", at_local_date(2026, 4, 28), 7);
    EXPECT_NE(s.find("EXEC_ES_"), std::string::npos);
    EXPECT_NE(s.find("_7"), std::string::npos);
}

// ===== update_market_data populates prev_close map =====

TEST_F(ExecutionManagerTest, UpdateMarketDataDoesNotThrow) {
    ExecutionManager em;
    EXPECT_NO_THROW(em.update_market_data("ES", 1000.0, 4500.0));
    EXPECT_NO_THROW(em.update_market_data("ES", 1100.0, 4510.0));
}

// ---------------------------------------------------------------------------
// Futures behaviour preservation.
//
// TrendFollowingStrategy sets Position::average_price to price_history.back() --
// the latest mark, by design (trend_following.cpp:623, REALIZED_ONLY daily
// settlement). So for futures the mark fallback prices the fill at a real,
// one-session-stale close, and removing it would replace a correct fill with a
// skip. MARK_FALLBACK is the default precisely so the two futures runners, which
// pass no policy argument, keep the behaviour they have always had.
// ---------------------------------------------------------------------------

TEST_F(ExecutionManagerTest, MarkFallbackPricesFuturesFillFromTheLatestMark) {
    ExecutionManager em;
    // ZC on a Monday with no Sunday print: average_price holds Friday's close.
    std::unordered_map<std::string, Position> curr{{"ZC", make_position("ZC", 10.0, 450.25)}};
    std::unordered_map<std::string, Position> prev;
    std::unordered_map<std::string, double> prices;  // no T-1 print

    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());

    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u) << "futures must still generate the execution";
    EXPECT_DOUBLE_EQ(r.value()[0].fill_price.as_double(), 450.25)
        << "fill must be priced at the mark, exactly as before the STRICT policy existed";
    EXPECT_DOUBLE_EQ(r.value()[0].filled_quantity.as_double(), 10.0);
}

TEST_F(ExecutionManagerTest, MarkFallbackClosesFuturesPositionAtTheLatestMark) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr;
    std::unordered_map<std::string, Position> prev{{"ZC", make_position("ZC", 10.0, 450.25)}};
    std::unordered_map<std::string, double> prices;

    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());

    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u) << "the close-out must still be generated for futures";
    EXPECT_DOUBLE_EQ(r.value()[0].fill_price.as_double(), 450.25);
}

// The default must remain MARK_FALLBACK. If this flips, both futures runners
// silently change behaviour without their call sites being touched.
TEST_F(ExecutionManagerTest, DefaultPolicyIsMarkFallbackSoFuturesCallersAreUnaffected) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr{{"ES", make_position("ES", 1.0, 4500.0)}};
    std::unordered_map<std::string, Position> prev;
    std::unordered_map<std::string, double> prices;

    auto defaulted = em.generate_daily_executions(curr, prev, prices,
                                                  std::chrono::system_clock::now());
    auto explicit_mark = em.generate_daily_executions(curr, prev, prices,
                                                      std::chrono::system_clock::now(),
                                                      PricingPolicy::MARK_FALLBACK);
    ASSERT_TRUE(defaulted.is_ok());
    ASSERT_TRUE(explicit_mark.is_ok());
    ASSERT_EQ(defaulted.value().size(), explicit_mark.value().size());
    ASSERT_EQ(defaulted.value().size(), 1u);
    EXPECT_DOUBLE_EQ(defaulted.value()[0].fill_price.as_double(),
                     explicit_mark.value()[0].fill_price.as_double());
}

// ============================================================================
// E2-F29: SEC/TAF regulatory fees must reach a SELL fill.
//
// TransactionCostManager gates the fees on `quantity < 0`; the live caller passed
// |quantity| for both sides, so on a config with apply_regulatory_fees the sell
// side was charged exactly what the buy side was. SELL cost - BUY cost must equal
// sec_fee + taf for the same clip.
// ============================================================================
TEST_F(ExecutionManagerTest, SellSideCarriesRegulatoryFeesWhenConfigured) {
    ExecutionManager em;
    transaction_cost::AssetCostConfig cfg;
    cfg.symbol = "TIERD";
    cfg.asset_type = AssetType::EQUITY;
    cfg.commission_per_unit = 0.0035;
    cfg.min_commission_per_order = 0.35;
    cfg.max_commission_per_order = 1e9;
    cfg.apply_regulatory_fees = true;
    cfg.sec_fee_per_million = 20.60;
    cfg.finra_taf_per_share = 0.000195;
    cfg.finra_taf_cap_per_trade = 9.79;
    em.get_transaction_cost_manager().register_asset_config(cfg);

    const double qty = 1000.0, px = 50.0;
    std::unordered_map<std::string, double> prices{{"TIERD", px}};
    std::unordered_map<std::string, Position> flat;
    std::unordered_map<std::string, Position> held{{"TIERD", make_position("TIERD", qty, px)}};

    auto buy = em.generate_daily_executions(held, flat, prices, std::chrono::system_clock::now());
    auto sell = em.generate_daily_executions(flat, held, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(buy.is_ok() && sell.is_ok());
    ASSERT_EQ(buy.value().size(), 1u);
    ASSERT_EQ(sell.value().size(), 1u);
    ASSERT_EQ(buy.value()[0].side, Side::BUY);
    ASSERT_EQ(sell.value()[0].side, Side::SELL);

    const double sec_fee = (qty * px / 1e6) * cfg.sec_fee_per_million;   // 1.03
    const double taf = std::min(qty * cfg.finra_taf_per_share, cfg.finra_taf_cap_per_trade);  // 0.195
    EXPECT_NEAR(sell.value()[0].commissions_fees.as_double() - buy.value()[0].commissions_fees.as_double(),
                sec_fee + taf, 1e-9)
        << "SELL must carry SEC + TAF on top of the BUY-side commission";
    EXPECT_NEAR(sell.value()[0].total_transaction_costs.as_double() -
                    buy.value()[0].total_transaction_costs.as_double(),
                sec_fee + taf, 1e-9);
}
