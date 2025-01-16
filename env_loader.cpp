#include "env_loader.hpp"
#include <iostream>

void EnvLoader::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open .env file: " + filepath);
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines or comments
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Find the position of the '=' character
        size_t delimiter_pos = line.find('=');
        if (delimiter_pos == std::string::npos) {
            continue;  // Invalid line format, skip it
        }

        // Extract key and value
        std::string key = line.substr(0, delimiter_pos);
        std::string value = line.substr(delimiter_pos + 1);

        // Remove trailing or leading whitespaces
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        value.erase(0, value.find_first_not_of(" \t\r\n"));

        // Set environment variable
        setenv(key.c_str(), value.c_str(), 1);
    }
    file.close();
}
