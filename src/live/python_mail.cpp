// src/live/python_mail.cpp
#include "trade_ngin/live/python_mail.hpp"

#include <cstdlib>
#include <sstream>

#include "trade_ngin/core/timed_process.hpp"

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

std::string python_mail_test_to() { return env_or("QT_MAIL_TEST_TO", ""); }

std::string python_mail_command(const PythonMailRequest& r, const PythonMailSettings& s) {
    std::string cmd = "env PYTHONPATH=" + shell_quote(s.app_dir) +
                      " PYTHONDONTWRITEBYTECODE=1 MPLCONFIGDIR=/tmp/mpl " + shell_quote(s.python) +
                      " -m algogators_rpc.services.report --portfolio-id " +
                      shell_quote(r.portfolio_id) + " --date " + shell_quote(r.date);
    if (r.no_session) cmd += " --no-session";
    if (!r.alert.empty()) cmd += " --alert " + shell_quote(r.alert);
    for (const auto& path : r.attachments) {
        if (!path.empty()) cmd += " --attach " + shell_quote(path);
    }
    if (!r.test_to.empty()) cmd += " --to " + shell_quote(r.test_to);
    return cmd + " 2>&1";
}

std::optional<std::string> send_via_python(const PythonMailRequest& r,
                                           const PythonMailSettings& settings) {
    try {
        const auto result =
            core::run_with_timeout(python_mail_command(r, settings), settings.timeout);
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
