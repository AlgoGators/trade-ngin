// STAGED CANDIDATE; not compiled or SQL authority evidence.
#include "trade_ngin/data/qt_empty_model_owner_reference.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include <stdexcept>
namespace trade_ngin {
Result<nlohmann::json> qt_empty_model_owner_reference(const nlohmann::json& d,
    int64_t version,const std::string& producer,const std::string& registry,int64_t revision){
    try {
        const auto valid_text=[](const std::string& s){
            size_t characters=0;for(unsigned char c:s)if((c&0xc0)!=0x80)++characters;
            if(characters>4096 || s.find('\0')!=std::string::npos || s.find_first_not_of(" \t\r\n")==std::string::npos)return false;
            (void)nlohmann::json(s).dump(-1,' ',false,nlohmann::json::error_handler_t::strict);return true;
        };
        auto bytes=canonical_qt_empty_model_owner_bytes(d);
        if(bytes.is_error()||version<=0||revision<0||!valid_text(producer)||!valid_text(registry))throw std::invalid_argument("ref");
        auto digest=qt_sha256_hex(bytes.value());if(digest.is_error())throw std::invalid_argument("ref");
        return nlohmann::json{{"schema_version","qt-empty-model-owner-reference/v2"},
            {"publication_id",d.at("publication_id")},{"strategy_id",d.at("strategy_id")},{"publication_version",version},
            {"seed_digest",d.at("seed_digest")},{"proposal_manifest_digest",d.at("proposal_manifest_digest")},
            {"producer_version",producer},{"owner_document_digest",digest.value()},
            {"configuration_digest",d.at("configuration_digest")},{"qt_digest",d.at("qt_digest")},
            {"registry_id",registry},{"registry_revision",revision},{"configured_owner_names",d.at("configured_owner_names")}};
    }catch(const std::exception&){return make_error<nlohmann::json>(ErrorCode::INVALID_ARGUMENT,"qt_empty_owner_reference_invalid");}
}
} // namespace trade_ngin
