#include "trade_ngin/core/config_base.hpp"

namespace trade_ngin {

Result<void> ConfigBase::save_to_file(const std::string& filepath) const {
    try {
        nlohmann::json j = to_json();
        std::ofstream file(filepath);
        if (!file.is_open()) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Failed to open file for writing: " + filepath, "ConfigBase");
        }
        file << std::setw(4) << j << std::endl;
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error saving config: ") + e.what(), "ConfigBase");
    }
}

Result<void> ConfigBase::load_from_file(const std::string& filepath) {
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Failed to open file for reading: " + filepath, "ConfigBase");
        }
        nlohmann::json j;
        file >> j;
        from_json(j);
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error loading config: ") + e.what(), "ConfigBase");
    }
}

}  // namespace trade_ngin
