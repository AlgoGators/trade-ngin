// Rendering-only probe for an owned disposable database. No transport methods.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#undef private
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/apps/qt_processed_report.hpp"
#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/csv_exporter.hpp"

namespace {
std::string read(const std::string& path){std::ifstream f(path);return {std::istreambuf_iterator<char>(f),{}};}
bool safe_dsn(const std::string& dsn){
    std::map<std::string,std::string> fields;std::istringstream input(dsn);std::string field;
    while(input>>field){
        const auto split=field.find('=');if(split==std::string::npos)return false;
        auto name=field.substr(0,split),value=field.substr(split+1);
        if(value.empty()||value.find_first_of("'\"\\")!=std::string::npos||!fields.emplace(name,value).second)return false;
        if(name!="host"&&name!="dbname"&&name!="user"&&name!="port"&&name!="connect_timeout"&&name!="password")return false;
    }
    return fields["host"].starts_with("/tmp/algolens-repair-pg-")&&
        fields["dbname"].starts_with("algolens_test_")&&fields["port"]=="5432"&&
        fields["user"]=="postgres"&&fields["password"]=="synthetic-test-only";
}
}
int main(int argc,char** argv){
    using namespace trade_ngin;
    const auto delivery_guard = reinterpret_cast<int(*)()>(dlsym(RTLD_DEFAULT, "qt_no_delivery_guard_loaded"));
    if(!delivery_guard || delivery_guard()!=1)return 86;
    LoggerConfig log;log.destination=LogDestination::NONE;Logger::instance().initialize(log);
    const char* raw=std::getenv("ALGOLENS_TEST_DB");
    if(!raw||!safe_dsn(raw)||argc<6)return 2;
    try{
        const std::string book=argv[1],day=argv[2],strategy_id=argv[3];
        const std::filesystem::path directory=argv[4];
        if(!directory.is_absolute()||!std::filesystem::is_directory(directory)||
           directory.filename().string().find("qt-report-")!=0)return 2;
        const auto parsed=std::chrono::year_month_day{std::chrono::year(std::stoi(day.substr(0,4))),
            std::chrono::month(std::stoul(day.substr(5,2))),std::chrono::day(std::stoul(day.substr(8,2)))};
        if(day.size()!=10||day[4]!='-'||day[7]!='-'||!parsed.ok())return 2;
        const Timestamp date=std::chrono::sys_days(parsed);
        std::vector<std::string> names;for(int i=5;i<argc;++i)names.emplace_back(argv[i]);
        PostgresDatabase db(raw);if(db.connect().is_error())return 3;
        auto loaded=load_qt_investor_report_snapshot(db,strategy_id,names,book,date,{});
        if(loaded.is_error()){std::cout<<"REPORT_BLOCKED=1\n";return 12;}
        auto& view=loaded.value();if(!view.display)return 13;
        auto& registry=InstrumentRegistry::instance();
        {std::lock_guard<std::mutex> lock(registry.mutex_);
            EquitySpec spec;spec.exchange="TEST";spec.currency="USD";spec.margin_requirement=0.5;
            registry.instruments_["SYN"]=std::make_shared<EquityInstrument>("SYN",spec);
            registry.instruments_["IMM"]=std::make_shared<EquityInstrument>("IMM",spec);
            FuturesSpec future;future.root_symbol="FUT";future.exchange="TEST";future.currency="USD";
            future.multiplier=50;future.tick_size=0.25;future.commission_per_contract=0.01;
            future.initial_margin=1000;future.maintenance_margin=900;future.weight=1;
            future.trading_hours="00:00-23:59";
            registry.instruments_["FUT"]=std::make_shared<FuturesInstrument>("FUT",future);}
        EmailSender renderer(EmailSenderConfig{});CSVExporter csv(directory.string());
        const auto& source=view.calculations;
        auto html=[&](const CurrentReportQuantityProjection* display){
            return renderer.generate_trading_report_body(source.by_strategy,source.combined,std::nullopt,
                {{"Current Portfolio Value",1000000},{"Gross Leverage",1},{"Net Leverage",0}}, {},
                day,"Synthetic",true,{{"SYN",102},{"IMM",102},{"FUT",102}},nullptr,source.by_strategy,
                {{"SYN",101},{"IMM",101},{"FUT",101}},{{"SYN",100},{"IMM",100},{"FUT",100}}, {},display);};
        auto baseline=csv.export_current_positions(date,source.by_strategy,{{"SYN",102},{"IMM",102},{"FUT",102}},1000000,990,100,{},true);
        if(baseline.is_error())return 14;
        const auto original_csv=read(baseline.value());
        auto projected=csv.export_current_positions(date,source.by_strategy,{{"SYN",102},{"IMM",102},{"FUT",102}},1000000,990,100,{},true,&*view.display);
        if(projected.is_error())return 14;
        nlohmann::json quantities=nlohmann::json::object();
        for(auto& [key,value]:view.display->quantity_exact)quantities[key.strategy_name][key.symbol]=value;
        std::cout<<nlohmann::json{{"baseline_html",html(nullptr)},{"report_html",html(&*view.display)},
            {"baseline_csv",original_csv},{"report_csv",read(projected.value())},{"quantities",quantities},
            {"delivery_guard_loaded",true},{"delivery_calls",0}}.dump()<<'\n';
        return 0;
    }catch(const std::exception&){std::cout<<"REPORT_BLOCKED=1\n";return 11;}
}
