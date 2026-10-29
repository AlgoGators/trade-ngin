// include/trade_ngin/live/python_mail.hpp
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace trade_ngin::live {

/**
 * The book e-mail sent by the report module (services/rpc/algogators_rpc/services/report).
 *
 * The engine builds the report exactly as it always has (EmailSender::generate_trading_report_body,
 * the day's flags, the charts, the CSV files) and hands all of it to the module in a manifest: the
 * module only lays it out in the fund's design and sends it over the portfolio's SMTP settings
 * (email.json). Every figure, banner and footnote is the engine's; nothing is recomputed. The
 * module's exit status 0 means the server accepted the message, and the caller then records the
 * send (qt::record_sent) as it always has.
 *
 * The module is run as `<python> -m algogators_rpc.services.report --manifest <file> ...` with
 * PYTHONPATH=<app>. The manifest sits in a private directory (0700, the file 0600) that is
 * removed after the call; credentials stay in the process environment and the config files,
 * never in the manifest or the command line.
 */
struct PythonMailRequest {
    std::string portfolio_id;
    std::string date;       ///< YYYY-MM-DD, the day being sent
    std::string subject;    ///< as the engine would send it: the day's flags and the QT tag applied
    std::string body_html;  ///< the engine's report body
    /// The charts the body refers to, {Content-ID, base64 PNG} (EmailSender::chart_images).
    std::vector<std::pair<std::string, std::string>> images;
    std::vector<std::string> attachments;  ///< the CSV files, as paths
    /// QT_MAIL_TEST_TO: send to this address only (no Cc). Empty: the portfolio's recipients.
    std::string test_to;
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

/// QT_MAIL_TEST_TO: a test send goes to this one address and is never recorded as the day's
/// send (sent_at stays empty, a day already sent can be sent again). Empty when unset.
std::string python_mail_test_to();

/// The manifest the module reads (JSON): subject, body, images, attachments.
std::string python_mail_manifest_json(const PythonMailRequest& request);

/// The shell command that runs the module for this request and manifest file.
std::string python_mail_command(const PythonMailRequest& request,
                                const PythonMailSettings& settings,
                                const std::string& manifest_path);

/// Send through the module. nullopt: delivered. Otherwise the reason (never a password).
std::optional<std::string> send_via_python(const PythonMailRequest& request,
                                           const PythonMailSettings& settings);

}  // namespace trade_ngin::live
