#pragma once

#include "trade_ngin/git_version.hpp"

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
    return value;
}

}  // namespace trade_ngin::test
