#include "trade_ngin/core/json_wrapper.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

namespace trade_ngin {

// Private implementation class for JsonWrapper
class JsonWrapper::Impl {
public:
    Impl() = default;
    
    explicit Impl(const std::string& json_str) {
        try {
            json_ = nlohmann::json::parse(json_str);
        } catch (const nlohmann::json::parse_error& e) {
            std::cerr << "JSON parse error: " << e.what() << std::endl;
            json_ = nlohmann::json::object();
        }
    }
    
    Impl(const Impl& other) : json_(other.json_) {}
    
    nlohmann::json json_;
};

// JsonWrapper implementation

JsonWrapper::JsonWrapper() : impl_(std::make_unique<Impl>()) {}

JsonWrapper::JsonWrapper(const std::string& json_str) : impl_(std::make_unique<Impl>(json_str)) {}

JsonWrapper::~JsonWrapper() = default;

JsonWrapper::JsonWrapper(JsonWrapper&& other) noexcept = default;

JsonWrapper::JsonWrapper(const JsonWrapper& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}

JsonWrapper& JsonWrapper::operator=(JsonWrapper&& other) noexcept = default;

JsonWrapper& JsonWrapper::operator=(const JsonWrapper& other) {
    if (this != &other) {
        impl_ = std::make_unique<Impl>(*other.impl_);
    }
    return *this;
}

void JsonWrapper::set_bool(const std::string& key, bool value) {
    impl_->json_[key] = value;
}

void JsonWrapper::set_int(const std::string& key, int value) {
    impl_->json_[key] = value;
}

void JsonWrapper::set_double(const std::string& key, double value) {
    impl_->json_[key] = value;
}

void JsonWrapper::set_string(const std::string& key, const std::string& value) {
    impl_->json_[key] = value;
}

void JsonWrapper::set_object(const std::string& key, const JsonWrapper& value) {
    impl_->json_[key] = value.impl_->json_;
}

bool JsonWrapper::get_bool(const std::string& key, bool default_value) const {
    if (impl_->json_.contains(key) && impl_->json_[key].is_boolean()) {
        return impl_->json_[key].get<bool>();
    }
    return default_value;
}

int JsonWrapper::get_int(const std::string& key, int default_value) const {
    if (impl_->json_.contains(key) && impl_->json_[key].is_number_integer()) {
        return impl_->json_[key].get<int>();
    }
    return default_value;
}

double JsonWrapper::get_double(const std::string& key, double default_value) const {
    if (impl_->json_.contains(key) && impl_->json_[key].is_number()) {
        return impl_->json_[key].get<double>();
    }
    return default_value;
}

std::string JsonWrapper::get_string(const std::string& key, const std::string& default_value) const {
    if (impl_->json_.contains(key) && impl_->json_[key].is_string()) {
        return impl_->json_[key].get<std::string>();
    }
    return default_value;
}

JsonWrapper JsonWrapper::get_object(const std::string& key) const {
    if (impl_->json_.contains(key) && impl_->json_[key].is_object()) {
        JsonWrapper result;
        result.impl_->json_ = impl_->json_[key];
        return result;
    }
    return JsonWrapper();
}

bool JsonWrapper::contains(const std::string& key) const {
    return impl_->json_.contains(key);
}

std::vector<std::string> JsonWrapper::keys() const {
    std::vector<std::string> result;
    if (impl_->json_.is_object()) {
        for (auto it = impl_->json_.begin(); it != impl_->json_.end(); ++it) {
            result.push_back(it.key());
        }
    }
    return result;
}

std::string JsonWrapper::to_string(bool pretty) const {
    if (pretty) {
        return impl_->json_.dump(4);
    } else {
        return impl_->json_.dump();
    }
}

bool JsonWrapper::from_string(const std::string& json_str) {
    try {
        impl_->json_ = nlohmann::json::parse(json_str);
        return true;
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
        return false;
    }
}

bool JsonWrapper::is_empty() const {
    return impl_->json_.empty();
}

// Template specializations for get_array and set_array for common types

template<>
void JsonWrapper::set_array<int>(const std::string& key, const std::vector<int>& values) {
    impl_->json_[key] = values;
}

template<>
void JsonWrapper::set_array<double>(const std::string& key, const std::vector<double>& values) {
    impl_->json_[key] = values;
}

template<>
void JsonWrapper::set_array<std::string>(const std::string& key, const std::vector<std::string>& values) {
    impl_->json_[key] = values;
}

template<>
void JsonWrapper::set_array<bool>(const std::string& key, const std::vector<bool>& values) {
    // Convert to vector<int> because vector<bool> is a specialization that may not work as expected
    std::vector<int> int_values;
    int_values.reserve(values.size());
    for (bool value : values) {
        int_values.push_back(value ? 1 : 0);
    }
    impl_->json_[key] = int_values;
}

template<>
std::vector<int> JsonWrapper::get_array<int>(const std::string& key) const {
    if (impl_->json_.contains(key) && impl_->json_[key].is_array()) {
        try {
            return impl_->json_[key].get<std::vector<int>>();
        } catch (const nlohmann::json::type_error& e) {
            std::cerr << "JSON type error: " << e.what() << std::endl;
        }
    }
    return {};
}

template<>
std::vector<double> JsonWrapper::get_array<double>(const std::string& key) const {
    if (impl_->json_.contains(key) && impl_->json_[key].is_array()) {
        try {
            return impl_->json_[key].get<std::vector<double>>();
        } catch (const nlohmann::json::type_error& e) {
            std::cerr << "JSON type error: " << e.what() << std::endl;
        }
    }
    return {};
}

template<>
std::vector<std::string> JsonWrapper::get_array<std::string>(const std::string& key) const {
    if (impl_->json_.contains(key) && impl_->json_[key].is_array()) {
        try {
            return impl_->json_[key].get<std::vector<std::string>>();
        } catch (const nlohmann::json::type_error& e) {
            std::cerr << "JSON type error: " << e.what() << std::endl;
        }
    }
    return {};
}

template<>
std::vector<bool> JsonWrapper::get_array<bool>(const std::string& key) const {
    if (impl_->json_.contains(key) && impl_->json_[key].is_array()) {
        try {
            auto int_values = impl_->json_[key].get<std::vector<int>>();
            std::vector<bool> bool_values;
            bool_values.reserve(int_values.size());
            for (int value : int_values) {
                bool_values.push_back(value != 0);
            }
            return bool_values;
        } catch (const nlohmann::json::type_error& e) {
            std::cerr << "JSON type error: " << e.what() << std::endl;
        }
    }
    return {};
}

} // namespace trade_ngin 