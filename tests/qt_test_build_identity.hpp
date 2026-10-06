#pragma once

#include "trade_ngin/git_version.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace trade_ngin::test {

inline constexpr const char* kFixtureBuildPlaceholder = "local-qt-controlled";

inline void bind_fixture_to_compiled_build(nlohmann::json& value) {
    if (value.is_string()) {
        auto text = value.get<std::string>();
        for (auto position = text.find(kFixtureBuildPlaceholder);
             position != std::string::npos;
             position = text.find(kFixtureBuildPlaceholder,
                                  position + std::char_traits<char>::length(TRADE_NGIN_GIT_SHA))) {
            text.replace(position, std::char_traits<char>::length(kFixtureBuildPlaceholder),
                         TRADE_NGIN_GIT_SHA);
        }
        value = std::move(text);
        return;
    }
    if (value.is_array()) {
        for (auto& child : value) {
            bind_fixture_to_compiled_build(child);
        }
        return;
    }
    if (value.is_object()) {
        for (auto& [key, child] : value.items()) {
            static_cast<void>(key);
            bind_fixture_to_compiled_build(child);
        }
    }
}

inline nlohmann::json fixture_for_compiled_build(nlohmann::json value) {
    bind_fixture_to_compiled_build(value);
    // Finalization fixtures embed a canonical JSON document and its digest.
    // Rebinding its authority requires rebinding the test-only digest chain too.
    if (value.is_object() && value.contains("original_output_json") &&
        value.contains("provenance")) {
        const auto output = nlohmann::json::parse(value.at("original_output_json").get<std::string>());
        const auto encoded = canonical_qt_desk_source_json(output).value();
        value["original_output_json"] = encoded;
        value["provenance"]["original_run_result_digest"] = qt_sha256_hex(encoded).value();
        auto unsigned_value = value;
        unsigned_value.erase("context_fingerprint");
        value["context_fingerprint"] = qt_sha256_hex(
            canonical_qt_desk_source_json(unsigned_value).value()).value();
    }
    return value;
}

}  // namespace trade_ngin::test
