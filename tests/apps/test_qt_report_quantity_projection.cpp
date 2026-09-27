#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
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

TEST(QtReportProjectionTest, ExactFractionsSignedRowsAndStableManifest) {
    const auto before=components(); auto after=before;
    after[0].position.quantity=qty("7.125");
    const auto projected=build_qt_report_quantity_projection(before,after,snapshot(),decision,digest(after));
    ASSERT_EQ(projected.status,"eligible");
    ASSERT_TRUE(projected.projection);
    EXPECT_EQ(projected.projection->quantity_exact.at({"alpha","SYN"}),"7.125");
    EXPECT_EQ(projected.projection->quantity_exact.at({"beta","ALT"}),"-3");
    const auto unchanged=build_qt_report_quantity_projection(before,before,snapshot(),decision,digest(before));
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
        EXPECT_EQ(build_qt_report_quantity_projection(before,after,snapshot(),decision,claim).status,"unavailable");
    }
    EXPECT_EQ(build_qt_report_quantity_projection({},before,snapshot(),decision,digest(before)).status,"unavailable");
    EXPECT_EQ(build_qt_report_quantity_projection(before,before,snapshot(),"",digest(before)).status,"unavailable");
}

TEST(QtReportProjectionTest, FrozenCalculationBasisMustMatchBeforeSnapshot) {
    auto before=components(); before[0].position.average_price=Price(99);
    EXPECT_EQ(build_qt_report_quantity_projection(before,before,snapshot(),decision,digest(before)).status,"unavailable");
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
TEST(QtReportProjectionTest, WholeHtmlAndCsvChangeOnlyChosenQuantityCell) {
    RegistryGuard registry;
    const auto directory=std::filesystem::temp_directory_path()/
        ("qt_projection_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{directory};
    CSVExporter exporter(directory.string());
    EmailSender renderer(EmailSenderConfig{}); // Rendering only; never initialize or call transport.
    for(const auto& chosen:std::vector<std::string>{"7","2.5","-5","5","0"}) {
        auto before=components(),after=before;after[0].position.quantity=qty(chosen);
        auto eligible=build_qt_report_quantity_projection(before,after,snapshot(),decision,digest(after));
        ASSERT_TRUE(eligible.projection);
        const auto original=build_qt_report_quantity_projection(before,before,snapshot(),decision,digest(before));
        EXPECT_EQ(eligible.row_manifest_digest,original.row_manifest_digest);
        const auto source=snapshot();
        const auto baseline=html(renderer,source,nullptr);
        const auto changed=html(renderer,source,&*eligible.projection);
        auto expected=baseline;
        const std::string old_cell="<td>SYN</td>\n<td>5</td>";
        const auto offset=expected.find(old_cell);
        ASSERT_NE(offset,std::string::npos);
        expected.replace(offset,old_cell.size(),"<td>SYN</td>\n<td>"+chosen+"</td>");
        EXPECT_EQ(changed,expected);
        if(chosen!="5") { EXPECT_NE(changed,baseline); }
        auto first=exporter.export_current_positions(day(),source.by_strategy,{{"SYN",12},{"ALT",13}},1000000,990,100,{},true);
        ASSERT_TRUE(first.is_ok());const auto csv_before=read(first.value());
        auto second=exporter.export_current_positions(day(),source.by_strategy,{{"SYN",12},{"ALT",13}},1000000,990,100,{},true,&*eligible.projection);
        ASSERT_TRUE(second.is_ok());auto csv_expected=csv_before;
        const std::string csv_old="Alpha,SYN,5,"; const auto csv_offset=csv_expected.find(csv_old);
        ASSERT_NE(csv_offset,std::string::npos);csv_expected.replace(csv_offset,csv_old.size(),"Alpha,SYN,"+chosen+",");
        EXPECT_EQ(read(second.value()),csv_expected);
        if(chosen!="5") { EXPECT_NE(read(second.value()),csv_before); }
    }
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
        auto combined=pos("SYN","5");combined.quantity+=before[1].position.quantity;
        ReportPositionSnapshot source{{{"alpha",{{"SYN",before[0].position}}},{"beta",{{"SYN",before[1].position}}}},
            {{"SYN",combined}},"BOOK","RUN",{"alpha","beta"},"qt",day(),{{"alpha",1},{"beta",1}}};
        auto eligible=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
        ASSERT_TRUE(eligible.projection);
        const auto baseline=html(renderer,source,nullptr);auto expected=baseline;
        const auto header=expected.find(">Beta</h3>");ASSERT_NE(header,std::string::npos);
        const std::string old_cell="<td>SYN</td>\n<td>"+second_quantity+"</td>";
        const auto offset=expected.find(old_cell,header);ASSERT_NE(offset,std::string::npos);
        expected.replace(offset,old_cell.size(),"<td>SYN</td>\n<td>"+after[1].position.quantity.to_string()+"</td>");
        EXPECT_EQ(html(renderer,source,&*eligible.projection),expected);
        const auto baseline_csv=exporter.export_current_positions(day(),source.by_strategy,{{"SYN",12}},1000000,990,100,{},true);
        ASSERT_TRUE(baseline_csv.is_ok());auto expected_csv=read(baseline_csv.value());
        const std::string csv_cell="Beta,SYN,"+second_quantity+",";
        const auto csv_offset=expected_csv.find(csv_cell);ASSERT_NE(csv_offset,std::string::npos);
        expected_csv.replace(csv_offset,csv_cell.size(),"Beta,SYN,"+after[1].position.quantity.to_string()+",");
        const auto rendered_csv=exporter.export_current_positions(day(),source.by_strategy,{{"SYN",12}},1000000,990,100,{},true,&*eligible.projection);
        ASSERT_TRUE(rendered_csv.is_ok());EXPECT_EQ(read(rendered_csv.value()),expected_csv);
    }
}

TEST(QtReportProjectionTest, ExplicitUnchangedZeroRemainsInPublishedBook) {
    auto before=components();auto zero=before[0];zero.key.symbol="ZERO";zero.instrument.symbol="ZERO";
    zero.position=pos("ZERO","0");before.push_back(zero);auto after=before;after[0].position.quantity=qty("7");
    const auto base=snapshot();
    ReportPositionSnapshot source{base.by_strategy,base.combined,"BOOK","RUN",{"alpha","beta"},"qt",day(),{{"alpha",2},{"beta",1}}};
    auto eligible=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
    ASSERT_TRUE(eligible.projection);EXPECT_EQ(eligible.projection->quantity_exact.size(),2u);
    after[0].position.quantity=qty("0");
    const auto closed=build_qt_report_quantity_projection(before,after,source,decision,digest(after));
    ASSERT_TRUE(closed.projection);EXPECT_EQ(closed.row_manifest_digest,eligible.row_manifest_digest);
    EXPECT_EQ(closed.projection->quantity_exact.at({"alpha","SYN"}),"0");
    EXPECT_EQ(closed.projection->quantity_exact.size(),2u);
    after.back().position.quantity=qty("1");
    EXPECT_EQ(build_qt_report_quantity_projection(before,after,source,decision,digest(after)).status,"unavailable");
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
} // namespace
} // namespace trade_ngin
