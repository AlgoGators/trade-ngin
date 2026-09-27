#include <gtest/gtest.h>
#include "trade_ngin/apps/qt_equity_position_transition.hpp"
#include "trade_ngin/live/execution_price_resolver.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"
#include <limits>

using namespace trade_ngin;
namespace {
Timestamp stamp() { return std::chrono::sys_days{std::chrono::year{2026}/9/25}; }
ComponentPositionKey key(std::string owner="owner-a") {
    return {"BOOK", "ENGINE", std::move(owner), "2026-09-25", "SYN", "qt"};
}
Position prior(double q, double basis) {
    return Position{"SYN", Quantity(q), Decimal(basis), Decimal(9), Decimal(7), stamp()};
}
CorpActionEvent event(CorpActionType type, double value) {
    CorpActionEvent result;
    result.symbol="SYN"; result.ex_date="2026-09-25"; result.type=type; result.value=value;
    result.basis_provenance=CorpActionEvent::BasisProvenance::FORMED_ON_OR_BEFORE_EX_DATE;
    result.basis_provenance_evidence="owned prior snapshot";
    return result;
}
}

TEST(QtEquityPositionTransition, FractionalLongAddUsesWeightedBasisAndGrossPnl) {
    auto result=produce_qt_equity_position_transition(key(), prior(1.5,10), Quantity(2), 14, 16, stamp(), {});
    ASSERT_TRUE(result.is_ok()); const auto& out=result.value();
    EXPECT_EQ(out.quantity_change.to_string(), "0.5");
    EXPECT_EQ(out.position.quantity.to_string(), "2");
    EXPECT_EQ(out.position.average_price.to_string(), "11");
    EXPECT_EQ(out.gross_trade_realized_pnl.to_string(), "0");
    EXPECT_EQ(out.position.realized_pnl.to_string(), "0");
    EXPECT_EQ(out.position.unrealized_pnl.to_string(), "10");
    EXPECT_EQ(out.key, key());
}

TEST(QtEquityPositionTransition, FractionalShortAddUsesWeightedBasis) {
    auto result=produce_qt_equity_position_transition(key(), prior(-1.5,10), Quantity(-2), 14, 16, stamp(), {});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().quantity_change.to_string(), "-0.5");
    EXPECT_EQ(result.value().position.average_price.to_string(), "11");
    EXPECT_EQ(result.value().position.unrealized_pnl.to_string(), "-10");
}

TEST(QtEquityPositionTransition, PartialReductionRealizesOnlyClosedQuantityAndPreservesBasis) {
    auto result=produce_qt_equity_position_transition(key(), prior(1.5,10), Quantity(0.5), 14, 16, stamp(), {});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().gross_trade_realized_pnl.to_string(), "4");
    EXPECT_EQ(result.value().position.average_price.to_string(), "10");
    EXPECT_EQ(result.value().position.unrealized_pnl.to_string(), "3");
}

TEST(QtEquityPositionTransition, LongFlipRealizesClosedHoldingAndUsesFillBasisForNewShort) {
    auto result=produce_qt_equity_position_transition(key(), prior(1.5,10), Quantity(-0.5), 14, 16, stamp(), {});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().quantity_change.to_string(), "-2");
    EXPECT_EQ(result.value().gross_trade_realized_pnl.to_string(), "6");
    EXPECT_EQ(result.value().position.average_price.to_string(), "14");
    EXPECT_EQ(result.value().position.unrealized_pnl.to_string(), "-1");
}

TEST(QtEquityPositionTransition, ShortFlipRealizesClosedHoldingAndUsesFillBasisForNewLong) {
    auto result=produce_qt_equity_position_transition(key(), prior(-1.5,10), Quantity(0.5), 6, 16, stamp(), {});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().gross_trade_realized_pnl.to_string(), "6");
    EXPECT_EQ(result.value().position.average_price.to_string(), "6");
    EXPECT_EQ(result.value().position.unrealized_pnl.to_string(), "5");
}

TEST(QtEquityPositionTransition, CompleteCloseHasNoUnrealizedAndRetainsMainFlatBasisSemantics) {
    auto result=produce_qt_equity_position_transition(key(), prior(1.5,10), Quantity(0), 14, std::nullopt, stamp(), {});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().position.quantity.to_string(), "0");
    EXPECT_EQ(result.value().position.average_price.to_string(), "14");
    EXPECT_EQ(result.value().position.unrealized_pnl.to_string(), "0");
    EXPECT_EQ(result.value().gross_trade_realized_pnl.to_string(), "6");
}

TEST(QtEquityPositionTransition, ReferencePriceRoundsIntoActualDecimal8FillBeforeGrossPnl) {
    // Main ExecutionManager persists Decimal8 fill_price; BaseStrategy reads
    // that rounded fill rather than the original higher precision model close.
    auto result=produce_qt_equity_position_transition(key(), prior(100000,10), Quantity(0),
                                                    14.123456789, std::nullopt, stamp(), {});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().position.average_price.to_string(), "14.12345679");
    EXPECT_EQ(result.value().gross_trade_realized_pnl.to_string(), "412345.679");
}

TEST(QtEquityPositionTransition, NewFractionalHoldingNeedsNoInventedPriorBasis) {
    auto result=produce_qt_equity_position_transition(key(), std::nullopt, Quantity(0.25), 8, 12, stamp(), {});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().position.average_price.to_string(), "8");
    EXPECT_EQ(result.value().position.unrealized_pnl.to_string(), "1");
}

TEST(QtEquityPositionTransition, SingleDecimal8UnitChangeIsNeverSuppressedByTradeEpsilon) {
    auto result=produce_qt_equity_position_transition(key(), prior(1,10), Quantity::from_raw(100000001), 10, 11, stamp(), {});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().quantity_change.to_string(), "0.00000001");
    EXPECT_EQ(result.value().position.quantity.to_string(), "1.00000001");
    EXPECT_EQ(result.value().position.unrealized_pnl.to_string(), "1.00000001");
}

TEST(QtEquityPositionTransition, CarryNeedsNoFillPriceAndDoesNotCarryPreviousDailyRealized) {
    auto result=produce_qt_equity_position_transition(key(), prior(1,10), Quantity(1), std::nullopt, 12, stamp(), {});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().quantity_change.to_string(), "0");
    EXPECT_EQ(result.value().position.average_price.to_string(), "10");
    EXPECT_EQ(result.value().position.realized_pnl.to_string(), "0");
    EXPECT_EQ(result.value().position.unrealized_pnl.to_string(), "2");
}

TEST(QtEquityPositionTransition, SplitRestatesPriorBeforeComputingExactSelectedDelta) {
    auto result=produce_qt_equity_position_transition(key(), prior(1.5,12), Quantity(3), std::nullopt, 7, stamp(), {event(CorpActionType::SPLIT,2)});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().quantity_change.to_string(), "0");
    EXPECT_EQ(result.value().restated_previous.quantity.to_string(), "3");
    EXPECT_EQ(result.value().position.average_price.to_string(), "6");
    EXPECT_EQ(result.value().position.unrealized_pnl.to_string(), "3");
    ASSERT_EQ(result.value().adjustments.size(), 1U);
    EXPECT_EQ(result.value().adjustments[0].type, CorpActionType::SPLIT);
}

TEST(QtEquityPositionTransition, DividendUsesRawExDateCloseAndExplicitEligibility) {
    auto action=event(CorpActionType::DIVIDEND,2); action.close_at_ex_date=10; action.qty_at_ex_date=3;
    auto result=produce_qt_equity_position_transition(key(), prior(1.5,12), Quantity(1.5), std::nullopt, 11, stamp(), {action});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().position.average_price.to_string(), "10");
    EXPECT_EQ(result.value().position.quantity.to_string(), "1.5");
    EXPECT_EQ(result.value().position.unrealized_pnl.to_string(), "1.5");
    ASSERT_EQ(result.value().adjustments.size(),1U);
    EXPECT_DOUBLE_EQ(result.value().adjustments[0].quantity_before,3);
}

TEST(QtEquityPositionTransition, ProvenPostEventBasisIsNotRestatedTwice) {
    auto action=event(CorpActionType::ADR_SPLIT,2);
    action.basis_provenance=CorpActionEvent::BasisProvenance::FORMED_AFTER_EX_DATE;
    auto result=produce_qt_equity_position_transition(key(), prior(1.5,12), Quantity(1.5), std::nullopt, 13, stamp(), {action});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().position.average_price.to_string(),"12");
    EXPECT_TRUE(result.value().adjustments.empty());
}

TEST(QtEquityPositionTransition, OwnersSharingOneSymbolRemainIndependentAndNeverNet) {
    auto a=produce_qt_equity_position_transition(key("a"), prior(1.5,10), Quantity(-0.5), 14, 16, stamp(), {});
    auto b=produce_qt_equity_position_transition(key("b"), prior(-1.5,12), Quantity(0.5), 14, 16, stamp(), {});
    ASSERT_TRUE(a.is_ok()); ASSERT_TRUE(b.is_ok());
    EXPECT_NE(a.value().key,b.value().key);
    EXPECT_EQ(a.value().gross_trade_realized_pnl.to_string(),"6");
    EXPECT_EQ(b.value().gross_trade_realized_pnl.to_string(),"-3");
    EXPECT_EQ(a.value().position.quantity.to_string(),"-0.5");
    EXPECT_EQ(b.value().position.quantity.to_string(),"0.5");
}

TEST(QtEquityPositionTransition, MissingOrInvalidRequiredPricesRefuseWithoutInputMutation) {
    auto old=prior(1.5,10);
    for(auto value : {0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_FALSE(produce_qt_equity_position_transition(key(),old,Quantity(2),value,16,stamp(),{}).is_ok());
        EXPECT_FALSE(produce_qt_equity_position_transition(key(),old,Quantity(2),14,value,stamp(),{}).is_ok());
    }
    EXPECT_FALSE(produce_qt_equity_position_transition(key(),old,Quantity(2),std::nullopt,16,stamp(),{}).is_ok());
    EXPECT_FALSE(produce_qt_equity_position_transition(key(),old,Quantity(2),14,std::nullopt,stamp(),{}).is_ok());
    EXPECT_EQ(old.quantity.to_string(),"1.5"); EXPECT_EQ(old.average_price.to_string(),"10");
    EXPECT_EQ(old.realized_pnl.to_string(),"7");
}

TEST(QtEquityPositionTransition, HeldZeroBasisAndUnscopedInputsRefuse) {
    EXPECT_FALSE(produce_qt_equity_position_transition(key(),prior(1,0),Quantity(2),14,16,stamp(),{}).is_ok());
    auto bad=key(); bad.portfolio_type="system";
    EXPECT_FALSE(produce_qt_equity_position_transition(bad,prior(1,10),Quantity(2),14,16,stamp(),{}).is_ok());
    bad=key(); bad.symbol="OTHER";
    EXPECT_FALSE(produce_qt_equity_position_transition(bad,prior(1,10),Quantity(2),14,16,stamp(),{}).is_ok());
    bad=key(); bad.date="2026-02-30";
    EXPECT_FALSE(produce_qt_equity_position_transition(bad,prior(1,10),Quantity(2),14,16,stamp(),{}).is_ok());
}

TEST(QtEquityPositionTransition, UnknownMalformedDuplicateOrLifecycleActionsRefuseAtomically) {
    auto old=prior(1.5,12); auto action=event(CorpActionType::SPLIT,2);
    for(auto type : {CorpActionType::SPINOFF,CorpActionType::TERMINATION,CorpActionType::UNKNOWN}) {
        action.type=type;
        EXPECT_FALSE(produce_qt_equity_position_transition(key(),old,Quantity(3),14,16,stamp(),{action}).is_ok());
    }
    action=event(CorpActionType::SPLIT,2);
    action.basis_provenance=CorpActionEvent::BasisProvenance::UNKNOWN;
    EXPECT_FALSE(produce_qt_equity_position_transition(key(),old,Quantity(3),14,16,stamp(),{action}).is_ok());
    action=event(CorpActionType::SPLIT,0);
    EXPECT_FALSE(produce_qt_equity_position_transition(key(),old,Quantity(3),14,16,stamp(),{action}).is_ok());
    action=event(CorpActionType::SPLIT,2);
    EXPECT_FALSE(produce_qt_equity_position_transition(key(),old,Quantity(3),14,16,stamp(),{action,action}).is_ok());
    action.symbol="OTHER";
    EXPECT_FALSE(produce_qt_equity_position_transition(key(),old,Quantity(3),14,16,stamp(),{action}).is_ok());
    action=event(CorpActionType::DIVIDEND,2); action.close_at_ex_date=10; action.qty_at_ex_date=0;
    EXPECT_FALSE(produce_qt_equity_position_transition(key(),old,Quantity(3),14,16,stamp(),{action}).is_ok());
    EXPECT_EQ(old.quantity.to_string(),"1.5"); EXPECT_EQ(old.average_price.to_string(),"12");
}

TEST(QtEquityPositionTransition, UnrepresentableActionOrDeltaRefusesInsteadOfWrapping) {
    auto action=event(CorpActionType::SPLIT,1e20);
    EXPECT_FALSE(produce_qt_equity_position_transition(key(),prior(1.5,12),Quantity(3),14,16,stamp(),{action}).is_ok());
    auto old=prior(1,12); old.quantity=Quantity::from_raw(std::numeric_limits<int64_t>::min());
    EXPECT_FALSE(produce_qt_equity_position_transition(key(),old,Quantity::from_raw(std::numeric_limits<int64_t>::max()),14,16,stamp(),{}).is_ok());
}

TEST(QtEquityExecutionPriceResolver, ActualLoadedPriceWideningIsExplicitAndNeverLooksForward) {
    std::vector<Bar> bars{Bar{stamp()-std::chrono::hours{48},10,10,10,10,100,"SYN"},
                          Bar{stamp()+std::chrono::hours{24},99,99,99,99,100,"SYN"}};
    auto fallback=ExecutionPriceResolver::latest_close_at_or_before(bars,stamp());
    ASSERT_EQ(fallback.size(),1U); EXPECT_DOUBLE_EQ(fallback.at("SYN").price,10);
    std::unordered_map<std::string,double> prices;
    auto admitted=ExecutionPriceResolver::fill_missing(prices,fallback,{"SYN"},stamp(),2);
    EXPECT_TRUE(admitted.all_priced()); EXPECT_DOUBLE_EQ(prices.at("SYN"),10);
    ASSERT_EQ(admitted.widened.size(),1U);
    prices.clear(); auto refused=ExecutionPriceResolver::fill_missing(prices,fallback,{"SYN"},stamp(),1);
    EXPECT_FALSE(refused.all_priced()); EXPECT_TRUE(prices.empty());
}

TEST(QtEquityPositionTransition, ExactSingleUnitResidualRetainsReductionBasisDespiteDoubleCancellation) {
    for(const auto sign : {int64_t{1},int64_t{-1}}) {
        auto result=produce_qt_equity_position_transition(key(),prior(sign*1000000000.0,10),
            Quantity::from_raw(sign),14,16,stamp(),{});
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value().position.quantity.raw_value(),sign);
        EXPECT_EQ(result.value().position.average_price.to_string(),"10");
        EXPECT_EQ(result.value().position.unrealized_pnl.raw_value(),sign*6);
    }
}

TEST(QtEquityPositionTransition, RefusedActionSequenceExposesNoPartialSuccessorOrInputMutation) {
    std::optional<Position> previous=prior(1.5,12);
    auto valid=event(CorpActionType::SPLIT,2);
    auto invalid=event(CorpActionType::DIVIDEND,0); invalid.close_at_ex_date=10; invalid.qty_at_ex_date=1.5;
    std::vector<CorpActionEvent> actions{valid,invalid};
    auto result=produce_qt_equity_position_transition(key(),previous,Quantity(3),14,16,stamp(),actions);
    ASSERT_TRUE(result.is_error()); ASSERT_NE(result.error(),nullptr);
    EXPECT_EQ(result.error()->code(),ErrorCode::INVALID_DATA);
    EXPECT_THROW(result.value(),TradeError);
    EXPECT_EQ(previous->quantity.to_string(),"1.5");
    EXPECT_EQ(previous->average_price.to_string(),"12");
    EXPECT_EQ(previous->realized_pnl.to_string(),"7");
    EXPECT_EQ(previous->unrealized_pnl.to_string(),"9");
    EXPECT_EQ(previous->last_update,stamp());
    EXPECT_DOUBLE_EQ(actions[0].value,2); EXPECT_DOUBLE_EQ(actions[1].value,0);
    EXPECT_EQ(actions[0].basis_provenance_evidence,"owned prior snapshot");
}

TEST(QtEquityPositionTransition, ActualTypedEquityCashCostsRemainSeparateFromGrossTradePnl) {
    auto transition=produce_qt_equity_position_transition(key(),prior(1.5,10),Quantity(-0.5),14,16,stamp(),{});
    ASSERT_TRUE(transition.is_ok());
    transaction_cost::TransactionCostManager manager;
    transaction_cost::CostChargeObservation consumed;
    const auto charge=manager.calculate_costs("SYN",transition.value().quantity_change.as_double(),
        14,1000000,1,AssetType::EQUITY,&consumed);
    EXPECT_DOUBLE_EQ(charge.commissions_fees,0.28);
    EXPECT_GT(charge.total_transaction_costs,charge.commissions_fees);
    EXPECT_EQ(consumed.asset_lookup.path,transaction_cost::AssetLookupPath::fallback);
    EXPECT_EQ(consumed.commission_per_unit,0.005);
    EXPECT_EQ(transition.value().gross_trade_realized_pnl.to_string(),"6");
    EXPECT_EQ(transition.value().position.realized_pnl.to_string(),"6");
    EXPECT_EQ(transition.value().position.average_price.to_string(),"14");
}
