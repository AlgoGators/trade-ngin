// include/trade_ngin/core/record_file.hpp
#pragma once

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <system_error>

#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {
namespace record_file {

/**
 * @brief How every file of the acceptance record is written (the directory TRADE_NGIN_SERIES_DUMP_DIR
 *        names; a production run never sets it): a run starts from empty files, and a failed write
 *        is reported.
 *
 * The record is nine files: calendar, hold, withheld, series, history and final_marks (the consumed
 * series record, written once at the end of a backtest) and onepass_days_<id>, onepass_book_<id>
 * and estimator_<id> (one row at a time, by the rebalance and the trend sleeve). begin_run removes
 * every one of them from the directory, so a second run into a directory, or a run that stops
 * before its last write, never leaves one run's rows beside another's. The first row a process
 * writes into a directory begins the run there if nothing has; a backtest begins it when the
 * record is enabled. A file that cannot be removed, opened or written is reported once, at WARNING.
 */
inline bool is_record_file(const std::string& name) {
    for (const char* fixed : {"calendar.csv", "hold.csv", "withheld.csv", "series.csv",
                              "history.csv", "final_marks.csv"}) {
        if (name == fixed) return true;
    }
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".csv") != 0) return false;
    for (const char* prefix : {"onepass_days_", "onepass_book_", "estimator_"}) {
        if (name.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

namespace detail {

struct State {
    std::mutex mutex;
    std::set<std::string> begun;     // directories whose run has begun in this process
    std::set<std::string> reported;  // paths already reported
};

inline State& state() {
    static State s;
    return s;
}

inline void report(State& s, const std::string& path, const std::string& what) {
    if (!s.reported.insert(path).second) return;
    WARN("ACCEPTANCE_RECORD " + path + ": " + what +
         "; this run's acceptance record is incomplete and must not be compared");
}

inline bool begin(State& s, const std::string& dir) {
    s.begun.insert(dir);
    bool clean = true;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec), end;
    if (ec) {
        report(s, dir, "the directory could not be read (" + ec.message() + ")");
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            report(s, dir, "the directory could not be read (" + ec.message() + ")");
            return false;
        }
        const std::string name = it->path().filename().string();
        if (!is_record_file(name)) continue;
        std::error_code removed;
        if (!std::filesystem::remove(it->path(), removed) || removed) {
            report(s, it->path().string(),
                   "a file of an earlier run could not be removed (" + removed.message() + ")");
            clean = false;
        }
    }
    return clean;
}

}  // namespace detail

/// Begins a run's record in the directory: every record file left there is removed.
inline bool begin_run(const std::string& dir) {
    auto& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return detail::begin(s, dir);
}

/// Reports a record file that could not be opened or written (once per path).
inline void report_failure(const std::string& path, const std::string& what) {
    auto& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::report(s, path, what);
}

/// Opens <dir>/<name> to add rows, with the header line when the file is new. nullptr, reported,
/// when it cannot be opened.
inline std::FILE* open_rows(const std::string& dir, const std::string& name,
                            const std::string& header) {
    auto& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.begun.count(dir) == 0) detail::begin(s, dir);
    const std::string path = dir + "/" + name;
    std::error_code ec;
    const bool fresh = !std::filesystem::exists(path, ec);
    std::FILE* out = std::fopen(path.c_str(), "a");
    if (out == nullptr) {
        detail::report(s, path, "the file could not be opened");
        return nullptr;
    }
    if (fresh) std::fprintf(out, "%s\n", header.c_str());
    return out;
}

/// Closes a file open_rows returned; a row that did not reach it is reported.
inline void close_rows(std::FILE* out, const std::string& dir, const std::string& name) {
    if (out == nullptr) return;
    const bool failed = std::ferror(out) != 0;
    if (std::fclose(out) != 0 || failed) {
        auto& s = detail::state();
        std::lock_guard<std::mutex> lock(s.mutex);
        detail::report(s, dir + "/" + name, "a row could not be written");
    }
}

}  // namespace record_file
}  // namespace trade_ngin
