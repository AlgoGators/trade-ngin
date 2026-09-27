// Test-only real processor/finalizer probe. Owned route-less socket only.
#include "trade_ngin/data/qt_desk_upstream.hpp"
#include "trade_ngin/data/qt_desk_processor.hpp"
#include "trade_ngin/data/qt_desk_accounting.hpp"
#include <cstdlib>
#include <iostream>
int main(int argc,char** argv) {
    try {
        const char* raw=std::getenv("ALGOLENS_TEST_DB");if(!raw)return 2;
        std::string dsn(raw);
        if(dsn.rfind("host=/tmp/algolens-repair-pg-",0)!=0 ||
           dsn.find(" dbname=algolens_test_")==std::string::npos ||
           dsn.find("hostaddr")!=std::string::npos ||
           dsn.find("service=")!=std::string::npos)return 2;
        pqxx::connection c(dsn);
        if(argc==4&&std::string(argv[1])=="--verify-original") {
            pqxx::work tx(c);
            auto rows=tx.exec("SELECT to_jsonb(d) FROM trading.qt_decisions d WHERE decision_id="+tx.quote(argv[2])+"::uuid");
            if(rows.size()!=1||rows[0][0].is_null())return 10;
            auto decision=nlohmann::json::parse(rows[0][0].c_str());
            auto r=trade_ngin::verify_qt_desk_accounting_outputs(tx,decision,argv[3]);
            if(r.is_error()){std::cout<<"EQUITY_ORIGINAL_REFUSED=1\n";return 10;}
            std::cout<<"EQUITY_ORIGINAL_VERIFIED=1\n";return 0;
        }
        if(argc==5&&std::string(argv[1])=="--finalize") {
            auto r=trade_ngin::finalize_qt_desk_accounting(c,argv[2],argv[3],argv[4]);
            if(r.is_error()){std::cout<<"EQUITY_FINALIZATION_REFUSED=1\n";return 10;}
            std::cout<<r.value().dump()<<'\n';return 0;
        }
        if(argc==7&&std::string(argv[1])=="--sourced") {
            auto r=trade_ngin::process_qt_desk_sourced_decision(c,argv[2],argv[3],argv[4],argv[5],argv[6]);
            if(r.is_error()){std::cout<<"EQUITY_SOURCED_REFUSED=1\n";return 10;}
            std::cout<<"PROCESSED=1 REPLAYED="<<r.value().replayed<<'\n';return 0;
        }
        return 2;
    } catch(const std::exception&) {return 11;}
}
