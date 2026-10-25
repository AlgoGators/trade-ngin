// include/trade_ngin/live/python_mail.hpp
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace trade_ngin::live {

/**
 * The book e-mail sent by a script instead of by the engine's own SMTP code.
 *
 * The engine builds the report (the HTML body, the chart PNGs, the CSVs); this hands them to
 * `scripts/send_book_email.py` through a manifest in a private temp directory, runs the script
 * with a time limit, and reports its exit status. The script owns the MIME message and the SMTP
 * conversation. The manifest holds the SMTP password, so the directory is 0700, the files are
 * 0600, and the directory is removed whatever the outcome.
 *
 * Exit 0 from the script means the server accepted the message; the caller then records the send
 * exactly as it does for the built-in sender (qt::record_sent).
 */
struct PythonMailImage {
    std::string cid;     ///< Content-ID the body refers to, without the angle brackets
    std::string base64;  ///< the PNG, base64-encoded (how the report keeps its charts)
    std::string file;    ///< or a PNG file read as it is (the logo); used when `base64` is empty
};

/// An inline image as the manifest lists it: a file holding base64 text or raw bytes.
struct InlineFile {
    std::string cid;
    std::string path;
    bool base64{false};
};

struct PythonMailRequest {
    std::string smtp_host;
    int smtp_port{587};
    std::string username;
    std::string password;
    std::string from_email;
    bool use_tls{true};
    std::vector<std::string> to;
    std::vector<std::string> cc;
    std::string subject;
    std::string body_html;
    std::vector<PythonMailImage> inline_images;
    std::vector<std::string> attachment_paths;  ///< CSVs; sent under their own file names
};

struct PythonMailSettings {
    std::string python{"python3"};
    std::string script{"/app/scripts/send_book_email.py"};
    std::chrono::milliseconds timeout{std::chrono::seconds(60)};
};

/// QT_MAIL_PYTHON, QT_MAIL_SCRIPT, QT_MAIL_TIMEOUT (seconds) over the defaults.
PythonMailSettings python_mail_settings_from_env();

/// True when QT_MAIL_BACKEND=python.
bool python_mail_selected();

/// Comma- or semicolon-separated addresses (QT_MAIL_CC); blanks dropped.
std::vector<std::string> split_addresses(const std::string& csv);

/// The manifest the script reads (the password included).
std::string build_manifest_json(const PythonMailRequest& request, const std::string& body_path,
                                const std::vector<InlineFile>& inline_files);

/// Send through the script. nullopt: delivered. Otherwise the reason (never the password).
std::optional<std::string> send_via_python(const PythonMailRequest& request,
                                           const PythonMailSettings& settings);

}  // namespace trade_ngin::live
