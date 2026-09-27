#include "trade_ngin/apps/qt_processed_report.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace trade_ngin {
namespace {
using Json=nlohmann::json;
constexpr const char* decision_id="40000000-0000-4000-8000-000000000001";
constexpr const char* attempt_id="50000000-0000-4000-8000-000000000001";
constexpr const char* model_id="10000000-0000-4000-8000-000000000001";
Timestamp report_day(){return Timestamp(std::chrono::seconds(1790337600));}
void same_publisher(Json& value) {
    if(value.is_object()) for(auto it=value.begin();it!=value.end();++it) {
        if(it.key()=="strategy_id") it.value()="component-1";
        else same_publisher(it.value());
    } else if(value.is_array()) for(auto& item:value) same_publisher(item);
}
Json evidence() {
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"contracts"/"qt-workflow-v1.json";
    std::ifstream input(path);if(!input.good()) throw std::runtime_error("fixture missing");
    auto payload=Json::parse(input).at("preview_clean");same_publisher(payload);
    Json quantities=Json::array(),keys=Json::array(),before=Json::array(),after=Json::array();
    for(size_t i=0;i<2;++i) {
        auto key=payload["selection_rows"][i]["key"];key["portfolio_type"]="qt";keys.push_back(key);
        quantities.push_back({{"key",key},{"quantity_exact",payload["selection_rows"][i]["quantity_exact"]}});
        Json accounting={{"key",key},{"quantity_exact",i==0?"4":"2"},{"average_price_exact","100"},
            {"daily_unrealized_pnl_exact","2"},{"daily_realized_pnl_exact","3"},
            {"last_update","2026-09-25T12:00:00Z"}};
        before.push_back(accounting);accounting["quantity_exact"]=payload["selection_rows"][i]["quantity_exact"];
        accounting["average_price_exact"]="111";accounting["daily_realized_pnl_exact"]="9";after.push_back(accounting);
    }
    const auto digest=qt_digest_v1({{"selection_rows",quantities}}).value();
    payload["selected_book_digest"]=digest;
    payload["evaluation"]["selected_risk"]["evaluated_book_digest"]=digest;
    payload["evaluation"]["selected_costs"]["evaluated_book_digest"]=digest;
    payload.erase("payload_digest");payload["payload_digest"]=qt_digest_v1(payload).value();
    Json preview={{"preview_id",payload["preview_id"]},{"book_id",payload["book_id"]},
        {"source_day",payload["source_day"]},{"draft_id",payload["draft_id"]},
        {"draft_revision",payload["draft_revision"]},{"source_digest",payload["source_digest"]},
        {"provenance_digest",payload["provenance_digest"]},{"read_set_digest",payload["read_set_digest"]},
        {"selected_book_digest",digest},{"payload_digest",payload["payload_digest"]},
        {"policy_version","synthetic-policy-v1"},{"evaluator_build","local-qt-controlled"},
        {"availability","ready"},{"state","confirmed_decision"},{"payload",payload},
        {"read_set_payload",{{"saved_accounting",before}}}};
    Json reference={{"schema_version","qt-desk-decision/v1"},{"decision_id",decision_id},
        {"preview_id",preview["preview_id"]},{"book_id",preview["book_id"]},{"source_day",preview["source_day"]},
        {"preview_payload_digest",preview["payload_digest"]},{"selected_book_digest",digest},
        {"read_set_digest",preview["read_set_digest"]}};
    Json decision={{"decision_id",decision_id},{"preview_id",preview["preview_id"]},{"book_id",preview["book_id"]},
        {"source_day",preview["source_day"]},{"status","confirmed_decision"},{"draft_id",preview["draft_id"]},
        {"draft_revision",preview["draft_revision"]},{"provenance_digest",preview["provenance_digest"]},
        {"selected_book_digest",digest},{"read_set_digest",preview["read_set_digest"]},
        {"workflow_capability_version",1},{"submitter_grant_version",1},{"policy_version",preview["policy_version"]},
        {"model_publication_id",model_id},{"created_by",1},{"payload",reference}};
    Json publication={{"schema_version","qt-desk-publication/v1"},{"decision_id",decision_id},
        {"attempt_id",attempt_id},{"observation_id","60000000-0000-4000-8000-000000000001"},
        {"book_id",preview["book_id"]},{"source_day",preview["source_day"]},{"model_publication_id",model_id},
        {"preview_payload_digest",preview["payload_digest"]},{"read_set_digest",preview["read_set_digest"]},
        {"selected_book_digest",digest},{"published_book_digest",digest},
        {"observation_digest",std::string(64,'a')},{"results_digest",std::string(64,'b')},
        {"before_accounting",before},{"after_accounting",after},
        {"report_scope",{{"portfolio_id",preview["book_id"]},{"strategy_id","component-1"},
            {"strategy_names",{"synthetic-alpha","synthetic-beta"}},{"portfolio_type","qt"},{"date",preview["source_day"]}}}};
    Json receipt={{"decision_id",decision_id},{"attempt_id",attempt_id},{"status","processed"},
        {"published_book_digest",digest},{"publication_payload",publication},{"report_eligibility_status","eligible"},
        {"report_reason_codes",Json::array()},{"row_manifest_digest",qt_digest_v1({{"component_keys",keys}}).value()}};
    return {{"workflow_required",true},{"decision",decision},{"preview",preview},{"receipt",receipt}};
}
Result<QtInvestorReportSnapshot> project(const Json& value, const StrategyPositionRows& current={}) {
    return project_processed_qt_report_evidence(value,"component-1",{"synthetic-alpha","synthetic-beta"},
        "synthetic-book-A",report_day(),current);
}
TEST(QtProcessedReportTest, UsesActualBeforeAccountingOnlyForCalculationsAndAfterOnlyForDisplay) {
    auto result=project(evidence());ASSERT_TRUE(result.is_ok())<<result.error()->what();
    const auto& view=result.value();ASSERT_TRUE(view.display);
    EXPECT_EQ(view.calculations.by_strategy.at("synthetic-alpha").at("SYN").quantity.to_string(),"4");
    EXPECT_EQ(view.calculations.by_strategy.at("synthetic-beta").at("SYN").quantity.to_string(),"2");
    EXPECT_EQ(view.calculations.combined.at("SYN").quantity.to_string(),"6");
    EXPECT_EQ(view.calculations.combined.at("SYN").average_price.to_string(),"100");
    EXPECT_EQ(view.calculations.combined.at("SYN").realized_pnl.to_string(),"3");
    EXPECT_EQ(view.display->quantity_exact.at({"synthetic-alpha","SYN"}),"5");
    EXPECT_EQ(view.display->quantity_exact.at({"synthetic-beta","SYN"}),"1");
}
TEST(QtProcessedReportTest, RefusesMissingFailedStaleTamperedOrDifferentScopeEvidence) {
    for(int change=0;change<10;++change) {
        auto value=evidence();auto& receipt=value["receipt"];auto& publication=receipt["publication_payload"];
        if(change==0)value.erase("receipt");
        if(change==1)receipt["status"]="failed";
        if(change==2)receipt["report_eligibility_status"]="unavailable";
        if(change==3)receipt["published_book_digest"]=std::string(64,'0');
        if(change==4)receipt["row_manifest_digest"]=std::string(64,'0');
        if(change==5)publication["report_scope"]["strategy_id"]="OTHER";
        if(change==6)publication["before_accounting"][0]["quantity_exact"]="99";
        if(change==7)publication["after_accounting"][0]["quantity_exact"]="99";
        if(change==8)publication["after_accounting"][0]["last_update"]="invalid";
        if(change==9)publication["model_publication_id"]="10000000-0000-4000-8000-000000000099";
        EXPECT_TRUE(project(value).is_error())<<change;
    }
}
TEST(QtProcessedReportTest, ChangedCurrentPlannedRowsCannotReuseEligibleReceipt) {
    StrategyPositionRows current{{"synthetic-alpha",{{"NEW",Position("NEW",Quantity(1),Price(10),Decimal(0),Decimal(0),report_day())}}}};
    EXPECT_TRUE(project(evidence(),current).is_error());
}
TEST(QtProcessedReportTest, CanonicalReceiptScopePreservesCallerStrategyCalculationOrder) {
    auto result=project_processed_qt_report_evidence(evidence(),"component-1",
        {"synthetic-beta","synthetic-alpha"},"synthetic-book-A",report_day(),{});
    ASSERT_TRUE(result.is_ok())<<result.error()->what();
    EXPECT_EQ(result.value().calculations.strategy_names,(std::vector<std::string>{"synthetic-beta","synthetic-alpha"}));
    ASSERT_TRUE(result.value().display);
    EXPECT_EQ(result.value().display->quantity_exact.at({"synthetic-alpha","SYN"}),"5");
}
class ProcessedReportDatabase : public PostgresDatabase {
public:
    ProcessedReportDatabase() : PostgresDatabase("mock://qt-processed-report") {}
    Json facts=evidence();
    bool failed=false;
    int receipt_reads=0,legacy_reads=0;
    std::string requested_book,requested_day;
    Result<Json> load_qt_processed_report_evidence(const std::string& book,const std::string& day) override {
        ++receipt_reads;requested_book=book;requested_day=day;
        if(failed)return make_error<Json>(ErrorCode::DATABASE_ERROR,"synthetic failure");
        return Result<Json>(facts);
    }
    Result<ReportPositionRows> load_report_positions_by_date(
        const std::string&,const std::vector<std::string>&,const std::string&,
        const Timestamp&,const std::string& type) override {
        ++legacy_reads;EXPECT_EQ(type,"qt");
        return Result<ReportPositionRows>(ReportPositionRows{{"synthetic-alpha",{{"SYN",
            Position("SYN",Quantity(4),Price(100),Decimal(2),Decimal(3),report_day())}}}});
    }
};
Result<QtInvestorReportSnapshot> load(ProcessedReportDatabase& db) {
    return load_qt_investor_report_snapshot(db,"component-1",{"synthetic-alpha","synthetic-beta"},
        "synthetic-book-A",report_day(),{});
}
TEST(QtProcessedReportLoaderTest, EnabledBookLoadsVerifiedReceiptWithoutLegacyPositionRead) {
    ProcessedReportDatabase db;auto result=load(db);
    ASSERT_TRUE(result.is_ok())<<result.error()->what();ASSERT_TRUE(result.value().display);
    EXPECT_EQ(db.receipt_reads,1);EXPECT_EQ(db.legacy_reads,0);
    EXPECT_EQ(db.requested_book,"synthetic-book-A");EXPECT_EQ(db.requested_day,"2026-09-25");
    EXPECT_EQ(result.value().calculations.combined.at("SYN").quantity.to_string(),"6");
    EXPECT_EQ(result.value().display->quantity_exact.at({"synthetic-alpha","SYN"}),"5");
}
TEST(QtProcessedReportLoaderTest, ExplicitlyDisabledCapabilityRetainsExistingReportSnapshot) {
    ProcessedReportDatabase db;db.facts={{"workflow_required",false}};auto result=load(db);
    ASSERT_TRUE(result.is_ok())<<result.error()->what();EXPECT_FALSE(result.value().display);
    EXPECT_EQ(db.receipt_reads,1);EXPECT_EQ(db.legacy_reads,1);
    EXPECT_EQ(result.value().calculations.combined.at("SYN").quantity.to_string(),"4");
}
TEST(QtProcessedReportLoaderTest, MissingUnknownMalformedOrFailedEvidenceCannotFallBack) {
    for(int change=0;change<8;++change){
        ProcessedReportDatabase db;
        if(change==0)db.failed=true;
        if(change==1)db.facts=Json::object();
        if(change==2)db.facts={{"workflow_required",nullptr}};
        if(change==3)db.facts={{"workflow_required","false"}};
        if(change==4)db.facts={{"workflow_required",false},{"receipt",nullptr}};
        if(change==5)db.facts={{"workflow_required",true}};
        if(change==6)db.facts["receipt"]["status"]="failed";
        if(change==7)db.facts["receipt"]["row_manifest_digest"]=std::string(64,'0');
        EXPECT_TRUE(load(db).is_error())<<change;
        EXPECT_EQ(db.receipt_reads,1)<<change;EXPECT_EQ(db.legacy_reads,0)<<change;
    }
}
} // namespace
} // namespace trade_ngin
