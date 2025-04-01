#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <type_traits>
#include <optional>

// Forward declaration of JSON implementation
namespace nlohmann {
    template<typename T, typename SFINAE>
    class adl_serializer;
    
    template<template<typename U, typename V, typename... Args> class ObjectType,
             template<typename U, typename... Args> class ArrayType,
             class StringType, class BooleanType, class NumberIntegerType,
             class NumberUnsignedType, class NumberFloatType,
             template<typename U> class AllocatorType,
             template<typename T, typename SFINAE> class JSONSerializer>
    class basic_json;
    
    using json = basic_json<std::map, std::vector, std::string, bool, std::int64_t,
                           std::uint64_t, double, std::allocator, adl_serializer>;
}

namespace trade_ngin {

/**
 * @brief Wrapper around JSON library to isolate dependency
 * 
 * This class encapsulates the nlohmann::json dependency and provides
 * an abstraction layer for serialization/deserialization.
 */
class JsonWrapper {
public:
    /**
     * @brief Default constructor
     */
    JsonWrapper();
    
    /**
     * @brief Constructor from JSON string
     * @param json_str JSON string
     */
    explicit JsonWrapper(const std::string& json_str);
    
    /**
     * @brief Destructor
     */
    ~JsonWrapper();
    
    /**
     * @brief Move constructor
     */
    JsonWrapper(JsonWrapper&& other) noexcept;
    
    /**
     * @brief Copy constructor
     */
    JsonWrapper(const JsonWrapper& other);
    
    /**
     * @brief Move assignment operator
     */
    JsonWrapper& operator=(JsonWrapper&& other) noexcept;
    
    /**
     * @brief Copy assignment operator
     */
    JsonWrapper& operator=(const JsonWrapper& other);
    
    // =============================================
    // Value setting methods
    // =============================================
    
    /**
     * @brief Set a boolean value
     * @param key Key
     * @param value Boolean value
     */
    void set_bool(const std::string& key, bool value);
    
    /**
     * @brief Set an integer value
     * @param key Key
     * @param value Integer value
     */
    void set_int(const std::string& key, int value);
    
    /**
     * @brief Set a double value
     * @param key Key
     * @param value Double value
     */
    void set_double(const std::string& key, double value);
    
    /**
     * @brief Set a string value
     * @param key Key
     * @param value String value
     */
    void set_string(const std::string& key, const std::string& value);
    
    /**
     * @brief Set a nested object
     * @param key Key
     * @param value JsonWrapper object
     */
    void set_object(const std::string& key, const JsonWrapper& value);
    
    /**
     * @brief Set an array of values
     * @param key Key
     * @param values Vector of values
     */
    template<typename T>
    void set_array(const std::string& key, const std::vector<T>& values);
    
    // =============================================
    // Value getting methods
    // =============================================
    
    /**
     * @brief Get a boolean value
     * @param key Key
     * @param default_value Default value if key does not exist
     * @return Boolean value
     */
    bool get_bool(const std::string& key, bool default_value = false) const;
    
    /**
     * @brief Get an integer value
     * @param key Key
     * @param default_value Default value if key does not exist
     * @return Integer value
     */
    int get_int(const std::string& key, int default_value = 0) const;
    
    /**
     * @brief Get a double value
     * @param key Key
     * @param default_value Default value if key does not exist
     * @return Double value
     */
    double get_double(const std::string& key, double default_value = 0.0) const;
    
    /**
     * @brief Get a string value
     * @param key Key
     * @param default_value Default value if key does not exist
     * @return String value
     */
    std::string get_string(const std::string& key, const std::string& default_value = "") const;
    
    /**
     * @brief Get a nested object
     * @param key Key
     * @return JsonWrapper object, empty if key does not exist
     */
    JsonWrapper get_object(const std::string& key) const;
    
    /**
     * @brief Get an array of values
     * @param key Key
     * @return Vector of values, empty if key does not exist
     */
    template<typename T>
    std::vector<T> get_array(const std::string& key) const;
    
    /**
     * @brief Check if a key exists
     * @param key Key to check
     * @return true if key exists, false otherwise
     */
    bool contains(const std::string& key) const;
    
    /**
     * @brief Get all keys in the JSON object
     * @return Vector of keys
     */
    std::vector<std::string> keys() const;
    
    /**
     * @brief Convert to string
     * @param pretty Whether to format with indentation
     * @return JSON string
     */
    std::string to_string(bool pretty = false) const;
    
    /**
     * @brief Parse from string
     * @param json_str JSON string
     * @return true if parsing succeeded, false otherwise
     */
    bool from_string(const std::string& json_str);
    
    /**
     * @brief Check if the JSON object is empty
     * @return true if empty, false otherwise
     */
    bool is_empty() const;

private:
    // Pimpl idiom to hide nlohmann::json implementation
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// =============================================
// Serializable interface
// =============================================

/**
 * @brief Interface for JSON serializable objects
 * 
 * Classes that inherit from this interface can be
 * serialized to and from JSON.
 */
class JsonSerializable {
public:
    virtual ~JsonSerializable() = default;
    
    /**
     * @brief Serialize to JsonWrapper
     * @return JsonWrapper object
     */
    virtual JsonWrapper to_json() const = 0;
    
    /**
     * @brief Deserialize from JsonWrapper
     * @param json JsonWrapper object
     */
    virtual void from_json(const JsonWrapper& json) = 0;
    
    /**
     * @brief Serialize to string
     * @param pretty Whether to format with indentation
     * @return JSON string
     */
    std::string to_json_string(bool pretty = false) const {
        return to_json().to_string(pretty);
    }
    
    /**
     * @brief Deserialize from string
     * @param json_str JSON string
     * @return true if parsing succeeded, false otherwise
     */
    bool from_json_string(const std::string& json_str) {
        JsonWrapper json;
        if (!json.from_string(json_str)) {
            return false;
        }
        from_json(json);
        return true;
    }
};

} // namespace trade_ngin 