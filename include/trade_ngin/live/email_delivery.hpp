// The daily report's last step, for the three live runners.
//
// The runner builds the body; this puts the day's flags on it (a day the risk module could not
// answer, a carried day), then either writes the body file or sends the mail, as the report plan
// says, and tells the runner whether it worked. The body written is the body sent. A report that
// was not built, not written or not sent makes the run exit kEmailFailureExitCode; nothing the
// day stored changes (the stores happen before the report).

#pragma once

#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "trade_ngin/live/carried_day.hpp"
#include "trade_ngin/live/email_body_file.hpp"
#include "trade_ngin/live/risk_module_failure.hpp"

namespace trade_ngin {
namespace live {

/// The exit code of a run whose report failed. 3 is a risk-module failure with the day stored.
inline constexpr int kEmailFailureExitCode = 4;

/// What happened to the report. `ok` is false for an empty body, a body file that could not be
/// written, or a send that returned an error; `message` then says which, and `subject` and
/// `body` are what was written or sent.
struct EmailDelivery {
    bool ok{false};
    std::string message;
    std::string subject;
    std::string body;
};

/// The day's flags on the subject and the body: the risk-module failure (subject and top of body)
/// and the carried day (top of body). `carried_day_note` is empty when the day was not carried.
inline void apply_day_flags(std::string& subject, std::string& body,
                            const std::optional<nlohmann::json>& risk_module_failure,
                            bool carried_day, const std::string& carried_day_note) {
    if (risk_module_failure) {
        subject = risk_module_failure_email_subject(subject);
        body = flag_email_body_for_risk_module_failure(body, *risk_module_failure);
    }
    // A carried day's email says it in the report's own plain note ("No Executions: No
    // session was found for yesterday ..."); no banner is added for it.
    (void)carried_day;
    (void)carried_day_note;
}

/// `send` mails (subject, body) and returns nothing on success or the error's text.
using EmailSendFn =
    std::function<std::optional<std::string>(const std::string& subject, const std::string& body)>;

inline EmailDelivery deliver_email_report(const EmailReportPlan& plan,
                                          const std::string& portfolio_id, const std::string& date,
                                          std::string subject, std::string body,
                                          const std::optional<nlohmann::json>& risk_module_failure,
                                          bool carried_day, const std::string& carried_day_note,
                                          const EmailSendFn& send) {
    EmailDelivery out;
    if (body.empty()) {
        out.message = "the report body is empty";
        return out;
    }
    apply_day_flags(subject, body, risk_module_failure, carried_day, carried_day_note);
    out.subject = subject;
    out.body = body;
    if (plan.write_body_file) {
        const std::string path = email_body_file_path(plan.body_dir, portfolio_id, date);
        if (!write_email_body_file(path, body)) {
            out.message = "EMAIL_BODY_FILE could not write " + path;
            return out;
        }
        out.ok = true;
        out.message = "EMAIL_BODY_FILE wrote " + path + " (" + std::to_string(body.size()) +
                      " bytes); nothing is mailed";
        return out;
    }
    const std::optional<std::string> error = send(subject, body);
    if (error) {
        out.message = "Failed to send email: " + *error;
        return out;
    }
    out.ok = true;
    out.message = "Email report sent successfully";
    return out;
}

/// What a futures runner returns: a risk-module failure keeps its own code (3: the day is stored,
/// the book held); otherwise a report that failed returns kEmailFailureExitCode.
inline int futures_run_exit_code(const std::optional<nlohmann::json>& risk_module_failure,
                                 bool email_failed) {
    if (risk_module_failure) return live_run_exit_code(risk_module_failure);
    return email_failed ? kEmailFailureExitCode : 0;
}

}  // namespace live
}  // namespace trade_ngin
