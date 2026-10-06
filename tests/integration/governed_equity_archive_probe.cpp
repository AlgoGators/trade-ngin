// Read-only proof against the owned actual-run fixture, never fabricated evidence.
#include <cstdlib>
#include <iostream>
#include <pqxx/pqxx>
#include "trade_ngin/data/qt_model_publication.hpp"
int main(int argc,char** argv) {
    if(argc!=2)return 64;
    const char* raw=std::getenv("TRADE_NGIN_121124_TEST_DSN");
    const std::string dsn=raw?raw:"";
    if(!dsn.starts_with("host=/tmp/algolens-repair-pg-") ||
       dsn.find(" dbname=trade_ngin_121124_p2_test ")==std::string::npos ||
       dsn.find("hostaddr")!=std::string::npos || dsn.find("service=")!=std::string::npos)return 65;
    try {
        pqxx::connection conn(dsn);pqxx::work tx(conn);
        auto result=trade_ngin::load_qt_model_publication_record(tx,argv[1]);
        if(result.is_error()){std::cerr<<result.error()->what()<<'\n';return 2;}
        std::cout<<"GOVERNED_EQUITY_ARCHIVE_VERIFIED=1\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 3;}
}
