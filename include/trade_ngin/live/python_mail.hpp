// include/trade_ngin/live/python_mail.hpp
#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace trade_ngin::live {

/**
 * The book e-mail sent by the report module (services/rpc/algogators_rpc/services/report).
 *
 * The module reads the portfolio's day from the database, renders the report (HTML, charts) and
 * sends it over the portfolio's SMTP settings (email.json). The engine only says which portfolio
 * and day, runs the module with a time limit, and reads its exit status: 0 means the server
 * accepted the message, and the caller then records the send (qt::record_sent) as it always has.
 * The module is run as `<python> -m algogators_rpc.services.report ...` with PYTHONPATH=<app>;
 * credentials stay in the process environment and the config files, never in the command line.
 */
struct PythonMailRequest {
    std::string portfolio_id;
    std::string date;  ///< YYYY-MM-DD, the day being sent
};

struct PythonMailSettings {
    std::string python{"/opt/rpc/venv/bin/python"};
    std::string app_dir{"/opt/rpc/app"};
    std::chrono::milliseconds timeout{std::chrono::seconds(120)};
};

/// QT_MAIL_PYTHON, QT_MAIL_APP, QT_MAIL_TIMEOUT (seconds) over the defaults.
PythonMailSettings python_mail_settings_from_env();

/// True when QT_MAIL_BACKEND=python.
bool python_mail_selected();

/// The shell command that runs the module for this request.
std::string python_mail_command(const PythonMailRequest& request,
                                const PythonMailSettings& settings);

/// Send through the module. nullopt: delivered. Otherwise the reason (never a password).
std::optional<std::string> send_via_python(const PythonMailRequest& request,
                                           const PythonMailSettings& settings);

}  // namespace trade_ngin::live
