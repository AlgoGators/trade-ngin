#include "qt_desk_csv.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <utility>
#include <vector>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <sys/random.h>
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/live/csv_exporter.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/core/qt_sha256.hpp"

namespace trade_ngin::qt_desk_cli {
QtDeskCsvOutput::QtDeskCsvOutput(int fd,std::string parent,std::string name,dev_t device,ino_t inode)
    :parent_fd_(fd),parent_path_(std::move(parent)),name_(std::move(name)),device_(device),inode_(inode) {}
QtDeskCsvOutput::~QtDeskCsvOutput(){if(parent_fd_>=0)::close(parent_fd_);}
Result<std::unique_ptr<QtDeskCsvOutput>> QtDeskCsvOutput::admit(const std::string& path){
    const auto fail=[] {return make_error<std::unique_ptr<QtDeskCsvOutput>>(ErrorCode::INVALID_ARGUMENT,
        "desk_csv_output_refused","qt_desk_csv");};
    if(path.empty()||path.size()>4096||path[0]!='/'||path.back()=='/'||
       path.find('\0')!=std::string::npos||path.find("//")!=std::string::npos)return fail();
    std::vector<std::string> parts;
    size_t begin=1;
    while(begin<path.size()){
        const auto end=path.find('/',begin);
        auto part=path.substr(begin,end==std::string::npos?path.size()-begin:end-begin);
        if(part.empty()||part=="."||part=="..")return fail();
        parts.push_back(std::move(part));
        if(end==std::string::npos)break;
        begin=end+1;
    }
    if(parts.empty())return fail();
    const auto name=parts.back();parts.pop_back();
    if(name.size()>128||!name.ends_with(".csv")||!std::all_of(name.begin(),name.end(),[](char c){
        return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-';}))return fail();
    int fd=::open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return fail();
    for(const auto& part:parts){
        const int next=::openat(fd,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        ::close(fd);fd=next;
        if(fd<0)return fail();
    }
    struct stat parent{},existing{};
    if(::fstat(fd,&parent)!=0||!S_ISDIR(parent.st_mode)){
        ::close(fd);return fail();
    }
    errno=0;
    if(::fstatat(fd,name.c_str(),&existing,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT){
        ::close(fd);return fail();
    }
    auto slash=path.rfind('/');auto directory=slash==0?std::string("/"):path.substr(0,slash);
    return std::unique_ptr<QtDeskCsvOutput>(new QtDeskCsvOutput(fd,directory,name,parent.st_dev,parent.st_ino));
}
namespace {
using J=nlohmann::json;
constexpr size_t byte_limit=8*1024*1024;
void need(bool ok){if(!ok)throw std::invalid_argument("desk_csv_refused");}
std::string text(const J& value){need(value.is_string());auto s=value.get<std::string>();
    need(!s.empty()&&s.size()<=4096&&s.find('\0')==std::string::npos);return s;}
J one(pqxx::work& tx,const std::string& query){auto rows=tx.exec(query);
    need(rows.size()==1&&!rows[0][0].is_null());auto s=rows[0][0].as<std::string>();
    need(s.size()<=byte_limit);return J::parse(s);}
std::string hash(const J& value,bool financial=false){auto bytes=financial?canonical_qt_desk_source_json(value):canonical_qt_desk_input_json(value);need(bytes.is_ok());
    auto result=qt_sha256_hex(bytes.value());need(result.is_ok());return result.value();}
Decimal exact(const J& value){auto result=parse_qt_quantity_exact(text(value));need(result.is_ok());return result.value();}
double positive(const J& value){auto s=text(value);need(s.size()<=64);double number=0;
    auto parsed=std::from_chars(s.data(),s.data()+s.size(),number,std::chars_format::general);
    need(parsed.ec==std::errc{}&&parsed.ptr==s.data()+s.size()&&std::isfinite(number)&&number>0);return number;}
std::string key_id(const J& key){need(key.is_object()&&key.size()==6);
    for(auto name:{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"})text(key.at(name));
    return key.dump();}
// Catalog stream identity is the admitted proposal scope; rendering retains QT keys.
std::string catalog_id(J key){key["portfolio_type"]="qt_proposal";return key_id(key);}
AssetType asset(const J& value){auto name=text(value);
    need(name=="EQUITY"||name=="FUTURE");return name=="EQUITY"?AssetType::EQUITY:AssetType::FUTURE;}
Timestamp midnight(const std::string& day){need(day.size()==10&&day[4]=='-'&&day[7]=='-');
    for(size_t i=0;i<day.size();++i)if(i!=4&&i!=7)need(day[i]>='0'&&day[i]<='9');
    using namespace std::chrono;
    year_month_day date{year{std::stoi(day.substr(0,4))},month{static_cast<unsigned>(std::stoi(day.substr(5,2)))},
                       std::chrono::day{static_cast<unsigned>(std::stoi(day.substr(8,2)))}};
    need(date.ok());return sys_days{date};}
struct File {
    int fd=-1;explicit File(int descriptor):fd(descriptor){}~File(){if(fd>=0)::close(fd);}
    File(const File&)=delete;File& operator=(const File&)=delete;
};
int open_directory(const std::string& path){
    need(!path.empty()&&path[0]=='/');File current(::open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    need(current.fd>=0);size_t begin=1;
    while(begin<path.size()){
        auto end=path.find('/',begin);auto part=path.substr(begin,end==std::string::npos?path.size()-begin:end-begin);
        need(!part.empty()&&part!="."&&part!="..");
        const int next=::openat(current.fd,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);need(next>=0);
        ::close(current.fd);current.fd=next;if(end==std::string::npos)break;begin=end+1;
    }
    auto fd=current.fd;current.fd=-1;return fd;
}
struct Scratch {
    int parent;std::string name;int fd=-1;dev_t device=0;ino_t inode=0;
    std::vector<std::string> files;
    explicit Scratch(int parent_fd):parent(parent_fd){
        for(int attempt=0;attempt<32;++attempt){
            std::array<unsigned char,16> bytes{};size_t got=0;
            while(got<bytes.size()){auto n=::getrandom(bytes.data()+got,bytes.size()-got,0);
                if(n<0&&errno==EINTR)continue;need(n>0);got+=static_cast<size_t>(n);}
            name=".qt-desk-csv-";constexpr char hex[]="0123456789abcdef";
            for(auto b:bytes){name+=hex[b>>4];name+=hex[b&15];}
            if(::mkdirat(parent,name.c_str(),0700)==0)break;
            need(errno==EEXIST);name.clear();
        }
        need(!name.empty());fd=::openat(parent,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0){::unlinkat(parent,name.c_str(),AT_REMOVEDIR);throw std::invalid_argument("desk_csv_refused");}
        struct stat st{};if(::fstat(fd,&st)!=0||!S_ISDIR(st.st_mode)){
            ::close(fd);fd=-1;::unlinkat(parent,name.c_str(),AT_REMOVEDIR);throw std::invalid_argument("desk_csv_refused");}
        device=st.st_dev;inode=st.st_ino;
    }
    ~Scratch(){if(fd>=0){
        // Only these known files in the directory exclusively created by this call.
        for(const auto& file:files)::unlinkat(fd,file.c_str(),0);
        struct stat now{};if(::fstatat(parent,name.c_str(),&now,AT_SYMLINK_NOFOLLOW)==0&&
            S_ISDIR(now.st_mode)&&now.st_dev==device&&now.st_ino==inode)::unlinkat(parent,name.c_str(),AT_REMOVEDIR);
        ::close(fd);
    }}
};
std::string bounded_read(int fd){struct stat before{};need(::fstat(fd,&before)==0&&S_ISREG(before.st_mode)&&
    before.st_nlink==1&&before.st_size>0&&static_cast<uint64_t>(before.st_size)<=byte_limit);
    std::string bytes(static_cast<size_t>(before.st_size),'\0');size_t done=0;
    while(done<bytes.size()){auto n=::read(fd,bytes.data()+done,bytes.size()-done);
        if(n<0&&errno==EINTR)continue;need(n>0);done+=static_cast<size_t>(n);}
    char extra=0;need(::read(fd,&extra,1)==0);struct stat after{};
    need(::fstat(fd,&after)==0&&after.st_dev==before.st_dev&&after.st_ino==before.st_ino&&
         after.st_size==before.st_size&&after.st_mtim.tv_sec==before.st_mtim.tv_sec&&after.st_mtim.tv_nsec==before.st_mtim.tv_nsec);
    return bytes;
}
void write_all(int fd,const std::string& bytes){size_t done=0;
    while(done<bytes.size()){auto n=::write(fd,bytes.data()+done,bytes.size()-done);
        if(n<0&&errno==EINTR)continue;need(n>0);done+=static_cast<size_t>(n);}
    need(::fsync(fd)==0);
}
// If publication or transaction closure fails, remove only our proved inode.
// A racing replacement is never unlinked.
struct PublishedFile {
    int parent;std::string name;dev_t device;ino_t inode;bool armed=false;
    ~PublishedFile(){if(armed){struct stat now{};
        if(::fstatat(parent,name.c_str(),&now,AT_SYMLINK_NOFOLLOW)==0&&S_ISREG(now.st_mode)&&
           now.st_dev==device&&now.st_ino==inode)::unlinkat(parent,name.c_str(),0);
    }}
};
struct Snapshot {
    StrategyPositionsMap rows;std::unordered_map<std::string,double> prices;
    DeskCsvInstruments instruments;double equity=0,gross=0,net=0;J manifest;
};
Snapshot snapshot(pqxx::work& tx,const J& d,const J& preview,const J& receipt,const std::string& input_id){
    const auto book=text(d.at("book_id")),day=text(d.at("source_day"));
    const auto input=one(tx,"SELECT to_jsonb(i) FROM trading.qt_desk_accounting_inputs i WHERE input_id="+tx.quote(input_id)+"::uuid");
    const auto result=one(tx,"SELECT to_jsonb(r) FROM trading.desk_run_results r WHERE decision_id="+tx.quote(text(d.at("decision_id")))+"::uuid");
    need(input.at("decision_id")==d.at("decision_id")&&result.at("input_id")==input_id&&
         result.at("portfolio_id")==book&&result.at("date")==day);
    const auto& in=input.at("payload");const auto& financial=result.at("payload");
    need(hash(in)==text(input.at("content_digest"))&&hash(financial,true)==text(result.at("content_digest"))&&
         financial.at("input_digest")==input.at("content_digest"));
    const auto schema=text(in.at("schema_version"));
    const bool equity=schema=="qt-equity-accounting-input/v1";
    need(equity||schema=="qt-futures-accounting-input/v1"||schema=="qt-futures-accounting-input/v2");
    need(in.at("currency")=="USD"&&in.at("decision_id")==d.at("decision_id")&&
         in.at("book_id")==book&&in.at("source_day")==day);
    const auto snapshot_id=preview.at("read_set_payload").at("risk_limits").at("id");
    need(snapshot_id.is_number_integer()&&snapshot_id.get<int64_t>()>0);
    const auto captured=one(tx,"SELECT to_jsonb(s) FROM trading.qt_evaluation_snapshots s WHERE snapshot_id="+tx.quote(snapshot_id.get<int64_t>()));
    const auto& source=captured.at("payload");
    need(captured.at("book_id")==book&&captured.at("source_day")==day&&
         captured.at("model_publication_id")==d.at("model_publication_id")&&hash(source)==text(captured.at("content_digest"))&&
         captured.at("content_digest")==preview.at("read_set_payload").at("risk_limits").at("content_digest"));
    std::map<std::string,AssetType> catalog;
    for(const auto& row:source.at("instrument_catalog"))need(catalog.emplace(catalog_id(row.at("key")),asset(row.at("instrument_type"))).second);
    std::map<std::string,DeskCsvInstrument> valuations;
    for(const auto& value:source.at("engine_inputs").at("risk_inputs").at("valuations")){
        need(value.at("quote_currency")=="USD");const auto& instrument=value.at("instrument");
        const auto symbol=text(instrument.at("symbol"));auto type=asset(instrument.at("instrument_type"));
        const double multiplier=positive(value.at("price_multiplier"));
        need((type!=AssetType::EQUITY||multiplier==1)&&valuations.emplace(symbol,DeskCsvInstrument{type,multiplier}).second);
    }
    std::map<std::string,J> prices;
    for(const auto& row:in.at("instruments"))need(prices.emplace(text(row.at("symbol")),row).second);
    const auto& after=receipt.at("publication_payload").at("after_accounting");
    need(after.is_array()&&after.size()<=4096&&after.size()==catalog.size());
    std::map<std::string,J> sorted;std::set<std::string> symbols,engines;
    for(const auto& row:after)need(sorted.emplace(key_id(row.at("key")),row).second);
    Snapshot out;J manifest_rows=J::array(),price_sources=J::array();
    std::map<std::pair<std::string,std::string>,std::string> owners;
    for(const auto& [id,row]:sorted){
        const auto& key=row.at("key");need(key.at("portfolio_id")==book&&key.at("date")==day&&key.at("portfolio_type")=="qt");
        const auto symbol=text(key.at("symbol")),engine=text(key.at("strategy_id")),name=text(key.at("strategy_name"));
        const auto catalog_key=catalog_id(key);need(catalog.contains(catalog_key)&&prices.contains(symbol)&&valuations.contains(symbol));
        const auto& v=valuations.at(symbol);need(v.asset_type==catalog.at(catalog_key)&&
            (equity?v.asset_type==AssetType::EQUITY:v.asset_type==AssetType::FUTURE));
        const auto quantity=exact(row.at("quantity_exact")),basis=exact(row.at("average_price_exact"));
        need((v.asset_type==AssetType::FUTURE ? basis.is_positive() :
              basis.raw_value()>=0&&(quantity.raw_value()==0||basis.is_positive()))&&
             (v.asset_type!=AssetType::FUTURE||quantity.raw_value()%100000000LL==0));
        const auto& price_row=prices.at(symbol);J provenance;
        double price=0;
        if(equity){const auto& mark=price_row.at("mark");need(mark.at("date")==in.at("previous_day"));
            price=positive(mark.at("price_model_number"));
            provenance={{"symbol",symbol},{"asset_type","EQUITY"},{"price_model_number",mark.at("price_model_number")},
                {"price_day",mark.at("date")},{"source_id",mark.at("source_id")},{"source_digest",mark.at("source_digest")},
                {"price_frame_id",mark.at("price_frame_id")},{"multiplier_model_number","1"}};
        }else{const auto field=schema=="qt-futures-accounting-input/v2"?"price_model_number":"price_exact";
            price=positive(price_row.at(field));
            provenance={{"symbol",symbol},{"asset_type","FUTURE"},{"price_model_number",price_row.at(field)},
                {"price_day",in.at("previous_day")},{"source_id",price_row.at("source_id")},
                {"multiplier_model_number",source.at("engine_inputs").at("risk_inputs").at("valuations").at(0).at("price_multiplier")}};
            // Use this symbol's captured multiplier, never a transaction-cost point value.
            for(const auto& value:source.at("engine_inputs").at("risk_inputs").at("valuations"))
                if(value.at("instrument").at("symbol")==symbol)provenance["multiplier_model_number"]=value.at("price_multiplier");
        }
        if(symbols.insert(symbol).second){out.prices.emplace(symbol,price);out.instruments.emplace(symbol,v);price_sources.push_back(provenance);}
        engines.insert(engine);auto owner_key=std::make_pair(engine,name);auto label=owners.find(owner_key);
        if(label==owners.end()){
            std::ostringstream suffix;suffix<<std::setfill('0')<<std::setw(6)<<owners.size()+1;
            label=owners.emplace(owner_key,"OWNER_"+suffix.str()).first;
        }
        need(out.rows[label->second].emplace(symbol,Position(symbol,quantity,basis,Decimal(0),Decimal(0),midnight(day))).second);
        const auto notional=quantity.as_double()*price*v.multiplier;need(std::isfinite(notional));
        out.gross+=std::abs(notional);out.net+=notional;
        manifest_rows.push_back({{"key",key},{"quantity_exact",quantity.to_string()},
            {"csv_strategy","Owner "+label->second.substr(6)}});
    }
    need(prices.size()==symbols.size()&&valuations.size()==symbols.size());
    std::set<std::string> actual_engines;
    for(const auto& row:financial.at("live_results")){
        need(row.at("portfolio_id")==book&&row.at("date")==day&&row.at("portfolio_type")=="qt");
        need(actual_engines.insert(text(row.at("strategy_id"))).second);
        const auto value=exact(row.at("current_portfolio_value_exact"));need(value.is_positive());out.equity+=value.as_double();
    }
    need(actual_engines==engines&&std::isfinite(out.equity)&&std::isfinite(out.gross)&&std::isfinite(out.net));
    std::sort(price_sources.begin(),price_sources.end(),[](const J& a,const J& b){return a.at("symbol")<b.at("symbol");});
    out.manifest={{"schema_version","qt-desk-csv-snapshot/v1"},{"snapshot_kind","original_committed_accounting"},
        {"decision_id",d.at("decision_id")},{"attempt_id",receipt.at("attempt_id")},{"accounting_input_id",input_id},
        {"selected_book_digest",receipt.at("published_book_digest")},{"book_id",book},{"source_day",day},
        {"accounting_input_digest",input.at("content_digest")},{"financial_output_digest",result.at("content_digest")},
        {"evaluation_snapshot_id",snapshot_id},{"evaluation_snapshot_digest",captured.at("content_digest")},
        {"valuation_sources",price_sources},{"rows",manifest_rows}};
    return out;
}
} // namespace
Result<std::string> export_committed_qt_desk_csv(pqxx::connection& connection,
    const QtDeskProcessedReceipt& committed,const std::string& input_id,const std::string& source_day,const QtDeskCsvOutput& output){
    try{
        const auto& publication=committed.publication_payload;const auto book=text(publication.at("book_id"));
        need(publication.at("source_day")==source_day&&publication.at("decision_id")==committed.decision_id&&
             publication.at("attempt_id")==committed.attempt_id&&publication.at("observation_id")==input_id&&
             publication.at("selected_book_digest")==committed.published_book_digest);
        auto evidence=load_qt_desk_report_evidence(connection,book,source_day);need(evidence.is_ok());
        const auto& proof=evidence.value();need(proof.at("workflow_required")==true);
        const auto d=proof.at("decision"),preview=proof.at("preview"),receipt=proof.at("receipt");
        need(d.at("decision_id")==committed.decision_id&&receipt.at("decision_id")==committed.decision_id&&
             receipt.at("attempt_id")==committed.attempt_id&&receipt.at("status")=="processed"&&
             receipt.at("published_book_digest")==committed.published_book_digest&&receipt.at("publication_payload")==publication);
        pqxx::work tx(connection);
        tx.exec("SELECT pg_advisory_xact_lock(hashtextextended('algolens:qt-book:'||upper(btrim("+tx.quote(book)+")),0))");
        // Repeat the actual complete durable/current proof while the canonical book is held.
        auto current=capture_qt_desk_processed_facts(tx,d,preview,receipt);need(current.is_ok());
        auto data=snapshot(tx,d,preview,receipt,input_id);
        const auto parent_current=[&](const struct stat* published=nullptr){
            File current_path(open_directory(output.parent_path_));struct stat path{},held{},target{};
            need(::fstat(current_path.fd,&path)==0&&S_ISDIR(path.st_mode)&&path.st_dev==output.device_&&path.st_ino==output.inode_);
            need(::fstat(output.parent_fd_,&held)==0&&S_ISDIR(held.st_mode)&&held.st_dev==output.device_&&held.st_ino==output.inode_);
            errno=0;const auto exists=::fstatat(current_path.fd,output.name_.c_str(),&target,AT_SYMLINK_NOFOLLOW);
            if(published)need(exists==0&&S_ISREG(target.st_mode)&&target.st_dev==published->st_dev&&target.st_ino==published->st_ino);
            else need(exists!=0&&errno==ENOENT);
        };
        parent_current();Scratch scratch(output.parent_fd_);
        CSVExporter exporter("/proc/self/fd/"+std::to_string(scratch.fd));
        const auto filename=source_day+"_positions.csv";scratch.files.push_back(filename);
        auto rendered=exporter.export_desk_positions(midnight(source_day),data.rows,data.prices,data.equity,data.gross,data.net,data.instruments);
        need(rendered.is_ok()&&rendered.value()=="/proc/self/fd/"+std::to_string(scratch.fd)+"/"+filename);
        File plain(::openat(scratch.fd,filename.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC));need(plain.fd>=0);
        auto bytes="# QT desk snapshot: "+data.manifest.dump()+"\n"+bounded_read(plain.fd);
        need(bytes.size()<=byte_limit);auto digest=qt_sha256_hex(bytes);need(digest.is_ok());
        scratch.files.push_back("committed.csv");
        File final(::openat(scratch.fd,"committed.csv",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));need(final.fd>=0);
        write_all(final.fd,bytes);parent_current();
        struct stat completed{};need(::fstat(final.fd,&completed)==0&&S_ISREG(completed.st_mode));
        PublishedFile published{output.parent_fd_,output.name_,completed.st_dev,completed.st_ino};
        need(::linkat(scratch.fd,"committed.csv",output.parent_fd_,output.name_.c_str(),0)==0);published.armed=true;
        need(::fsync(output.parent_fd_)==0);
        // Accounting was committed before entry. This read-only work is closed
        // before success; it never creates another receipt, execution or charge.
        parent_current(&completed);tx.commit();parent_current(&completed);
        published.armed=false;return digest.value();
    }catch(const std::exception&){
        return make_error<std::string>(ErrorCode::INVALID_DATA,"desk_csv_committed_snapshot_refused","qt_desk_csv");
    }
}
}
