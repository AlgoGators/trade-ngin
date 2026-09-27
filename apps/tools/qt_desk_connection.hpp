#pragma once
// Shared explicit local connection admission. No inherited connection defaults.
#include <libpq-fe.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <charconv>
#include <map>
#include <memory>
#include <string>

namespace trade_ngin::qt_desk_cli {
inline constexpr std::size_t max_connection_bytes = 4096;
inline bool canonical_uuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!(value[i] >= '0' && value[i] <= '9') &&
                   !(value[i] >= 'a' && value[i] <= 'f')) return false;
    }
    return true;
}

inline bool canonical_day(const std::string& value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (std::size_t i = 0; i < value.size(); ++i)
        if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9')) return false;
    const int year = std::stoi(value.substr(0, 4));
    const int month = std::stoi(value.substr(5, 2));
    const int day = std::stoi(value.substr(8, 2));
    if (year == 0 || month < 1 || month > 12 || day < 1) return false;
    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}

inline bool read_connection(int fd, std::string& material) {
    struct stat info {};
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || fstat(fd, &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_size < 1 ||
        info.st_size > static_cast<off_t>(max_connection_bytes)) return false;
    material.resize(static_cast<std::size_t>(info.st_size));
    const auto count = pread(fd, material.data(), material.size(), 0);
    close(fd);
    if (count != static_cast<ssize_t>(material.size())) return false;
    return std::all_of(material.begin(), material.end(),
                       [](unsigned char c) { return c >= 32 && c <= 126; });
}

inline std::string quote_connection_value(const std::string& value) {
    std::string out("'");
    for (char c : value) {
        if (c == '\\' || c == '\'') out.push_back('\\');
        out.push_back(c);
    }
    return out + "'";
}

inline bool explicit_local_connection(const std::string& material, std::string& dsn) {
    char* error = nullptr;
    std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)> parsed(
        PQconninfoParse(material.c_str(), &error), &PQconninfoFree);
    if (error) PQfreemem(error);
    if (!parsed) return false;
    std::map<std::string, std::string> supplied;
    for (auto* option = parsed.get(); option->keyword; ++option) {
        if (!option->val) continue;
        const std::string key(option->keyword);
        if (key != "host" && key != "port" && key != "dbname" && key != "user" &&
            key != "password" && key != "connect_timeout") return false;
        supplied.emplace(key, option->val);
    }
    for (const char* key : {"host", "dbname", "user", "password"})
        if (!supplied.contains(key)) return false;
    const auto& host = supplied.at("host");
    if (host.empty() || host.front() != '/' || host.find(',') != std::string::npos ||
        supplied.at("dbname").empty() || supplied.at("user").empty()) return false;
    // Override every optional input: no service, credential-file or env fallback.
    dsn = "host=" + quote_connection_value(host) + " dbname=" + quote_connection_value(supplied.at("dbname")) +
          " user=" + quote_connection_value(supplied.at("user")) +
          " password=" + quote_connection_value(supplied.at("password"));
    const auto port = supplied.contains("port") ? supplied.at("port") : "5432";
    int port_number = 0;
    const auto parsed_port = std::from_chars(port.data(), port.data() + port.size(), port_number);
    if (parsed_port.ec != std::errc() || parsed_port.ptr != port.data() + port.size() ||
        port_number < 1 || port_number > 65535) return false;
    dsn += " port=" + quote_connection_value(port) +
           " connect_timeout=2 passfile=/dev/null sslmode=disable gssencmode=disable application_name=qt_desk_run";
    return true;
}
}  // namespace trade_ngin::qt_desk_cli
