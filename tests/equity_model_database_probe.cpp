// Test-only real adapter. No SQL emitter, discovery, delivery, or MODEL invocation.
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include <cstdlib>
#include <iostream>

using namespace trade_ngin;

int main(int argc,char** argv) {
    try {
        const char* raw=std::getenv("ALGOLENS_TEST_DB");
        if(!raw || argc!=2) return 2;
        const std::string dsn(raw),mode(argv[1]);
        if(dsn.rfind("host=/tmp/algolens-repair-pg-",0)!=0 ||
           dsn.find(" dbname=algolens_test_")==std::string::npos ||
           dsn.find("hostaddr")!=std::string::npos || dsn.find("service=")!=std::string::npos) return 2;
        PostgresDatabase db(dsn);
        if(db.connect().is_error()) return 3;
        nlohmann::json result;
        if(mode=="--history" || mode=="--bad-table" || mode=="--bad-date") {
            const std::string positions=mode=="--bad-table"?"trading.positions;SELECT 1":"trading.positions";
            const std::string executions=mode=="--bad-table"?"trading.positions":"trading.executions";
            const std::string date=mode=="--bad-date"?"2026-02-31":"2026-09-25";
            auto inception=db.get_position_inception_dates("LIVE_TREND","MODEL","BOOK",{"SYN","FUT"},date,positions);
            auto holding=db.get_current_holding_start_dates("LIVE_TREND","MODEL","BOOK",{"SYN"},date,positions);
            auto buy=db.get_last_buy_dates("LIVE_TREND","MODEL","BOOK",{"SYN"},"2026-01-01",date,executions);
            if(mode!="--history") {
                result={{"inception_refused",inception.is_error()},{"holding_refused",holding.is_error()},
                    {"buy_refused",buy.is_error()}};
            } else {
                if(inception.is_error() || holding.is_error() || buy.is_error()) return 10;
                result={{"inception",inception.value()},{"holding",holding.value()},{"buy",buy.value()}};
            }
        } else if(mode=="--store" || mode=="--store-and-wait") {
            auto unit=db.begin_unit_of_work();if(unit.is_error()) return 10;
            Position position;position.symbol="SYN";position.quantity=7;position.average_price=11;
            if(!core::parse_utc_date("2026-09-25",position.last_update)) return 11;
            auto stored=db.store_positions(*unit.value(),{position},"LIVE_TREND","MODEL","BOOK","trading.positions");
            if(stored.is_error()) { std::cout<<"REFUSED\n";return 10; }
            if(mode=="--store-and-wait") {
                std::cout<<"UOW_WRITTEN\n"<<std::flush;
                std::string release;std::getline(std::cin,release);
                if(release!="commit") return 12;
            }
            if(unit.value()->commit().is_error()) return 10;
            result={{"committed",true}};
        } else return 2;
        std::cout<<result.dump()<<'\n';return 0;
    } catch(const std::exception&) { return 11; }
}
