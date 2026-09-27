#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/core/qt_sha256.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace trade_ngin {
namespace {
using Json = nlohmann::json;
constexpr size_t kMaxBytes = 1'048'576, kMaxString = 4096, kMaxArray = 4096;
constexpr std::array<std::string_view, 6> kKeyFields = {
    "portfolio_id", "strategy_id", "strategy_name", "date", "symbol", "portfolio_type"};

// Python's len(str) counts Unicode codepoints, not encoded bytes. Strict
// JSON serialization below rejects invalid UTF-8 even for in-memory values.
bool text_within_limit(std::string_view text) {
    size_t codepoints = 0;
    for (unsigned char byte : text)
        if ((byte & 0xC0) != 0x80 && ++codepoints > kMaxString) return false;
    return true;
}

bool in(std::string_view words, std::string_view name) {
    size_t begin = 0;
    while (begin < words.size()) {
        auto end = words.find(' ', begin);
        if (end == std::string_view::npos) end = words.size();
        if (words.substr(begin, end - begin) == name) return true;
        begin = end + 1;
    }
    return false;
}

constexpr std::string_view kExact =
    "quantity_exact average_price_exact previous_net_quantity_exact proposed_net_quantity_exact "
    "prior_quantity_exact selected_quantity_exact cash_cost_exact total_exact";
constexpr std::string_view kDiagnostic =
    "weight_diagnostic cost_penalty limit_diagnostic actual_diagnostic value_diagnostic";
constexpr std::string_view kDigest =
    "source_digest provenance_digest draft_digest payload_digest optimizer_book_digest selected_book_digest "
    "read_set_digest evaluated_book_digest published_book_digest row_manifest_digest expected_source_digest "
    "expected_provenance_digest expected_digest seed_digest request_digest";
constexpr std::string_view kUuid =
    "draft_id preview_id decision_id request_id idempotency_key seed_publication_id model_publication_id";
constexpr std::string_view kInteger =
    "version draft_revision expected_draft_revision approvals_count required_approvals";
constexpr std::string_view kBoolean =
    "required available can_save_draft can_confirm can_approve editable confirmable requires_override "
    "passed acknowledge_warnings retryable report_ready";
constexpr std::string_view kArray =
    "selection_rows seed_rows saved_qt_rows aggregate_bindings component_keys current_weights "
    "target_weights solved_weights trace diagnostics breaches metrics by_component unavailable_reasons "
    "approvals report_blocked_reasons reason_codes";
constexpr std::string_view kObject =
    "key capability action_grants evaluation optimizer selected_risk selected_costs receipt report_eligibility error";
constexpr std::string_view kString =
    "schema_version book_id source_day workflow_state read_only_reason state portfolio_id strategy_id "
    "strategy_name date symbol portfolio_type basis_status asset_type origin status availability "
    "instrument_type config_source_id market_snapshot_id source_id code unit message action person_id "
    "display_label user_id approved_at created_at updated_at evaluator_build policy_version";
constexpr std::string_view kSorted = "selection_rows seed_rows saved_qt_rows component_keys by_component";
constexpr std::string_view kTextArray = "trace diagnostics unavailable_reasons report_blocked_reasons reason_codes";

std::set<std::string> words(std::string_view text) {
    std::set<std::string> out;
    size_t begin = 0;
    while (begin < text.size()) {
        auto end = text.find(' ', begin);
        if (end == std::string_view::npos) end = text.size();
        out.insert(std::string(text.substr(begin, end - begin)));
        begin = end + 1;
    }
    return out;
}

std::set<std::string> field_names(const Json& value) {
    std::set<std::string> out;
    for (auto it = value.begin(); it != value.end(); ++it) out.insert(it.key());
    return out;
}

void check_shape(const Json& value, std::string_view field) {
    if (!value.is_object()) return;
    const auto names = field_names(value);
    if (field == "key" || field == "component_keys") {
        if (names != words("portfolio_id strategy_id strategy_name date symbol portfolio_type"))
            throw std::invalid_argument("invalid_qt_key");
    } else if (field == "selection_rows") {
        if (names != words("key quantity_exact") &&
            names != words("key quantity_exact basis_status average_price_exact asset_type editable origin"))
            throw std::invalid_argument("invalid_qt_payload");
    } else if (field == "seed_rows") {
        if (names != words("key quantity_exact average_price_exact") &&
            names != words("key quantity_exact basis_status average_price_exact asset_type editable origin"))
            throw std::invalid_argument("invalid_qt_payload");
    } else if (field == "by_component") {
        if (names != words("key prior_quantity_exact selected_quantity_exact cash_cost_exact source_id"))
            throw std::invalid_argument("invalid_qt_payload");
    } else {
        static const std::map<std::string_view, std::string_view> shapes = {
            {"saved_qt_rows", "key quantity_exact basis_status average_price_exact asset_type editable origin"},
            {"aggregate_bindings", "instrument_type symbol component_keys previous_net_quantity_exact proposed_net_quantity_exact"},
            {"current_weights", "instrument_type symbol weight_diagnostic"},
            {"target_weights", "instrument_type symbol weight_diagnostic"},
            {"solved_weights", "instrument_type symbol weight_diagnostic"},
            {"breaches", "code limit_diagnostic actual_diagnostic"},
            {"metrics", "code value_diagnostic unit source_id"},
            {"approvals", "person_id display_label user_id approved_at"},
            {"capability", "required available version"},
            {"action_grants", "can_save_draft can_confirm can_approve"},
            {"evaluation", "optimizer selected_risk selected_costs"},
            {"optimizer", "status evaluated_book_digest aggregate_bindings current_weights target_weights solved_weights trace cost_penalty diagnostics config_source_id"},
            {"selected_risk", "status evaluated_book_digest passed breaches metrics config_source_id market_snapshot_id diagnostics"},
            {"selected_costs", "status evaluated_book_digest by_component total_exact diagnostics"},
            {"receipt", "status published_book_digest report_eligibility"},
            {"report_eligibility", "status reason_codes row_manifest_digest"},
            {"error", "code message retryable"},
        };
        const auto shape = shapes.find(field);
        if (shape != shapes.end() && names != words(shape->second))
            throw std::invalid_argument("invalid_qt_payload");
    }
}

std::array<std::string, 6> key_sort_value(const Json& object) {
    const Json& key = object.contains("key") ? object.at("key") : object;
    if (!key.is_object() || key.size() != 6) throw std::invalid_argument("invalid_qt_key");
    std::array<std::string, 6> out;
    for (size_t i = 0; i < kKeyFields.size(); ++i) {
        auto field = kKeyFields[i];
        if (!key.contains(std::string(field)) || !key.at(std::string(field)).is_string())
            throw std::invalid_argument("invalid_qt_key");
        out[i] = key.at(std::string(field)).get_ref<const std::string&>();
    }
    return out;
}

bool diagnostic_text(std::string_view text) {
    if (text.empty()) return false;
    size_t i = text[0] == '-' ? 1 : 0;
    if (i == text.size()) return false;
    if (text[i] == '0') ++i;
    else {
        if (text[i] < '1' || text[i] > '9') return false;
        do { ++i; } while (i < text.size() && text[i] >= '0' && text[i] <= '9');
    }
    if (i < text.size() && text[i] == '.') {
        const auto start = ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        if (start == i) return false;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
        const auto start = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        if (start == i) return false;
    }
    if (i != text.size() || text.size() > kMaxString) return false;
    double parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(parsed))
        return false;
    const auto mantissa = text.substr(0, text.find_first_of("eE"));
    const bool nonzero = mantissa.find_first_of("123456789") != std::string_view::npos;
    return parsed != 0.0 || !nonzero;
}

bool date_text(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (size_t i = 0; i < text.size(); ++i)
        if (i != 4 && i != 7 && (text[i] < '0' || text[i] > '9')) return false;
    const int year = std::stoi(std::string(text.substr(0, 4)));
    const int month = std::stoi(std::string(text.substr(5, 2)));
    const int day = std::stoi(std::string(text.substr(8, 2)));
    if (year < 1 || month < 1 || month > 12) return false;
    constexpr int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    return day >= 1 && day <= days[month] +
        (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) ? 1 : 0);
}

void validate(Json& value, std::string_view field, unsigned depth) {
    if (depth > 32) throw std::invalid_argument("invalid_qt_payload");
    if (value.is_object()) {
        if (value.size() > 128) throw std::invalid_argument("invalid_qt_payload");
        check_shape(value, field);
        for (auto it = value.begin(); it != value.end(); ++it) {
            const auto& name = it.key();
            if (!(in(kExact,name)||in(kDiagnostic,name)||in(kDigest,name)||in(kUuid,name)||
                  in(kInteger,name)||in(kBoolean,name)||in(kArray,name)||in(kObject,name)||in(kString,name)))
                throw std::invalid_argument("invalid_qt_payload");
            validate(it.value(), name, depth + 1);
        }
        return;
    }
    if (value.is_array()) {
        if (!in(kArray, field) || value.size() > kMaxArray)
            throw std::invalid_argument("invalid_qt_payload");
        for (auto& item : value) validate(item, field, depth + 1);
        if (in(kSorted, field)) {
            std::vector<std::pair<std::array<std::string, 6>, Json>> keyed;
            for (auto& item : value) keyed.emplace_back(key_sort_value(item), std::move(item));
            std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < keyed.size(); ++i) {
                if (i && keyed[i-1].first == keyed[i].first) throw std::invalid_argument("invalid_qt_key");
                value[i] = std::move(keyed[i].second);
            }
        } else if (field == "aggregate_bindings") {
            auto identity = [](const Json& row) {
                return std::pair(row.at("instrument_type").get<std::string>(),
                                 row.at("symbol").get<std::string>());
            };
            std::sort(value.begin(), value.end(), [&](const Json& a, const Json& b) {
                return identity(a) < identity(b);
            });
            for (size_t i = 1; i < value.size(); ++i)
                if (identity(value[i-1]) == identity(value[i]))
                    throw std::invalid_argument("invalid_qt_payload");
        }
        return;
    }
    if (value.is_null()) {
        if (in("average_price_exact total_exact "
               "read_only_reason config_source_id market_snapshot_id source_id passed version user_id",field) ||
            in(kDiagnostic,field)||in(kDigest,field)||in(kUuid,field)||in(kObject,field)) return;
        throw std::invalid_argument("invalid_qt_payload");
    }
    if (in(kTextArray,field)) {
        if (value.is_string() && text_within_limit(value.get_ref<const std::string&>())) return;
    } else if (in(kExact,field)) {
        if (value.is_string() && parse_qt_quantity_exact(value.get_ref<const std::string&>()).is_ok()) return;
        throw std::invalid_argument("invalid_qt_exact");
    } else if (in(kDiagnostic,field)) {
        if (value.is_string() && diagnostic_text(value.get_ref<const std::string&>())) return;
    } else if (in(kDigest,field)) {
        if (value.is_string()) {
            const auto& s = value.get_ref<const std::string&>();
            if (s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos) return;
        }
    } else if (in(kUuid,field)) {
        if (value.is_string()) {
            const auto& s = value.get_ref<const std::string&>();
            if (s.size() == 36) {
                bool valid = true;
                for (size_t i = 0; i < s.size(); ++i)
                    valid &= (i==8||i==13||i==18||i==23) ? s[i]=='-' :
                        ((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'));
                if (valid) return;
            }
        }
    } else if (in(kInteger,field)) {
        if (value.is_number_integer() &&
            (!value.is_number_unsigned() || value.get<uint64_t>() <= static_cast<uint64_t>(INT64_MAX))) return;
    } else if (in(kBoolean,field)) {
        if (value.is_boolean()) return;
    } else if (in(kString,field)) {
        if (value.is_string()) {
            const auto& s = value.get_ref<const std::string&>();
            if (field == "user_id" &&
                (s.empty() || s.find_first_not_of("0123456789") != std::string::npos ||
                 (s.size() > 1 && s.front() == '0') || s.size() > 19 ||
                 (s.size() == 19 && s > "9223372036854775807")))
                throw std::invalid_argument("invalid_qt_payload");
            if (text_within_limit(s) && (field != "schema_version" || s == "qt-workflow/v1") &&
                ((field != "date" && field != "source_day") || date_text(s))) return;
        }
    }
    throw std::invalid_argument("invalid_qt_payload");
}
}  // namespace

Result<Quantity> parse_qt_quantity_exact(std::string_view text) {
    if (text.empty() || text.size() > 21) return make_error<Quantity>(ErrorCode::INVALID_ARGUMENT, "invalid_qt_exact");
    const bool negative = text.front() == '-';
    size_t i = negative ? 1 : 0;
    uint64_t whole = 0, frac = 0;
    const auto start = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        if (whole > UINT64_C(92233720368)) return make_error<Quantity>(ErrorCode::INVALID_ARGUMENT, "invalid_qt_exact");
        whole = whole * 10 + static_cast<unsigned>(text[i++] - '0');
    }
    if (i == start || (i - start > 1 && text[start] == '0'))
        return make_error<Quantity>(ErrorCode::INVALID_ARGUMENT, "invalid_qt_exact");
    unsigned decimals = 0;
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            if (++decimals > 8) return make_error<Quantity>(ErrorCode::INVALID_ARGUMENT, "invalid_qt_exact");
            frac = frac * 10 + static_cast<unsigned>(text[i++] - '0');
        }
        if (!decimals) return make_error<Quantity>(ErrorCode::INVALID_ARGUMENT, "invalid_qt_exact");
    }
    if (i != text.size() || whole > UINT64_C(92233720368))
        return make_error<Quantity>(ErrorCode::INVALID_ARGUMENT, "invalid_qt_exact");
    while (decimals++ < 8) frac *= 10;
    const uint64_t magnitude = whole * UINT64_C(100000000) + frac;
    if (magnitude > (negative ? (UINT64_C(1)<<63) : static_cast<uint64_t>(INT64_MAX)))
        return make_error<Quantity>(ErrorCode::INVALID_ARGUMENT, "invalid_qt_exact");
    const int64_t raw = negative && magnitude == (UINT64_C(1)<<63) ? INT64_MIN :
        (negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude));
    auto result = Quantity::from_raw(raw);
    if (result.to_string() != text) return make_error<Quantity>(ErrorCode::INVALID_ARGUMENT, "invalid_qt_exact");
    return result;
}

Result<nlohmann::json> parse_qt_json(std::string_view utf8) {
    if (utf8.size() > kMaxBytes)
        return make_error<Json>(ErrorCode::INVALID_ARGUMENT, "invalid_qt_payload", "qt_wire");
    try {
        std::vector<std::set<std::string>> keys;
        auto reject_duplicates = [&keys](int, Json::parse_event_t event, Json& parsed) {
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            else if (event == Json::parse_event_t::object_end) keys.pop_back();
            else if (event == Json::parse_event_t::key) {
                if (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second)
                    throw std::invalid_argument("invalid_qt_payload");
            }
            return true;
        };
        Json value = Json::parse(utf8.begin(), utf8.end(), reject_duplicates);
        if (canonical_qt_bytes(value).is_error())
            return make_error<Json>(ErrorCode::INVALID_ARGUMENT, "invalid_qt_payload", "qt_wire");
        return value;
    } catch (const std::exception& e) {
        return make_error<Json>(ErrorCode::JSON_PARSE_ERROR, e.what(), "qt_wire");
    }
}

Result<std::string> canonical_qt_bytes(const nlohmann::json& value) {
    try {
        if (!value.is_object()) return make_error<std::string>(ErrorCode::INVALID_ARGUMENT, "invalid_qt_payload");
        auto normalized = value;
        validate(normalized, {}, 0);
        auto bytes = normalized.dump(-1, ' ', false, Json::error_handler_t::strict);
        if (bytes.size() > kMaxBytes) return make_error<std::string>(ErrorCode::INVALID_ARGUMENT, "invalid_qt_payload");
        return bytes;
    } catch (const std::exception& e) {
        return make_error<std::string>(ErrorCode::INVALID_ARGUMENT, e.what(), "qt_wire");
    }
}

Result<std::string> qt_digest_v1(const nlohmann::json& value) {
    auto bytes = canonical_qt_bytes(value);
    if (bytes.is_error()) return make_error<std::string>(bytes.error()->code(), bytes.error()->what(), "qt_wire");
    return qt_sha256_hex(bytes.value());
}

}  // namespace trade_ngin

