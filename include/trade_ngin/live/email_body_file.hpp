// The daily report's body as a file (T-NETTING; HD 2026-10-10: --send-email wins).
//
// TRADE_NGIN_EMAIL_BODY_DIR names a directory. On a run that does NOT send (no --send-email and a
// date given: every test run), a futures runner with the variable set builds the report body
// exactly as for a send and writes it there as email_body_<portfolio>_<date>.html, and mails
// nothing. On a run that sends (--send-email, or a run without a date: production), the variable
// is IGNORED: the report is built and mailed as if the variable did not exist, one WARN line says
// so, and no file is written. So the file path cannot run in production, and a variable left in a
// production environment cannot silence the daily email. An empty value is the same as unset.
// Without --send-email and without the variable nothing is built.

#pragma once

#include <fstream>
#include <string>

namespace trade_ngin {
namespace live {

inline constexpr const char* kEmailBodyDirEnv = "TRADE_NGIN_EMAIL_BODY_DIR";

/// What a runner does about the report at the end of the day.
struct EmailReportPlan {
    bool send{false};             ///< build the report and mail it
    bool write_body_file{false};  ///< build the report and write its body to body_dir; mail nothing
    std::string body_dir;         ///< where, when write_body_file
    bool variable_ignored{false}; ///< the variable is set and the report is being sent: warn
    std::string ignored_dir;      ///< its value, for the warning
    bool build() const { return send || write_body_file; }
};

/// `send_email` is the runner's own flag (--send-email, or a run without a date). `body_dir_env`
/// is std::getenv(kEmailBodyDirEnv): null or empty means not set.
inline EmailReportPlan plan_email_report(bool send_email, const char* body_dir_env) {
    EmailReportPlan plan;
    const std::string dir = body_dir_env ? body_dir_env : "";
    plan.send = send_email;
    if (send_email) {
        plan.variable_ignored = !dir.empty();
        plan.ignored_dir = dir;
        return plan;
    }
    plan.write_body_file = !dir.empty();
    plan.body_dir = dir;
    return plan;
}

/// The one WARN line of a sending run that found the variable set.
inline std::string email_body_dir_ignored_warning(const EmailReportPlan& plan) {
    return std::string("EMAIL_BODY_FILE ") + kEmailBodyDirEnv + " is set (" + plan.ignored_dir +
           ") and IGNORED because --send-email was given: the report is mailed as usual and no "
           "body file is written";
}

inline std::string email_body_file_path(const std::string& body_dir,
                                        const std::string& portfolio_id,
                                        const std::string& date) {
    return body_dir + "/email_body_" + portfolio_id + "_" + date + ".html";
}

/// Writes `body`, byte for byte, replacing the file. False when it could not be written.
inline bool write_email_body_file(const std::string& path, const std::string& body) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << body;
    file.close();
    return !file.fail();
}

}  // namespace live
}  // namespace trade_ngin
