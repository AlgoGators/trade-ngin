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

void write_private(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << data;
    f.close();
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace,
                    ec);
}

// One temp directory per call, removed when it goes out of scope.
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec);
#ifdef _WIN32
        const unsigned long pid = 0;
#else
        const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
        path_ = base / ("tn_mail_" + std::to_string(pid) + "_" + std::to_string(counter++));
        ok_ = fs::create_directories(path_, ec) && !ec;
        if (ok_) {
            fs::permissions(path_, fs::perms::owner_all, fs::perm_options::replace, ec);
        }
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    bool ok() const { return ok_; }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
    bool ok_{false};
};

// The last non-empty line of the script's output: where it states the reason.
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
    s.script = env_or("QT_MAIL_SCRIPT", s.script);
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

std::vector<std::string> split_addresses(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        const auto b = cur.find_first_not_of(" \t");
        const auto e = cur.find_last_not_of(" \t");
        if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
        cur.clear();
    };
    for (char c : csv) {
        if (c == ',' || c == ';') {
            flush();
        } else {
            cur += c;
        }
    }
    flush();
    return out;
}

std::string build_manifest_json(
    const PythonMailRequest& r, const std::string& body_path,
    const std::vector<InlineFile>& inline_files) {
    nlohmann::json m;
    m["smtp"] = {{"host", r.smtp_host},        {"port", r.smtp_port},
                 {"username", r.username},     {"password", r.password},
                 {"from_email", r.from_email}, {"use_tls", r.use_tls}};
    m["subject"] = r.subject;
    m["to"] = r.to;
    m["cc"] = r.cc;
    m["body_html_path"] = body_path;
    m["inline"] = nlohmann::json::array();
    for (const auto& f : inline_files) {
        m["inline"].push_back({{"cid", f.cid}, {"path", f.path}, {"base64", f.base64}});
    }
    m["attachments"] = nlohmann::json::array();
    for (const auto& p : r.attachment_paths) {
        m["attachments"].push_back(
            {{"path", p}, {"filename", fs::path(p).filename().string()}, {"mime", "text/csv"}});
    }
    return m.dump();
}

std::optional<std::string> send_via_python(const PythonMailRequest& r,
                                           const PythonMailSettings& settings) {
    ScratchDir dir;
    if (!dir.ok()) return std::string("could not create a temp directory for the mail script");

    try {
        const fs::path body = dir.path() / "body.html";
        write_private(body, r.body_html);

        std::vector<InlineFile> inline_files;
        int n = 0;
        for (const auto& img : r.inline_images) {
            if (!img.base64.empty()) {
                const fs::path p = dir.path() / ("img" + std::to_string(n++) + ".b64");
                write_private(p, img.base64);
                inline_files.push_back({img.cid, p.string(), true});
            } else if (!img.file.empty()) {
                inline_files.push_back({img.cid, img.file, false});
            }
        }

        const fs::path manifest = dir.path() / "manifest.json";
        write_private(manifest, build_manifest_json(r, body.string(), inline_files));

        const std::string command = shell_quote(settings.python) + " " +
                                    shell_quote(settings.script) + " --manifest " +
                                    shell_quote(manifest.string()) + " 2>&1";
        const auto result = core::run_with_timeout(command, settings.timeout);
        if (!result.started) return std::string("the mail script could not be started");
        if (result.timed_out) {
            return "the mail script did not finish within " +
                   std::to_string(
                       std::chrono::duration_cast<std::chrono::seconds>(settings.timeout).count()) +
                   " s";
        }
        if (result.status != 0) {
            const std::string why = last_line(result.output);
            return "the mail script failed (status " + std::to_string(result.status) + ")" +
                   (why.empty() ? std::string() : ": " + why);
        }
        return std::nullopt;
    } catch (const std::exception& e) {
        return std::string("the mail script could not be run: ") + e.what();
    }
}

}  // namespace trade_ngin::live
