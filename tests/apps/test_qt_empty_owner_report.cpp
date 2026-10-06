#include <gtest/gtest.h>
#include "trade_ngin/apps/qt_empty_owner_report.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "../data/empty_owner_vectors.hpp"
using namespace trade_ngin;
namespace {
using J=nlohmann::json;
const std::string decision="f0000000-0000-4000-8000-000000000002";
J owner(){return J::parse(empty_owner_test::fixture).at("document");}
Timestamp date(){return std::chrono::sys_days(std::chrono::year(2026)/9/26);}
std::string selected(){return qt_digest_v1(J{{"selection_rows",J::array()}}).value();}
ReportPositionSnapshot snapshot(){return {{{"empty-alpha",{}}},{},"EQ_BOOK","LIVE_EQUITY_MEAN_REVERSION",{"empty-alpha"},"qt",date(),{{"empty-alpha",0}}};}
TEST(QtEmptyOwnerReport, ExactEmptyOwnerMapsWithoutSentinel){auto result=build_qt_empty_owner_report_quantity_projection(owner(),snapshot(),decision,selected());ASSERT_EQ(result.status,"eligible");ASSERT_TRUE(result.projection);EXPECT_TRUE(result.projection->quantity_exact.empty());EXPECT_TRUE(result.reason_codes.empty());EXPECT_EQ(result.row_manifest_digest,qt_digest_v1(J{{"component_keys",J::array()}}).value());}
TEST(QtEmptyOwnerReport, GenericEmptyProjectionStillRefuses){EXPECT_EQ(build_qt_report_quantity_projection({}, {}, snapshot(),decision,selected()).status,"unavailable");}
TEST(QtEmptyOwnerReport, InvalidDecisionRefuses){EXPECT_EQ(build_qt_empty_owner_report_quantity_projection(owner(),snapshot(),"bad",selected()).status,"unavailable");}
TEST(QtEmptyOwnerReport, WrongSelectedDigestRefuses){EXPECT_EQ(build_qt_empty_owner_report_quantity_projection(owner(),snapshot(),decision,std::string(64,'0')).status,"unavailable");}
TEST(QtEmptyOwnerReport, CallerReadyFlagRefuses){auto o=owner();o["ready"]=true;EXPECT_EQ(build_qt_empty_owner_report_quantity_projection(o,snapshot(),decision,selected()).status,"unavailable");}
TEST(QtEmptyOwnerReport, WrongDayRefuses){auto s=snapshot();ReportPositionSnapshot bad{s.by_strategy,s.combined,s.portfolio_id,s.strategy_id,s.strategy_names,s.portfolio_type,s.date+std::chrono::hours(24),s.evidence_counts};EXPECT_EQ(build_qt_empty_owner_report_quantity_projection(owner(),bad,decision,selected()).status,"unavailable");}
TEST(QtEmptyOwnerReport, ForeignBookRefuses){auto s=snapshot();ReportPositionSnapshot bad{s.by_strategy,s.combined,"OTHER",s.strategy_id,s.strategy_names,s.portfolio_type,s.date,s.evidence_counts};EXPECT_EQ(build_qt_empty_owner_report_quantity_projection(owner(),bad,decision,selected()).status,"unavailable");}
TEST(QtEmptyOwnerReport, ForeignMemberRefuses){ReportPositionSnapshot bad{{{"other",{}}},{},"EQ_BOOK","LIVE_EQUITY_MEAN_REVERSION",{"other"},"qt",date(),{{"other",0}}};EXPECT_EQ(build_qt_empty_owner_report_quantity_projection(owner(),bad,decision,selected()).status,"unavailable");}
TEST(QtEmptyOwnerReport, CountCannotInventAnEmptyScope){ReportPositionSnapshot bad{{{"empty-alpha",{}}},{},"EQ_BOOK","LIVE_EQUITY_MEAN_REVERSION",{"empty-alpha"},"qt",date(),{{"empty-alpha",1}}};EXPECT_EQ(build_qt_empty_owner_report_quantity_projection(owner(),bad,decision,selected()).status,"unavailable");}
TEST(QtEmptyOwnerReport, SentinelPositionRefuses){Position p{"SYN",Quantity(0),Price(1),Decimal(0),Decimal(0),date()};ReportPositionSnapshot bad{{{"empty-alpha",{{"SYN",p}}}},{},"EQ_BOOK","LIVE_EQUITY_MEAN_REVERSION",{"empty-alpha"},"qt",date(),{{"empty-alpha",0}}};EXPECT_EQ(build_qt_empty_owner_report_quantity_projection(owner(),bad,decision,selected()).status,"unavailable");}
TEST(QtEmptyOwnerReport, ForeignGroupedOwnerRefuses){ReportPositionSnapshot bad{{{"empty-alpha",{}},{"foreign",{}}},{},"EQ_BOOK","LIVE_EQUITY_MEAN_REVERSION",{"empty-alpha"},"qt",date(),{{"empty-alpha",0}}};EXPECT_EQ(build_qt_empty_owner_report_quantity_projection(owner(),bad,decision,selected()).status,"unavailable");}
TEST(QtEmptyOwnerReport, MissingGroupedOwnerRefuses){ReportPositionSnapshot bad{{},{},"EQ_BOOK","LIVE_EQUITY_MEAN_REVERSION",{"empty-alpha"},"qt",date(),{{"empty-alpha",0}}};EXPECT_EQ(build_qt_empty_owner_report_quantity_projection(owner(),bad,decision,selected()).status,"unavailable");}
}
