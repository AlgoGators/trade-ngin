// Test-only real imported adjustment/registration. Owned Unix socket only.
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
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
        nlohmann::json result;
        if(mode=="--empty-metadata" || mode=="--empty-registry") {
            auto db=std::make_shared<PostgresDatabase>(dsn);
            if(db->connect().is_error()) return 3;
            if(mode=="--empty-registry") {
                auto& registry=InstrumentRegistry::instance();
                if(registry.initialize(db).is_error() || registry.load_instruments().is_error()) return 10;
                result={{"instruments",registry.get_all_instruments().size()}};
            } else {
                auto queried=db->get_contract_metadata();
                if(queried.is_error() || !queried.value()) return 10;
                const auto& table=queried.value();
                if(!table->ValidateFull().ok()) return 10;
                result={{"rows",table->num_rows()},{"fields",nlohmann::json::array()}};
                for(int i=0;i<table->num_columns();++i) {
                    if(!table->column(i) || table->column(i)->length()!=0) return 10;
                    result["fields"].push_back({{"name",table->field(i)->name()},
                        {"type",table->field(i)->type()->ToString()}});
                }
            }
            std::cout<<result.dump()<<'\n';return 0;
        }
        if(mode=="--registry") {
            auto db=std::make_shared<PostgresDatabase>(dsn);
            if(db->connect().is_error()) return 3;
            auto& registry=InstrumentRegistry::instance();
            if(registry.initialize(db).is_error() || registry.load_instruments().is_error()) return 10;
            const auto future=registry.get_futures_instrument("ES");if(!future) return 10;
            if(registry.load_equity_instruments({"ES","SYN"}).is_error()) return 10;
            const auto equity=registry.get_equity_instrument("ES");if(!equity) return 10;
            result={{"future_preserved",registry.get_futures_instrument("ES")==future},
                {"variant_is_future",registry.get_instrument("ES.v.0")==future},
                {"generic_is_equity",registry.get_instrument("ES")==equity},
                {"future_multiplier",future->get_multiplier()},
                {"equity_point_value",equity->get_point_value()},
                {"equity_commission",equity->get_spec().commission_per_share},
                {"equity_exchange",equity->get_exchange()}};
            const auto count=registry.get_all_instruments().size();
            auto invalid=registry.load_equity_instruments({"ADDED","invalid;symbol"});
            result["invalid_refused_without_partial_registration"]=invalid.is_error() &&
                registry.get_all_instruments().size()==count;
            std::cout<<result.dump()<<'\n';return 0;
        }
        if(mode!="--equity" && mode!="--futures" && mode!="--equity-all") return 2;
        Timestamp start,end;
        if(!core::parse_utc_date("2026-09-23",start) || !core::parse_utc_date("2026-09-26",end)) return 2;
        pqxx::connection connection(dsn);pqxx::work transaction(connection);
        auto snapshot=PostgresDatabase::read_market_data_snapshot(transaction,
            mode=="--equity-all"?std::vector<std::string>{}:std::vector<std::string>{"SYN"},start,end,
            mode=="--futures"?AssetClass::FUTURES:AssetClass::EQUITIES,DataFrequency::DAILY,"ohlcv");
        if(snapshot.is_error()) return 10;
        result=nlohmann::json::array();
        for(const auto& row:snapshot.value().rows)
            result.push_back({{"symbol",row["symbol"].as<std::string>()},
                {"time",row["time"].as<std::string>()},{"close",row["close"].as<double>()},
                {"high",row["high"].as<double>()},{"volume",row["volume"].as<double>()}});
        transaction.commit();std::cout<<result.dump()<<'\n';return 0;
    } catch(const std::exception&) { return 11; }
}
