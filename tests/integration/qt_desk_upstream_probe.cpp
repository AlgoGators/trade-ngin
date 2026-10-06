// Disposable owned socket only; no production discovery or delivery paths.
#include "trade_ngin/data/qt_desk_upstream.hpp"
#include "trade_ngin/data/qt_desk_market_capture.hpp"
#include "trade_ngin/data/qt_desk_processor.hpp"
#include "trade_ngin/apps/qt_desk_cycle.hpp"
#include <cstdlib>
#include <iostream>
int main(int argc,char** argv){
 try{
  if(argc==2&&std::string(argv[1])=="--original"){
   nlohmann::json j;std::cin>>j;auto r=trade_ngin::produce_qt_futures_accounting(j.at("decision"),j.at("selection"),j.at("input"));if(r.is_error())return 10;std::cout<<r.value().dump()<<'\n';return 0;
  }
  const char* raw=std::getenv("ALGOLENS_TEST_DB");if(!raw)return 2;std::string dsn(raw);
  if(dsn.rfind("host=/tmp/algolens-repair-pg-",0)!=0||dsn.find(" dbname=algolens_test_")==std::string::npos||dsn.find("hostaddr")!=std::string::npos||dsn.find("service=")!=std::string::npos)return 2;
  pqxx::connection c(dsn);
  if(argc==4&&std::string(argv[1])=="--anchor"){auto r=trade_ngin::create_qt_first_day_anchor(c,argv[2],argv[3]);if(r.is_error()){std::cout<<r.error()->what()<<'\n';return 10;}std::cout<<r.value().dump()<<'\n';return 0;}
  if(argc==7&&std::string(argv[1])=="--first-day"){auto r=trade_ngin::process_qt_desk_first_day_decision(c,argv[2],argv[3],argv[4],argv[5],argv[6]);if(r.is_error()){std::cout<<r.error()->what()<<'\n';return 10;}std::cout<<"PROCESSED=1 REPLAYED="<<r.value().replayed<<'\n';return 0;}
  if(argc==2&&std::string(argv[1])=="--capture"){nlohmann::json j;std::cin>>j;auto r=trade_ngin::capture_qt_desk_market_source(c,j);if(r.is_error()){std::cout<<r.error()->what()<<'\n';return 10;}std::cout<<r.value().dump()<<'\n';return 0;}
  if(argc==7&&std::string(argv[1])=="--sourced"){auto r=trade_ngin::process_qt_desk_sourced_decision(c,argv[2],argv[3],argv[4],argv[5],argv[6]);if(r.is_error()){std::cout<<r.error()->what()<<'\n';return 10;}std::cout<<"PROCESSED=1 REPLAYED="<<r.value().replayed<<'\n';return 0;}
  if(argc==2&&std::string(argv[1])=="--market"){nlohmann::json j;std::cin>>j;auto r=trade_ngin::publish_qt_desk_market_source(c,j);if(r.is_error())return 10;std::cout<<r.value().dump()<<'\n';return 0;}
  if(argc==5&&std::string(argv[1])=="--finalize"){auto r=trade_ngin::finalize_qt_desk_accounting(c,argv[2],argv[3],argv[4]);if(r.is_error()){std::cout<<r.error()->what()<<'\n';return 10;}std::cout<<r.value().dump()<<'\n';return 0;}
  return 2;
 }catch(const std::exception&){return 11;}
}
