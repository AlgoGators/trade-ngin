// Owned DB, actual new EQ methods only. No strategy/runner/email invocation.
#include <cstdlib>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"
using namespace trade_ngin;
int main(int argc,char** argv) {
    if(argc!=3)return 64;
    const std::string mode=argv[1],timezone=argv[2];
    if((mode!="closes" && mode!="equity" && mode!="equity-valid") ||
       (timezone!="UTC" && timezone!="America/New_York"))return 64;
    const char* raw=std::getenv("ALGOLENS_TEST_DB");
    const std::string dsn=raw?raw:"";
    if(dsn.rfind("host=/tmp/algolens-repair-pg-",0)!=0 ||
       dsn.find(" dbname=algolens_test_")==std::string::npos ||
       dsn.find("hostaddr")!=std::string::npos || dsn.find("service=")!=std::string::npos)return 65;
    auto database=std::make_shared<PostgresDatabase>(dsn);
    if(database->connect().is_error())return 66;
    if(database->execute_direct_query("SET TIME ZONE '"+timezone+"'").is_error())return 67;
    if(mode=="closes") {
        auto result=database->get_historical_closes({"SYN","ZYN"},"2026-09-25","2026-09-25");
        if(result.is_error())return 68;
        std::cout<<nlohmann::json(result.value()).dump()<<'\n';
    } else {
        LiveResultsManager manager(database,true,"LIVE_EQUITY_MEAN_REVERSION",
            "EQUITY_MR_PORTFOLIO","system","EQUITY_MEAN_REVERSION");
        manager.set_equity(mode=="equity-valid"?12000:0);
        if(manager.save_equity_curve(std::chrono::system_clock::from_time_t(1790294400)).is_error())return 69;
        std::cout<<"EQ_CURVE_SAVED=1\n";
    }
    return 0;
}
