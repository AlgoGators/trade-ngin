#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#undef private
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/apps/qt_report_quantity_projection.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/live/csv_exporter.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/core/chart_generator.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>


namespace trade_ngin {
namespace {
Timestamp day() {return Timestamp(std::chrono::seconds(1790337600));}
Quantity qty(const std::string& text) {return parse_qt_quantity_exact(text).value();}
Position pos(std::string symbol,std::string amount) {
    return Position(symbol,qty(amount),Price(10),Decimal(2),Decimal(3),day());
}
std::vector<ComponentPositionCandidate> components() {
    return {{{"BOOK","RUN","alpha","2026-09-25","SYN","qt"},
        {AssetType::EQUITY,"SYN"},true,std::nullopt,pos("SYN","5"),false},
        {{"BOOK","RUN","beta","2026-09-25","ALT","qt"},
        {AssetType::EQUITY,"ALT"},false,std::nullopt,pos("ALT","-3"),false}};
}
ReportPositionSnapshot snapshot() {
    StrategyPositionRows rows{{"alpha",{{"SYN",pos("SYN","5")}}},
                              {"beta",{{"ALT",pos("ALT","-3")}}}};
    return {rows,{{"SYN",pos("SYN","5")},{"ALT",pos("ALT","-3")}},"BOOK","RUN",
        {"alpha","beta"},"qt",day(),{{"alpha",1},{"beta",1}}};
}
std::string digest(const std::vector<ComponentPositionCandidate>& rows) {
    nlohmann::json selected=nlohmann::json::array();
    for(const auto& row:rows) {
        const auto& k=row.key;
        selected.push_back({{"key",{{"portfolio_id",k.portfolio_id},{"strategy_id",k.strategy_id},
            {"strategy_name",k.strategy_name},{"date",k.date},{"symbol",k.symbol},{"portfolio_type",k.portfolio_type}}},
            {"quantity_exact",row.position.quantity.to_string()}});
    }
    return qt_digest_v1({{"selection_rows",selected}}).value();
}
constexpr const char* decision="40000000-0000-4000-8000-000000000001";
ReportPositionSnapshot saved(const std::vector<ComponentPositionCandidate>& before,
                             const std::vector<ComponentPositionCandidate>& after) {
    return build_qt_saved_report_snapshot(before,after,{"alpha","beta"},"BOOK","RUN",day());
}
ComponentPositionCandidate extra(const ComponentPositionCandidate& base,std::string symbol,std::string amount) {
    auto row=base; row.key.symbol=symbol; row.instrument.symbol=symbol; row.position=pos(symbol,amount); return row;
}

TEST(QtReportProjectionTest, ExactFractionsSignedRowsAndStableManifest) {
    const auto before=components(); auto after=before;
    after[0].position.quantity=qty("7.125");
    const auto projected=build_qt_report_quantity_projection(before,after,saved(before,after),decision,digest(after));
    ASSERT_EQ(projected.status,"eligible");
    ASSERT_TRUE(projected.projection);
    EXPECT_EQ(projected.projection->quantity_exact.at({"alpha","SYN"}),"7.125");
    EXPECT_EQ(projected.projection->quantity_exact.at({"beta","ALT"}),"-3");
    const auto unchanged=build_qt_report_quantity_projection(before,before,saved(before,before),decision,digest(before));
    EXPECT_EQ(unchanged.row_manifest_digest,projected.row_manifest_digest);
}

TEST(QtReportProjectionTest, RefusesMissingChangedAmbiguousOrMalformedRows) {
    const auto before=components();
    for(int mutation=0;mutation<7;++mutation) {
        auto after=before;
        if(mutation==0) after.pop_back();
        if(mutation==1) {auto add=after[0];add.key.symbol="NEW";add.position.symbol="NEW";after.push_back(add);}
        if(mutation==2) after[0].position.symbol="OTHER";
        if(mutation==3) after[0].key.strategy_id="OTHER";
        if(mutation==4) after[0].key.date="2026-09-26";
        if(mutation==5) after.push_back(after[0]);
        const auto claim=mutation==6?std::string(64,'0'):(mutation==5?digest(before):digest(after));
        // PLAN14 r2 (review finding 4): an added key is allowed now, so mutation 1 is judged against a
        // snapshot built from its own saved rows; it is refused only because its instrument is SYN's.
        EXPECT_EQ(build_qt_report_quantity_projection(before,after,mutation==1?saved(before,after):snapshot(),decision,claim).status,
                  "unavailable") << mutation;
    }
    {   // Control for mutation 1: the same added key with its own instrument is eligible.
        auto after=before; auto add=after[0]; add.key.symbol="NEW"; add.position.symbol="NEW"; add.instrument.symbol="NEW";
        after.push_back(add);
        EXPECT_EQ(build_qt_report_quantity_projection(before,after,saved(before,after),decision,digest(after)).status,"eligible");
    }
    // Finding 4: nothing selected is the size refusal left (the snapshot here is otherwise consistent).
    EXPECT_EQ(build_qt_report_quantity_projection({},{},saved({},{}),decision,digest({})).status,"unavailable");
    // Evidence counts are before-row counts: a snapshot claiming before rows the accounting lacks is refused
    // (saved(before,before) differs from saved({},before) only in evidence_counts).
    EXPECT_EQ(build_qt_report_quantity_projection({},before,saved({},before),decision,digest(before)).status,"eligible");
    EXPECT_EQ(build_qt_report_quantity_projection({},before,saved(before,before),decision,digest(before)).status,"unavailable");
    EXPECT_EQ(build_qt_report_quantity_projection(before,before,snapshot(),"",digest(before)).status,"unavailable");
}

TEST(QtReportProjectionTest, SnapshotMustEqualSavedPositions) {
    const auto before=components(); auto after=before; after[0].position.quantity=qty("7");
    EXPECT_EQ(build_qt_report_quantity_projection(before,after,snapshot(),decision,digest(after)).status,"unavailable");
    EXPECT_EQ(build_qt_report_quantity_projection(before,after,saved(before,after),decision,digest(after)).status,"eligible");
}

TEST(QtReportProjectionTest, OpeningFromZeroAddsRowAndManifestKey) {
    auto before=components(); before.push_back(extra(before[0],"NEW","0"));
    auto after=before; after.back().position.quantity=qty("2");
    const auto source=saved(before,after);
    ASSERT_TRUE(source.by_strategy.at("alpha").contains("NEW"));
    const auto opened=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
    ASSERT_EQ(opened.status,"eligible");
    EXPECT_EQ(opened.projection->quantity_exact.at({"alpha","NEW"}),"2");
    const auto unchanged=build_qt_report_quantity_projection(before,before,saved(before,before),decision,digest(before));
    ASSERT_EQ(unchanged.status,"eligible");
    EXPECT_NE(opened.row_manifest_digest,unchanged.row_manifest_digest);
}

TEST(QtReportProjectionTest, ClosedTodayShowsZeroAndLeavesCombined) {
    const auto before=components(); auto after=before; after[0].position.quantity=qty("0");
    const auto source=saved(before,after);
    ASSERT_TRUE(source.by_strategy.at("alpha").at("SYN").quantity.is_zero());
    EXPECT_FALSE(source.combined.contains("SYN"));
    const auto closed=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
    ASSERT_EQ(closed.status,"eligible");
    EXPECT_EQ(closed.projection->quantity_exact.at({"alpha","SYN"}),"0");
}

TEST(QtReportProjectionTest, ZeroBeforeAndAfterStaysHidden) {
    auto before=components(); before.push_back(extra(before[0],"ZERO","0")); const auto after=before;
    const auto source=saved(before,after);
    EXPECT_FALSE(source.by_strategy.at("alpha").contains("ZERO"));
    const auto projected=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
    ASSERT_EQ(projected.status,"eligible"); EXPECT_EQ(projected.projection->quantity_exact.size(),2u);
}

TEST(QtReportProjectionTest, SplitSymbolOneLegClosedKeepsOpenLegInCombined) {
    auto before=components(); before[1].key.symbol="SYN"; before[1].instrument.symbol="SYN"; before[1].position=pos("SYN","5");
    auto after=before; after[0].position.quantity=qty("0");
    const auto source=saved(before,after);
    EXPECT_TRUE(source.by_strategy.at("alpha").at("SYN").quantity.is_zero());
    EXPECT_EQ(source.combined.at("SYN").quantity.to_string(),"5");
    EXPECT_EQ(build_qt_report_quantity_projection(before,after,source,decision,digest(after)).status,"eligible");
}

TEST(QtReportProjectionTest, FuturesOpeningMustBeWhole) {
    auto before=components(); before.push_back(extra(before[0],"FUT","0")); before.back().instrument.type=AssetType::FUTURE;
    auto after=before; after.back().position.quantity=qty("1.5");
    EXPECT_EQ(build_qt_report_quantity_projection(before,after,saved(before,after),decision,digest(after)).status,"unavailable");
    after.back().position.quantity=qty("2");
    EXPECT_EQ(build_qt_report_quantity_projection(before,after,saved(before,after),decision,digest(after)).status,"eligible");
}

// PLAN14: v2 is the shared contract (v1's nine cases plus keys absent before). Types come from the
// selection rows exactly as the processed-report loader types them: a row without a selection
// type is refused there, so it is refused here.
TEST(QtReportProjectionTest, SharedManifestVectorsV2MatchApiDigests) {
    std::ifstream in(std::filesystem::path(__FILE__).parent_path().parent_path()/"contracts"/"qt-report-manifest-vectors-v2.json");
    ASSERT_TRUE(in.good()); const auto vectors=nlohmann::json::parse(in);
    ASSERT_EQ(vectors.at("schema_version"),"qt-report-manifest-vectors/v2");
    ASSERT_EQ(vectors.at("case_count"),21);
    ASSERT_EQ(vectors.at("cases").size(),21u); // F9: a truncated vector file must fail here
    size_t eligible=0,refused=0;
    for(const auto& c:vectors.at("cases")) {
        std::map<std::string,AssetType> types;
        for(const auto& s:c.at("selection_rows")) types[s.at("key").dump()]=s.at("asset_type")=="FUTURE"?AssetType::FUTURE:AssetType::EQUITY;
        bool typed=true;
        auto rows=[&](const nlohmann::json& list){
            std::vector<ComponentPositionCandidate> out;
            for(const auto& r:list){ auto k=r.at("key"); const auto portfolio_type=k.at("portfolio_type").get<std::string>(); k.erase("portfolio_type");
                ComponentPositionKey key{k.at("portfolio_id"),k.at("strategy_id"),k.at("strategy_name"),k.at("date"),k.at("symbol"),portfolio_type};
                const auto found=types.find(k.dump());
                if(found==types.end()) {typed=false;continue;}
                out.push_back({key,{found->second,key.symbol},true,std::nullopt,
                    Position(key.symbol,qty(r.at("quantity_exact")),qty(r.at("average_price_exact")),Decimal(0),Decimal(0),day()),false});}
            return out; };
        const auto before=rows(c.at("before")),after=rows(c.at("after"));
        std::set<std::string> scope; for(const auto* side:{&before,&after}) for(const auto& r:*side) scope.insert(r.key.strategy_name);
        QtReportEligibility result{"unavailable",{},std::nullopt,std::nullopt};
        if(typed) try {
            const auto source=build_qt_saved_report_snapshot(before,after,{scope.begin(),scope.end()},"QT_BOOK","RUN",day());
            result=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
        } catch(const std::exception&) {}
        if(c.at("expected_manifest_digest").is_null()) { ++refused; EXPECT_EQ(result.status,"unavailable") << c.at("name"); continue; }
        ++eligible;
        ASSERT_EQ(result.status,"eligible") << c.at("name");
        EXPECT_EQ(*result.row_manifest_digest,c.at("expected_manifest_digest").get<std::string>()) << c.at("name");
        std::set<CurrentReportRowKey> shown;
        for(const auto& k:c.at("expected_shown_keys")) shown.insert({k.at("strategy_name"),k.at("symbol")});
        std::set<CurrentReportRowKey> projected;
        for(const auto& [k,_]:result.projection->quantity_exact) projected.insert(k);
        EXPECT_EQ(projected,shown) << c.at("name");
    }
    EXPECT_EQ(eligible,14u); EXPECT_EQ(refused,7u);
}

// ---- PLAN14 (F3): a key with no row before processing is treated as before = 0 ----
ComponentPositionCandidate leg(std::string strategy,std::string symbol,std::string amount,AssetType type=AssetType::EQUITY,
                               std::string price="10",std::string owner="RUN") {
    return {{"BOOK",owner,strategy,"2026-09-25",symbol,"qt"},{type,symbol},true,std::nullopt,
        Position(symbol,qty(amount),qty(price),Decimal(2),Decimal(3),day()),false};
}
// Refusal from the snapshot builder or the projection both mean "not ready".
QtReportEligibility project(const std::vector<ComponentPositionCandidate>& before,const std::vector<ComponentPositionCandidate>& after) {
    try {return build_qt_report_quantity_projection(before,after,saved(before,after),decision,digest(after));}
    catch(const std::exception&) {return {"unavailable",{"snapshot_refused"},std::nullopt,std::nullopt};}
}
// Independent manifest: the shown keys in ComponentPositionKey order.
std::string manifest(std::vector<ComponentPositionKey> keys) {
    std::sort(keys.begin(),keys.end()); nlohmann::json list=nlohmann::json::array();
    for(const auto& k:keys) list.push_back({{"portfolio_id",k.portfolio_id},{"strategy_id",k.strategy_id},
        {"strategy_name",k.strategy_name},{"date",k.date},{"symbol",k.symbol},{"portfolio_type",k.portfolio_type}});
    return qt_digest_v1({{"component_keys",list}}).value();
}

TEST(QtReportProjectionTest, OpenFromAbsentFuturesAndEquityGetOwnRows) {
    const auto before=components(); auto after=before;
    after.push_back(leg("alpha","FUT","2",AssetType::FUTURE,"11"));
    after.push_back(leg("beta","NEW","9.25",AssetType::EQUITY,"11"));
    const auto source=saved(before,after);
    ASSERT_TRUE(source.by_strategy.at("alpha").contains("FUT"));
    EXPECT_EQ(source.by_strategy.at("beta").at("NEW").quantity.to_string(),"9.25");
    EXPECT_EQ(source.combined.at("FUT").quantity.to_string(),"2");
    EXPECT_EQ(source.combined.at("NEW").quantity.to_string(),"9.25");
    EXPECT_EQ(source.evidence_counts.at("alpha"),1u); EXPECT_EQ(source.evidence_counts.at("beta"),1u); // before rows only
    const auto opened=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
    ASSERT_EQ(opened.status,"eligible");
    EXPECT_EQ(opened.projection->quantity_exact.at({"alpha","FUT"}),"2");
    EXPECT_EQ(opened.projection->quantity_exact.at({"beta","NEW"}),"9.25");
    EXPECT_EQ(opened.projection->quantity_exact.size(),4u);
    std::vector<ComponentPositionKey> keys; for(const auto& row:after) keys.push_back(row.key);
    EXPECT_EQ(*opened.row_manifest_digest,manifest(keys));
}

TEST(QtReportProjectionTest, ShortOpenedFromAbsentShowsSignedRow) {
    const auto before=components(); auto after=before; after.push_back(leg("alpha","CL","-3",AssetType::FUTURE));
    const auto opened=project(before,after);
    ASSERT_EQ(opened.status,"eligible");
    EXPECT_EQ(opened.projection->quantity_exact.at({"alpha","CL"}),"-3");
    EXPECT_EQ(saved(before,after).combined.at("CL").quantity.to_string(),"-3");
}

TEST(QtReportProjectionTest, AbsentToZeroIsNotShown) {
    const auto before=components(); auto after=before; after.push_back(leg("alpha","NEW","0"));
    const auto source=saved(before,after);
    EXPECT_FALSE(source.by_strategy.at("alpha").contains("NEW")); EXPECT_FALSE(source.combined.contains("NEW"));
    const auto projected=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
    ASSERT_EQ(projected.status,"eligible");
    EXPECT_EQ(projected.projection->quantity_exact.size(),2u);
    EXPECT_EQ(*projected.row_manifest_digest,manifest({before[0].key,before[1].key}));
    EXPECT_NE(projected.projection->published_book_digest,digest(before)); // the selection still binds the zero key
}

TEST(QtReportProjectionTest, FutureClosedToZeroShowsZeroForTheDay) {
    auto before=components(); before.push_back(leg("alpha","FUT","5",AssetType::FUTURE));
    auto after=before; after.back().position.quantity=qty("0");
    const auto source=saved(before,after);
    EXPECT_TRUE(source.by_strategy.at("alpha").at("FUT").quantity.is_zero()); EXPECT_FALSE(source.combined.contains("FUT"));
    const auto closed=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
    ASSERT_EQ(closed.status,"eligible");
    EXPECT_EQ(closed.projection->quantity_exact.at({"alpha","FUT"}),"0");
    EXPECT_EQ(*closed.row_manifest_digest,manifest({before[0].key,before[1].key,before[2].key}));
}

TEST(QtReportProjectionTest, OpeningIntoAnEmptyBeforeBookGetsItsRow) {
    const std::vector<ComponentPositionCandidate> before;
    const std::vector<ComponentPositionCandidate> after{leg("alpha","NQ","2",AssetType::FUTURE)};
    const auto opened=project(before,after);
    ASSERT_EQ(opened.status,"eligible");
    EXPECT_EQ(opened.projection->quantity_exact.at({"alpha","NQ"}),"2");
    EXPECT_EQ(*opened.row_manifest_digest,manifest({after[0].key}));
    EXPECT_EQ(project(before,{}).status,"unavailable"); // nothing selected at all stays refused
}

TEST(QtReportProjectionTest, TwoStrategyIdsRefusedIncludingOnAnAbsentKey) {
    auto before=components(); before[1].key.strategy_id="OTHER";
    EXPECT_EQ(project(before,before).status,"unavailable");
    const auto base=components(); auto after=base; after.push_back(leg("beta","NEW","1",AssetType::EQUITY,"10","OTHER"));
    EXPECT_EQ(project(base,after).status,"unavailable");
}

TEST(QtReportProjectionTest, BeforeKeyMissingFromAfterRefused) {
    const auto before=components(); auto after=before; after.pop_back();
    EXPECT_EQ(project(before,after).status,"unavailable");
    EXPECT_EQ(build_qt_report_quantity_projection(before,after,snapshot(),decision,digest(after)).status,"unavailable");
    after.push_back(leg("beta","NEW","1")); // same size, different key set: still a missing before key
    EXPECT_EQ(project(before,after).status,"unavailable");
}

TEST(QtReportProjectionTest, AbsentKeyKeepsScopeTypeAndWholeFutureRefusals) {
    const auto before=components();
    for(int mutation=0;mutation<8;++mutation) {
        auto row=leg("alpha","NEW","2");
        if(mutation==0) row.key.strategy_name="gamma";              // outside the report's strategies
        if(mutation==1) row.key.date="2026-09-26";
        if(mutation==2) row.key.portfolio_id="OTHER";
        if(mutation==3) row.key.portfolio_type="qt_proposal";
        if(mutation==4) row.instrument.symbol="SYN";                 // instrument must be the row's own
        if(mutation==5) row.position.symbol="SYN";
        if(mutation==6) {row.instrument.type=AssetType::FUTURE;row.position.quantity=qty("1.5");}
        if(mutation==7) row.instrument.type=AssetType::OPTION;
        auto after=before; after.push_back(row);
        EXPECT_EQ(project(before,after).status,"unavailable") << mutation;
    }
    auto after=before; after.push_back(leg("alpha","NEW","2")); after.push_back(leg("alpha","NEW","3"));
    EXPECT_EQ(project(before,after).status,"unavailable"); // duplicate absent key
}

// ---- PLAN14 r2 (review findings 2, 3, 6): refusals reached by the projection itself ----
// The builder refuses the same inputs first, so these use hand-built snapshots that are otherwise
// consistent (each has an eligible control) to reach the projection's own checks.
ReportPositionSnapshot hand_built(StrategyPositionRows rows,std::unordered_map<std::string,Position> combined,
                                  std::vector<std::string> names,std::unordered_map<std::string,size_t> counts) {
    return {std::move(rows),std::move(combined),"BOOK","RUN",std::move(names),"qt",day(),std::move(counts)};
}

TEST(QtReportProjectionTest, HandBuiltSnapshotBeforeKeyMissingFromAfterRefused) {
    const auto before=components(); // alpha/SYN 5, beta/ALT -3
    const std::vector<ComponentPositionCandidate> after{before[0]}; // beta/ALT is missing after processing
    const auto rows=[&](size_t beta_before){return hand_built({{"alpha",{{"SYN",before[0].position}}},{"beta",{}}},
        {{"SYN",before[0].position}},{"alpha","beta"},{{"alpha",1},{"beta",beta_before}});};
    // Reaches qt_report_quantity_projection.cpp "a before key missing from after": nothing else differs.
    EXPECT_EQ(build_qt_report_quantity_projection(before,after,rows(1),decision,digest(after)).status,"unavailable");
    // Control: the same snapshot is eligible when the before accounting really held only alpha/SYN.
    EXPECT_EQ(build_qt_report_quantity_projection(after,after,rows(0),decision,digest(after)).status,"eligible");
}

TEST(QtReportProjectionTest, HandBuiltSnapshotAfterOnlyKeyOutsideConfiguredNamesRefused) {
    const std::vector<ComponentPositionCandidate> before{components()[0]}; // alpha/SYN 5
    auto after=before; after.push_back(leg("gamma","NEW","2"));         // gamma is not a report strategy
    const auto syn=before[0].position,fresh=after[1].position;
    // Reaches the projection's configured-name check for an after-only key; without it the snapshot
    // below (which omits gamma) would be eligible and silently drop gamma/NEW.
    EXPECT_EQ(build_qt_report_quantity_projection(before,after,
        hand_built({{"alpha",{{"SYN",syn}}}},{{"SYN",syn}},{"alpha"},{{"alpha",1}}),decision,digest(after)).status,"unavailable");
    // Control: with gamma configured, the same key is shown.
    const auto shown=build_qt_report_quantity_projection(before,after,
        hand_built({{"alpha",{{"SYN",syn}}},{"gamma",{{"NEW",fresh}}}},{{"SYN",syn},{"NEW",fresh}},{"alpha","gamma"},{{"alpha",1},{"gamma",0}}),
        decision,digest(after));
    ASSERT_EQ(shown.status,"eligible"); EXPECT_EQ(shown.projection->quantity_exact.at({"gamma","NEW"}),"2");
}

// Finding 3: the shared vector case is refused in the vector harness by typing (as the loader does);
// this companion drives the same rows to the superset rule in both the builder and the projection.
TEST(QtReportProjectionTest, VectorBeforeKeyMissingFromAfterReachesTheSupersetRule) {
    std::ifstream in(std::filesystem::path(__FILE__).parent_path().parent_path()/"contracts"/"qt-report-manifest-vectors-v2.json");
    ASSERT_TRUE(in.good()); const auto vectors=nlohmann::json::parse(in);
    const nlohmann::json* found=nullptr;
    for(const auto& c:vectors.at("cases")) if(c.at("name")=="before_key_missing_from_after_refused") found=&c;
    ASSERT_NE(found,nullptr); ASSERT_TRUE(found->at("expected_manifest_digest").is_null());
    auto rows=[](const nlohmann::json& list){ // every generator row in this case is EQUITY
        std::vector<ComponentPositionCandidate> out;
        for(const auto& r:list){ const auto& k=r.at("key");
            ComponentPositionKey key{k.at("portfolio_id"),k.at("strategy_id"),k.at("strategy_name"),k.at("date"),k.at("symbol"),k.at("portfolio_type")};
            out.push_back({key,{AssetType::EQUITY,key.symbol},true,std::nullopt,
                Position(key.symbol,qty(r.at("quantity_exact")),qty(r.at("average_price_exact")),Decimal(0),Decimal(0),day()),false});}
        return out; };
    const auto before=rows(found->at("before")),after=rows(found->at("after"));
    ASSERT_EQ(before.size(),2u); ASSERT_EQ(after.size(),1u);
    try {build_qt_saved_report_snapshot(before,after,{"alpha"},"QT_BOOK","RUN",day()); ADD_FAILURE() << "builder accepted";}
    catch(const std::invalid_argument& error) {EXPECT_STREQ(error.what(),"before_key_not_in_after");}
    const auto consistent=build_qt_saved_report_snapshot(after,after,{"alpha"},"QT_BOOK","RUN",day());
    EXPECT_EQ(build_qt_report_quantity_projection(after,after,consistent,decision,digest(after)).status,"eligible"); // control
    const ReportPositionSnapshot claimed{consistent.by_strategy,consistent.combined,consistent.portfolio_id,consistent.strategy_id,
        consistent.strategy_names,consistent.portfolio_type,consistent.date,{{"alpha",2}}};
    EXPECT_EQ(build_qt_report_quantity_projection(before,after,claimed,decision,digest(after)).status,"unavailable");
}

// Finding 6: a short opened from a zero before row (the absent-before short is covered above).
TEST(QtReportProjectionTest, ShortOpenedFromZeroRowShowsSignedRow) {
    auto before=components(); before.push_back(leg("alpha","CL","0",AssetType::FUTURE));
    auto after=before; after.back().position.quantity=qty("-3");
    const auto source=saved(before,after);
    EXPECT_EQ(source.combined.at("CL").quantity.to_string(),"-3");
    const auto opened=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
    ASSERT_EQ(opened.status,"eligible");
    EXPECT_EQ(opened.projection->quantity_exact.at({"alpha","CL"}),"-3");
    EXPECT_EQ(*opened.row_manifest_digest,manifest({before[0].key,before[1].key,before[2].key}));
    EXPECT_EQ(opened.row_manifest_digest,project(components(),[&]{auto a=components();a.push_back(after.back());return a;}()).row_manifest_digest);
}

// Review finding 7 (INFO): a book with no before rows whose only selection stays at 0 is eligible with
// no rows, by the rule as stated (native only; vectors v2 are unchanged).
TEST(QtReportProjectionTest, EmptyBeforeBookAllZeroIsEligibleWithNoRows) {
    const std::vector<ComponentPositionCandidate> after{leg("alpha","NQ","0",AssetType::FUTURE)};
    const auto result=project({},after);
    ASSERT_EQ(result.status,"eligible");
    EXPECT_TRUE(result.projection->quantity_exact.empty());
    EXPECT_EQ(*result.row_manifest_digest,manifest({}));
}

class RegistryGuard {
    InstrumentRegistry& registry_=InstrumentRegistry::instance();
    std::unordered_map<std::string,std::shared_ptr<Instrument>> old_;
public:
    RegistryGuard() {
        std::lock_guard<std::mutex> lock(registry_.mutex_); old_=registry_.instruments_;
        for(const auto* symbol:{"SYN","ALT"}) {
            EquitySpec spec; spec.exchange="TEST"; spec.currency="USD"; spec.margin_requirement=0.5;
            registry_.instruments_[symbol]=std::make_shared<EquityInstrument>(symbol,spec);
        }
    }
    ~RegistryGuard() {std::lock_guard<std::mutex> lock(registry_.mutex_);registry_.instruments_=std::move(old_);}
};
std::string read(const std::string& path) {std::ifstream input(path);return {std::istreambuf_iterator<char>(input),{}};}
std::string html(EmailSender& renderer,const ReportPositionSnapshot& source,const CurrentReportQuantityProjection* display) {
    return renderer.generate_trading_report_body(source.by_strategy,source.combined,std::nullopt,
        {{"Current Portfolio Value",1000000},{"Gross Leverage",1},{"Net Leverage",0}}, {},
        "2026-09-25","Synthetic",true,{{"SYN",12},{"ALT",13}},nullptr,
        source.by_strategy,{{"SYN",11},{"ALT",12}},{{"SYN",10},{"ALT",11}}, {},display);
}
TEST(QtReportProjectionTest, WholeHtmlAndCsvRenderSavedPositions) {
    RegistryGuard registry; EmailSender renderer(EmailSenderConfig{}); // Rendering only; never initialize or call transport.
    const auto directory=std::filesystem::temp_directory_path()/("qt_recompute_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{directory};
    CSVExporter exporter(directory.string());
    for(const auto& chosen:std::vector<std::string>{"7","2.5","-5","5"}) {
        const auto before=components(); auto after=before; after[0].position.quantity=qty(chosen);
        const auto source=saved(before,after);
        const auto projected=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
        ASSERT_TRUE(projected.projection);
        // The QT path renders exactly what the renderer draws for the saved positions.
        EXPECT_EQ(html(renderer,source,&*projected.projection),html(renderer,source,nullptr));
        // Both exports write the same dated file: read the plain one before the QT export replaces it.
        const auto plain=exporter.export_current_positions(day(),source.by_strategy,{{"SYN",12},{"ALT",13}},1000000,990,100,{},true);
        ASSERT_TRUE(plain.is_ok()); const auto plain_csv=read(plain.value());
        const auto qt=exporter.export_current_positions(day(),source.by_strategy,{{"SYN",12},{"ALT",13}},1000000,990,100,{},true,&*projected.projection);
        ASSERT_TRUE(qt.is_ok()); EXPECT_EQ(read(qt.value()),plain_csv);
    }
    // Independent arithmetic: 7 shares x average price 10 x multiplier 1 = $70.00.
    const auto before=components(); auto after=before; after[0].position.quantity=qty("7");
    const auto seven=build_qt_report_quantity_projection(before,after,saved(before,after),decision,digest(after));
    ASSERT_TRUE(seven.projection);
    const auto rendered=html(renderer,saved(before,after),&*seven.projection);
    EXPECT_NE(rendered.find("<td>SYN</td>\n<td>7</td>"),std::string::npos);
    EXPECT_NE(rendered.find("<td>$70.00</td>"),std::string::npos);
}

TEST(QtReportProjectionTest, SameSymbolSplitAndNetZeroKeepWholeDocuments) {
    RegistryGuard registry; EmailSender renderer(EmailSenderConfig{});
    const auto directory=std::filesystem::temp_directory_path()/
        ("qt_projection_split_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{directory};
    CSVExporter exporter(directory.string());
    for(const auto& [second_quantity,chosen]:std::vector<std::pair<std::string,std::string>>{{"5","1"},{"-5","-4"},{"5","0"}}) {
        auto before=components();
        before[1].key.symbol="SYN";before[1].instrument.symbol="SYN";
        before[1].position=pos("SYN",second_quantity);
        auto after=before;
        after[1].position.quantity=qty(chosen);
        const auto source=saved(before,after);
        auto eligible=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
        ASSERT_TRUE(eligible.projection);
        const auto rendered=html(renderer,source,&*eligible.projection);
        const auto header=rendered.find(">Beta</h3>");ASSERT_NE(header,std::string::npos);
        EXPECT_NE(rendered.find("<td>SYN</td>\n<td>"+chosen+"</td>",header),std::string::npos); // {"5","0"}: Beta shows 0
        if(chosen!="0") { EXPECT_EQ(rendered,html(renderer,source,nullptr)); }
        const auto baseline_csv=exporter.export_current_positions(day(),source.by_strategy,{{"SYN",12}},1000000,990,100,{},true);
        ASSERT_TRUE(baseline_csv.is_ok());const auto expected_csv=read(baseline_csv.value());
        EXPECT_NE(expected_csv.find("Beta,SYN,"+chosen+","),std::string::npos);
        const auto rendered_csv=exporter.export_current_positions(day(),source.by_strategy,{{"SYN",12}},1000000,990,100,{},true,&*eligible.projection);
        ASSERT_TRUE(rendered_csv.is_ok());EXPECT_EQ(read(rendered_csv.value()),expected_csv);
    }
}

TEST(QtReportProjectionTest, IncompleteProjectionCannotEmitOrOverwriteReport) {
    auto before=components();auto eligible=build_qt_report_quantity_projection(before,before,snapshot(),decision,digest(before));
    ASSERT_TRUE(eligible.projection);eligible.projection->quantity_exact.erase({"beta","ALT"});
    EmailSender renderer(EmailSenderConfig{});
    EXPECT_THROW(html(renderer,snapshot(),&*eligible.projection),std::invalid_argument);
    const auto directory=std::filesystem::temp_directory_path()/
        ("qt_projection_refusal_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{directory};
    CSVExporter exporter(directory.string());
    auto result=exporter.export_current_positions(day(),snapshot().by_strategy,{},1000000,990,100,{},true,&*eligible.projection);
    EXPECT_TRUE(result.is_error());EXPECT_TRUE(std::filesystem::is_empty(directory));
}

TEST(QtReportProjectionTest, FractionalFuturesNeverProduceEligibleDisplay) {
    auto before=components();before[0].instrument.type=AssetType::FUTURE;auto after=before;
    after[0].position.quantity=qty("7.125");
    EXPECT_EQ(build_qt_report_quantity_projection(before,after,snapshot(),decision,digest(after)).status,"unavailable");
}

TEST(QtReportProjectionTest, ClosedTodayRowRendersAtZero) {
    RegistryGuard registry; EmailSender renderer(EmailSenderConfig{});
    const auto before=components(); auto after=before; after[0].position.quantity=qty("0");
    const auto source=saved(before,after);
    const auto projected=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
    ASSERT_TRUE(projected.projection);
    const auto rendered=html(renderer,source,&*projected.projection);
    EXPECT_NE(rendered.find("<td>SYN</td>\n<td>0</td>"),std::string::npos);
    EXPECT_EQ(html(renderer,source,nullptr).find("<td>SYN</td>"),std::string::npos); // legacy path keeps skipping zero rows
}

TEST(QtReportProjectionTest, EveryRowClosedStillShowsStrategyTable) {
    RegistryGuard registry; EmailSender renderer(EmailSenderConfig{});
    const auto before=components(); auto after=before; after[0].position.quantity=qty("0");
    const auto closed=build_qt_report_quantity_projection(before,after,saved(before,after),decision,digest(after));
    ASSERT_TRUE(closed.projection);
    const auto rendered=html(renderer,saved(before,after),&*closed.projection);
    const auto alpha=rendered.find(">Alpha</h3>"); ASSERT_NE(alpha,std::string::npos);
    EXPECT_NE(rendered.find("<strong>Positions:</strong> 0",alpha),std::string::npos);
}

TEST(QtReportProjectionTest, MissingInstrumentForShownRowBlocksReport) {
    RegistryGuard registry; EmailSender renderer(EmailSenderConfig{});
    auto before=components(); before.push_back(extra(before[0],"UNREG","0"));
    auto after=before; after.back().position.quantity=qty("1");
    const auto source=saved(before,after);
    const auto projected=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
    ASSERT_TRUE(projected.projection);
    EXPECT_ANY_THROW(html(renderer,source,&*projected.projection));
}

// ---- PLAN14 (F5): whole HTML and CSV from the pre-decision snapshot and from the saved snapshot ----
// Only the positions tables and their total lines may differ; every other byte is identical, and the
// tables equal independently computed expectations (HTML notional: quantity x saved average price x
// multiplier; CSV notional: quantity x market price x multiplier). The three position-derived sections
// (Portfolio Composition, Symbols Reference, Rollover Warning) are drawn by the body only with a
// database, and a database-backed body also runs gnuplot subprocesses for its charts, so each of the
// three is driven through its own renderer entry point with the snapshot's combined map: that map is
// exactly the `positions` argument generate_trading_report_body passes to them.
struct Instrumented {AssetType type; double multiplier,margin;};
const std::map<std::string,Instrumented>& instruments() {
    static const std::map<std::string,Instrumented> table{{"SYN",{AssetType::EQUITY,1,0.5}},{"ALT",{AssetType::EQUITY,1,0.5}},
        {"FUT",{AssetType::FUTURE,50,1000}},{"NEW",{AssetType::FUTURE,20,800}}};
    return table;
}
const std::unordered_map<std::string,double>& market() {
    static const std::unordered_map<std::string,double> prices{{"SYN",12},{"ALT",13},{"FUT",102},{"NEW",103}};
    return prices;
}
class DocumentRegistry {
    InstrumentRegistry& registry_=InstrumentRegistry::instance();
    std::unordered_map<std::string,std::shared_ptr<Instrument>> old_;
public:
    DocumentRegistry() {
        std::lock_guard<std::mutex> lock(registry_.mutex_); old_=registry_.instruments_;
        for(const auto& [symbol,item]:instruments()) {
            if(item.type==AssetType::EQUITY) {
                EquitySpec spec; spec.exchange="TEST"; spec.currency="USD"; spec.margin_requirement=item.margin;
                registry_.instruments_[symbol]=std::make_shared<EquityInstrument>(symbol,spec);
            } else {
                FuturesSpec spec; spec.root_symbol=symbol; spec.exchange="TEST"; spec.currency="USD";
                spec.multiplier=item.multiplier; spec.tick_size=0.25; spec.commission_per_contract=0.01;
                spec.initial_margin=item.margin; spec.maintenance_margin=item.margin*0.9; spec.weight=1;
                spec.trading_hours="00:00-23:59";
                registry_.instruments_[symbol]=std::make_shared<FuturesInstrument>(symbol,spec);
            }
        }
    }
    ~DocumentRegistry() {std::lock_guard<std::mutex> lock(registry_.mutex_);registry_.instruments_=std::move(old_);}
};
// Records the section queries; never executes anything.
class RecordingDatabase : public PostgresDatabase {
public:
    RecordingDatabase() : PostgresDatabase("mock://qt-plan14-report-sections") {}
    std::vector<std::string> queries;
    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& sql) override {
        queries.push_back(sql);
        return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::DATABASE_ERROR,"recorded only","qt_plan14_test");
    }
};
// Standard explicit-instantiation access to the two private section renderers (no #define private).
template<class Tag,typename Tag::type Member> struct Expose {friend typename Tag::type member(Tag) {return Member;}};
struct SymbolsSection {
    using type=std::string (EmailSender::*)(const std::unordered_map<std::string,Position>&,std::shared_ptr<DatabaseInterface>,const std::string&);
    friend type member(SymbolsSection);
};
struct RolloverSection {
    using type=std::string (EmailSender::*)(const std::unordered_map<std::string,Position>&,const std::string&,
                                            std::shared_ptr<DatabaseInterface>,const std::string&);
    friend type member(RolloverSection);
};
template struct Expose<SymbolsSection,&EmailSender::format_symbols_table_for_positions>;
template struct Expose<RolloverSection,&EmailSender::format_rollover_warning>;

struct Row {std::string strategy,symbol,quantity_text; double quantity,average;};
std::string fixed2(double value) {char text[64]; std::snprintf(text,sizeof(text),"%.2f",value); return text;}
std::string money(double value) {
    auto text=fixed2(value); const int start=text[0]=='-'?1:0;
    for(int at=static_cast<int>(text.find('.'))-3;at>start;at-=3) text.insert(static_cast<size_t>(at),",");
    return text;
}
std::string title(std::string name) {name[0]=static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));return name;}
std::string expected_tables(const std::vector<Row>& rows) {
    std::map<std::string,std::vector<Row>> grouped; for(const auto& row:rows) grouped[row.strategy].push_back(row);
    std::ostringstream html; double all_notional=0,all_margin=0; int all_active=0;
    for(const auto& [strategy,list]:grouped) {
        double total=0,margin=0; int active=0;
        for(const auto& row:list) {
            const auto& item=instruments().at(row.symbol);
            total+=std::abs(row.quantity*row.average*item.multiplier);
            margin+=std::abs(row.quantity)*(item.type==AssetType::EQUITY?row.average:item.margin);
            if(row.quantity!=0) ++active;
        }
        html<<"<h3 style=\"margin-top: 20px; margin-bottom: 10px; color: #333; border-left: 4px solid #2c5aa0; padding-left: 12px;\">"
            <<title(strategy)<<"</h3>\n<table>\n<tr><th>Symbol</th><th>Quantity</th><th>Market Price</th><th>Notional</th><th>% of Total</th></tr>\n";
        for(const auto& row:list) {
            const double notional=std::abs(row.quantity*row.average*instruments().at(row.symbol).multiplier);
            html<<"<tr>\n<td>"<<row.symbol<<"</td>\n<td>"<<row.quantity_text<<"</td>\n<td>$"<<fixed2(market().at(row.symbol))
                <<"</td>\n<td>$"<<money(notional)<<"</td>\n<td>"<<fixed2(total>0?notional/total*100.0:0.0)<<"%</td>\n</tr>\n";
        }
        html<<"</table>\n<div style=\"font-size: 13px; color: #666; margin: 8px 0 20px 0; padding-left: 16px;\">\n"
            <<"<strong>Positions:</strong> "<<active<<" | <strong>Notional:</strong> $"<<money(total)
            <<" | <strong>Margin:</strong> $"<<money(margin)<<"\n</div>\n";
        all_notional+=total; all_margin+=margin; all_active+=active;
    }
    html<<"<div class=\"summary-stats\" style=\"margin-top: 20px; border-top: 2px solid #2c5aa0; padding-top: 15px;\">\n"
        <<"<div class=\"metric\"><strong>Active Positions:</strong> "<<all_active<<"</div>\n"
        <<"<div class=\"metric\"><strong>Total Notional:</strong> $"<<money(all_notional)<<"</div>\n"
        <<"<div class=\"metric\"><strong>Total Margin Posted:</strong> $"<<money(all_margin)<<"</div>\n</div>\n";
    return html.str();
}
// Row order inside one strategy table follows unordered_map iteration (the renderer's existing order).
std::string sorted_rows(std::string region) {
    for(size_t at=0;(at=region.find("</th></tr>\n",at))!=std::string::npos;) {
        at+=11; const auto end=region.find("</table>\n",at);
        std::vector<std::string> rows;
        for(size_t from=at;from<end;) {const auto to=region.find("</tr>\n",from)+6; rows.push_back(region.substr(from,to-from)); from=to;}
        std::sort(rows.begin(),rows.end()); std::string joined; for(const auto& row:rows) joined+=row;
        region.replace(at,end-at,joined); at+=joined.size();
    }
    return region;
}
struct Parts {std::string head,tables,tail;};
Parts split_positions(const std::string& html) {
    const std::string open="<h2>Today's Positions</h2>\n";
    const auto start=html.find(open); if(start==std::string::npos) throw std::runtime_error("positions header missing");
    const auto from=start+open.size(); const auto total=html.find("<strong>Total Margin Posted:</strong>",from);
    if(total==std::string::npos) throw std::runtime_error("portfolio total missing");
    const auto end=html.find("</div>\n</div>\n",total)+14;
    return {html.substr(0,from),html.substr(from,end-from),html.substr(end)};
}
std::vector<std::string> split(const std::string& text,char separator) {
    std::vector<std::string> out; std::string field; std::istringstream input(text);
    while(std::getline(input,field,separator)) out.push_back(field);
    return out;
}
// CSV rows: strategy, symbol and exact quantity text compare exactly; numbers are parsed, because the
// exporter's stream precision is 2 decimals on the first row and 6 on later rows.
void expect_csv_rows(const std::vector<std::string>& lines,const std::vector<Row>& rows,const std::string& label) {
    std::map<std::pair<std::string,std::string>,std::vector<std::string>> actual;
    for(size_t i=2;i<lines.size();++i) {
        const auto fields=split(lines[i],','); ASSERT_EQ(fields.size(),13u) << label << lines[i];
        ASSERT_TRUE(actual.emplace(std::make_pair(fields[0],fields[1]),fields).second) << label << lines[i];
    }
    ASSERT_EQ(actual.size(),rows.size()) << label;
    for(const auto& row:rows) {
        const auto found=actual.find({title(row.strategy),row.symbol}); ASSERT_NE(found,actual.end()) << label << row.symbol;
        const auto& fields=found->second; const double price=market().at(row.symbol);
        const double notional=row.quantity*price*instruments().at(row.symbol).multiplier;
        EXPECT_EQ(fields[2],row.quantity_text) << label << row.symbol;
        const std::vector<double> numbers{price,notional,std::abs(notional)/990*100,std::abs(notional)/1000000*100,0,0,0,0,0,0};
        for(size_t i=0;i<numbers.size();++i) EXPECT_NEAR(std::stod(fields[3+i]),numbers[i],0.0051) << label << row.symbol << " column " << 3+i;
    }
}
struct Sections {std::vector<std::string> labels; std::vector<double> percentages; std::string symbols,rollover; std::vector<std::string> queries;};
Sections position_sections(EmailSender& renderer,const std::unordered_map<std::string,Position>& combined) {
    auto db=std::make_shared<RecordingDatabase>();
    const auto chart=ChartGenerator::fetch_portfolio_composition_data(combined,market(),"2026-09-25");
    Sections out{chart.labels,chart.values,(renderer.*member(SymbolsSection{}))(combined,db,"2026-09-24"),
                 (renderer.*member(RolloverSection{}))(combined,"2026-09-25",db,""),{}};
    out.queries=db->queries; return out;
}
std::string in_list(const std::string& sql) {
    const std::string marker="\"Databento Symbol\" IN ("; auto at=sql.find(marker);
    if(at==std::string::npos) return "<none>";
    at+=marker.size(); return sql.substr(at,sql.find(')',at)-at);
}
// Independent: QT saved rows summed by symbol across strategies, nonzero only.
void expect_sections(const Sections& got,const std::vector<Row>& rows,const std::string& label) {
    std::map<std::string,double> net; for(const auto& row:rows) if(row.quantity!=0) net[row.symbol]+=row.quantity;
    std::vector<std::pair<std::string,double>> notional; double total=0; std::string symbols;
    for(const auto& [symbol,quantity]:net) {
        if(quantity==0) continue;
        const double value=std::abs(quantity*market().at(symbol)*instruments().at(symbol).multiplier);
        notional.push_back({symbol,value}); total+=value; symbols+=(symbols.empty()?"'":", '")+symbol+"'";
    }
    std::sort(notional.begin(),notional.end(),[](const auto& a,const auto& b){return a.second>b.second;});
    ASSERT_EQ(got.labels.size(),notional.size()) << label; ASSERT_EQ(got.percentages.size(),notional.size()) << label;
    for(size_t i=0;i<notional.size();++i) {
        EXPECT_EQ(got.labels[i],notional[i].first) << label;
        EXPECT_NEAR(got.percentages[i],notional[i].second/total*100.0,1e-9) << label;
    }
    ASSERT_EQ(got.queries.size(),2u) << label; // symbols reference, then rollover
    EXPECT_EQ(in_list(got.queries[0]),symbols) << label;
    EXPECT_EQ(in_list(got.queries[1]),symbols) << label;
    EXPECT_EQ(got.symbols.rfind("<p>Unable to load symbols data: ",0),0u) << label;
    EXPECT_EQ(got.rollover,"") << label;
}

TEST(QtReportProjectionTest, WholeDocumentsChangeOnlyPositionDerivedSections) {
    DocumentRegistry registry; EmailSender renderer(EmailSenderConfig{}); // Rendering only; never initialize or call transport.
    const auto directory=std::filesystem::temp_directory_path()/
        ("qt_plan14_whole_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{directory};
    CSVExporter exporter(directory.string());
    const std::vector<ComponentPositionCandidate> before{leg("alpha","SYN","5"),leg("alpha","FUT","5",AssetType::FUTURE),leg("beta","ALT","-3")};
    const std::vector<Row> pre_rows{{"alpha","SYN","5",5,10},{"alpha","FUT","5",5,10},{"beta","ALT","-3",-3,10}};
    const auto pre=saved(before,before);
    const auto pre_projection=build_qt_report_quantity_projection(before,before,pre,decision,digest(before));
    ASSERT_EQ(pre_projection.status,"eligible");
    const auto yesterday=pre.by_strategy; // the same prior-day input for both documents
    auto document=[&](const ReportPositionSnapshot& source,const CurrentReportQuantityProjection& display) {
        return renderer.generate_trading_report_body(source.by_strategy,source.combined,std::nullopt,
            {{"Current Portfolio Value",1000000},{"Gross Leverage",1},{"Net Leverage",0}}, {},
            "2026-09-25","Synthetic",true,market(),nullptr,yesterday,
            {{"SYN",11},{"ALT",12},{"FUT",101},{"NEW",102}},{{"SYN",10},{"ALT",11},{"FUT",100},{"NEW",101}}, {},&display);
    };
    auto csv=[&](const ReportPositionSnapshot& source,const CurrentReportQuantityProjection& display) {
        const auto written=exporter.export_current_positions(day(),source.by_strategy,market(),1000000,990,100,{},true,&display);
        if(written.is_error()) throw std::runtime_error("csv export refused");
        return split(read(written.value()),'\n');
    };
    const auto pre_html=split_positions(document(pre,*pre_projection.projection));
    const auto pre_csv=csv(pre,*pre_projection.projection);
    EXPECT_EQ(sorted_rows(pre_html.tables),sorted_rows(expected_tables(pre_rows)));
    expect_csv_rows(pre_csv,pre_rows,"pre ");
    expect_sections(position_sections(renderer,pre.combined),pre_rows,"pre ");

    struct Scenario {std::string name; std::vector<ComponentPositionCandidate> after; std::vector<Row> rows;};
    auto changed=before; changed[1]=leg("alpha","FUT","7",AssetType::FUTURE,"11");
    auto opened=before; opened.push_back(leg("beta","NEW","2",AssetType::FUTURE,"11"));
    auto closed=before; closed[1]=leg("alpha","FUT","0",AssetType::FUTURE,"11");
    const std::vector<Scenario> scenarios{
        {"futures_5_to_7",changed,{{"alpha","SYN","5",5,10},{"alpha","FUT","7",7,11},{"beta","ALT","-3",-3,10}}},
        {"future_open_from_absent",opened,{{"alpha","SYN","5",5,10},{"alpha","FUT","5",5,10},{"beta","ALT","-3",-3,10},{"beta","NEW","2",2,11}}},
        {"future_close_to_zero",closed,{{"alpha","SYN","5",5,10},{"alpha","FUT","0",0,11},{"beta","ALT","-3",-3,10}}}};
    for(const auto& scenario:scenarios) {
        const std::string label=scenario.name+" ";
        const auto post=[&]{try {return std::optional<ReportPositionSnapshot>(saved(before,scenario.after));}
                            catch(const std::exception&) {return std::optional<ReportPositionSnapshot>();}}();
        ASSERT_TRUE(post) << label << "saved snapshot refused";
        const auto projection=build_qt_report_quantity_projection(before,scenario.after,*post,decision,digest(scenario.after));
        ASSERT_EQ(projection.status,"eligible") << label;
        const auto post_html=split_positions(document(*post,*projection.projection));
        EXPECT_EQ(post_html.head,pre_html.head) << label;   // every byte before the positions tables
        EXPECT_EQ(post_html.tail,pre_html.tail) << label;   // every byte after the portfolio total lines
        EXPECT_NE(post_html.tables,pre_html.tables) << label;
        EXPECT_EQ(sorted_rows(post_html.tables),sorted_rows(expected_tables(scenario.rows))) << label;
        const auto post_csv=csv(*post,*projection.projection);
        ASSERT_GE(post_csv.size(),2u); EXPECT_EQ(post_csv[0],pre_csv[0]) << label; EXPECT_EQ(post_csv[1],pre_csv[1]) << label;
        expect_csv_rows(post_csv,scenario.rows,label);
        expect_sections(position_sections(renderer,post->combined),scenario.rows,label);
    }
}
} // namespace
} // namespace trade_ngin
