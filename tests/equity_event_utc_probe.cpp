#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include "trade_ngin/data/postgres_database.hpp"
using namespace trade_ngin;
int main(int argc,char** argv) {
    if(argc!=2)return 64;
    const std::string mode=argv[1];
    if(mode!="--utc" && mode!="--new-york")return 64;
    const char* raw=std::getenv("ALGOLENS_TEST_DB");
    const std::string dsn=raw?raw:"";
    if(dsn.rfind("host=/tmp/algolens-repair-pg-",0)!=0 ||
       dsn.find(" dbname=algolens_test_")==std::string::npos ||
       dsn.find("hostaddr")!=std::string::npos || dsn.find("service=")!=std::string::npos)return 65;
    PostgresDatabase database(dsn);if(database.connect().is_error())return 66;
    const std::string timezone=mode=="--utc"?"UTC":"America/New_York";
    if(database.execute_direct_query("SET TIME ZONE '"+timezone+"'").is_error())return 67;
    auto result=database.get_per_bar_corporate_actions({"SYN","ZYN"},"2026-09-25","2026-09-25");
    if(result.is_error())return 68;
    auto rows=nlohmann::json::array();
    for(const auto& event:result.value())rows.push_back({{"date",event.date_str},
        {"symbol",event.ticker},{"action",event.action},{"value",event.value}});
    std::cout<<rows.dump()<<'\n';return 0;
}
