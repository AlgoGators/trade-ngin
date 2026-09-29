#include "trade_ngin/apps/qt_processed_report.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <tuple>

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
// One selection row per leg; the default legs reproduce the fixture's two rows exactly.
// PLAN14: before == absent means the key had no row before processing (a brand-new key).
constexpr const char* absent="absent";
struct Leg {std::string strategy,symbol,before,after;};
Json evidence(const std::vector<Leg>& legs={{"synthetic-alpha","SYN","4","5"},{"synthetic-beta","SYN","2","1"}}) {
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"contracts"/"qt-workflow-v1.json";
    std::ifstream input(path);if(!input.good()) throw std::runtime_error("fixture missing");
    auto payload=Json::parse(input).at("preview_clean");same_publisher(payload);
    const auto row=payload["selection_rows"][0];auto& costs=payload["evaluation"]["selected_costs"];
    const auto cost=costs["by_component"][0];payload["selection_rows"]=Json::array();costs["by_component"]=Json::array();
    Json quantities=Json::array(),keys=Json::array(),before=Json::array(),after=Json::array();
    for(const auto& leg:legs) {
        auto selection=row;selection["key"]["strategy_name"]=leg.strategy;selection["key"]["symbol"]=leg.symbol;
        selection["quantity_exact"]=leg.after;payload["selection_rows"].push_back(selection);
        const bool had_row=leg.before!=absent;const std::string prior=had_row?leg.before:"0";
        auto component=cost;component["key"]=selection["key"];component["prior_quantity_exact"]=prior;
        component["selected_quantity_exact"]=leg.after;costs["by_component"].push_back(component);
        auto key=selection["key"];key["portfolio_type"]="qt";
        if(prior!="0"||leg.after!="0")keys.push_back(key); // shown rows only
        quantities.push_back({{"key",key},{"quantity_exact",leg.after}});
        Json accounting={{"key",key},{"quantity_exact",prior},{"average_price_exact","100"},
            {"daily_unrealized_pnl_exact","2"},{"daily_realized_pnl_exact","3"},
            {"last_update","2026-09-25T12:00:00Z"}};
        if(had_row)before.push_back(accounting);
        accounting["quantity_exact"]=leg.after;
        accounting["average_price_exact"]="111";accounting["daily_realized_pnl_exact"]="9";after.push_back(accounting);
    }
    costs["total_exact"]=Quantity::from_raw(1000000*static_cast<int64_t>(legs.size())).to_string(); // 0.01 per leg
    std::sort(keys.begin(),keys.end(),[](const Json& a,const Json& b){
        const auto tuple=[](const Json& k){return std::make_tuple(k.at("portfolio_id").get<std::string>(),
            k.at("strategy_id").get<std::string>(),k.at("strategy_name").get<std::string>(),k.at("date").get<std::string>(),
            k.at("symbol").get<std::string>(),k.at("portfolio_type").get<std::string>());};
        return tuple(a)<tuple(b);});
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
TEST(QtProcessedReportTest, UsesSavedAccountingForCalculationsAndDisplay) {
    auto result=project(evidence());ASSERT_TRUE(result.is_ok())<<result.error()->what();
    const auto& view=result.value();ASSERT_TRUE(view.display);
    EXPECT_EQ(view.calculations.by_strategy.at("synthetic-alpha").at("SYN").quantity.to_string(),"5");
    EXPECT_EQ(view.calculations.by_strategy.at("synthetic-beta").at("SYN").quantity.to_string(),"1");
    EXPECT_EQ(view.calculations.combined.at("SYN").quantity.to_string(),"6");
    EXPECT_EQ(view.calculations.combined.at("SYN").average_price.to_string(),"111");
    EXPECT_EQ(view.calculations.combined.at("SYN").realized_pnl.to_string(),"9");
    EXPECT_EQ(view.display->quantity_exact.at({"synthetic-alpha","SYN"}),"5");
    EXPECT_EQ(view.display->quantity_exact.at({"synthetic-beta","SYN"}),"1");
}
TEST(QtProcessedReportTest, ChangedAndOpenedRowsComeFromSavedPositions) {
    auto result=project(evidence({{"synthetic-alpha","SYN","5","7"},{"synthetic-beta","NEW","0","2"}}));
    ASSERT_TRUE(result.is_ok())<<result.error()->what();
    const auto& view=result.value();ASSERT_TRUE(view.display);
    EXPECT_EQ(view.calculations.by_strategy.at("synthetic-alpha").at("SYN").quantity.to_string(),"7");
    ASSERT_TRUE(view.calculations.by_strategy.at("synthetic-beta").contains("NEW"));
    EXPECT_EQ(view.calculations.by_strategy.at("synthetic-beta").at("NEW").quantity.to_string(),"2");
    EXPECT_EQ(view.calculations.combined.at("SYN").quantity.to_string(),"7");
    EXPECT_EQ(view.calculations.combined.at("NEW").quantity.to_string(),"2");
    EXPECT_EQ(view.display->quantity_exact.at({"synthetic-alpha","SYN"}),"7");
    EXPECT_EQ(view.display->quantity_exact.at({"synthetic-beta","NEW"}),"2");
    EXPECT_EQ(view.display->quantity_exact.size(),2u);
}
// PLAN14 (F3): a key with no row before processing gets its own row and does not hold the report back.
TEST(QtProcessedReportTest, NewKeyWithNoBeforeRowGetsItsOwnRow) {
    const auto value=evidence({{"synthetic-alpha","SYN","4","5"},{"synthetic-beta","NEW",absent,"2"}});
    ASSERT_EQ(value["receipt"]["publication_payload"]["before_accounting"].size(),1u);
    ASSERT_EQ(value["receipt"]["publication_payload"]["after_accounting"].size(),2u);
    auto result=project(value);ASSERT_TRUE(result.is_ok())<<result.error()->what();
    const auto& view=result.value();ASSERT_TRUE(view.display);
    EXPECT_EQ(view.calculations.by_strategy.at("synthetic-beta").at("NEW").quantity.to_string(),"2");
    EXPECT_EQ(view.calculations.by_strategy.at("synthetic-beta").at("NEW").average_price.to_string(),"111");
    EXPECT_EQ(view.calculations.combined.at("NEW").quantity.to_string(),"2");
    EXPECT_EQ(view.calculations.combined.at("SYN").quantity.to_string(),"5");
    EXPECT_EQ(view.calculations.evidence_counts.at("synthetic-beta"),0u); // counts stay before-row counts
    EXPECT_EQ(view.display->quantity_exact.at({"synthetic-beta","NEW"}),"2");
    EXPECT_EQ(view.display->quantity_exact.size(),2u);
}
TEST(QtProcessedReportTest, NewKeyLeftAtZeroIsNotShown) {
    auto result=project(evidence({{"synthetic-alpha","SYN","4","5"},{"synthetic-beta","NEW",absent,"0"}}));
    ASSERT_TRUE(result.is_ok())<<result.error()->what();
    const auto& view=result.value();ASSERT_TRUE(view.display);
    EXPECT_TRUE(view.calculations.by_strategy.at("synthetic-beta").empty());
    EXPECT_FALSE(view.calculations.combined.contains("NEW"));
    EXPECT_EQ(view.display->quantity_exact.size(),1u);
}
TEST(QtProcessedReportTest, PositionClosedTodayKeepsItsRowAtZero) {
    auto result=project(evidence({{"synthetic-alpha","SYN","4","0"},{"synthetic-beta","SYN","2","1"}}));
    ASSERT_TRUE(result.is_ok())<<result.error()->what();
    const auto& view=result.value();ASSERT_TRUE(view.display);
    EXPECT_TRUE(view.calculations.by_strategy.at("synthetic-alpha").at("SYN").quantity.is_zero());
    EXPECT_EQ(view.display->quantity_exact.at({"synthetic-alpha","SYN"}),"0");
    EXPECT_EQ(view.display->quantity_exact.at({"synthetic-beta","SYN"}),"1");
    EXPECT_EQ(view.calculations.combined.at("SYN").quantity.to_string(),"1"); // closed row leaves the combined map
}
// PLAN14 r2 (review finding 3): on this path the saved rows must equal the selected rows exactly
// (qt_processed_report.cpp: after quantities vs selected_rows), so dropping a saved row is refused there,
// before the builder or projection runs.
TEST(QtProcessedReportTest, SavedRowsMissingABeforeKeyStayRefused) {
    auto value=evidence({{"synthetic-alpha","SYN","4","5"},{"synthetic-beta","SYN","2","1"}});
    auto& publication=value["receipt"]["publication_payload"];
    publication["after_accounting"].erase(1);
    EXPECT_TRUE(project(value).is_error());
}
// The realistic superset violation: the selection itself (and so the saved rows) omits a key that had a
// before row. Before rows are typed from the selection, so this path refuses the untyped before row; it
// cannot reach the superset rule, which is proven directly in QtReportProjectionTest
// (HandBuiltSnapshotBeforeKeyMissingFromAfterRefused, VectorBeforeKeyMissingFromAfterReachesTheSupersetRule).
TEST(QtProcessedReportTest, SelectionOmittingABeforeKeyIsRefused) {
    auto value=evidence({{"synthetic-alpha","SYN","4","5"}});
    const auto control=project(value);ASSERT_TRUE(control.is_ok())<<control.error()->what();
    auto extra=value["receipt"]["publication_payload"]["before_accounting"][0];
    extra["key"]["strategy_name"]="synthetic-beta";extra["quantity_exact"]="2";
    value["receipt"]["publication_payload"]["before_accounting"].push_back(extra);
    value["preview"]["read_set_payload"]["saved_accounting"].push_back(extra); // publication before == read-set before
    EXPECT_TRUE(project(value).is_error());
}
// PLAN14 r2 (review finding 5): a book's first position, with no before rows at all.
TEST(QtProcessedReportTest, FirstPositionOfABookWithNoBeforeRows) {
    const auto value=evidence({{"synthetic-alpha","NQ",absent,"2"}});
    ASSERT_TRUE(value["receipt"]["publication_payload"]["before_accounting"].empty());
    ASSERT_TRUE(value["preview"]["read_set_payload"]["saved_accounting"].empty());
    auto result=project(value);ASSERT_TRUE(result.is_ok())<<result.error()->what();
    const auto& view=result.value();ASSERT_TRUE(view.display);
    EXPECT_EQ(view.calculations.by_strategy.at("synthetic-alpha").at("NQ").quantity.to_string(),"2");
    EXPECT_TRUE(view.calculations.by_strategy.at("synthetic-beta").empty());
    EXPECT_EQ(view.calculations.combined.at("NQ").quantity.to_string(),"2");
    EXPECT_EQ(view.calculations.evidence_counts.at("synthetic-alpha"),0u);
    EXPECT_EQ(view.display->quantity_exact.at({"synthetic-alpha","NQ"}),"2");
    EXPECT_EQ(view.display->quantity_exact.size(),1u);
}
TEST(QtProcessedReportTest, EveryKeyNewInBothStrategiesGetsRows) {
    auto result=project(evidence({{"synthetic-alpha","SYN",absent,"5"},{"synthetic-beta","NEW",absent,"-2"}}));
    ASSERT_TRUE(result.is_ok())<<result.error()->what();
    const auto& view=result.value();ASSERT_TRUE(view.display);
    EXPECT_EQ(view.display->quantity_exact.at({"synthetic-alpha","SYN"}),"5");
    EXPECT_EQ(view.display->quantity_exact.at({"synthetic-beta","NEW"}),"-2");
    EXPECT_EQ(view.calculations.combined.size(),2u);
}
TEST(QtProcessedReportTest, FirstSelectionLeftAtZeroIsEligibleWithNoRows) {
    auto result=project(evidence({{"synthetic-alpha","NQ",absent,"0"}}));
    ASSERT_TRUE(result.is_ok())<<result.error()->what();
    const auto& view=result.value();ASSERT_TRUE(view.display);
    EXPECT_TRUE(view.display->quantity_exact.empty());
    EXPECT_TRUE(view.calculations.combined.empty());
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
