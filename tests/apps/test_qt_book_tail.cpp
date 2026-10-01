#include "trade_ngin/apps/qt_book_tail.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
using namespace trade_ngin;
namespace {
using J=nlohmann::json;
J fixture() {
    std::ifstream f(std::filesystem::path(__FILE__).parent_path().parent_path()/"contracts/qt-equity-finalization-wire.json");
    J j;f>>j;return j;
}
J key(const char* owner,const char* day,const char* stream) {
    return {{"portfolio_id","BOOK"},{"strategy_id","ENGINE"},{"strategy_name",owner},
        {"date",day},{"symbol","SYN"},{"portfolio_type",stream}};
}
J futures_input() {
    return {{"schema_version","qt-futures-accounting-input/v1"},
        {"decision_id","decision"},{"book_id","BOOK"},{"source_day","2026-09-26"},
        {"previous_day","2026-09-25"},{"accounting_source_id","synthetic-ledger"},
        {"prior_finalization_source_id","synthetic-finalization"},
        {"timestamp","2026-09-26T00:00:00Z"},{"currency","USD"},
        {"previous_positions",J::array({{{"key",key("alpha","2026-09-25","qt")},
            {"quantity_exact","5"},{"average_price_exact","100"}}})},
        {"previous_totals",J::array({{{"strategy_id","ENGINE"},{"equity_exact","1000"},{"total_pnl_exact","20"}}})},
        {"cost_config",{{"explicit_fee_per_contract","1.5"},{"min_adv","100"},
            {"min_participation","0"},{"max_participation","0.1"}}},
        {"instruments",J::array({{{"symbol","SYN"},{"price_exact","101"},{"adv_exact","100000"},
            {"volatility_multiplier_exact","1"},{"source_id","synthetic-prior-close"},
            {"baseline_spread_ticks","1"},{"min_spread_ticks","1"},{"max_spread_ticks","10"},
            {"spread_cost_multiplier","0.5"},{"max_impact_bps","100"},{"tick_size","0.01"},
            {"point_value","5"},{"max_total_implicit_bps","200"}}})}};
}
J futures_selection(const char* qty) {
    return J::array({{{"key",key("alpha","2026-09-26","qt_proposal")},
        {"asset_type","FUTURE"},{"editable",true},{"quantity_exact",qty},{"average_price_exact","100"}}});
}
}
class QtFuturesBookTail : public testing::TestWithParam<const char*> {};
TEST_P(QtFuturesBookTail, ExactChoiceIsNotReplacedByPreviousBook) {
    const J d={{"decision_id","decision"},{"book_id","BOOK"},{"source_day","2026-09-26"}};
    const auto selection=futures_selection(GetParam()),input=futures_input();
    auto r=run_book_tail(QtFuturesBookTailInputs{d,selection,input});
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "missing error");
    EXPECT_EQ(r.value().at("observation").at("fills")[0].at("selected_quantity_exact"),GetParam());
    if(std::string(GetParam())=="5")EXPECT_TRUE(r.value().at("executions").empty());
    else {
        ASSERT_EQ(r.value().at("executions").size(),1u);
        EXPECT_EQ(r.value().at("executions")[0].at("quantity_exact"),std::string(GetParam())=="7"?"2":"5");
    }
}
INSTANTIATE_TEST_SUITE_P(Chosen,QtFuturesBookTail,testing::Values("7","5","0"));
TEST(QtBookTail, FuturesCannotSilentlyRoundFractionalChoice) {
    const J d={{"decision_id","decision"},{"book_id","BOOK"},{"source_day","2026-09-26"}};
    const auto selection=futures_selection("7.25"),input=futures_input();
    EXPECT_TRUE(run_book_tail(QtFuturesBookTailInputs{d,selection,input}).is_error());
}
TEST(QtBookTail, EquityUsesOriginalFractionalDecisionAndActualAccounting) {
    auto f=fixture();auto original=J::parse(f.at("original_output_json").get<std::string>());
    const auto& decision=f.at("decision");
    const auto& input=f.at("original_input");
    // The immutable output records the exact selected quantity per owner.
    J selection=J::array();
    for(const auto& fill:original.at("observation").at("fills")) {
        auto key=fill.at("key");key["portfolio_type"]="qt_proposal";
        selection.push_back({{"key",key},{"asset_type","EQUITY"},{"editable",true},
            {"quantity_exact",fill.at("selected_quantity_exact")},
            {"average_price_exact",fill.at("average_price_exact")}});
    }
    const auto before=selection;
    auto r=run_book_tail(QtEquityBookTailInputs{decision,selection,input,original.at("producer_authority")});
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "missing error");
    EXPECT_EQ(r.value(),original);
    EXPECT_EQ(selection,before);
}
TEST(QtBookTail, MissingGovernedEquityInputCannotProduceFinancialRows) {
    const J empty=J::object(),selection=J::array();
    EXPECT_TRUE(run_book_tail(QtEquityBookTailInputs{empty,selection,empty,empty}).is_error());
}
TEST(QtBookTail, MissingGovernedFuturesInputCannotProduceFinancialRows) {
    const J empty=J::object(),selection=J::array();
    EXPECT_TRUE(run_book_tail(QtFuturesBookTailInputs{empty,selection,empty}).is_error());
}
