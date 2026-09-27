// Test-only actual immutable native loader, not MODEL or a financial runner.
#include "trade_ngin/data/qt_model_publication.hpp"
#include <cstdlib>
#include <dlfcn.h>
#include <iostream>
int main(int argc,char** argv){try{
 auto marker=reinterpret_cast<int(*)()>(dlsym(RTLD_DEFAULT,"qt_no_delivery_guard_loaded"));
 if(!marker||marker()!=1||argc!=3)return 63;
 const std::string mode=argv[1],value=argv[2];
 if(mode!="record"&&mode!="scope")return 64;
 const char* raw=std::getenv("ALGOLENS_TEST_DB");const std::string dsn=raw?raw:"";
 if(dsn.rfind("host=/tmp/algolens-repair-pg-",0)!=0||dsn.find(" dbname=algolens_test_")==std::string::npos||dsn.find("hostaddr")!=std::string::npos||dsn.find("service=")!=std::string::npos)return 65;
 pqxx::connection c(dsn);pqxx::work tx(c);
 if(mode=="scope"){
  auto scope=trade_ngin::load_qt_model_publication_scope(tx,"EQ_BOOK",value);
  if(scope.is_error()){std::cout<<"EMPTY_ARCHIVE_REFUSED=1\n";return 0;}
  std::cout<<"EMPTY_ARCHIVE="<<scope.value().dump()<<'\n';return 0;
 }
 auto record=trade_ngin::load_qt_model_publication_record(tx,value);
 if(record.is_error()){std::cout<<"EMPTY_ARCHIVE_REFUSED=1\n";return 0;}
 nlohmann::json out{{"kind",record.value().kind==trade_ngin::QtModelPublicationKind::EmptyOwnerV2?"empty_owner_v2":"legacy_v1"},{"reference",record.value().reference}};
 std::cout<<"EMPTY_ARCHIVE="<<out.dump()<<'\n';return 0;
}catch(const std::exception&){std::cerr<<"EMPTY_LOADER_SETUP_FAILURE\n";return 77;}}
