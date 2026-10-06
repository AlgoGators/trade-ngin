// Actual governed read-only prior admission, owned socket fixture only.
#include "trade_ngin/apps/equity_model_prior.hpp"
#include <cstdlib>
#include <iostream>
int main(int argc,char** argv){try{
    if(argc!=6)return 64;
    const char* raw=std::getenv("ALGOLENS_TEST_DB");if(!raw)return 65;
    const std::string dsn(raw);
    if(dsn.rfind("host=/tmp/algolens-repair-pg-",0)!=0||dsn.find(" dbname=algolens_test_")==std::string::npos||
        dsn.find("hostaddr")!=std::string::npos||dsn.find("service=")!=std::string::npos)return 65;
    auto selected=trade_ngin::parse_equity_model_prior_arguments(
        {"--verified-desk-prior","--prior-decision",argv[1],"--prior-finalization",argv[2]});
    if(selected.is_error())return 64;
    pqxx::connection connection(dsn);pqxx::work tx(connection);
    const trade_ngin::EquityModelPriorOwner owner{argv[3],"LIVE_EQUITY_MEAN_REVERSION",
        "EQUITY_MEAN_REVERSION",argv[4],argv[5]};
    auto result=trade_ngin::load_verified_equity_model_prior(tx,selected.value(),owner);
    if(result.is_error()){std::cout<<"EQUITY_MODEL_PRIOR_REFUSED=1\n";return 10;}
    nlohmann::json positions=nlohmann::json::object();
    for(const auto& [symbol,row]:result.value().positions)positions[symbol]={
        {"quantity_exact",row.quantity.to_string()},{"average_price_exact",row.average_price.to_string()}};
    std::cout<<nlohmann::json{{"positions",positions},{"reference",result.value().replay_reference},
        {"financial",result.value().financial},{"basis_positions",result.value().basis_positions}}.dump()<<'\n';
    // No commit, write or authority/release operation exists on this path.
    return 0;
}catch(const std::exception&){return 11;}}
