// include/trade_ngin/live/python_mail.hpp
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/live/risk_module_failure.hpp"

namespace trade_ngin::live {

/// The runner's exit code when the day is stored but the book e-mail was not sent (PR #205).
inline constexpr int kBookEmailFailureExitCode = 4;

/// What a futures runner returns: a risk-module failure keeps its own code (3: the day is stored,
/// the book held); otherwise a book e-mail that was not sent returns kBookEmailFailureExitCode.
inline int futures_run_exit_code(const std::optional<nlohmann::json>& risk_module_failure,
                                 bool email_failed) {
    if (risk_module_failure) return live_run_exit_code(risk_module_failure);
    return email_failed ? kBookEmailFailureExitCode : 0;
}

/**
 * The book e-mail: the only way a live runner reports its day. The report module
 * (services/rpc/algogators_rpc/services/report) reads the portfolio's stored day from the
 * database, renders it (HTML, charts) and sends it over the portfolio's SMTP settings
 * (email.json). The engine says which portfolio and day, adds what only the run knows (no session
 * the day before, a risk module that held the book) and the files it exported, runs the module
 * with a time limit and reads its exit status: 0 means the server accepted the message, and the
 * caller then records the send (qt::record_sent) as it always has.
 *
 * The module is run as `<python> -m algogators_rpc.services.report ...` with PYTHONPATH=<app>;
 * credentials stay in the process environment and the config files, never in the command line.
 */
struct PythonMailRequest {
    std::string portfolio_id;
    std::string date;  ///< YYYY-MM-DD, the day being sent
    bool no_session{false};  ///< the run found no session on the day before and carried its book
    std::string alert;       ///< the run's risk-module failure (risk_module_failure_text), or empty
    std::vector<std::string> attachments;  ///< files the run exported, as paths
    /// QT_MAIL_TEST_TO: send to this one address (no Cc). Empty: the portfolio's recipients.
    std::string test_to;
};

struct PythonMailSettings {
    std::string python{"/opt/rpc/venv/bin/python"};
    std::string app_dir{"/opt/rpc/app"};
    std::chrono::milliseconds timeout{std::chrono::seconds(120)};
};

/// QT_MAIL_PYTHON, QT_MAIL_APP, QT_MAIL_TIMEOUT (seconds) over the defaults.
PythonMailSettings python_mail_settings_from_env();

/// QT_MAIL_TEST_TO: a test send goes to this one address and is never recorded as the day's
/// send (sent_at stays empty, a day already sent can be sent again). Empty when unset.
std::string python_mail_test_to();

/// The shell command that runs the module for this request.
std::string python_mail_command(const PythonMailRequest& request,
                                const PythonMailSettings& settings);

/// Send through the module. nullopt: delivered. Otherwise the reason (never a password).
std::optional<std::string> send_via_python(const PythonMailRequest& request,
                                           const PythonMailSettings& settings);

}  // namespace trade_ngin::live
