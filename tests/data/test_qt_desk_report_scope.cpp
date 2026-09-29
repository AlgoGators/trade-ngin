// PLAN14 r2 (review finding 5): test-only seam for qt_desk_processor.cpp report().
// The production translation unit is compiled into this separate executable (qt_desk_report_scope_tests)
// so its anonymous-namespace report() can be called directly. Production code, libtrade_ngin and the
// other test targets are unchanged. No database connection is made; report() takes JSON only.
#include "../../src/data/qt_desk_processor.cpp"
#include <gtest/gtest.h>

namespace trade_ngin {
namespace {
constexpr const char* scope_decision="40000000-0000-4000-8000-000000000001";
Json scope_key(const std::string& name,const std::string& symbol,const std::string& owner="RUN") {
    return {{"portfolio_id","BOOK"},{"strategy_id",owner},{"strategy_name",name},{"date","2026-09-25"},
            {"symbol",symbol},{"portfolio_type","qt"}};
}
Json scope_row(const std::string& name,const std::string& symbol,const std::string& quantity,const std::string& owner="RUN") {
    return {{"key",scope_key(name,symbol,owner)},{"quantity_exact",quantity},{"average_price_exact","10"},
            {"daily_unrealized_pnl_exact","0"},{"daily_realized_pnl_exact","0"},{"last_update","2026-09-25T20:00:00Z"}};
}
// The decision's selection is the saved (after) key set, typed per key.
Json scope_preview(const Json& after,const std::map<std::string,std::string>& types={}) {
    Json rows=Json::array();
    for(const auto& row:after) {
        auto key=row.at("key"); key.erase("portfolio_type");
        const auto symbol=key.at("symbol").get<std::string>();
        rows.push_back({{"key",key},{"asset_type",types.contains(symbol)?types.at(symbol):"EQUITY"}});
    }
    return {{"payload",{{"selection_rows",rows}}}};
}
std::string scope_published(const Json& after) {
    Json rows=Json::array();
    for(const auto& row:after) rows.push_back({{"key",row.at("key")},{"quantity_exact",row.at("quantity_exact")}});
    return qt_digest_v1({{"selection_rows",rows}}).value();
}
Json expected_scope(std::vector<std::string> names) {
    return {{"portfolio_id","BOOK"},{"strategy_id","RUN"},{"strategy_names",names},{"portfolio_type","qt"},{"date","2026-09-25"}};
}
QtReportEligibility run_report(const Json& before,const Json& after,Json& scope,const std::map<std::string,std::string>& types={}) {
    return report(before,after,scope_preview(after,types),scope_decision,scope_published(after),scope);
}

TEST(QtDeskReportScopeTest, NewKeyUnderAStrategyWithNoBeforeRowsKeepsItsScope) {
    const Json before=Json::array({scope_row("alpha","SYN","5")});
    const Json after=Json::array({scope_row("alpha","SYN","5"),scope_row("beta","NEW","2")});
    Json scope; const auto result=run_report(before,after,scope);
    ASSERT_EQ(result.status,"eligible");
    EXPECT_EQ(scope,expected_scope({"alpha","beta"})); // names from before and after
    EXPECT_EQ(result.projection->quantity_exact.at({"beta","NEW"}),"2");
}
TEST(QtDeskReportScopeTest, FirstPositionOfAnEmptyBookTakesBookAndDayFromTheSavedRows) {
    const Json before=Json::array();
    const Json after=Json::array({scope_row("alpha","NQ","2")});
    Json scope; const auto result=run_report(before,after,scope,{{"NQ","FUTURE"}});
    ASSERT_EQ(result.status,"eligible");
    EXPECT_EQ(scope,expected_scope({"alpha"}));
    EXPECT_EQ(result.projection->quantity_exact.at({"alpha","NQ"}),"2");
}
TEST(QtDeskReportScopeTest, UnchangedKeySetKeepsTheBeforeOnlyScope) {
    const Json before=Json::array({scope_row("alpha","SYN","5"),scope_row("beta","ALT","-3")});
    const Json after=Json::array({scope_row("alpha","SYN","7"),scope_row("beta","ALT","-3")});
    Json scope; const auto result=run_report(before,after,scope);
    ASSERT_EQ(result.status,"eligible");
    EXPECT_EQ(scope,expected_scope({"alpha","beta"}));
}
TEST(QtDeskReportScopeTest, EmptySelectionIsRefusedWithoutScope) {
    Json scope=Json::object(); const auto result=run_report(Json::array(),Json::array(),scope);
    EXPECT_EQ(result.status,"unavailable"); EXPECT_TRUE(scope.is_null());
}
TEST(QtDeskReportScopeTest, TwoStrategyIdsAcrossBeforeAndAfterAreRefused) {
    const Json before=Json::array({scope_row("alpha","SYN","5")});
    const Json after=Json::array({scope_row("alpha","SYN","5"),scope_row("beta","NEW","2","OTHER")});
    Json scope; EXPECT_EQ(run_report(before,after,scope).status,"unavailable");
}
TEST(QtDeskReportScopeTest, BeforeKeyOutsideTheSelectionIsRefused) {
    const Json before=Json::array({scope_row("alpha","SYN","5"),scope_row("alpha","ALT","3")});
    const Json after=Json::array({scope_row("alpha","SYN","5")});
    Json scope; EXPECT_EQ(run_report(before,after,scope).status,"unavailable");
}
TEST(QtDeskReportScopeTest, FractionalFutureOpenedFromAbsentIsRefused) {
    const Json before=Json::array({scope_row("alpha","SYN","5")});
    const Json after=Json::array({scope_row("alpha","SYN","5"),scope_row("alpha","ES","1.5")});
    Json scope; EXPECT_EQ(run_report(before,after,scope,{{"ES","FUTURE"}}).status,"unavailable");
}
} // namespace
} // namespace trade_ngin
