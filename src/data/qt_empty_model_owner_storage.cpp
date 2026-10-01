// STAGED CANDIDATE; no build or PostgreSQL execution evidence yet.
#include "trade_ngin/data/qt_empty_model_owner_storage.hpp"
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
namespace trade_ngin {
namespace {
void need(bool yes){if(!yes)throw std::runtime_error("qt_empty_owner_storage_unsupported");}
}
bool require_qt_empty_owner_storage_capability(pqxx::work& tx){
    // Catalog rendering is search-path-sensitive: a harmless application
    // function named now() makes pg_get_expr render the valid built-in default
    // as pg_catalog.now(). Validate under a fixed catalog path instead.
    tx.exec("SET LOCAL search_path = pg_catalog");
    const auto relations=tx.exec("SELECT to_regclass('trading.qt_empty_model_owner_publications') IS NOT NULL,"
        "to_regclass('trading.qt_storage_capabilities') IS NOT NULL");
    need(relations.size()==1);
    const bool archive_present=relations[0][0].as<bool>(),capabilities_present=relations[0][1].as<bool>();
    if(!archive_present&&!capabilities_present)return false;
    need(capabilities_present);
    const auto availability=tx.exec("SELECT to_regclass('trading.qt_empty_model_owner_publications') IS NOT NULL,"
        "(SELECT count(*) FROM trading.qt_storage_capabilities WHERE capability_name='qt_empty_model_owner_publication_v2'),"
        "(SELECT count(*) FROM trading.qt_storage_capabilities WHERE capability_name='qt_empty_model_owner_publication_v2' AND capability_version=1)");
    need(availability.size()==1);const bool present=availability[0][0].as<bool>();
    const auto count=availability[0][1].as<int>();if(!present&&count==0)return false;
    need(present&&count==1&&availability[0][2].as<int>()==1);
    tx.exec("LOCK TABLE trading.qt_empty_model_owner_publications,trading.qt_model_seed_publications,trading.qt_storage_capabilities IN ROW EXCLUSIVE MODE");
    const auto relation=tx.exec("SELECT relkind='r' AND relpersistence='p' AND NOT relrowsecurity AND NOT relforcerowsecurity AND NOT relispartition AND NOT relhasrules AND NOT EXISTS(SELECT 1 FROM pg_rewrite r WHERE r.ev_class=pg_class.oid) FROM pg_class WHERE oid='trading.qt_empty_model_owner_publications'::regclass");
    need(relation.size()==1&&relation[0][0].as<bool>());
    using Attr=std::tuple<std::string,bool,std::string>;
    const std::map<std::string,Attr> expected={
        {"publication_id",{"uuid",true,""}},{"attempt_id",{"uuid",false,""}},
        {"schema_version",{"text",true,""}},{"portfolio_id",{"text",true,""}},
        {"strategy_id",{"text",true,""}},{"source_day",{"date",true,""}},
        {"publication_version",{"bigint",true,""}},{"registry_id",{"text",true,""}},
        {"registry_revision",{"bigint",true,""}},{"configured_owner_names",{"jsonb",true,""}},
        {"configuration_snapshot",{"jsonb",true,""}},{"configuration_digest",{"text",true,""}},
        {"fresh_empty_batches",{"jsonb",true,""}},{"inspection_capture",{"jsonb",true,""}},
        {"system_components",{"jsonb",true,""}},{"seed_digest",{"text",true,""}},
        {"proposal_components",{"jsonb",true,""}},{"proposal_manifest_digest",{"text",true,""}},
        {"qt_components",{"jsonb",true,""}},{"qt_digest",{"text",true,""}},
        {"producer_version",{"text",true,""}},{"created_at",{"timestamp with time zone",true,"now()"}}};
    const auto columns=tx.exec("SELECT a.attname,format_type(a.atttypid,a.atttypmod),a.attnotnull,COALESCE(pg_get_expr(d.adbin,d.adrelid),''),a.attidentity,a.attgenerated,a.attisdropped FROM pg_attribute a LEFT JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum WHERE a.attrelid='trading.qt_empty_model_owner_publications'::regclass AND a.attnum>0");
    need(columns.size()==expected.size());std::set<std::string> seen;
    for(const auto& column:columns){const auto name=column[0].as<std::string>();
        need(expected.contains(name)&&seen.insert(name).second&&!column[6].as<bool>());
        need(expected.at(name)==Attr{column[1].as<std::string>(),column[2].as<bool>(),column[3].as<std::string>()});
        need(column[4].as<std::string>().empty()&&column[5].as<std::string>().empty());}
    const std::map<std::string,std::string> checks={
        {"schema_version","CHECK((schema_version='qt-empty-model-owner-publication/v2'::text))"},
        {"portfolio_id","CHECK((length(btrim(portfolio_id))>0))"},
        {"strategy_id","CHECK((strategy_id='LIVE_EQUITY_MEAN_REVERSION'::text))"},
        {"publication_version","CHECK((publication_version>0))"},
        {"registry_id","CHECK((length(btrim(registry_id))>0))"},
        {"registry_revision","CHECK((registry_revision>=0))"},
        {"configured_owner_names","CHECK(((jsonb_typeof(configured_owner_names)='array'::text)AND(jsonb_array_length(configured_owner_names)>0)AND(jsonb_array_length(configured_owner_names)<=4096)))"},
        {"configuration_snapshot","CHECK((jsonb_typeof(configuration_snapshot)='object'::text))"},
        {"configuration_digest","CHECK((configuration_digest~'^[0-9a-f]{64}$'::text))"},
        {"fresh_empty_batches","CHECK(((jsonb_typeof(fresh_empty_batches)='array'::text)AND(jsonb_array_length(fresh_empty_batches)>0)AND(jsonb_array_length(fresh_empty_batches)<=4096)))"},
        {"inspection_capture","CHECK((jsonb_typeof(inspection_capture)='object'::text))"},
        {"system_components","CHECK(((jsonb_typeof(system_components)='array'::text)AND(jsonb_array_length(system_components)=0)))"},
        {"seed_digest","CHECK((seed_digest~'^[0-9a-f]{64}$'::text))"},
        {"proposal_components","CHECK(((jsonb_typeof(proposal_components)='array'::text)AND(jsonb_array_length(proposal_components)<=4096)))"},
        {"proposal_manifest_digest","CHECK((proposal_manifest_digest~'^[0-9a-f]{64}$'::text))"},
        {"qt_components","CHECK(((jsonb_typeof(qt_components)='array'::text)AND(jsonb_array_length(qt_components)<=4096)))"},
        {"qt_digest","CHECK((qt_digest~'^[0-9a-f]{64}$'::text))"},
        {"producer_version","CHECK((length(btrim(producer_version))>0))"}};
    const auto constraints=tx.exec("SELECT conname,regexp_replace(pg_get_constraintdef(oid),'\\s','','g'),convalidated,condeferrable,condeferred,coninhcount,conparentid FROM pg_constraint WHERE conrelid='trading.qt_empty_model_owner_publications'::regclass AND contype='c'");
    need(constraints.size()==checks.size());seen.clear();for(const auto& c:constraints){
        const auto name=c[0].as<std::string>();std::string column;
        for(const auto& [field,definition]:checks)if(name==(field=="proposal_manifest_digest"?"qt_empty_owner_proposal_manifest_digest_check":"qt_empty_model_owner_publications_"+field+"_check"))column=field;
        need(!column.empty()&&seen.insert(column).second&&c[1].as<std::string>()==checks.at(column));
        need(c[2].as<bool>()&&!c[3].as<bool>()&&!c[4].as<bool>()&&c[5].as<int>()==0&&c[6].as<long long>()==0);}
    const auto keys=tx.exec("SELECT c.contype,pg_get_constraintdef(c.oid),c.convalidated,c.condeferrable,c.condeferred,i.indisvalid,i.indisready,i.indislive,i.indpred IS NULL,i.indexprs IS NULL,i.indnatts=i.indnkeyatts,c.coninhcount,c.conparentid,i.indisunique,i.indisprimary FROM pg_constraint c LEFT JOIN pg_index i ON i.indexrelid=c.conindid WHERE c.conrelid='trading.qt_empty_model_owner_publications'::regclass AND c.contype<>'c'");
    std::set<std::string> definitions={"PRIMARY KEY (publication_id)","UNIQUE (attempt_id)","UNIQUE (portfolio_id, source_day, publication_version)","UNIQUE (portfolio_id, strategy_id, source_day, attempt_id)"};
    need(keys.size()==definitions.size());for(const auto& k:keys){need(definitions.erase(k[1].as<std::string>())==1);
        need(k[2].as<bool>()&&!k[3].as<bool>()&&!k[4].as<bool>()&&k[5].as<bool>()&&k[6].as<bool>()&&k[7].as<bool>()&&k[8].as<bool>()&&k[9].as<bool>()&&k[10].as<bool>()&&k[11].as<int>()==0&&k[12].as<long long>()==0&&k[13].as<bool>()&&k[14].as<bool>()==(k[0].as<std::string>()=="p"));}
    const auto guard=tx.exec("SELECT f.prorettype='trigger'::regtype AND f.pronargs=0 AND f.prokind='f' AND f.provolatile='v' AND NOT f.prosecdef AND NOT f.proisstrict AND f.proconfig=ARRAY['search_path=pg_catalog']::text[] AND l.lanname='plpgsql' AND md5(f.prosrc)='3f534a777759e6917a067afbf7e85a80' FROM pg_proc f JOIN pg_language l ON l.oid=f.prolang WHERE f.oid=to_regprocedure('trading.guard_qt_model_publication_identity()')");
    need(guard.size()==1&&guard[0][0].as<bool>());
    const auto immutable=tx.exec("SELECT f.prorettype='trigger'::regtype AND f.pronargs=0 AND f.prokind='f' AND f.provolatile='v' AND NOT f.prosecdef AND NOT f.proisstrict AND f.proconfig IS NULL AND l.lanname='plpgsql' AND md5(f.prosrc)='e2ecd4f309bfd52dd1ef3506b9d3b0e3' FROM pg_proc f JOIN pg_language l ON l.oid=f.prolang WHERE f.oid=to_regprocedure('trading.refuse_qt_seed_mutation()')");
    need(immutable.size()==1&&immutable[0][0].as<bool>());
    const auto triggers=tx.exec("SELECT CASE WHEN tgrelid='trading.qt_model_seed_publications'::regclass THEN 'trading.qt_model_seed_publications' ELSE 'trading.qt_empty_model_owner_publications' END,tgname,tgtype,tgfoid=to_regprocedure('trading.guard_qt_model_publication_identity()'),tgfoid=to_regprocedure('trading.refuse_qt_seed_mutation()'),tgenabled='O' AND NOT tgisinternal AND tgqual IS NULL AND tgattr::text='' AND tgnargs=0 AND octet_length(tgargs)=0 AND tgconstraint=0 AND NOT tgdeferrable AND NOT tginitdeferred AND tgparentid=0 AND tgoldtable IS NULL AND tgnewtable IS NULL FROM pg_trigger WHERE tgrelid IN ('trading.qt_empty_model_owner_publications'::regclass,'trading.qt_model_seed_publications'::regclass) AND NOT tgisinternal");
    need(triggers.size()==6);std::set<std::tuple<std::string,std::string,int>> trigger_names;
    for(const auto& t:triggers){const auto relation_name=t[0].as<std::string>(),name=t[1].as<std::string>();const int type=t[2].as<int>();need(t[5].as<bool>());
        if(type==7)need(t[3].as<bool>()&&name==(relation_name=="trading.qt_model_seed_publications"?"qt_model_seed_identity_guard":"qt_empty_model_owner_identity_guard"));
        else need(t[4].as<bool>()&&((type==27&&name==(relation_name=="trading.qt_model_seed_publications"?"qt_model_seed_immutable":"qt_empty_model_owner_immutable"))||(type==34&&name==(relation_name=="trading.qt_model_seed_publications"?"qt_model_seed_no_truncate":"qt_empty_model_owner_no_truncate"))));
        need(trigger_names.insert({relation_name,name,type}).second);}
    return true;
}
} // namespace trade_ngin
