#include "trade_ngin/data/qt_desk_read_set.hpp"

#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trade_ngin {
namespace {
using Json = nlohmann::json;

[[noreturn]] void invalid() { throw std::invalid_argument("invalid_qt_read_set"); }

void shape(const Json& j, std::initializer_list<std::string_view> names) {
    if (!j.is_object() || j.size() != names.size()) invalid();
    for (auto name : names) if (!j.contains(std::string(name))) invalid();
}

std::string text(const Json& j, bool required = false) {
    if (!j.is_string()) invalid();
    const auto s = j.get<std::string>();
    // nlohmann validates UTF-8 when dumping; count Unicode scalar values here.
    std::size_t count = 0;
    bool nonblank = false;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char lead = static_cast<unsigned char>(s[i]);
        unsigned width = lead < 0x80 ? 1 : lead >= 0xC2 && lead <= 0xDF ? 2 :
                         lead >= 0xE0 && lead <= 0xEF ? 3 :
                         lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
        if (!width || i + width > s.size()) invalid();
        std::uint32_t cp = lead & (width == 1 ? 0x7F : width == 2 ? 0x1F : width == 3 ? 0x0F : 0x07);
        for (unsigned k = 1; k < width; ++k) {
            unsigned char b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) invalid();
            cp = (cp << 6) | (b & 0x3F);
        }
        if ((width == 2 && cp < 0x80) || (width == 3 && cp < 0x800) ||
            (width == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) ||
            cp > 0x10FFFF) invalid();
        if (++count > 4096) invalid();
        const bool blank = cp == 0x20 || (cp >= 0x09 && cp <= 0x0D) ||
            (cp >= 0x1C && cp <= 0x1F) ||
            cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
            (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
            cp == 0x202F || cp == 0x205F || cp == 0x3000;
        nonblank |= !blank;
        i += width;
    }
    if (required && !nonblank) invalid();
    return s;
}

std::string literal(const Json& j, std::initializer_list<std::string_view> values) {
    auto s = text(j);
    for (auto value : values) if (s == value) return s;
    invalid();
}

std::int64_t integer(const Json& j, bool positive = false) {
    if (!j.is_number_integer()) invalid();
    if (j.is_number_unsigned() && j.get<std::uint64_t>() > INT64_MAX) invalid();
    auto n = j.get<std::int64_t>();
    if (positive && n <= 0) invalid();
    return n;
}

void boolean(const Json& j) { if (!j.is_boolean()) invalid(); }
void optional(const Json& j, void (*check)(const Json&)) { if (!j.is_null()) check(j); }
void nonempty(const Json& j) { (void)text(j, true); }

void day(const Json& j) {
    auto s = text(j);
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') invalid();
    for (std::size_t i = 0; i < s.size(); ++i)
        if (i != 4 && i != 7 && !std::isdigit(static_cast<unsigned char>(s[i]))) invalid();
    int y = std::stoi(s.substr(0, 4)), m = std::stoi(s.substr(5, 2)), d = std::stoi(s.substr(8, 2));
    if (y < 1 || m < 1 || m > 12) invalid();
    int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) days[2] = 29;
    if (d < 1 || d > days[m]) invalid();
}

void timestamp(const Json& j) {
    auto s = text(j);
    if (s.size() < 20 || s.back() != 'Z' || s[10] != 'T' || s[13] != ':' || s[16] != ':') invalid();
    day(s.substr(0, 10));
    for (std::size_t i : {11u,12u,14u,15u,17u,18u})
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) invalid();
    if (std::stoi(s.substr(11,2)) > 23 || std::stoi(s.substr(14,2)) > 59 ||
        std::stoi(s.substr(17,2)) > 59) invalid();
    if (s.size() > 20) {
        if (s[19] != '.' || s.size() < 22 || s.size() > 27) invalid();
        for (std::size_t i = 20; i + 1 < s.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) invalid();
    }
}

void digest(const Json& j) {
    auto s = text(j);
    if (s.size() != 64) invalid();
    for (auto c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) invalid();
}

void uuid(const Json& j) {
    auto s = text(j);
    if (s.size() != 36) invalid();
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') invalid();
        } else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) invalid();
    }
}

void exact(const Json& j) {
    auto s = text(j);
    if (parse_qt_quantity_exact(s).is_error()) invalid();
}

void account(const Json& j) {
    auto s = text(j);
    if (s.empty() || s[0] < '1' || s[0] > '9') invalid();
    std::uint64_t n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') invalid();
        unsigned digit = static_cast<unsigned>(c - '0');
        if (n > (static_cast<std::uint64_t>(INT64_MAX) - digit) / 10) invalid();
        n = n * 10 + digit;
    }
}

void positive(const Json& j) { (void)integer(j, true); }
void any_integer(const Json& j) { (void)integer(j); }

void key(const Json& j) {
    shape(j, {"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"});
    nonempty(j.at("portfolio_id")); nonempty(j.at("strategy_id")); nonempty(j.at("strategy_name"));
    day(j.at("date")); nonempty(j.at("symbol"));
    literal(j.at("portfolio_type"), {"system","qt_proposal","qt"});
}

std::vector<std::string> key_id(const Json& row) {
    auto& k = row.at("key");
    std::vector<std::string> id;
    for (auto name : {"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"}) {
        id.push_back(k.at(name).get<std::string>());
    }
    return id;
}

void row(const Json& j, bool proposal) {
    if (proposal) shape(j, {"key","quantity_exact","average_price_exact","position_revision"});
    else shape(j, {"key","quantity_exact","average_price_exact"});
    key(j.at("key")); exact(j.at("quantity_exact")); exact(j.at("average_price_exact"));
    if (proposal) {
        optional(j.at("position_revision"), uuid);
        if (j.at("key").at("portfolio_type") != "qt_proposal") invalid();
    }
}

void accounting(const Json& j) {
    shape(j, {"key","quantity_exact","average_price_exact","daily_unrealized_pnl_exact",
              "daily_realized_pnl_exact","last_update"});
    key(j.at("key"));
    if (j.at("key").at("portfolio_type") != "qt") invalid();
    exact(j.at("quantity_exact")); exact(j.at("average_price_exact"));
    exact(j.at("daily_unrealized_pnl_exact")); exact(j.at("daily_realized_pnl_exact"));
    timestamp(j.at("last_update"));
}

template <class Validate, class Identity>
void ordered_array(Json& j, Validate validate, Identity identity) {
    if (!j.is_array() || j.size() > 4096) invalid();
    using Id = decltype(identity(std::declval<const Json&>()));
    std::vector<std::pair<Id, Json>> items;
    items.reserve(j.size());
    for (auto& value : j) {
        validate(value);
        items.emplace_back(identity(value), value);
    }
    std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (std::size_t i = 1; i < items.size(); ++i) if (items[i - 1].first == items[i].first) invalid();
    j = Json::array();
    for (auto& item : items) j.push_back(std::move(item.second));
}

std::int64_t numeric_id(const Json& j) {
    return integer(j.at("id"), true);
}

void publication(const Json& j) {
    if(j.is_object() && j.contains("schema_version")) {
        shape(j,{"schema_version","publication_id","strategy_id","publication_version","seed_digest",
                 "proposal_manifest_digest","producer_version","owner_document_digest","configuration_digest",
                 "qt_digest","registry_id","registry_revision","configured_owner_names"});
        literal(j.at("schema_version"),{"qt-empty-model-owner-reference/v2"});
        literal(j.at("strategy_id"),{"LIVE_EQUITY_MEAN_REVERSION"});
        uuid(j.at("publication_id"));positive(j.at("publication_version"));
        for(auto field:{"seed_digest","proposal_manifest_digest","owner_document_digest","configuration_digest","qt_digest"})digest(j.at(field));
        nonempty(j.at("producer_version"));nonempty(j.at("registry_id"));
        if(integer(j.at("registry_revision"))<0)invalid();
        const auto& owners=j.at("configured_owner_names");
        if(!owners.is_array()||owners.empty()||owners.size()>4096)invalid();
        std::string previous;
        for(const auto& owner:owners){auto name=text(owner,true);if(!previous.empty()&&!(previous<name))invalid();previous=name;}
        return;
    }
    shape(j, {"publication_id","strategy_id","publication_version","seed_digest",
              "proposal_manifest_digest","producer_version"});
    uuid(j.at("publication_id")); nonempty(j.at("strategy_id"));
    positive(j.at("publication_version")); digest(j.at("seed_digest"));
    optional(j.at("proposal_manifest_digest"), digest); nonempty(j.at("producer_version"));
}

void audit(const Json& j) {
    shape(j, {"id","user_id","source_app","strategy_id","symbol","before_digest","after_digest"});
    positive(j.at("id")); optional(j.at("user_id"), account);
    nonempty(j.at("source_app")); nonempty(j.at("strategy_id")); nonempty(j.at("symbol"));
    digest(j.at("before_digest")); digest(j.at("after_digest"));
}

void draft(const Json& j) {
    shape(j, {"status","draft_id","revision","digest"});
    auto status = literal(j.at("status"), {"absent","present"});
    optional(j.at("draft_id"), uuid); any_integer(j.at("revision")); optional(j.at("digest"), digest);
    if (status == "absent") {
        if (!j.at("draft_id").is_null() || j.at("revision") != 0 || !j.at("digest").is_null()) invalid();
    } else if (j.at("draft_id").is_null() || integer(j.at("revision")) <= 0 ||
               j.at("digest").is_null()) invalid();
}

void registry(const Json& j) {
    shape(j, {"id","strategy_type","portfolio_id","is_active","lifecycle","updated_at"});
    nonempty(j.at("id")); nonempty(j.at("strategy_type")); nonempty(j.at("portfolio_id"));
    boolean(j.at("is_active")); nonempty(j.at("lifecycle")); timestamp(j.at("updated_at"));
}

void membership(const Json& j) {
    shape(j, {"strategy_id","portfolio_id"});
    nonempty(j.at("strategy_id")); nonempty(j.at("portfolio_id"));
}

void capability(const Json& j) {
    shape(j, {"status","enabled","version"});
    auto status = literal(j.at("status"), {"absent","present"});
    optional(j.at("enabled"), boolean); optional(j.at("version"), positive);
    if ((status == "absent") != (j.at("enabled").is_null() && j.at("version").is_null())) invalid();
}

void grant(const Json& j) {
    shape(j, {"user_id","capability","active","version"});
    account(j.at("user_id")); literal(j.at("capability"), {"qt_submit","qt_approve"});
    boolean(j.at("active")); positive(j.at("version"));
}

void risk(const Json& j) {
    shape(j, {"status","id","published_at","content_digest"});
    auto status = literal(j.at("status"), {"absent","present"});
    optional(j.at("id"), positive); optional(j.at("published_at"), timestamp);
    optional(j.at("content_digest"), digest);
    bool missing = j.at("id").is_null() && j.at("published_at").is_null() &&
                   j.at("content_digest").is_null();
    if ((status == "absent") != missing) invalid();
}

void inventory(const Json& j) {
    shape(j, {"id","strategy_id","published_at","content_digest"});
    positive(j.at("id")); nonempty(j.at("strategy_id"));
    timestamp(j.at("published_at")); digest(j.at("content_digest"));
}

void portfolio(const Json& j) {
    shape(j, {"status","source_id","source_day","capital_exact","content_digest"});
    auto status = literal(j.at("status"), {"absent","present"});
    optional(j.at("source_id"), nonempty); optional(j.at("source_day"), day);
    optional(j.at("capital_exact"), exact); optional(j.at("content_digest"), digest);
    bool missing = j.at("source_id").is_null() && j.at("source_day").is_null() &&
                   j.at("capital_exact").is_null() && j.at("content_digest").is_null();
    if ((status == "absent") != missing) invalid();
}

void external(const Json& j) {
    shape(j, {"name","status","source_id","version","as_of","valid_until","digest","reason"});
    literal(j.at("name"), {"mark","history","cost","multiplier","universe","instrument_type",
                           "quantity_rule","evaluator_policy"});
    auto status = literal(j.at("status"), {"available","unavailable"});
    optional(j.at("source_id"), nonempty); optional(j.at("version"), nonempty);
    optional(j.at("as_of"), timestamp); optional(j.at("valid_until"), timestamp);
    optional(j.at("digest"), digest); optional(j.at("reason"), nonempty);
    bool complete = !j.at("source_id").is_null() && !j.at("version").is_null() &&
                    !j.at("as_of").is_null() && !j.at("valid_until").is_null() &&
                    !j.at("digest").is_null();
    bool missing = j.at("source_id").is_null() && j.at("version").is_null() &&
                   j.at("as_of").is_null() && j.at("valid_until").is_null() &&
                   j.at("digest").is_null();
    if (status == "available" ? (!complete || !j.at("reason").is_null()) :
                                 (!missing || j.at("reason").is_null())) invalid();
}

void evaluator(const Json& j) {
    shape(j, {"status","build","policy_version"});
    auto status = literal(j.at("status"), {"available","unavailable"});
    optional(j.at("build"), nonempty); optional(j.at("policy_version"), nonempty);
    if ((status == "unavailable") != (j.at("build").is_null() && j.at("policy_version").is_null()))
        invalid();
}

void provenance(const Json& j) {
    shape(j, {"status","source_digest","chain_digest","seed_digest"});
    literal(j.at("status"), {"ready","provenance_unresolved"});
    optional(j.at("source_digest"), digest); optional(j.at("chain_digest"), digest);
    optional(j.at("seed_digest"), digest);
}

void normalize(Json& j) {
    shape(j, {"schema_version","book_id","source_day","source_rows","saved_rows","system_rows",
              "saved_accounting",
              "publication_refs","audit_refs","draft","registry","memberships","capability",
              "grants","risk_limits","risk_inventory","portfolio_inputs","external_sources",
              "evaluator","provenance"});
    literal(j.at("schema_version"), {"qt-read-set/v1"});
    auto book = text(j.at("book_id"), true), source_day = text(j.at("source_day"));
    day(j.at("source_day"));
    for (auto pair : {std::pair{"source_rows","qt_proposal"}, {"saved_rows","qt"},
                      {"system_rows","system"}}) {
        auto& rows = j.at(pair.first);
        ordered_array(rows, [&](const Json& r) {
            row(r, std::string_view(pair.first) == "source_rows");
            auto& k = r.at("key");
            if (k.at("portfolio_id") != book || k.at("date") != source_day ||
                k.at("portfolio_type") != pair.second) invalid();
        }, key_id);
    }
    ordered_array(j.at("saved_accounting"), [&](const Json& r) {
        accounting(r);
        if (r.at("key").at("portfolio_id") != book ||
            r.at("key").at("date") != source_day) invalid();
    }, key_id);
    ordered_array(j.at("publication_refs"), publication,
                  [](const Json& x) { return x.at("publication_id").get<std::string>(); });
    ordered_array(j.at("audit_refs"), audit, numeric_id);
    draft(j.at("draft"));
    ordered_array(j.at("registry"), registry,
                  [](const Json& x) { return x.at("id").get<std::string>(); });
    ordered_array(j.at("memberships"), membership, [](const Json& x) {
        return std::vector<std::string>{x.at("strategy_id").get<std::string>(),
                                       x.at("portfolio_id").get<std::string>()};
    });
    capability(j.at("capability"));
    ordered_array(j.at("grants"), grant, [](const Json& x) {
        return std::vector<std::string>{x.at("user_id").get<std::string>(),
                                       x.at("capability").get<std::string>()};
    });
    risk(j.at("risk_limits"));
    ordered_array(j.at("risk_inventory"), inventory, numeric_id);
    portfolio(j.at("portfolio_inputs"));
    ordered_array(j.at("external_sources"), external,
                  [](const Json& x) { return x.at("name").get<std::string>(); });
    static const std::vector<std::string> names = {"cost","evaluator_policy","history",
        "instrument_type","mark","multiplier","quantity_rule","universe"};
    if (j.at("external_sources").size() != names.size()) invalid();
    for (std::size_t i = 0; i < names.size(); ++i)
        if (j.at("external_sources")[i].at("name") != names[i]) invalid();
    evaluator(j.at("evaluator")); provenance(j.at("provenance"));
}

std::string timestamp_key(const Json& j) {
    timestamp(j);
    auto s = text(j);
    auto fraction = s.size() > 20 ? s.substr(20, s.size() - 21) : "";
    fraction.append(6 - fraction.size(), '0');
    return s.substr(0,19) + "." + fraction + "Z";
}

bool nonnull(const Json& j, std::initializer_list<std::string_view> fields) {
    for (auto field : fields) if (j.at(std::string(field)).is_null()) return false;
    return true;
}

bool ready(const Json& j, const std::string& captured_at) {
    if (captured_at.substr(0,10) != j.at("source_day").get<std::string>()) return false;
    const auto& cap = j.at("capability");
    const auto& risk_input = j.at("risk_limits");
    const auto& portfolio_input = j.at("portfolio_inputs");
    const auto& eval = j.at("evaluator");
    const auto& proof = j.at("provenance");
    if (cap.at("status") != "present" || !nonnull(cap, {"enabled","version"}) ||
        cap.at("enabled") != true || risk_input.at("status") != "present" ||
        !nonnull(risk_input, {"id","published_at","content_digest"}) ||
        portfolio_input.at("status") != "present" ||
        !nonnull(portfolio_input, {"source_id","source_day","capital_exact","content_digest"}) ||
        portfolio_input.at("source_day") != j.at("source_day") ||
        eval.at("status") != "available" || !nonnull(eval, {"build","policy_version"}) ||
        proof.at("status") != "ready" || !nonnull(proof, {"source_digest","chain_digest","seed_digest"}))
        return false;
    const auto captured = timestamp_key(captured_at);
    for (const auto& source : j.at("external_sources")) {
        if (source.at("status") != "available" || timestamp_key(source.at("as_of")) > captured ||
            captured > timestamp_key(source.at("valid_until"))) return false;
    }
    const auto& rows = j.at("saved_rows");
    const auto& accounts = j.at("saved_accounting");
    if (rows.size() != accounts.size()) return false;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (key_id(rows[i]) != key_id(accounts[i]) ||
            rows[i].at("quantity_exact") != accounts[i].at("quantity_exact") ||
            rows[i].at("average_price_exact") != accounts[i].at("average_price_exact")) return false;
    return true;
}
}  // namespace

Result<std::string> canonical_qt_desk_read_set_bytes(const nlohmann::json& payload) {
    try {
        auto document = payload;
        normalize(document);
        auto bytes = document.dump(-1, ' ', false, Json::error_handler_t::strict);
        if (bytes.size() > 1'048'576) invalid();
        return bytes;
    } catch (const std::exception&) {
        return make_error<std::string>(ErrorCode::INVALID_DATA,
                                       "invalid_qt_read_set", "qt_desk_read_set");
    }
}

Result<std::string> qt_desk_read_set_digest(const nlohmann::json& payload) {
    auto bytes = canonical_qt_desk_read_set_bytes(payload);
    if (bytes.is_error()) return make_error<std::string>(ErrorCode::INVALID_DATA,
                                                         "invalid_qt_read_set", "qt_desk_read_set");
    return qt_sha256_hex(bytes.value());
}

Result<QtDeskReadSetCapture> admit_qt_desk_read_set(
    const nlohmann::json& payload, std::string_view captured_at) {
    try {
        auto captured = std::string(captured_at);
        timestamp(captured);
        auto bytes = canonical_qt_desk_read_set_bytes(payload);
        if (bytes.is_error()) invalid();
        auto document = Json::parse(bytes.value());
        if (!ready(document, captured)) return make_error<QtDeskReadSetCapture>(
            ErrorCode::INVALID_DATA, "qt_read_set_unavailable", "qt_desk_read_set");
        auto hash = qt_sha256_hex(bytes.value());
        if (hash.is_error()) invalid();
        auto source_day = document.at("source_day").get<std::string>();
        return QtDeskReadSetCapture{std::move(document), hash.value(), source_day, captured};
    } catch (const std::exception&) {
        return make_error<QtDeskReadSetCapture>(ErrorCode::INVALID_DATA,
            "invalid_qt_read_set", "qt_desk_read_set");
    }
}
}  // namespace trade_ngin
