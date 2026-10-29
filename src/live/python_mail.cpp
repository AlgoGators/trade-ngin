// src/live/python_mail.cpp
#include "trade_ngin/live/python_mail.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "trade_ngin/core/timed_process.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace trade_ngin::live {

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

// One shell word. POSIX: single quotes, each embedded quote written as quote-backslash-quote-quote.
// Windows (cmd.exe): double quotes.
std::string shell_quote(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    return out + "'";
#endif
}

// One private directory per call (0700), removed with everything in it when it goes out of scope.
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec);
        if (ec) return;
#ifdef _WIN32
        const unsigned long pid = 0;
#else
        const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
        path_ = base / ("tn_mail_" + std::to_string(pid) + "_" + std::to_string(counter++));
        ok_ = fs::create_directories(path_, ec) && !ec;
        if (ok_) fs::permissions(path_, fs::perms::owner_all, fs::perm_options::replace, ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        if (!path_.empty()) fs::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    bool ok() const { return ok_; }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
    bool ok_{false};
};

bool write_private(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << data;
    f.close();
    if (!f) return false;
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace,
                    ec);
    return true;
}

// The last non-empty line of the module's output: where it states the reason.
std::string last_line(const std::string& out) {
    std::string line, last;
    std::istringstream in(out);
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) last = line;
    }
    return last;
}

}  // namespace

PythonMailSettings python_mail_settings_from_env() {
    PythonMailSettings s;
    s.python = env_or("QT_MAIL_PYTHON", s.python);
    s.app_dir = env_or("QT_MAIL_APP", s.app_dir);
    const std::string t = env_or("QT_MAIL_TIMEOUT", "");
    if (!t.empty()) {
        try {
            const long sec = std::stol(t);
            if (sec > 0) s.timeout = std::chrono::seconds(sec);
        } catch (const std::exception&) {
            // an unreadable value keeps the default
        }
    }
    return s;
}

bool python_mail_selected() { return env_or("QT_MAIL_BACKEND", "") == "python"; }

std::string python_mail_test_to() { return env_or("QT_MAIL_TEST_TO", ""); }

std::string python_mail_manifest_json(const PythonMailRequest& r) {
    nlohmann::json m;
    m["version"] = 1;
    m["portfolio_id"] = r.portfolio_id;
    m["date"] = r.date;
    m["subject"] = r.subject;
    m["body_html"] = r.body_html;
    m["images"] = nlohmann::json::array();
    for (const auto& [cid, data] : r.images) {
        m["images"].push_back({{"cid", cid}, {"base64", data}});
    }
    m["attachments"] = r.attachments;
    // Invalid UTF-8 in the body (a symbol name, a reason text) is replaced, never a failed send.
    return m.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string python_mail_command(const PythonMailRequest& r, const PythonMailSettings& s,
                                const std::string& manifest_path) {
    std::string cmd = "env PYTHONPATH=" + shell_quote(s.app_dir) +
                      " PYTHONDONTWRITEBYTECODE=1 " + shell_quote(s.python) +
                      " -m algogators_rpc.services.report --portfolio-id " +
                      shell_quote(r.portfolio_id) + " --date " + shell_quote(r.date) +
                      " --manifest " + shell_quote(manifest_path);
    if (!r.test_to.empty()) cmd += " --to " + shell_quote(r.test_to);
    return cmd + " 2>&1";
}

std::optional<std::string> send_via_python(const PythonMailRequest& r,
                                           const PythonMailSettings& settings) {
    try {
        ScratchDir scratch;
        if (!scratch.ok()) return std::string("the mail manifest directory could not be created");
        const fs::path manifest = scratch.path() / "manifest.json";
        if (!write_private(manifest, python_mail_manifest_json(r))) {
            return std::string("the mail manifest could not be written");
        }
        const auto result = core::run_with_timeout(
            python_mail_command(r, settings, manifest.string()), settings.timeout);
        if (!result.started) return std::string("the mail module could not be started");
        if (result.timed_out) {
            return "the mail module did not finish within " +
                   std::to_string(
                       std::chrono::duration_cast<std::chrono::seconds>(settings.timeout).count()) +
                   " s";
        }
        if (result.status != 0) {
            const std::string why = last_line(result.output);
            return "the mail module failed (status " + std::to_string(result.status) + ")" +
                   (why.empty() ? std::string() : ": " + why);
        }
        return std::nullopt;
    } catch (const std::exception& e) {
        return std::string("the mail module could not be run: ") + e.what();
    }
}

}  // namespace trade_ngin::live
