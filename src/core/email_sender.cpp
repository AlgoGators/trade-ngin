#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/report_risk_lines.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <vector>
#include "trade_ngin/core/chart_generator.hpp"
#include "trade_ngin/core/email_netting_text.hpp"
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/report_charts.hpp"
#include "trade_ngin/core/report_day_before.hpp"
#include "trade_ngin/core/report_held_symbols.hpp"
#include "trade_ngin/core/report_price_text.hpp"
#include "trade_ngin/core/report_settled_marks.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/transaction_cost/netting.hpp"

namespace trade_ngin {

namespace {
// The positions in symbol order. The map hands them out in hash order, which moves with the
// build; the rows and every running total are walked in this order instead (T-EMAIL P2-3).
std::vector<std::pair<std::string, const Position*>> positions_by_symbol(
    const std::unordered_map<std::string, Position>& positions) {
    std::vector<std::pair<std::string, const Position*>> ordered;
    ordered.reserve(positions.size());
    for (const auto& [symbol, position] : positions)
        ordered.emplace_back(symbol, &position);
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return ordered;
}
// The numbered footnotes of the report (HD 10-13/14): a small number at the top right of a value; the
// line for it sits right below the block it belongs to. The numbers follow the order the notes first
// appear in the email (1, 2, 3 ...), whatever their kind; a kind is numbered when first used.
//   kind 1 no price that day, 2 contract roll, 3 price rejected, 4 no price bar (last price shown),
//   5 netting arithmetic.
thread_local std::map<int, int> g_note_number;  // kind -> number in this email
thread_local std::set<int> g_note_flushed;       // kinds whose line was already printed
thread_local std::string g_netting_note;         // the text of kind 5, set by the executions tables
thread_local std::string g_single_title;   // an equity book's one strategy: its name, as a futures sleeve has
thread_local bool g_single_sleeve = false;       // one strategy: its summary line would repeat the book block
std::string note_sup(int kind) {
    auto it = g_note_number.find(kind);
    if (it == g_note_number.end())
        it = g_note_number.emplace(kind, static_cast<int>(g_note_number.size()) + 1).first;
    return "<sup style=\"font-size: 9px; color: #000;\">" + std::to_string(it->second) + "</sup>";
}
// The lines of the notes used since the last call, in number order: printed under the block just ended.
std::string notes_html() {
    static const char* const kText[] = {
        "", "No price received that day. P&amp;L is logged as 0 and updates on later days.",
        "Contract rolled that day. P&amp;L is not booked on the roll.",
        "Price rejected: too little trading volume that day. P&amp;L is booked with the next accepted price.",
        "No price bar for this contract that day. Its last known price is shown.", ""};
    std::map<int, int> by_number;
    for (const auto& [kind, number] : g_note_number)
        if (!g_note_flushed.count(kind)) by_number[number] = kind;
    std::string html;
    for (const auto& [number, kind] : by_number) {
        const std::string text = kind == 5 ? g_netting_note : kText[kind];
        g_note_flushed.insert(kind);
        if (text.empty()) continue;
        html += "<div style=\"font-size: 12px; color: #666; margin: 2px 0;\">" +
                std::to_string(number) + " " + text + "</div>\n";
    }
    return html;
}
std::string single_heading_html() {
    if (g_single_title.empty()) return "";
    return "<h3 style=\"margin-top: 20px; margin-bottom: 2px; color: #333; border-left: 4px solid "
           "#2c5aa0; padding-left: 12px;\">" + g_single_title + "</h3>\n"
           "<div style=\"font-size: 11px; color: #999; margin: 0 0 8px 16px;\">Equities</div>\n";
}
struct SingleSleeveScope {
    ~SingleSleeveScope() { g_single_sleeve = false; }
};
void reset_notes() {
    g_single_sleeve = false;
    g_single_title.clear();
    g_note_number.clear();
    g_note_flushed.clear();
    g_netting_note.clear();
}
}  // namespace

void EmailSender::reset_footnotes() {
    reset_notes();
}

std::string EmailSender::netting_footnote_text() {
    return g_netting_note;
}

// Global variables for curl email payload
std::string g_email_payload;
size_t g_payload_pos = 0;

size_t read_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    (void)userdata;
    size_t buffer_size = size * nitems;
    size_t remaining = g_email_payload.size() - g_payload_pos;
    size_t copy_size = std::min(buffer_size, remaining);

    if (copy_size > 0) {
        std::memcpy(buffer, g_email_payload.c_str() + g_payload_pos, copy_size);
        g_payload_pos += copy_size;
    }
    return copy_size;
}

EmailSender::EmailSender(std::shared_ptr<CredentialStore> credentials)
    : credentials_(std::move(credentials)),
      initialized_(false),
      holiday_checker_(HolidayChecker::resolve_holidays_path()) {}

EmailSender::EmailSender(const EmailSenderConfig& config)
    : credentials_(nullptr),
      config_(config),
      initialized_(false),
      holiday_checker_(HolidayChecker::resolve_holidays_path()) {}

Result<void> EmailSender::initialize() {
    if (credentials_) {
        auto load_result = load_config();
        if (load_result.is_error()) {
            return load_result;
        }
    }
    initialized_ = true;
    INFO("Email sender initialized successfully");
    return Result<void>();
}

Result<void> EmailSender::load_config() {
    try {
        // Load SMTP host
        auto smtp_host_result = credentials_->get<std::string>("email", "smtp_host");
        if (smtp_host_result.is_error()) {
            return make_error<void>(
                ErrorCode::INVALID_DATA,
                "Failed to get SMTP host: " + smtp_host_result.error()->to_string(), "EmailSender");
        }
        config_.smtp_host = smtp_host_result.value();

        // Load SMTP port
        auto smtp_port_result = credentials_->get<int>("email", "smtp_port");
        if (smtp_port_result.is_error()) {
            return make_error<void>(
                ErrorCode::INVALID_DATA,
                "Failed to get SMTP port: " + smtp_port_result.error()->to_string(), "EmailSender");
        }
        config_.smtp_port = smtp_port_result.value();

        // Load username
        auto username_result = credentials_->get<std::string>("email", "username");
        if (username_result.is_error()) {
            return make_error<void>(
                ErrorCode::INVALID_DATA,
                "Failed to get email username: " + username_result.error()->to_string(),
                "EmailSender");
        }
        config_.username = username_result.value();

        // Load password
        auto password_result = credentials_->get<std::string>("email", "password");
        if (password_result.is_error()) {
            return make_error<void>(
                ErrorCode::INVALID_DATA,
                "Failed to get email password: " + password_result.error()->to_string(),
                "EmailSender");
        }
        config_.password = password_result.value();

        // Load from email
        auto from_email_result = credentials_->get<std::string>("email", "from_email");
        if (from_email_result.is_error()) {
            return make_error<void>(
                ErrorCode::INVALID_DATA,
                "Failed to get from email: " + from_email_result.error()->to_string(),
                "EmailSender");
        }
        config_.from_email = from_email_result.value();

        // Load to emails
        auto to_emails_result = credentials_->get<std::vector<std::string>>("email", "to_emails");
        if (to_emails_result.is_error()) {
            return make_error<void>(
                ErrorCode::INVALID_DATA,
                "Failed to get to emails: " + to_emails_result.error()->to_string(), "EmailSender");
        }
        config_.to_emails = to_emails_result.value();

        // Load TLS setting
        auto use_tls_result = credentials_->get<bool>("email", "use_tls");
        if (use_tls_result.is_error()) {
            return make_error<void>(
                ErrorCode::INVALID_DATA,
                "Failed to get TLS setting: " + use_tls_result.error()->to_string(), "EmailSender");
        }
        config_.use_tls = use_tls_result.value();

        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "Failed to load email configuration: " + std::string(e.what()),
                                "EmailSender");
    }
}

Result<void> EmailSender::send_email(const std::string& subject, const std::string& body,
                                     bool is_html,
                                     const std::vector<std::string>& attachment_paths) {
    if (!initialized_) {
        return make_error<void>(ErrorCode::NOT_INITIALIZED, "Email sender not initialized",
                                "EmailSender");
    }

    CURL* curl;
    CURLcode res;

    curl = curl_easy_init();
    if (!curl) {
        return make_error<void>(ErrorCode::DATABASE_ERROR, "Failed to initialize curl",
                                "EmailSender");
    }

    try {
        // Set basic options
        std::string smtp_url =
            "smtp://" + config_.smtp_host + ":" + std::to_string(config_.smtp_port);
        curl_easy_setopt(curl, CURLOPT_URL, smtp_url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERNAME, config_.username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, config_.password.c_str());
        curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);  // Disable verbose output for production

        // Set mail options
        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, config_.from_email.c_str());

        struct curl_slist* recipients = nullptr;
        for (const auto& email : config_.to_emails) {
            recipients = curl_slist_append(recipients, email.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

        // Read logo file and encode to base64
        std::string logo_path = "assets/Algo.png";
        std::ifstream logo_file(logo_path, std::ios::binary);
        std::string logo_base64;

        if (logo_file.is_open()) {
            std::vector<unsigned char> logo_data((std::istreambuf_iterator<char>(logo_file)),
                                                 std::istreambuf_iterator<char>());
            logo_file.close();

            // Simple base64 encoding
            static const char base64_chars[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz"
                "0123456789+/";

            int i = 0;
            int j = 0;
            unsigned char char_array_3[3];
            unsigned char char_array_4[4];

            for (size_t idx = 0; idx < logo_data.size(); idx++) {
                char_array_3[i++] = logo_data[idx];
                if (i == 3) {
                    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                    char_array_4[1] =
                        ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                    char_array_4[2] =
                        ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                    char_array_4[3] = char_array_3[2] & 0x3f;

                    for (i = 0; i < 4; i++)
                        logo_base64 += base64_chars[char_array_4[i]];
                    i = 0;
                }
            }

            if (i) {
                for (j = i; j < 3; j++)
                    char_array_3[j] = '\0';

                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

                for (j = 0; j < i + 1; j++)
                    logo_base64 += base64_chars[char_array_4[j]];

                while (i++ < 3)
                    logo_base64 += '=';
            }
        }

        // Read and encode CSV attachments if provided
        std::vector<std::pair<std::string, std::string>> attachments;  // filename, base64_data
        for (const auto& attachment_path : attachment_paths) {
            std::ifstream csv_file(attachment_path, std::ios::binary);
            if (csv_file.is_open()) {
                std::vector<unsigned char> csv_data((std::istreambuf_iterator<char>(csv_file)),
                                                    std::istreambuf_iterator<char>());
                csv_file.close();

                // Base64 encode CSV data
                std::string csv_base64;
                static const char base64_chars[] =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                    "abcdefghijklmnopqrstuvwxyz"
                    "0123456789+/";

                int i = 0;
                int j = 0;
                unsigned char char_array_3[3];
                unsigned char char_array_4[4];

                for (size_t idx = 0; idx < csv_data.size(); idx++) {
                    char_array_3[i++] = csv_data[idx];
                    if (i == 3) {
                        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                        char_array_4[1] =
                            ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                        char_array_4[2] =
                            ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                        char_array_4[3] = char_array_3[2] & 0x3f;

                        for (i = 0; i < 4; i++)
                            csv_base64 += base64_chars[char_array_4[i]];
                        i = 0;
                    }
                }

                if (i) {
                    for (j = i; j < 3; j++)
                        char_array_3[j] = '\0';

                    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                    char_array_4[1] =
                        ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                    char_array_4[2] =
                        ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

                    for (j = 0; j < i + 1; j++)
                        csv_base64 += base64_chars[char_array_4[j]];

                    while (i++ < 3)
                        csv_base64 += '=';
                }

                // Extract filename from path
                size_t last_slash = attachment_path.find_last_of("/\\");
                std::string csv_filename = (last_slash != std::string::npos)
                                               ? attachment_path.substr(last_slash + 1)
                                               : attachment_path;

                attachments.push_back({csv_filename, csv_base64});
            } else {
                WARN("Failed to open CSV attachment file: " + attachment_path);
            }
        }

        // Create multipart email payload
        // Use multipart/mixed for attachments, with nested multipart/related for HTML+logo
        std::string outer_boundary = "----=_Outer_" + std::to_string(std::time(nullptr));
        std::string inner_boundary = "----=_Inner_" + std::to_string(std::time(nullptr));
        std::string content_type = is_html ? "text/html; charset=UTF-8" : "text/plain";

        // Determine top-level content type based on whether we have attachments
        std::string top_content_type =
            attachments.empty() ? "multipart/related" : "multipart/mixed";
        std::string top_boundary = attachments.empty() ? inner_boundary : outer_boundary;

        g_email_payload = "From: " + config_.from_email +
                          "\r\n"
                          "To: " +
                          config_.to_emails[0] +
                          "\r\n"
                          "Subject: " +
                          subject +
                          "\r\n"
                          "MIME-Version: 1.0\r\n"
                          "Content-Type: " +
                          top_content_type + "; boundary=\"" + top_boundary +
                          "\"\r\n"
                          "\r\n";

        // If we have attachments, create nested structure
        if (!attachments.empty()) {
            // Start outer multipart/mixed
            g_email_payload += "--" + outer_boundary + "\r\n";
            g_email_payload +=
                "Content-Type: multipart/related; boundary=\"" + inner_boundary + "\"\r\n";
            g_email_payload += "\r\n";
        }

        // Add HTML body
        g_email_payload += "--" + inner_boundary + "\r\n";
        g_email_payload += "Content-Type: " + content_type + "\r\n";
        g_email_payload += "Content-Transfer-Encoding: 7bit\r\n";
        g_email_payload += "\r\n";
        g_email_payload += body + "\r\n";

        // Add embedded logo
        if (!logo_base64.empty()) {
            g_email_payload += "\r\n--" + inner_boundary + "\r\n";
            g_email_payload += "Content-Type: image/png\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-ID: <algogators_logo>\r\n";
            g_email_payload += "Content-Disposition: inline; filename=\"Algo.png\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += logo_base64 + "\r\n";
        }

        // Add embedded equity curve chart (if generated)
        if (!chart_base64_.empty()) {
            g_email_payload += "\r\n--" + inner_boundary + "\r\n";
            g_email_payload += "Content-Type: image/png\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-ID: <equity_chart>\r\n";
            g_email_payload += "Content-Disposition: inline; filename=\"equity_curve.png\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += chart_base64_ + "\r\n";
        }

        // Add embedded PnL by symbol chart (if generated)
        if (!pnl_by_symbol_base64_.empty()) {
            g_email_payload += "\r\n--" + inner_boundary + "\r\n";
            g_email_payload += "Content-Type: image/png\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-ID: <pnl_by_symbol>\r\n";
            g_email_payload += "Content-Disposition: inline; filename=\"pnl_by_symbol.png\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += pnl_by_symbol_base64_ + "\r\n";
        }

        if (!drawdown_base64_.empty()) {
            g_email_payload += "\r\n--" + inner_boundary + "\r\n";
            g_email_payload += "Content-Type: image/png\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-ID: <drawdown>\r\n";
            g_email_payload += "Content-Disposition: inline; filename=\"drawdown.png\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += drawdown_base64_ + "\r\n";
        }

        if (!gross_leverage_base64_.empty()) {
            g_email_payload += "\r\n--" + inner_boundary + "\r\n";
            g_email_payload += "Content-Type: image/png\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-ID: <gross_leverage>\r\n";
            g_email_payload += "Content-Disposition: inline; filename=\"gross_leverage.png\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += gross_leverage_base64_ + "\r\n";
        }

        // Add embedded daily PnL chart (if generated)
        if (!daily_pnl_base64_.empty()) {
            g_email_payload += "\r\n--" + inner_boundary + "\r\n";
            g_email_payload += "Content-Type: image/png\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-ID: <daily_pnl>\r\n";
            g_email_payload += "Content-Disposition: inline; filename=\"daily_pnl.png\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += daily_pnl_base64_ + "\r\n";
        }

        if (!total_transaction_costs_base64_.empty()) {
            g_email_payload += "\r\n--" + inner_boundary + "\r\n";
            g_email_payload += "Content-Type: image/png\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-ID: <total_transaction_costs>\r\n";
            g_email_payload +=
                "Content-Disposition: inline; filename=\"total_transaction_costs.png\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += total_transaction_costs_base64_ + "\r\n";
        }

        if (!margin_posted_base64_.empty()) {
            g_email_payload += "\r\n--" + inner_boundary + "\r\n";
            g_email_payload += "Content-Type: image/png\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-ID: <margin_posted>\r\n";
            g_email_payload += "Content-Disposition: inline; filename=\"margin_posted.png\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += margin_posted_base64_ + "\r\n";
        }

        if (!portfolio_composition_base64_.empty()) {
            g_email_payload += "\r\n--" + inner_boundary + "\r\n";
            g_email_payload += "Content-Type: image/png\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-ID: <portfolio_composition>\r\n";
            g_email_payload +=
                "Content-Disposition: inline; filename=\"portfolio_composition.png\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += portfolio_composition_base64_ + "\r\n";
        }

        if (!cumulative_pnl_by_symbol_base64_.empty()) {
            g_email_payload += "\r\n--" + inner_boundary + "\r\n";
            g_email_payload += "Content-Type: image/png\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-ID: <cumulative_pnl_by_symbol>\r\n";
            g_email_payload +=
                "Content-Disposition: inline; filename=\"cumulative_pnl_by_symbol.png\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += cumulative_pnl_by_symbol_base64_ + "\r\n";
        }

        // Close inner multipart/related
        g_email_payload += "\r\n--" + inner_boundary + "--\r\n";

        // Add CSV attachments if present
        if (!attachments.empty()) {
            for (const auto& [csv_filename, csv_base64] : attachments) {
                g_email_payload += "\r\n--" + outer_boundary + "\r\n";
                g_email_payload += "Content-Type: text/csv; name=\"" + csv_filename + "\"\r\n";
                g_email_payload += "Content-Transfer-Encoding: base64\r\n";
                g_email_payload +=
                    "Content-Disposition: attachment; filename=\"" + csv_filename + "\"\r\n";
                g_email_payload += "\r\n";
                g_email_payload += csv_base64 + "\r\n";
            }

            // Close outer multipart/mixed
            g_email_payload += "\r\n--" + outer_boundary + "--\r\n";
        }

        g_payload_pos = 0;  // Reset position

        curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
        curl_easy_setopt(curl, CURLOPT_READDATA, nullptr);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

        // Perform the request
        res = curl_easy_perform(curl);

        // Cleanup
        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            return make_error<void>(ErrorCode::DATABASE_ERROR,
                                    "Failed to send email: " + std::string(curl_easy_strerror(res)),
                                    "EmailSender");
        }

        INFO("Email sent successfully to " + std::to_string(config_.to_emails.size()) +
             " recipients");
        return Result<void>();

    } catch (const std::exception& e) {
        curl_easy_cleanup(curl);
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Exception during email sending: " + std::string(e.what()),
                                "EmailSender");
    }
}

// DEAD-email-ag-filter: an anonymous namespace holding is_agricultural_future()
// and filter_non_agricultural_positions() stood here. The filter was
// [[maybe_unused]] and had no caller anywhere in the tree; the predicate had no
// caller except the filter. Both are deleted.
//
// The agricultural-symbol logic that IS live is not this one: the two futures
// runners each define their own is_agricultural_future lambda
// (live_portfolio.cpp and live_portfolio_conservative.cpp, around the Monday
// carry-forward block) and use it there. This copy in the email layer was a
// second, divergent definition of the same idea that nothing consulted -- the
// worst kind to leave lying about, because it reads like the email report
// filters agricultural positions, and it does not.

// The footer note by asset class (defined with the other report helpers below).
static std::string report_footer_note_html(const EmailReportContext& context);

std::string EmailSender::generate_trading_report_body(
    const std::unordered_map<std::string, Position>& positions,
    const std::optional<RiskResult>& risk_metrics,
    const std::map<std::string, double>& strategy_metrics,
    const std::vector<ExecutionReport>& executions, const std::string& date, bool is_daily_strategy,
    const std::unordered_map<std::string, double>& current_prices,
    std::shared_ptr<DatabaseInterface> db,
    const std::unordered_map<std::string, Position>& yesterday_positions,
    const std::unordered_map<std::string, double>& yesterday_close_prices,
    const std::unordered_map<std::string, double>& two_days_ago_close_prices,
    const std::map<std::string, double>& yesterday_daily_metrics,
    const std::string& chart_strategy_id, const std::string& chart_portfolio_id,
    const EmailReportContext& context) {
    (void)risk_metrics;
    reset_notes();
    {
        // An equity book is one strategy: its name and kind as a futures sleeve has them (HD 10-14).
        std::string name = chart_strategy_id;
        for (const char* prefix : {"LIVE_", "EQUITY_"})
            if (name.rfind(prefix, 0) == 0) name = name.substr(std::string(prefix).size());
        g_single_title = format_strategy_display_name(name);
    }
    std::ostringstream html;

    // Parse the date to check day of week
    std::tm tm = {};
    std::istringstream ss(date);
    ss >> std::get_time(&tm, "%Y-%m-%d");
    std::mktime(&tm);

    // Calculate yesterday's date
    std::string yesterday_date_str;
    std::string yesterday_holiday_name;
    bool is_yesterday_holiday = false;
    try {
        auto time_point = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        auto yesterday = time_point - std::chrono::hours(24);
        auto yesterday_time_t = std::chrono::system_clock::to_time_t(yesterday);
        std::tm yesterday_tm = *std::gmtime(&yesterday_time_t);
        std::ostringstream oss;
        oss << std::put_time(&yesterday_tm, "%Y-%m-%d");
        yesterday_date_str = oss.str();

        INFO("Checking holiday for yesterday's date: " + yesterday_date_str);

        // Check if yesterday was a holiday
        is_yesterday_holiday = holiday_checker_.is_holiday(yesterday_date_str);

        INFO("Is yesterday (" + yesterday_date_str + ") a holiday? " +
             std::string(is_yesterday_holiday ? "YES" : "NO"));

        if (is_yesterday_holiday) {
            yesterday_holiday_name = holiday_checker_.get_holiday_name(yesterday_date_str);
            INFO("Holiday name: " + yesterday_holiday_name);
        }
    } catch (...) {
        ERROR("Exception while calculating yesterday's date");
        yesterday_date_str = "Previous Day";
    }

    // T-8D-2 R61: the day the settled table, its block and the statistics are for is the runner's
    // own T-1 when it names one.
    const std::string settled_date =
        context.settled_date.empty() ? yesterday_date_str : context.settled_date;

    // T-8D-2 R59: what the report says about the day before comes from the runner's own
    // classification of it, never from the weekday. A futures runner that carried its book found
    // no session (context.prior_day_no_session): the settled table is then not shown, and the
    // calendar adds the holiday's name when that day is one. The equity runner's day before is
    // its previous session by construction. The runner's carried-day banner, put at the top of
    // the body by the runner, states the reason.
    const bool show_yesterday_pnl = !context.prior_day_no_session;
    const bool holiday_banner =
        context.prior_day_no_session && is_yesterday_holiday && !context.equities;

    // Generate HTML header and styles
    html << "<!DOCTYPE html>\n";
    html << "<html>\n<head>\n";
    html << "<meta charset=\"UTF-8\" />\n";
    html << "<style>\n";
    html << "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f9f9f9; }\n";
    html << ".container { max-width: 1200px; margin: 0 auto; background-color: white; padding: "
            "30px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n";
    html << "h1, h2, h3 { color: #333; font-family: Arial, sans-serif; }\n";
    html << "h1 { font-size: 24px; margin-bottom: 5px; }\n";
    html << "h2 { font-size: 20px; margin-top: 25px; margin-bottom: 10px; border-bottom: 2px solid "
            "#2c5aa0; padding-bottom: 5px; }\n";
    html << "h3 { font-size: 16px; margin-top: 20px; margin-bottom: 10px; }\n";
    html << "table { border-collapse: collapse; width: 100%; margin: 10px 0; font-size: 14px; "
            "font-family: Arial, sans-serif; }\n";
    html << "th, td { border: 1px solid #ddd; padding: 10px; text-align: left; }\n";
    html << "th { background-color: #f2f2f2; font-weight: bold; }\n";
    html << ".metric { margin: 8px 0; font-size: 14px; line-height: 1.6; font-family: Arial, "
            "sans-serif; }\n";
    html << ".positive { color: #1a7f37; font-weight: 500; }\n";
    html << ".negative { color: #b42318; font-weight: 500; }\n";
    html << ".neutral { color: #0b6efd; font-weight: 500; }\n";
    html << ".header-section { margin-bottom: 30px; display: flex; align-items: center; }\n";
    html << ".header-section img { width: 80px; height: 80px; margin-right: 20px; }\n";
    html << ".header-text { flex: 1; }\n";
    html << ".header-info { color: #666; font-size: 14px; margin-top: 10px; font-family: Arial, "
            "sans-serif; }\n";
    html << ".fund-branding { color: #2c5aa0; font-weight: bold; font-size: 16px; font-family: "
            "Arial, sans-serif; }\n";
    html << ".metrics-section { margin: 20px 0; }\n";
    html << ".metrics-category { background-color: #fff5e6; padding: 15px; border-radius: 5px; "
            "margin-bottom: 20px; }\n";
    html << ".footer-note { background-color: #fff9e6; border-left: 4px solid #ffc107; padding: "
            "15px; margin: 20px 0; font-size: 13px; color: #666; font-family: Arial, sans-serif; "
            "}\n";
    html << ".alert-note { background-color: #fee2e2; border-left: 4px solid #dc2626; padding: "
            "15px; margin: 20px 0; font-size: 13px; color: #991b1b; font-family: Arial, "
            "sans-serif; }\n";
    html << ".summary-stats { background-color: #fff5e6; padding: 15px; margin: 15px 0; "
            "border-radius: 5px; font-family: Arial, sans-serif; font-size: 14px; }\n";
    html << ".chart-container { margin: 20px 0; padding: 20px; background-color: #f8f9fa; "
            "border-radius: 8px; text-align: center; }\n";
    html << ".weekend-message { background-color: #e6f3ff; border-left: 4px solid #2c5aa0; "
            "padding: 20px; margin: 20px 0; font-size: 16px; }\n";
    html << "</style>\n";
    html << "</head>\n<body>\n";
    html << "<div class=\"container\">\n";

    // Header with logo and branding
    html << "<div class=\"header-section\">\n";
    html << "<img src=\"cid:algogators_logo\" alt=\"AlgoGators Logo\">\n";
    html << "<div class=\"header-text\">\n";
    html << "<span class=\"fund-branding\">AlgoGators</span><br>\n";
    html << "<h1>Daily Trading Report</h1>\n";
    // T-8D-2 R59: the book's own name, as the per-strategy overload prints it (the literal
    // "Trend Following Strategy" stood here, on the equity book's report).
    // An equity book is named by its strategy, "Mean Reversion", with its kind in the small grey line
    // under it, as a futures sleeve is (HD 2026-10-14, section 16.3).
    html << "<div class=\"header-info\">" << date << " | " << g_single_title << "</div>\n";
    html << "<div style=\"font-size: 11px; color: #999;\">Equities</div>\n";
    html << "</div>\n";
    html << "</div>\n";

    // The holiday banner (T-8D-2 R59: no banner is derived from the weekday)
    if (holiday_banner) {
        html << "<div style=\"background: linear-gradient(135deg, #fef3c7 0%, #fde68a 100%); "
                "border: 2px solid #f59e0b; border-radius: 8px; padding: 20px 30px; margin: 20px 0 "
                "30px 0; box-shadow: 0 4px 6px rgba(0,0,0,0.1);\">\n";
        html << "<h2 style=\"margin: 0 0 10px 0; color: #92400e; font-size: 20px; border-bottom: "
                "2px solid #92400e; padding-bottom: 8px; display: inline-block;\">Yesterday was "
             << yesterday_holiday_name << "</h2>\n";
        html << "<p style=\"margin: 15px 0 5px 0; color: #78350f; font-size: 15px; line-height: "
                "1.6;\">The latest futures settlement prices are not available, as futures markets "
                "were closed yesterday ("
             << yesterday_date_str
             << ") due to a federal holiday. The PnL for these contracts will be updated in the "
                "next report once settlement data is released.</p>\n";
        html << "<p style=\"margin: 5px 0 0 0; color: #92400e; font-weight: 600; font-size: "
                "14px;\">Please continue to monitor your positions closely.</p>\n";
        html << "</div>\n";
    }

    // Today's Positions (Forward-Looking - no PnL shown as it will be zero)
    html << "<h2>Today's Positions</h2>\n";

    html << format_positions_table(positions, is_daily_strategy, current_prices, strategy_metrics);
    html << notes_html();

    // Executions (if any)
    if (!executions.empty()) {
        html << "<h2>Daily Executions</h2>\n";
        html << format_executions_table(executions);
        html << notes_html();
    }

    // The settled day's positions, with their stored P&L: shown unless the runner found no
    // session on that day
    if (show_yesterday_pnl && !yesterday_positions.empty()) {
        html << format_yesterday_finalized_positions_table(
            yesterday_positions,
            two_days_ago_close_prices,  // the price each row was settled from
            yesterday_close_prices,     // the price it was settled at
            db,
            yesterday_daily_metrics,  // Yesterday's daily metrics
            yesterday_date_str, settled_date, context.equities);
        html << notes_html();
    } else if (!show_yesterday_pnl) {
        // The runner found no session on the day before (T-8D-2 R59): no settled table. The two
        // notes say why in the runner's terms, never from the weekday.
        html << "<div class=\"alert-note\">\n";
        if (holiday_banner) {
            html << "<strong>No Executions:</strong> Since yesterday (" << yesterday_date_str
                 << ") was a market holiday, no new market data is available. Positions remain "
                    "unchanged from the previous trading day, and no executions were generated.\n";
        } else {
            html << "<strong>No Executions:</strong> No session was found for yesterday ("
                 << yesterday_date_str
                 << "), so no new market data is available. Positions remain unchanged from the "
                    "previous trading day, and no executions were generated.\n";
        }
        html << "</div>\n";

        // Add a yellow note explaining why yesterday's PnL is not shown
        html << "<div class=\"footer-note\">\n";
        html << "<strong>Note:</strong> Yesterday's PnL data is not available.\n";
        html << "</div>\n";
    } else {
        // A session, and no row to print: nothing was held on the settled day and nothing closed
        // on it. The table is not left out silently (HD ruling Q11).
        html << "<h2>Yesterday's Finalized Position Results</h2>\n";
        html << "<p>No position was held on " << settled_date
             << ", and none closed on it.</p>\n";
    }

    /*    // Risk snapshot (if provided)
    if (risk_metrics.has_value()) {
        html << "<h2>Risk Snapshot</h2>\n";
        html << format_risk_metrics(*risk_metrics);
    }
        */

    // Strategy metrics
    if (!strategy_metrics.empty()) {
        html << "<div class=\"metrics-section\">\n";
        html << format_strategy_metrics(strategy_metrics, nullptr, date, settled_date, context);
        html << "</div>\n";
    }

    html << "<h2>Charts</h2>\n";
    core::ChartLedger chart_ledger;
    if (db) {
        // E2-F11: the caller names the book the charts query. This overload IS used in
        // production (the live equity runner); with the old hardcoded trend-following
        // strategy id and empty portfolio every chart query returned no rows.
        const std::string chart_portfolio_id_legacy = chart_portfolio_id;
        // Generate equity curve chart
        chart_base64_ = ChartGenerator::generate_equity_curve_chart(
            db, chart_strategy_id, chart_portfolio_id_legacy, date, 30);
        html << chart_ledger.record(std::string("Equity Curve"), !chart_base64_.empty(),
                                          ChartGenerator::last_chart_status(),
                                          ChartGenerator::last_chart_reason(), "no data to plot");
        if (!chart_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Equity Curve</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:equity_chart\" alt=\"Portfolio Equity Curve\" "
                    "style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px "
                    "8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        drawdown_base64_ = ChartGenerator::generate_drawdown_chart(db, chart_strategy_id, chart_portfolio_id_legacy, date, 30);
        html << chart_ledger.record(std::string("Drawdown"), !drawdown_base64_.empty(),
                                    ChartGenerator::last_chart_status(),
                                    ChartGenerator::last_chart_reason(), "no data to plot");
        if (!drawdown_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Drawdown</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:drawdown\" alt=\"Drawdown\" style=\"max-width: 100%; height: auto; "
                    "border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        // Generate PnL by symbol chart - ONLY if show_yesterday_pnl is true
        if (show_yesterday_pnl) {
            // An equity book: the bars are the settled table's Day's PnL column (HD 2026-10-14).
            std::vector<ChartPoint> days_pnl;
            for (const auto& [symbol, position] : yesterday_positions) {
                const auto from = two_days_ago_close_prices.find(symbol);
                const auto to = yesterday_close_prices.find(symbol);
                const bool priced = from != two_days_ago_close_prices.end() &&
                                    to != yesterday_close_prices.end();
                days_pnl.emplace_back(symbol, priced ? position.quantity.as_double() *
                                                           (to->second - from->second)
                                                     : 0.0);
            }
            pnl_by_symbol_base64_ = ChartGenerator::generate_days_pnl_chart(days_pnl, date);
            html << chart_ledger.record(std::string(ChartTitles::pnl_by_symbol_heading(context.equities)), !pnl_by_symbol_base64_.empty(),
                                              ChartGenerator::last_chart_status(),
                                              ChartGenerator::last_chart_reason(), "no data to plot");
            if (!pnl_by_symbol_base64_.empty()) {
                html << "<h3 style=\"margin-top: 20px; color: #333;\">"
                     << ChartTitles::pnl_by_symbol_heading(context.equities) << "</h3>\n";
                html << "<div style=\"width: 100%; max-width: 800px; margin: 20px auto; "
                        "text-align: center;\">\n";
                html << "<img src=\"cid:pnl_by_symbol\" alt=\"PnL by Symbol\" style=\"max-width: "
                        "100%; height: auto; border-radius: 8px; box-shadow: 0 2px 8px "
                        "rgba(0,0,0,0.1);\" />\n";
                html << "</div>\n";
            }
        }

        // Generate daily PnL chart
        daily_pnl_base64_ =
            ChartGenerator::generate_daily_pnl_chart(
                db, chart_strategy_id, chart_portfolio_id_legacy, date, 30);
        html << chart_ledger.record(std::string(ChartTitles::daily_pnl_heading(30)), !daily_pnl_base64_.empty(),
                                          ChartGenerator::last_chart_status(),
                                          ChartGenerator::last_chart_reason(), "no data to plot");
        if (!daily_pnl_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">"
                 << ChartTitles::daily_pnl_heading(30) << "</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:daily_pnl\" alt=\"Daily PnL\" style=\"max-width: 100%; height: "
                    "auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        gross_leverage_base64_ = ChartGenerator::generate_gross_leverage_chart(db, chart_strategy_id, chart_portfolio_id_legacy, date, 30);
        html << chart_ledger.record(std::string("Leverage"), !gross_leverage_base64_.empty(),
                                    ChartGenerator::last_chart_status(),
                                    ChartGenerator::last_chart_reason(), "no data to plot");
        if (!gross_leverage_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Leverage</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:gross_leverage\" alt=\"Leverage\" style=\"max-width: 100%; height: auto; "
                    "border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        total_transaction_costs_base64_ = ChartGenerator::generate_total_transaction_costs_chart(
            db, chart_strategy_id, chart_portfolio_id_legacy, date);
        html << chart_ledger.record(std::string("Cost per $1M Traded"), !total_transaction_costs_base64_.empty(),
                                          ChartGenerator::last_chart_status(),
                                          ChartGenerator::last_chart_reason(), "no data to plot");
        if (!total_transaction_costs_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Cost per $1M Traded</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:total_transaction_costs\" alt=\"Cost per $1M Traded\" "
                    "style=\"max-width: 100%; height: auto; "
                    "border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        margin_posted_base64_ = ChartGenerator::generate_margin_posted_chart(
            db, chart_strategy_id, chart_portfolio_id_legacy, date);

        html << chart_ledger.record(std::string("Margin Posted"), !margin_posted_base64_.empty(),

                                          ChartGenerator::last_chart_status(),

                                          ChartGenerator::last_chart_reason(), "no data to plot");

        if (!margin_posted_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Margin Posted</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:margin_posted\" alt=\"Margin Posted\" "
                    "style=\"max-width: 100%; height: auto; border-radius: 8px; "
                    "box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        portfolio_composition_base64_ =
            ChartGenerator::generate_portfolio_composition_chart(positions, current_prices, date);
        html << chart_ledger.record(std::string("Portfolio Composition"), !portfolio_composition_base64_.empty(),
                                          ChartGenerator::last_chart_status(),
                                          ChartGenerator::last_chart_reason(), "no positions");
        if (!portfolio_composition_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Portfolio Composition</h3>\n";
            html << "<div style=\"width: 100%; max-width: 800px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:portfolio_composition\" alt=\"Portfolio Composition\" "
                    "style=\"max-width: 100%; height: auto; border-radius: 8px; "
                    "box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        cumulative_pnl_by_symbol_base64_ = ChartGenerator::generate_cumulative_pnl_by_symbol_chart(
            db, chart_strategy_id, chart_portfolio_id_legacy, date, true);
        html << chart_ledger.record(std::string(ChartTitles::cumulative_pnl_heading()), !cumulative_pnl_by_symbol_base64_.empty(),
                                          ChartGenerator::last_chart_status(),
                                          ChartGenerator::last_chart_reason(), "no data to plot");
        if (!cumulative_pnl_by_symbol_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">"
                 << ChartTitles::cumulative_pnl_heading() << "</h3>\n";
            html << "<div style=\"width: 100%; max-width: 800px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:cumulative_pnl_by_symbol\" alt=\"Cumulative PnL by Symbol\" "
                    "style=\"max-width: 100%; height: auto; border-radius: 8px; "
                    "box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }
        INFO(chart_ledger.summary());
    }

    // Symbols Reference (moved after Portfolio Snapshot). T-8D-2 R59: it is a table of futures
    // contract metadata (and the rollover notice below reads the same table), so an equity
    // book's report carries neither.
    if (context.equities) {
        // nothing: the section is not printed
    } else if (db) {
        html << "<h2>Symbols Reference</h2>\n";
        try {
            html << format_symbols_table_for_positions(positions, db, yesterday_date_str);
        } catch (const std::exception& e) {
            html << "<p>Error loading symbols data: " << e.what() << "</p>\n";
        }
    } else {
        html << "<h2>Symbols Reference</h2>\n";
        html << "<p>Database unavailable; symbols reference not included.</p>\n";
    }


    // Footer note
    if (is_daily_strategy) {
        html << "<div class=\"footer-note\">\n";
        html << report_footer_note_html(context);
        html << "</div>\n";
    }

    html << "<hr style=\"margin-top: 30px; border: none; border-top: 1px solid #ddd;\">\n";
    html << "<p style=\"text-align: center; color: #999; font-size: 12px; margin-top: 20px; "
            "font-family: Arial, sans-serif;\">Generated by AlgoGator's Trade-ngin</p>\n";
    html << "</div>\n";
    html << "</body>\n</html>\n";

    return html.str();
}

// T-8D-2 R59: the footer note by asset class, with the book's start date from the runner (the
// live_start_date of its strategy_trading_days_metadata row) in place of a literal. A futures
// book keeps its data sentence and its universe note; an equity book has the data sentence
// without the front-month contract and no futures universe. With no start date handed, no start
// date sentence is printed.
static std::string report_footer_note_html(const EmailReportContext& context) {
    // HD 10-13: the one note of the email, after the numbered footnotes.
    std::string html = notes_html();
    html += "<strong>Note:</strong> This strategy is based on daily OHLCV data for ";
    html += context.equities ? "equity assets." : "futures assets. We currently only provide data "
                                                  "for the front-month contract and are working "
                                                  "on integrating roll data.";
    if (!context.live_start_date.empty()) {
        html += "<br><br>\nAll values reflect a trading start date of " +
                report_long_date(context.live_start_date) + ".";
    }
    return html + "\n";
}

// T-8D-2 R59: a share quantity prints to six decimals (an equity book holds fractional shares; at
// no decimals 0.193729 shares read 0); a contract count stays whole.
static std::string report_quantity_text(const std::shared_ptr<Instrument>& instrument,
                                        double quantity) {
    (void)instrument;  // HD 10-13: every quantity of the email prints to two decimals
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", quantity);
    return buf;
}

// The registry's instrument of a report symbol (the ".v.N" / ".c.N" suffix cut), or null.
static std::shared_ptr<Instrument> report_instrument_of(const std::string& symbol) {
    std::string lookup_sym = symbol;
    for (const char* suffix : {".v.", ".c."}) {
        const auto dotpos = lookup_sym.find(suffix);
        if (dotpos != std::string::npos)
            lookup_sym = lookup_sym.substr(0, dotpos);
    }
    try {
        return InstrumentRegistry::instance().get_instrument(lookup_sym);
    } catch (...) {
        return nullptr;
    }
}

// The decimals every price of this symbol prints with (report_price_places).
static int report_places_of(const std::string& symbol) {
    // HD 10-14: every price of the email has four decimals; the files carry the contract's own.
    (void)symbol;
    return 4;
}

// T-NETTING: how the netting figures are printed (one rounding, the adjustment as it acts on the
// cost and as the difference of its two printed neighbours) is email_netting_text.hpp.
using email_netting::netting_effect_text;
using email_netting::netting_money;

// The day's rows of one table: their own costs, their adjustments, and whether any row was netted.
struct NettingFooter {
    Decimal own_costs;
    Decimal adjustments;
    bool any_adjusted{false};
    void add(const ExecutionReport& exec) {
        own_costs += exec.total_transaction_costs;
        adjustments += exec.netting_adjustment;
        if (exec.netting_adjustment != Decimal()) any_adjusted = true;
    }
};

// T-ROLLX (LOOP_SPEC v6.1 section 6.5): the ROLL legs of a day as their own block (the two contract
// ids, the price of each leg, the cost: an upper bound, two outright legs), never inside the
// strategy's trade count or its traded notional; their cost inside every cost total.
static std::string format_roll_legs_block(const std::vector<ExecutionReport>& executions) {
    std::ostringstream html;
    double roll_cost = 0.0;
    int legs = 0;
    for (const auto& exec : executions) {
        if (exec.execution_type != ExecutionType::ROLL) continue;
        if (legs == 0) {
            html << "<table>\n";
            html << "<tr><th>Roll Leg</th><th>Symbol</th><th>Side</th><th>Quantity</th><th>Price</th>"
                    "<th>Cost</th></tr>\n";
        }
        ++legs;
        roll_cost += exec.total_transaction_costs.as_double();
        const bool closing = exec.exec_id.size() >= 2 &&
                             exec.exec_id.compare(exec.exec_id.size() - 2, 2, "RC") == 0;
        html << "<tr><td>" << (closing ? "closing" : "opening") << "</td><td>" << exec.symbol
             << "</td><td>" << (exec.side == Side::BUY ? "BUY" : "SELL") << "</td><td>"
             << report_quantity_text(report_instrument_of(exec.symbol),
                                     exec.filled_quantity.as_double())
             << "</td><td>" << report_price_dollars_places(exec.fill_price.as_double(), report_places_of(exec.symbol)) << "</td><td>"
             << report_dollars(exec.total_transaction_costs.as_double()) << "</td></tr>\n";
    }
    if (legs > 0) {
        html << "</table>\n";
    }
    return html.str();
}

std::string EmailSender::format_executions_table(const std::vector<ExecutionReport>& executions) {
    std::ostringstream html;

    if (executions.empty()) {
        html << "<p>No executions for today.</p>\n";
        return html.str();
    }

    html << single_heading_html();
    html << "<table>\n";
    html << "<tr><th>Symbol</th><th>Side</th><th>Quantity</th><th>Price</th><th>Notional</"
            "th><th>Transaction Cost</th></tr>\n";

    double total_transaction_cost = 0.0;
    double total_notional_traded = 0.0;
    size_t strategy_trades = 0;
    NettingFooter netting;
    email_netting::RoundingCheck cost_rows, notional_rows, own_rows;  // HD ruling Q3

    for (const auto& exec : executions) {
        netting.add(exec);
        cost_rows.add(transaction_cost::net_cost(exec).as_double());
        own_rows.add(exec.total_transaction_costs.as_double());
        // T-ROLLX: every row's cost counts; a ROLL leg is listed in its own block below, never
        // as a trade and never in the traded notional. The total is what the account was charged:
        // each row's cost after netting (the row's own cost stays in its cell).
        total_transaction_cost += transaction_cost::net_cost(exec).as_double();
        if (exec.execution_type != ExecutionType::STRATEGY) continue;
        ++strategy_trades;
        // Get contract multiplier for proper notional calculation
        double contract_multiplier = 1.0;
        bool has_multiplier = true;

        try {
            auto& registry = InstrumentRegistry::instance();
            // Normalize variant-suffixed symbols for lookup only (e.g., 6B.v.0 -> 6B)
            std::string lookup_sym = exec.symbol;
            auto dotpos = lookup_sym.find(".v.");
            if (dotpos != std::string::npos) {
                lookup_sym = lookup_sym.substr(0, dotpos);
            }
            dotpos = lookup_sym.find(".c.");
            if (dotpos != std::string::npos) {
                lookup_sym = lookup_sym.substr(0, dotpos);
            }
            auto instrument = registry.get_instrument(lookup_sym);
            if (instrument) {
                // The multiplier is the registry's (metadata "Contract Size"), the only source.
                contract_multiplier = instrument->get_multiplier();
            } else {
                has_multiplier = false;
            }
        } catch (...) {
            has_multiplier = false;
        }
        if (!has_multiplier) {
            ERROR("Email: " + exec.symbol + " is not in the instrument registry "
                  "(metadata.contract_metadata); its notional is shown as n/a and left out of "
                  "the total, not guessed");
        }

        double notional = has_multiplier ? exec.filled_quantity.as_double() *
                                               exec.fill_price.as_double() * contract_multiplier
                                         : 0.0;
        total_notional_traded += notional;
        if (has_multiplier) notional_rows.add(notional);

        std::string side_str = exec.side == Side::BUY ? "BUY" : "SELL";
        std::string side_class = exec.side == Side::BUY ? "positive" : "negative";

        html << "<tr>\n";
        html << "<td>" << exec.symbol << "</td>\n";
        html << "<td class=\"" << side_class << "\">" << side_str << "</td>\n";
        html << "<td>"
             << report_quantity_text(report_instrument_of(exec.symbol),
                                     exec.filled_quantity.as_double())
             << "</td>\n";
        html << "<td>" << report_price_dollars_places(exec.fill_price.as_double(), report_places_of(exec.symbol)) << "</td>\n";
        if (has_multiplier) {
            html << "<td>" << report_dollars(notional) << "</td>\n";
        } else {
            html << "<td>n/a</td>\n";
        }
        html << "<td>" << report_dollars(exec.total_transaction_costs.as_double())
             << "</td>\n";
        html << "</tr>\n";
    }

    html << "</table>\n";
    html << format_roll_legs_block(executions);

    // Helper to format numbers with commas
    auto format_with_commas = [](double value) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << value;
        std::string s = oss.str();

        // Handle negative numbers by processing the absolute value
        size_t start_pos = 0;
        if (!s.empty() && s[0] == '-') {
            start_pos = 1;
        }

        size_t dp = s.find('.');
        if (dp == std::string::npos)
            dp = s.size();
        int pos = static_cast<int>(dp) - 3;
        while (pos > static_cast<int>(start_pos)) {
            s.insert(static_cast<size_t>(pos), ",");
            pos -= 3;
        }
        return s;
    };

    html << "<div class=\"summary-stats\">\n";
    html << "<strong>Trades:</strong> " << strategy_trades << "<br>\n";
    html << "<strong>Notional Traded:</strong> " << report_dollars(total_notional_traded)
         << "<br>\n";
    html << "<strong>Transaction Costs:</strong> " << report_dollars(total_transaction_cost);
    if (netting.any_adjusted) {
        g_netting_note = "Netting between strategies: own costs " +
                         report_dollars(netting.own_costs.as_double()) + ", adjustment " +
                         netting_effect_text(netting.own_costs.as_double(), total_transaction_cost) +
                         ", transaction costs " + report_dollars(total_transaction_cost) + ".";
        html << note_sup(5);
    }
    html << "\n";
    html << "</div>\n";

    return html.str();
}

std::string EmailSender::format_positions_table(
    const std::unordered_map<std::string, Position>& positions, bool is_daily_strategy,
    const std::unordered_map<std::string, double>& current_prices,
    const std::map<std::string, double>& strategy_metrics) {
    (void)is_daily_strategy;
    std::ostringstream html;

    html << single_heading_html();
    html << "<table>\n";
    // An equity book (HD 2026-10-14, section 16): the average cost the shares were bought at and what
    // they have gained since, beside the quantity and the price.
    const bool equity_table = !g_single_title.empty();
    html << "<tr><th>Symbol</th><th>Quantity</th>";
    if (equity_table) html << "<th>Average Cost</th>";

    // Show only market price for forward-looking positions (no PnL - will be zero)
    html << "<th>Price</th>";

    html << "<th>Notional</th>";
    if (equity_table) html << "<th>Unrealized PnL</th>";
    html << "<th>% of Total</th></tr>\n";

    double total_notional = 0.0;
    double total_margin_posted = 0.0;
    int active_positions = 0;

    // First pass: calculate total notional
    std::vector<std::tuple<std::string, double, double, double, double, double>>
        position_data;  // symbol, qty, entry, market, notional, margin
    std::set<std::string> rows_without_close;
    std::vector<std::string> margin_left_out;

    for (const auto& [symbol, position_ptr] : positions_by_symbol(positions)) {
        const Position& position = *position_ptr;
        if (position.quantity.as_double() != 0.0) {
            active_positions++;

            // Get contract multiplier for proper notional calculation
            double contract_multiplier = 1.0;
            double notional = 0.0;

            // ONLY use instrument registry - no fallbacks allowed
            try {
                auto& registry = InstrumentRegistry::instance();
                // Normalize variant-suffixed symbols for lookup only (e.g., 6B.v.0 -> 6B)
                std::string lookup_sym = position.symbol;
                auto dotpos = lookup_sym.find(".v.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }
                dotpos = lookup_sym.find(".c.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }

                auto instrument = registry.get_instrument(lookup_sym);
                if (!instrument) {
                    ERROR("CRITICAL: Instrument " + lookup_sym +
                          " not found in registry for email generation!");
                    throw std::runtime_error("Missing instrument in registry: " + lookup_sym);
                }

                // Get contract multiplier for notional calculation from registry (ONLY source)
                contract_multiplier = instrument->get_multiplier();
                if (contract_multiplier <= 0) {
                    ERROR("CRITICAL: Invalid multiplier " + std::to_string(contract_multiplier) +
                          " for " + lookup_sym);
                    throw std::runtime_error("Invalid multiplier for: " + lookup_sym);
                }

                // Use the price/quantity overload so equities get
                // account-mode-aware margin (CASH = full notional, REG_T = 50%
                // long / 150% short) and futures get total dollars (now
                // multiplied internally by FuturesInstrument's new override).
                const double signed_qty = position.quantity.as_double();

                // BA-16: one row without a price must not abort the report.
                //
                // This read average_price only. On the equity path that column is a
                // cost basis, and 0 is its documented "no basis known" value
                // (AVERAGE_PRICE_LIFECYCLE rule 5) -- reachable for a held position
                // whose basis could not be resolved, which the runner already reports
                // as an ERROR. get_margin_requirement(0, qty) then returns 0, this
                // threw, and the bare `throw;` below aborted the WHOLE daily email:
                // one unpriceable row and nobody gets a report at all. A row with no
                // basis therefore falls back to the current close.
                //
                // T-8D D3 (a): the margin line is priced at the row's cost basis and the
                // current close is the fallback when no basis is known, the preference
                // MarginManager applies to the stored margin_posted and the per-strategy
                // tables below apply, so the email's margin and the database's agree. The
                // row's market price below stays on the close; the notional is not touched.
                double price_for_margin = 0.0;
                if (position.average_price.as_double() > 0.0) {
                    price_for_margin = position.average_price.as_double();
                } else {
                    auto margin_price_it = current_prices.find(symbol);
                    if (margin_price_it != current_prices.end() &&
                        margin_price_it->second > 0.0) {
                        price_for_margin = margin_price_it->second;
                    }
                }

                if (price_for_margin <= 0.0) {
                    // Neither a close nor a basis. Report it and leave this row out of
                    // the margin total; the email still goes out, and the row still
                    // appears with the figures that ARE known.
                    margin_left_out.push_back(symbol);
                    WARN("Daily email: no usable price for " + lookup_sym +
                         " (quantity " + std::to_string(signed_qty) +
                         ", no current close and average_price is 0) -- excluded from the "
                         "margin total. The email is still sent; margin posted is "
                         "understated by this position.");
                } else {
                    double total_initial_margin =
                        instrument->get_margin_requirement(price_for_margin, signed_qty);
                    if (total_initial_margin <= 0) {
                        // A positive price that still yields no margin is an instrument
                        // configuration problem, not a missing-data problem. Report it
                        // and carry on rather than suppressing the whole report.
                        margin_left_out.push_back(symbol);
                        WARN("Daily email: instrument " + lookup_sym +
                             " returned margin " + std::to_string(total_initial_margin) +
                             " for price=" + std::to_string(price_for_margin) +
                             ", qty=" + std::to_string(signed_qty) +
                             " -- excluded from the margin total.");
                    } else {
                        total_margin_posted += total_initial_margin;
                    }
                }

            } catch (const std::exception& e) {
                ERROR("CRITICAL: Failed to get instrument data for " + position.symbol + ": " +
                      e.what());
                throw;  // Re-throw - don't hide the error
            }

            // Use current market price if available, otherwise fall back to average price
            double market_price = position.average_price.as_double();
            auto price_it = current_prices.find(symbol);
            if (price_it != current_prices.end()) {
                market_price = price_it->second;
            } else {
                rows_without_close.insert(symbol);
            }

            // T-8D-2 R60 (T-8D D9): one price per row. The notional is the quantity at the price
            // the row prints (the Day T-1 close; the basis only when the row has no close), so
            // the row multiplies out and the total is the close-priced figure the stored
            // gross_notional and the leverage lines are on.
            notional = position.quantity.as_double() * market_price * contract_multiplier;
            total_notional += std::abs(notional);

            // Entry price (same as market price for forward-looking positions)
            double entry_price = position.average_price.as_double();

            // Store data for second pass
            position_data.push_back(std::make_tuple(symbol, position.quantity.as_double(),
                                                    entry_price, market_price, notional,
                                                    total_margin_posted));
        }
    }

    // Second pass: render rows with % of total
    for (const auto& [symbol, qty, entry_price, market_price, notional, margin] : position_data) {
        double pct_of_total =
            (total_notional > 0) ? (std::abs(notional) / total_notional * 100.0) : 0.0;

        html << "<tr>\n";
        html << "<td>" << symbol << "</td>\n";
        html << "<td>" << report_quantity_text(report_instrument_of(symbol), qty) << "</td>\n";

        if (equity_table) {
            html << "<td>" << report_price_dollars_places(entry_price, 4) << "</td>\n";
        }
        // Show only market price (entry price removed from display), at every decimal it has
        html << "<td>" << report_price_dollars_places(market_price, report_places_of(symbol))
             << (rows_without_close.count(symbol) ? note_sup(4) : std::string()) << "</td>\n";

        html << "<td>" << report_dollars(std::abs(notional)) << "</td>\n";
        if (equity_table) {
            html << "<td>" << report_dollars(qty * (market_price - entry_price)) << "</td>\n";
        }
        html << "<td>" << std::fixed << std::setprecision(2) << pct_of_total << "%</td>\n";
        html << "</tr>\n";
    }

    html << "</table>\n";
    html << "<div class=\"summary-stats\">\n";

    // Helper function to format numbers with commas
    auto format_with_commas = [](double value) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << value;
        std::string str = oss.str();

        // Handle negative numbers by processing the absolute value
        size_t start_pos = 0;
        if (!str.empty() && str[0] == '-') {
            start_pos = 1;
        }

        // Find decimal point
        size_t decimal_pos = str.find('.');
        if (decimal_pos == std::string::npos) {
            decimal_pos = str.length();
        }

        // Insert commas (starting from after the negative sign if present)
        int insert_pos = static_cast<int>(decimal_pos) - 3;
        while (insert_pos > static_cast<int>(start_pos)) {
            str.insert(static_cast<size_t>(insert_pos), ",");
            insert_pos -= 3;
        }

        return str;
    };

    html << "<strong>Active Positions:</strong> " << active_positions << "<br>\n";

    // The expected risk is in the performance metrics, the VaR in the account detail file (HD 10-13).

    html << "<strong>Total Notional:</strong> " << report_dollars(total_notional) << "<br>\n";
    html << "<strong>Total Margin Posted:</strong> " << report_dollars(total_margin_posted)
         << "\n";
    html << "</div>\n";

    return html.str();
}

// The two price columns of the settled day's table (report_settled_marks.hpp, HD 2026-10-12): the
// price the day was settled from and the price it was settled at, the pair the settle's own P&L
// arithmetic used for the row, as the runner hands them. A symbol the "settled at" map does not
// hold printed no bar that day: its cell reads "no bar" and one footnote under the table says so.
// A price the maps do not hold (NaN in the row data): nothing is known.
// The sentence under the table says what the two prices are and what a row multiplies out to; a
// futures book and the equity book each have their own (settled_table_note_html).
static std::optional<double> known_price(double price) {
    if (std::isnan(price))
        return std::nullopt;
    return price;
}

std::string EmailSender::format_yesterday_finalized_positions_table(
    const std::unordered_map<std::string, Position>& yesterday_positions,
    const std::unordered_map<std::string, double>& entry_prices,  // settled from
    const std::unordered_map<std::string, double>& exit_prices,   // settled at
    std::shared_ptr<DatabaseInterface> db, const std::map<std::string, double>& strategy_metrics,
    const std::string& yesterday_date, const std::string& settled_date, bool equities,
    const SettledZeroKinds& zero_kinds) {
    (void)db;
    std::ostringstream html;

    if (yesterday_positions.empty()) {
        return std::string();
    }

    html << "<h2>Yesterday's Finalized Position Results</h2>\n";
    html << single_heading_html();
    html << "<table>\n";
    html << "<tr>";
    html << "<th>Symbol</th>";
    html << "<th>Quantity</th>";
    if (equities) html << "<th>Average Cost</th>";
    html << "<th>" << kSettledPreviousPriceLabel << "</th>";
    html << "<th>" << kSettledLatestPriceLabel << "</th>";
    if (equities) html << "<th>Day's PnL</th><th>Unrealized PnL</th>";
    html << "<th>Realized PnL</th>";
    html << "</tr>\n";

    // Helper function to format numbers with commas
    auto format_with_commas = [](double value, int precision = 2) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(precision) << value;
        std::string str = oss.str();

        // Handle negative numbers by processing the absolute value
        size_t start_pos = 0;
        if (!str.empty() && str[0] == '-') {
            start_pos = 1;
        }

        // Find decimal point
        size_t decimal_pos = str.find('.');
        if (decimal_pos == std::string::npos) {
            decimal_pos = str.length();
        }

        // Insert commas (starting from after the negative sign if present)
        int insert_pos = static_cast<int>(decimal_pos) - 3;
        while (insert_pos > static_cast<int>(start_pos)) {
            str.insert(static_cast<size_t>(insert_pos), ",");
            insert_pos -= 3;
        }

        return str;
    };

    // Collect position data for sorting and aggregation
    std::vector<std::tuple<std::string, double, double, double, double>> position_data;
    double total_realized_pnl = 0.0;

    for (const auto& [symbol, position] : yesterday_positions) {
        double qty = position.quantity.as_double();

        // Skip positions with zero quantity (an equity position closed on the settled day keeps its
        // row: it was sold at the previous close, so it was held over none of the move)
        if (std::abs(qty) < 0.0001 && !(equities && std::abs(position.realized_pnl.as_double()) >= 1e-9))
            continue;

        // The two prices. A symbol the runner's "settled at" map does not hold printed no bar
        // that day: its price is not known (NaN here, "no bar" in the cell), never a zero.
        double entry_price = std::nan("");
        double exit_price = std::nan("");

        if (entry_prices.find(symbol) != entry_prices.end()) {
            entry_price = entry_prices.at(symbol);
        }
        if (exit_prices.find(symbol) != exit_prices.end()) {
            exit_price = exit_prices.at(symbol);
        }

        // Get realized PnL from the position (GROSS PnL - not net of commissions)
        // This is the PnL as stored in the positions table after finalization
        double realized_pnl = position.realized_pnl.as_double();

        position_data.emplace_back(symbol, qty, entry_price, exit_price, realized_pnl);

        total_realized_pnl += realized_pnl;
    }
    (void)total_realized_pnl;

    // Sort by symbol
    std::sort(position_data.begin(), position_data.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });

    // Render table rows
    bool any_no_bar_row = false;
    for (const auto& [symbol, qty, entry_price, exit_price, realized_pnl] : position_data) {
        const SettledPriceCells cells =
            settled_price_cells(known_price(entry_price), known_price(exit_price),
                                report_places_of(symbol));
        any_no_bar_row = any_no_bar_row || cells.no_bar;
        html << "<tr>";
        html << "<td>" << symbol << "</td>";
        html << "<td>" << std::fixed << std::setprecision(2) << qty << "</td>";
        if (equities) {
            html << "<td>" << report_price_dollars_places(yesterday_positions.at(symbol).average_price.as_double(), 4) << "</td>";
        }
        html << "<td>" << cells.previous << "</td>";

        html << "<td>" << cells.latest << (cells.no_bar ? note_sup(1) : std::string()) << "</td>";
        if (equities) {
            // the column that multiplies out: shares held over the move x (latest - previous)
            const double days_pnl = (known_price(entry_price) && known_price(exit_price))
                                        ? qty * (exit_price - entry_price)
                                        : 0.0;
            html << "<td>" << report_dollars(days_pnl) << "</td>";
            html << "<td>" << report_dollars(yesterday_positions.at(symbol).unrealized_pnl.as_double())
                 << "</td>";
        }

        // Realized PnL with color (GROSS PnL from positions table)
        std::string realized_pnl_class = (realized_pnl >= 0) ? "positive" : "negative";
        html << "<td class=\"" << realized_pnl_class << "\">";
        html << report_dollars(realized_pnl, 2)
             << (settled_zero_note(equities, symbol, qty, known_price(entry_price), known_price(exit_price),
                                realized_pnl, zero_kinds) == 2 ? note_sup(2) : std::string())
             << (settled_zero_note(equities, symbol, qty, known_price(entry_price), known_price(exit_price),
                                realized_pnl, zero_kinds) == 3 ? note_sup(3) : std::string());
        html << "</td>";

        html << "</tr>\n";
    }

    html << "</table>\n";
    (void)any_no_bar_row;
    html << notes_html();  // the lines of the notes used in the table, right below it

    // Add daily metrics as a section heading after the table - use passed yesterday_daily_metrics
    if (!strategy_metrics.empty() && !yesterday_date.empty()) {
        // T-8D-2 R61: the block is the settled day's.
        html << "<h2>Previous trading day (" << (settled_date.empty() ? yesterday_date : settled_date)
             << ")</h2>\n";
        html << "<div class=\"metrics-category\">\n";

        // Helper to format metrics with proper color
        auto format_metric_display = [&format_with_commas](const std::string& label, double value,
                                                           bool is_percentage) -> std::string {
            std::string value_class = "";
            if (std::fabs(value) < 1e-9) {
                value_class = " class=\"neutral\"";
            } else if (value > 0) {
                value_class = " class=\"positive\"";
            } else {
                value_class = " class=\"negative\"";
            }

            std::string formatted_value;
            if (is_percentage) {
                formatted_value = format_with_commas(value, 2) + "%";
            } else {
                formatted_value = report_dollars(value, 2);
            }

            return "<div class=\"metric\"><strong>" + label + ":</strong> <span" + value_class +
                   ">" + formatted_value + "</span></div>\n";
        };

        // Add Total Positions line
        html << "<div class=\"metric\"><strong>Total Positions:</strong> " << position_data.size()
             << "</div>\n";

        // Extract and display metrics in the specified order:
        // 1. Daily Return (as percentage)
        // 2. Daily Unrealized PnL (Gross) (as dollar amount)
        // 3. Daily Realized PnL (Gross) (as dollar amount)
        // 4. Daily Commissions (as dollar amount)
        // 5. Daily Total PnL (Net) (as dollar amount)

        auto daily_return_it = strategy_metrics.find("Daily Return");
        if (daily_return_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Return", daily_return_it->second, true);
        }

        auto daily_unrealized_it = strategy_metrics.find("Daily Unrealized PnL");
        if (daily_unrealized_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Unrealized PnL (Gross)",
                                          daily_unrealized_it->second, false);
        }

        auto daily_realized_it = strategy_metrics.find("Daily Realized PnL");
        if (daily_realized_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Realized PnL (Gross)", daily_realized_it->second,
                                          false);
        }

        auto daily_transaction_costs_it = strategy_metrics.find("Daily Transaction Costs");
        if (daily_transaction_costs_it != strategy_metrics.end()) {
            // Display transaction costs as positive value in black font (no color coding)
            double transaction_cost_value = std::abs(daily_transaction_costs_it->second);
            std::string formatted_transaction_cost =
                report_dollars(transaction_cost_value, 2);
            html << "<div class=\"metric\"><strong>Daily Transaction Costs:</strong> <span>"
                 << formatted_transaction_cost << "</span></div>\n";
        }
        auto daily_total_it = strategy_metrics.find("Daily Total PnL");
        if (daily_total_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Total PnL (Net)", daily_total_it->second, false);
        }

        html << "</div>\n";
    }

    return html.str();
}

std::string EmailSender::format_yesterday_finalized_positions_table(
    const StrategyPositionsMap& strategy_positions,
    const std::unordered_map<std::string, double>& entry_prices,
    const std::unordered_map<std::string, double>& exit_prices,
    std::shared_ptr<DatabaseInterface> db, const std::map<std::string, double>& strategy_metrics,
    const std::string& yesterday_date, const std::string& settled_date, bool equities,
    const SettledZeroKinds& zero_kinds) {
    (void)db;
    std::ostringstream html;
    SingleSleeveScope single_sleeve_scope;
    {
        int sleeves = 0;
        for (const auto& [name, positions] : strategy_positions) {
            (void)name;
            for (const auto& [symbol, pos] : positions) {
                (void)symbol;
                if (std::abs(pos.quantity.as_double()) >= 0.0001) { ++sleeves; break; }
            }
        }
        g_single_sleeve = sleeves == 1;
    }
    

    // Check if there are any positions across all strategies
    bool has_positions = false;
    for (const auto& [_, positions] : strategy_positions) {
        for (const auto& [__, pos] : positions) {
            if (std::abs(pos.quantity.as_double()) >= 0.0001) {
                has_positions = true;
                break;
            }
        }
        if (has_positions)
            break;
    }

    if (!has_positions) {
        return std::string();
    }

    html << "<h2>Yesterday's Finalized Position Results</h2>\n";

    // Helper function to format numbers with commas
    auto format_with_commas = [](double value, int precision = 2) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(precision) << value;
        std::string str = oss.str();

        size_t start_pos = 0;
        if (!str.empty() && str[0] == '-') {
            start_pos = 1;
        }

        size_t decimal_pos = str.find('.');
        if (decimal_pos == std::string::npos) {
            decimal_pos = str.length();
        }

        int insert_pos = static_cast<int>(decimal_pos) - 3;
        while (insert_pos > static_cast<int>(start_pos)) {
            str.insert(static_cast<size_t>(insert_pos), ",");
            insert_pos -= 3;
        }

        return str;
    };

    // Sort strategies alphabetically for consistent ordering
    std::vector<std::string> strategy_names;
    for (const auto& [name, _] : strategy_positions) {
        strategy_names.push_back(name);
    }
    std::sort(strategy_names.begin(), strategy_names.end());

    // Track total positions across all strategies
    int total_positions_count = 0;
    // One footnote under the sleeves' tables when any row of any of them printed no bar.
    bool any_no_bar_row = false;

    // Generate table for each strategy
    for (const auto& strategy_name : strategy_names) {
        const auto& positions = strategy_positions.at(strategy_name);

        // Check if strategy has any active positions
        std::vector<std::tuple<std::string, double, double, double, double>> position_data;
        double strategy_total_pnl = 0.0;

        for (const auto& [symbol, position] : positions) {
            double qty = position.quantity.as_double();
            if (std::abs(qty) < 0.0001)
                continue;

            // A symbol with no bar that day has no price (see the single table above).
            double entry_price = std::nan("");
            double exit_price = std::nan("");

            if (entry_prices.find(symbol) != entry_prices.end()) {
                entry_price = entry_prices.at(symbol);
            }
            if (exit_prices.find(symbol) != exit_prices.end()) {
                exit_price = exit_prices.at(symbol);
            }

            double realized_pnl = position.realized_pnl.as_double();
            position_data.emplace_back(symbol, qty, entry_price, exit_price, realized_pnl);
            strategy_total_pnl += realized_pnl;
        }

        if (position_data.empty()) {
            continue;
        }

        // Accumulate total positions count
        total_positions_count += position_data.size();

        // Sort by symbol
        std::sort(position_data.begin(), position_data.end(),
                  [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });

        // Strategy sub-header with styled left border accent
        std::string display_name = format_strategy_display_name(strategy_name);
        html << "<h3 style=\"margin-top: 20px; margin-bottom: 10px; color: #333; "
             << "border-left: 4px solid #2c5aa0; padding-left: 12px;\">" << display_name
             << "</h3>\n";

        // Table for this strategy
        html << "<table>\n";
        html << "<tr>";
        html << "<th>Symbol</th>";
        html << "<th>Quantity</th>";
        html << "<th>" << kSettledPreviousPriceLabel << "</th>";
        html << "<th>" << kSettledLatestPriceLabel << "</th>";
        html << "<th>Realized PnL</th>";
        html << "</tr>\n";

        for (const auto& [symbol, qty, entry_price, exit_price, realized_pnl] : position_data) {
            const SettledPriceCells cells =
                settled_price_cells(known_price(entry_price), known_price(exit_price),
                                report_places_of(symbol));
            any_no_bar_row = any_no_bar_row || cells.no_bar;
            html << "<tr>";
            html << "<td>" << symbol << "</td>";
            html << "<td>" << std::fixed << std::setprecision(2) << qty << "</td>";
            html << "<td>" << cells.previous << "</td>";

            html << "<td>" << cells.latest << (cells.no_bar ? note_sup(1) : std::string()) << "</td>";

            std::string realized_pnl_class = (realized_pnl >= 0) ? "positive" : "negative";
            html << "<td class=\"" << realized_pnl_class << "\">";
            html << report_dollars(realized_pnl, 2)
             << (settled_zero_note(equities, symbol, qty, known_price(entry_price), known_price(exit_price),
                                realized_pnl, zero_kinds) == 2 ? note_sup(2) : std::string())
             << (settled_zero_note(equities, symbol, qty, known_price(entry_price), known_price(exit_price),
                                realized_pnl, zero_kinds) == 3 ? note_sup(3) : std::string());
            html << "</td>";

            html << "</tr>\n";
        }

        html << "</table>\n";

        // Compact strategy-level summary
        std::string pnl_class = (strategy_total_pnl >= 0) ? "positive" : "negative";
        if (!g_single_sleeve) {
        html << "<div style=\"font-size: 13px; color: #666; margin: 8px 0 20px 0; padding-left: "
                "16px;\">\n";
        html << "<strong>Positions:</strong> " << position_data.size()
             << " | <strong>Total Realized PnL (Gross):</strong> <span class=\"" << pnl_class
             << "\">" << report_dollars(strategy_total_pnl, 2) << "</span>\n";
        html << "</div>\n";
        }
    }
    (void)any_no_bar_row;
    html << notes_html();  // the lines of the notes used in the table, right below it

    // Add daily metrics section
    if (!strategy_metrics.empty() && !yesterday_date.empty()) {
        // T-8D-2 R61: the block is the settled day's.
        html << "<h2>Previous trading day (" << (settled_date.empty() ? yesterday_date : settled_date)
             << ")</h2>\n";
        html << "<div class=\"metrics-category\">\n";

        auto format_metric_display = [&format_with_commas](const std::string& label, double value,
                                                           bool is_percentage) -> std::string {
            std::string value_class = "";
            if (std::fabs(value) < 1e-9) {
                value_class = " class=\"neutral\"";
            } else if (value > 0) {
                value_class = " class=\"positive\"";
            } else {
                value_class = " class=\"negative\"";
            }

            std::string formatted_value;
            if (is_percentage) {
                formatted_value = format_with_commas(value, 2) + "%";
            } else {
                formatted_value = report_dollars(value, 2);
            }

            return "<div class=\"metric\"><strong>" + label + ":</strong> <span" + value_class +
                   ">" + formatted_value + "</span></div>\n";
        };

        // Add Total Positions line
        html << "<div class=\"metric\"><strong>Total Positions:</strong> " << total_positions_count
             << "</div>\n";

        auto daily_return_it = strategy_metrics.find("Daily Return");
        if (daily_return_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Return", daily_return_it->second, true);
        }

        auto daily_unrealized_it = strategy_metrics.find("Daily Unrealized PnL");
        if (daily_unrealized_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Unrealized PnL (Gross)",
                                          daily_unrealized_it->second, false);
        }

        auto daily_realized_it = strategy_metrics.find("Daily Realized PnL");
        if (daily_realized_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Realized PnL (Gross)", daily_realized_it->second,
                                          false);
        }

        auto daily_costs_it = strategy_metrics.find("Daily Transaction Costs");
        if (daily_costs_it != strategy_metrics.end()) {
            double cost_value = std::abs(daily_costs_it->second);
            std::string formatted_costs = report_dollars(cost_value, 2);
            html << "<div class=\"metric\"><strong>Daily Transaction Costs:</strong> <span>"
                 << formatted_costs << "</span></div>\n";
        }
        auto daily_total_it = strategy_metrics.find("Daily Total PnL");
        if (daily_total_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Total PnL (Net)", daily_total_it->second, false);
        }

        html << "</div>\n";
    }

    return html.str();
}

/*
std::string EmailSender::format_risk_metrics(const RiskResult& risk_metrics) {
    std::ostringstream html;

    html << "<div class=\"metric\">\n";
    html << "<strong>Volatility:</strong> " << std::fixed << std::setprecision(2)
         << (risk_metrics.portfolio_var * 100.0) << "%<br>\n";
    html << "<strong>Jump Risk (99th):</strong> " << std::fixed << std::setprecision(2)
         << risk_metrics.jump_risk << "<br>\n";
    html << "<strong>Risk Scale:</strong> " << std::fixed << std::setprecision(2)
         << risk_metrics.recommended_scale << "\n";
    html << "</div>\n";

    return html.str();
}
*/

std::string EmailSender::format_strategy_metrics(
    const std::map<std::string, double>& strategy_metrics, const OverLimitReport* over_limit,
    const std::string& report_date, const std::string& settled_date,
    const EmailReportContext& context) {
    std::ostringstream html;

    // Helper function to format numbers with commas
    auto format_with_commas = [](double value, int precision = 2) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(precision) << value;
        std::string str = oss.str();

        // Handle negative numbers by processing the absolute value
        size_t start_pos = 0;
        if (!str.empty() && str[0] == '-') {
            start_pos = 1;
        }

        // Find decimal point
        size_t decimal_pos = str.find('.');
        if (decimal_pos == std::string::npos) {
            decimal_pos = str.length();
        }

        // Insert commas (starting from after the negative sign if present)
        int insert_pos = static_cast<int>(decimal_pos) - 3;
        while (insert_pos > static_cast<int>(start_pos)) {
            str.insert(static_cast<size_t>(insert_pos), ",");
            insert_pos -= 3;
        }

        return str;
    };

    // Helper function to format value with proper units and color
    auto format_metric = [&format_with_commas](const std::string& key, double value,
                                               const std::string& label_override =
                                                   std::string()) -> std::string {
        std::ostringstream result;
        std::string formatted_value;
        std::string value_class;

        // Determine formatting based on metric type
        if (key.find("P&L") != std::string::npos || key.find("PnL") != std::string::npos ||
            key.find("Portfolio Value") != std::string::npos ||
            key.find("Notional") != std::string::npos ||
            key.find("Cash Available") != std::string::npos ||
            key.find("Margin Posted") != std::string::npos ||
            key.find("Transaction Costs") != std::string::npos ||
            key.find("Roll Costs") != std::string::npos ||
            key.find("Gross Profit") != std::string::npos ||
            key.find("Gross Loss") != std::string::npos) {
            formatted_value = report_dollars(value);
        } else if (key.find("Return") != std::string::npos ||
                   key.find("Volatility") != std::string::npos ||
                   key.find("Cushion") != std::string::npos ||
                   key.find("Rate") != std::string::npos ||
                   key.find("Best Day") != std::string::npos ||
                   key.find("Worst Day") != std::string::npos ||
                   key.find("Deviation") != std::string::npos ||
                   key.find("Drawdown") != std::string::npos ||
                   key.find("Average Win") != std::string::npos ||
                   key.find("Average Loss") != std::string::npos) {
            formatted_value = format_with_commas(value) + "%";
        } else if (key.find("Leverage") != std::string::npos ||
                   (key.find("Ratio") != std::string::npos &&
                    key.find("Sharpe") == std::string::npos &&
                    key.find("Sortino") == std::string::npos)) {
            formatted_value = format_with_commas(value) + "x";
        } else if (key.find("Positions") != std::string::npos ||
                   key.find("Trades") != std::string::npos ||
                   key.find("Days") != std::string::npos) {
            formatted_value = std::to_string(static_cast<int>(std::round(value)));
        } else {
            formatted_value = format_with_commas(value);
        }

        // Apply color coding for P&L and Return values
        bool colorize = (key.find("P&L") != std::string::npos) ||
                        (key.find("PnL") != std::string::npos) ||
                        (key.find("Return") != std::string::npos);
        if (colorize) {
            if (std::fabs(value) < 1e-9) {
                value_class = " class=\"neutral\"";
            } else if (value > 0) {
                value_class = " class=\"positive\"";
            } else {
                value_class = " class=\"negative\"";
            }
        }

        const std::string shown_label = label_override.empty() ? key : label_override;
        result << "<div class=\"metric\"><strong>" << shown_label << ":</strong> <span"
               << value_class << ">" << formatted_value << "</span></div>\n";
        return result.str();
    };

    // HD 10-14: ONE block, "Performance Metrics", in four groups separated by a blank line.
    html << "<h2>Performance Metrics</h2>\n";
    html << "<div class=\"metrics-category\">\n";
    auto show = [&](const char* key, const char* label) {
        const auto it = strategy_metrics.find(key);
        if (it != strategy_metrics.end()) html << format_metric(key, it->second, label);
    };
    show("Total Unrealized PnL", "Unrealized PnL");
    show("Total Realized PnL", "Realized PnL");
    show("Total Transaction Costs", "Transaction Costs");
    show("Total PnL", "Total PnL (Net)");
    // The equity book's cumulative dividend income (T-8D-2 R44 (c)): informational.
    {
        const auto dividend_income = strategy_metrics.find(kReportDividendIncome);
        if (dividend_income != strategy_metrics.end()) {
            html << dividend_line_html(dividend_income->second);
        }
    }
    show("Current Portfolio Value", "Current Portfolio Value");
    html << "<br>\n";
    show("Total Cumulative Return", "Total Cumulative Return");
    show("Total Annualized Return", "Annualized Return");
    show("Sharpe Ratio", "Sharpe Ratio");
    show("Sortino Ratio", "Sortino Ratio");
    show("Volatility", "Volatility");
    show("Max Drawdown", "Max Drawdown");
    html << "<br>\n";
    show("Cash Available", "Cash Available");
    show("Margin Posted", "Margin Posted");
    if (!context.equities) {  // meaningless on a cash account (HD 2026-10-14, section 16.7)
        show("Equity-to-Margin Ratio", "Equity-to-Margin Ratio");
        show("Margin Cushion", "Margin Cushion");
    }
    html << "<br>\n";
    show("Gross Leverage", "Gross Leverage");
    show("Net Leverage", "Net Leverage");
    html << "</div>\n";

    return html.str();
}
std::string EmailSender::format_symbols_table_for_positions(
    const std::unordered_map<std::string, Position>& positions,
    std::shared_ptr<DatabaseInterface> db, const std::string& date) {
    std::ostringstream html;

    // 1) Collect normalized base symbols from non-zero positions only
    std::set<std::string> base_syms;
    INFO("EmailSender: Collecting symbols from positions for symbols reference table");
    for (const auto& [sym, pos] : positions) {
        if (pos.quantity.as_double() == 0.0)
            continue;
        std::string b = sym;
        if (auto p = b.find(".v."); p != std::string::npos)
            b = b.substr(0, p);
        if (auto p = b.find(".c."); p != std::string::npos)
            b = b.substr(0, p);
        std::transform(b.begin(), b.end(), b.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        b.erase(std::remove_if(b.begin(), b.end(),
                               [](unsigned char c) { return !(std::isalnum(c) || c == '/'); }),
                b.end());
        if (!b.empty()) {
            INFO("EmailSender: Position " + sym + " (qty=" + std::to_string(pos.quantity.as_double()) + ") -> normalized symbol: " + b);
            base_syms.insert(b);
        }
    }

    // Log all collected symbols
    std::ostringstream collected_syms;
    for (const auto& s : base_syms) {
        if (!collected_syms.str().empty()) collected_syms << ", ";
        collected_syms << s;
    }
    INFO("EmailSender: Collected symbols for query: " + collected_syms.str());

    if (base_syms.empty()) {
        html << "<p>No active positions to display symbol metadata for.</p>\n";
        return html.str();
    }

    auto make_in_list = [](const std::set<std::string>& S) {
        std::ostringstream oss;
        bool first = true;
        for (const auto& s : S) {
            if (!first)
                oss << ", ";
            oss << "'" << s << "'";
            first = false;
        }
        return oss.str();
    };
    const std::string in_list = make_in_list(base_syms);

    try {
        // 2) Padded schema + filter on EITHER Databento or IB symbol
        const std::string sql =
            "SELECT "
            "  CURRENT_TIMESTAMP AS \"time\","
            "  to_jsonb(json_build_object("
            "    'db',     \"Databento Symbol\","
            "    'ib',     \"IB Symbol\","
            "    'name',   \"Name\","
            "    'months', \"Contract Months\""
            "  ))::text AS \"symbol\","
            "  0.0::double precision AS \"open\","
            "  0.0::double precision AS \"high\","
            "  0.0::double precision AS \"low\","
            "  0.0::double precision AS \"close\","
            "  0.0::double precision AS \"volume\","
            "  0.0::double precision AS \"vwap\","
            "  0.0::double precision AS \"bid\","
            "  0.0::double precision AS \"ask\","
            "  0.0::double precision AS \"last\","
            "  0::bigint              AS \"count\","
            "  0.0::double precision AS \"open_interest\" "
            "FROM metadata.contract_metadata "
            "WHERE \"Databento Symbol\" IN (" +
            in_list +
            ") "
            "   OR \"IB Symbol\"       IN (" +
            in_list +
            ") "
            "ORDER BY \"Name\"";

        auto qr = db->execute_query(sql);
        if (qr.is_error()) {
            auto err = qr.error()->to_string();
            WARN(std::string("Symbols query failed: ") + err);
            html << "<p>Unable to load symbols data: " << err << "</p>\n";
            return html.str();
        }

        auto table = qr.value();
        if (!table || table->num_rows() == 0) {
            html << "<p>No symbol metadata found for active positions.</p>\n";
            return html.str();
        }

        auto combine_res = table->CombineChunks();
        if (!combine_res.ok()) {
            WARN(std::string("CombineChunks failed: ") + combine_res.status().ToString());
            html << "<p>Error loading symbols data</p>\n";
            return html.str();
        }
        auto combined = combine_res.ValueOrDie();

        // Find 'symbol' (JSON text) column
        int idx_symbol = -1;
        auto schema = combined->schema();
        for (int i = 0; i < schema->num_fields(); ++i) {
            if (schema->field(i)->name() == "symbol") {
                idx_symbol = i;
                break;
            }
        }
        if (idx_symbol < 0) {
            WARN("Symbols table: required metadata fields missing. Got fields: " +
                 schema->ToString());
            html << "<p>Error loading symbols data</p>\n";
            return html.str();
        }

        auto get_str = [](const std::shared_ptr<arrow::Array>& arr, int64_t i) -> std::string {
            if (!arr || arr->IsNull(i))
                return "";
            if (arr->type_id() == arrow::Type::STRING)
                return std::static_pointer_cast<arrow::StringArray>(arr)->GetString(i);
            if (arr->type_id() == arrow::Type::LARGE_STRING)
                return std::static_pointer_cast<arrow::LargeStringArray>(arr)->GetString(i);
            return "";
        };

        auto col_symbol = combined->column(idx_symbol)->chunk(0);

        // Render
        html << "<div class=\"summary-stats\" style=\"margin-top:6px;\">"
                "<strong>Month Codes:</strong> F=Jan, G=Feb, H=Mar, J=Apr, K=May, M=Jun, N=Jul, "
                "Q=Aug, U=Sep, V=Oct, X=Nov, Z=Dec"
                "</div>\n";
        html << "<table>\n";
        html << "<tr><th>Databento Symbol</th><th>IB Symbol</th>"
                "<th>Name</th><th>Contract Months</th></tr>\n";

        // Track matched symbols to detect gaps
        std::set<std::string> matched;

        // helper to uppercase once, reuse
        auto up = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        };

        // No hard-coded rows - only show symbols from actual positions

        for (int64_t i = 0; i < combined->num_rows(); ++i) {
            std::string json_txt = get_str(col_symbol, i);
            std::string db_sym, ib_sym, name, months;
            try {
                if (json_txt.empty()) {
                    WARN("Symbols reference: metadata row " + std::to_string(i) +
                         " has no symbol text; its cells print empty");
                } else {
                    nlohmann::json j = nlohmann::json::parse(json_txt);
                    db_sym = j.value("db", "");
                    ib_sym = j.value("ib", "");
                    name = j.value("name", "");
                    months = j.value("months", "");
                }
            } catch (const std::exception& e) {
                WARN("Symbols reference: metadata row " + std::to_string(i) +
                     " could not be read (" + e.what() + "); its cells print empty");
            }
            if (!db_sym.empty())
                matched.insert(up(db_sym));
            if (!ib_sym.empty())
                matched.insert(up(ib_sym));

            html << "<tr>\n";
            html << "<td>" << db_sym << "</td>\n";
            html << "<td>" << ib_sym << "</td>\n";
            html << "<td>" << name << "</td>\n";
            html << "<td>" << months << "</td>\n";
            html << "</tr>\n";
        }

        html << "</table>\n";

        // Show any active symbols we couldn’t match
        std::vector<std::string> missing;
        for (const auto& s : base_syms)
            if (!matched.count(s))
                missing.push_back(s);
        if (!missing.empty()) {
            html << "<p style=\"color:#b42318\"><strong>Note:</strong> metadata not found for: ";
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i)
                    html << ", ";
                html << missing[i];
            }
            html << ".</p>\n";
            WARN("Symbols reference: missing metadata for " + std::to_string(missing.size()) +
                 " symbols");
        }

    } catch (const std::exception& e) {
        WARN(std::string("Exception while formatting filtered symbols table: ") + e.what());
        html << "<p>Error loading symbols data</p>\n";
    }

    return html.str();
}

std::string EmailSender::format_strategy_display_name(const std::string& strategy_id) {
    std::string result;
    bool capitalize_next = true;

    for (size_t i = 0; i < strategy_id.size(); ++i) {
        char c = strategy_id[i];
        if (c == '_') {
            result += ' ';
            capitalize_next = true;
        } else if (capitalize_next) {
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            capitalize_next = false;
        } else {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    return result;
}

// T-8D D1: one label for the margin figure on every book ("Margin Posted"); what differs is the
// rule that produced it, and that is named in the sleeve's sub-header: the kind of instrument the
// sleeve holds and, for equities, the account mode its margin follows. "futures"; "equities, cash
// account"; "equities, Reg T account"; a sleeve holding more than one names each, in that order.
// Empty when no held row of the sleeve is in the registry.
static std::string sleeve_margin_kind(const std::unordered_map<std::string, Position>& positions) {
    bool futures = false;
    bool cash_equities = false;
    bool reg_t_equities = false;
    for (const auto& [symbol, position] : positions) {
        if (position.quantity.as_double() == 0.0)
            continue;
        std::string lookup_sym = position.symbol.empty() ? symbol : position.symbol;
        for (const char* suffix : {".v.", ".c."}) {
            const auto dotpos = lookup_sym.find(suffix);
            if (dotpos != std::string::npos)
                lookup_sym = lookup_sym.substr(0, dotpos);
        }
        std::shared_ptr<Instrument> instrument;
        try {
            instrument = InstrumentRegistry::instance().get_instrument(lookup_sym);
        } catch (...) {
        }
        if (!instrument)
            continue;
        if (const auto* equity = dynamic_cast<const EquityInstrument*>(instrument.get())) {
            (equity->get_account_mode() == EquityAccountMode::REG_T ? reg_t_equities
                                                                    : cash_equities) = true;
        } else if (instrument->get_type() == AssetType::FUTURE) {
            futures = true;
        }
    }
    std::string kind;
    const auto add = [&kind](const char* text) {
        kind += (kind.empty() ? "" : "; ") + std::string(text);
    };
    if (futures)
        add("futures");
    if (cash_equities)
        add("equities, cash account");
    if (reg_t_equities)
        add("equities, Reg T account");
    return kind;
}

std::string EmailSender::format_single_strategy_table(
    const std::string& strategy_name, const std::unordered_map<std::string, Position>& positions,
    const std::unordered_map<std::string, double>& current_prices) {
    std::ostringstream html;

    if (positions.empty()) {
        return std::string();
    }

    // Helper function to format numbers with commas
    auto format_with_commas = [](double value) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << value;
        std::string str = oss.str();

        size_t start_pos = 0;
        if (!str.empty() && str[0] == '-') {
            start_pos = 1;
        }

        size_t decimal_pos = str.find('.');
        if (decimal_pos == std::string::npos) {
            decimal_pos = str.length();
        }

        int insert_pos = static_cast<int>(decimal_pos) - 3;
        while (insert_pos > static_cast<int>(start_pos)) {
            str.insert(static_cast<size_t>(insert_pos), ",");
            insert_pos -= 3;
        }

        return str;
    };

    // Strategy sub-header with styled left border accent; the kind of instrument and the account
    // mode the sleeve's margin follows beside the name (T-8D D1).
    std::string display_name = format_strategy_display_name(strategy_name);
    const std::string margin_kind = sleeve_margin_kind(positions);
    html << "<h3 style=\"margin-top: 20px; margin-bottom: 2px; color: #333; "
         << "border-left: 4px solid #2c5aa0; padding-left: 12px;\">" << display_name << "</h3>\n";
    if (!margin_kind.empty()) {
        std::string kind_text = margin_kind;
        kind_text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(kind_text[0])));
        html << "<div style=\"font-size: 11px; color: #999; margin: 0 0 8px 16px;\">" << kind_text
             << "</div>\n";
    }

    // Position table
    html << "<table>\n";
    html << "<tr><th>Symbol</th><th>Quantity</th><th>Price</th><th>Notional</th><th>% of "
            "Total</th></tr>\n";

    double total_notional = 0.0;
    double total_margin_posted = 0.0;
    int active_positions = 0;

    // First pass: calculate total notional for this strategy
    std::vector<std::tuple<std::string, double, double, double, double>> position_data;
    std::set<std::string> rows_without_close;
    std::vector<std::string> margin_left_out;

    for (const auto& [symbol, position_ptr] : positions_by_symbol(positions)) {
        const Position& position = *position_ptr;
        if (position.quantity.as_double() != 0.0) {
            active_positions++;

            double contract_multiplier = 1.0;
            double notional = 0.0;
            double margin_for_position = 0.0;

            try {
                auto& registry = InstrumentRegistry::instance();
                std::string lookup_sym = position.symbol;
                auto dotpos = lookup_sym.find(".v.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }
                dotpos = lookup_sym.find(".c.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }

                auto instrument = registry.get_instrument(lookup_sym);
                if (!instrument) {
                    ERROR("CRITICAL: Instrument " + lookup_sym +
                          " not found in registry for email generation!");
                    throw std::runtime_error("Missing instrument in registry: " + lookup_sym);
                }

                contract_multiplier = instrument->get_multiplier();
                if (contract_multiplier <= 0) {
                    ERROR("CRITICAL: Invalid multiplier " + std::to_string(contract_multiplier) +
                          " for " + lookup_sym);
                    throw std::runtime_error("Invalid multiplier for: " + lookup_sym);
                }

                // Price/qty overload: returns total dollars for both equities
                // (account-mode-aware notional fraction) and futures (|qty| ×
                // per-contract margin, after the FuturesInstrument override).
                const double signed_qty = position.quantity.as_double();

                // FUT-email-zero-basis (C-5 B5). Margin is priced from a MARK, not from a
                // cost basis, and one row without a basis must not suppress the whole
                // report.
                //
                // This read average_price only. Zero is that column's documented "no basis
                // known" value (AVERAGE_PRICE_LIFECYCLE rule 5) and is reachable for a held
                // position whose basis could not be resolved, which the runner already
                // reports as an ERROR of its own. get_margin_requirement(0, qty) then
                // returns 0, this threw, and the `throw;` below carried it out of
                // format_single_strategy_table, out of format_strategy_positions_tables and
                // out of generate_trading_report_body: one unpriceable row and NOBODY got a
                // daily report at all. The catch(...) in the portfolio-total loop below
                // then swallowed the same failure silently, so the margin line understated
                // without saying so.
                //
                // This is the fix 9348920d made to the single-table equity overload
                // (format_positions_table, above), applied to the per-strategy futures
                // overload it missed. The price preference is kept EXACTLY as this
                // overload always had it: the row's own average_price first, and the
                // current close only as the fallback when the basis is missing. Preferring
                // the close here would re-price every equity row's margin (a basis of 50
                // against a close of 52 moves the line from 2,500 to 2,600) on a stream
                // this change promises to leave byte-identical; the single-table equity
                // overload above prefers the close, and that inconsistency is recorded
                // for the statistics pass rather than resolved by a side effect here.
                double price_for_margin = 0.0;
                if (position.average_price.as_double() > 0.0) {
                    price_for_margin = position.average_price.as_double();
                } else {
                    auto margin_price_it = current_prices.find(symbol);
                    if (margin_price_it != current_prices.end() && margin_price_it->second > 0.0) {
                        price_for_margin = margin_price_it->second;
                    }
                }

                // Ask the instrument, whatever price is available -- do NOT decide from
                // the price alone that the row is unpriceable. FuturesInstrument's margin
                // is |qty| x initial_margin and ignores the price entirely, so a futures
                // row with no basis still HAS a margin and must keep contributing it. Only
                // a non-positive answer FROM THE INSTRUMENT means there is nothing to add,
                // and that is what used to throw.
                margin_for_position =
                    instrument->get_margin_requirement(price_for_margin, signed_qty);
                if (margin_for_position <= 0) {
                    // Equities land here when neither a close nor a basis is known (margin
                    // is a fraction of notional, so price 0 gives 0); futures land here when
                    // the contract's own metadata carries no initial margin. Either way it
                    // is one row that cannot be added up, not a reason to withhold the
                    // report from everybody.
                    margin_left_out.push_back(symbol);
                    WARN("Daily email: no usable margin for " + lookup_sym + " (quantity " +
                         std::to_string(signed_qty) + ", price " +
                         std::to_string(price_for_margin) +
                         ") -- excluded from the margin total. The email is still sent; "
                         "margin posted is understated by this position.");
                    margin_for_position = 0.0;
                } else {
                    total_margin_posted += margin_for_position;
                }

            } catch (const std::exception& e) {
                ERROR("CRITICAL: Failed to get instrument data for " + position.symbol + ": " +
                      e.what());
                throw;
            }

            double market_price = position.average_price.as_double();
            auto price_it = current_prices.find(symbol);
            if (price_it != current_prices.end()) {
                market_price = price_it->second;
            } else {
                rows_without_close.insert(symbol);
            }

            // T-8D-2 R60: the notional at the price the row prints (the Day T-1 close; the basis
            // only when the row has no close), as in the single table.
            notional = position.quantity.as_double() * market_price * contract_multiplier;
            total_notional += std::abs(notional);

            position_data.push_back(std::make_tuple(symbol, position.quantity.as_double(),
                                                    market_price, notional, margin_for_position));
        }
    }

    // Second pass: render rows with % of total (relative to this strategy's total)
    for (const auto& [symbol, qty, market_price, notional, margin] : position_data) {
        double pct_of_total =
            (total_notional > 0) ? (std::abs(notional) / total_notional * 100.0) : 0.0;

        html << "<tr>\n";
        html << "<td>" << symbol << "</td>\n";
        html << "<td>" << report_quantity_text(report_instrument_of(symbol), qty) << "</td>\n";
        html << "<td>" << report_price_dollars_places(market_price, report_places_of(symbol))
             << (rows_without_close.count(symbol) ? note_sup(4) : std::string()) << "</td>\n";
        html << "<td>" << report_dollars(std::abs(notional)) << "</td>\n";
        html << "<td>" << std::fixed << std::setprecision(2) << pct_of_total << "%</td>\n";
        html << "</tr>\n";
    }

    html << "</table>\n";

    // Compact strategy-level summary
    if (!g_single_sleeve) {
    html << "<div style=\"font-size: 13px; color: #666; margin: 8px 0 20px 0; padding-left: "
            "16px;\">\n";
    html << "<strong>Positions:</strong> " << active_positions << " | <strong>Notional:</strong> "
         << report_dollars(total_notional) << " | <strong>Margin:</strong> "
         << report_dollars(total_margin_posted) << "\n";
    html << "</div>\n";
    }

    return html.str();
}

std::string EmailSender::format_strategy_positions_tables(
    const StrategyPositionsMap& strategy_positions,
    const std::unordered_map<std::string, double>& current_prices,
    const std::map<std::string, double>& strategy_metrics) {
    std::ostringstream html;
    SingleSleeveScope single_sleeve_scope;
    g_single_sleeve = false;
    {
        int sleeves = 0;
        for (const auto& [name, positions] : strategy_positions) {
            (void)name;
            for (const auto& [symbol, pos] : positions) {
                (void)symbol;
                if (pos.quantity.as_double() != 0.0) { ++sleeves; break; }
            }
        }
        g_single_sleeve = sleeves == 1;
    }
    

    if (strategy_positions.empty()) {
        html << "<p>No positions.</p>\n";
        return html.str();
    }

    // Helper function to format numbers with commas
    auto format_with_commas = [](double value) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << value;
        std::string str = oss.str();

        size_t start_pos = 0;
        if (!str.empty() && str[0] == '-') {
            start_pos = 1;
        }

        size_t decimal_pos = str.find('.');
        if (decimal_pos == std::string::npos) {
            decimal_pos = str.length();
        }

        int insert_pos = static_cast<int>(decimal_pos) - 3;
        while (insert_pos > static_cast<int>(start_pos)) {
            str.insert(static_cast<size_t>(insert_pos), ",");
            insert_pos -= 3;
        }

        return str;
    };

    // Sort strategies alphabetically for consistent ordering
    std::vector<std::string> strategy_names;
    for (const auto& [name, _] : strategy_positions) {
        strategy_names.push_back(name);
    }
    std::sort(strategy_names.begin(), strategy_names.end());

    // Track portfolio totals
    double portfolio_total_notional = 0.0;
    double portfolio_total_margin = 0.0;
    int portfolio_total_positions = 0;

    // Generate table for each strategy
    for (const auto& strategy_name : strategy_names) {
        const auto& positions = strategy_positions.at(strategy_name);

        // Skip empty strategies
        bool has_active_positions = false;
        for (const auto& [_, pos] : positions) {
            if (pos.quantity.as_double() != 0.0) {
                has_active_positions = true;
                break;
            }
        }
        if (!has_active_positions) {
            continue;
        }

        html << format_single_strategy_table(strategy_name, positions, current_prices);

        // Accumulate portfolio totals
        for (const auto& [symbol, position_ptr] : positions_by_symbol(positions)) {
            const Position& position = *position_ptr;
            if (position.quantity.as_double() != 0.0) {
                portfolio_total_positions++;

                try {
                    auto& registry = InstrumentRegistry::instance();
                    std::string lookup_sym = position.symbol;
                    auto dotpos = lookup_sym.find(".v.");
                    if (dotpos != std::string::npos) {
                        lookup_sym = lookup_sym.substr(0, dotpos);
                    }
                    dotpos = lookup_sym.find(".c.");
                    if (dotpos != std::string::npos) {
                        lookup_sym = lookup_sym.substr(0, dotpos);
                    }

                    auto instrument = registry.get_instrument(lookup_sym);
                    if (instrument) {
                        double contract_multiplier = instrument->get_multiplier();
                        // T-8D-2 R60: at the price the sleeve's row prints.
                        double market_price = position.average_price.as_double();
                        auto price_it = current_prices.find(symbol);
                        if (price_it != current_prices.end()) {
                            market_price = price_it->second;
                        }
                        double notional =
                            position.quantity.as_double() * market_price * contract_multiplier;
                        portfolio_total_notional += std::abs(notional);

                        // Use the price/qty overload (returns total dollars).
                        // Required for equities to get account-mode-aware
                        // margin instead of the legacy 0.0 sentinel.
                        //
                        // FUT-email-zero-basis: priced from the same mark the per-strategy
                        // table above uses, basis first and the current close only as the
                        // fallback, so the portfolio total equals the sum of the per-strategy
                        // tables printed immediately above it whether or not a basis exists.
                        // Reading average_price alone here dropped a no-basis row from the
                        // total, and the catch(...) below meant nothing said so.
                        const double signed_qty = position.quantity.as_double();
                        double price_for_margin = 0.0;
                        if (position.average_price.as_double() > 0.0) {
                            price_for_margin = position.average_price.as_double();
                        } else {
                            auto margin_price_it = current_prices.find(symbol);
                            if (margin_price_it != current_prices.end() &&
                                margin_price_it->second > 0.0) {
                                price_for_margin = margin_price_it->second;
                            }
                        }
                        const double m =
                            instrument->get_margin_requirement(price_for_margin, signed_qty);
                        if (m > 0.0) portfolio_total_margin += m;
                    }
                } catch (...) {
                    // Already logged in format_single_strategy_table
                }
            }
        }
    }

    // Portfolio-wide summary
    html << "<div class=\"summary-stats\" style=\"margin-top: 20px; border-top: 2px solid #2c5aa0; "
            "padding-top: 15px;\">\n";
    // T-8D-2 R60: the count is of sleeve rows (a symbol two sleeves hold is two), and the two
    // dollar lines below are the ACCOUNT's figures when the runner hands them (its gross notional
    // and its posted margin, which net sleeves holding one symbol on opposite sides); the sum
    // over the sleeves' tables otherwise.
    html << "<div class=\"metric\"><strong>Sleeve Positions:</strong> " << portfolio_total_positions
         << "</div>\n";
    const auto account_notional = strategy_metrics.find(kReportGrossNotional);
    if (account_notional != strategy_metrics.end()) {
        portfolio_total_notional = account_notional->second;
    }
    const auto account_margin = strategy_metrics.find("Margin Posted");
    if (account_margin != strategy_metrics.end()) {
        portfolio_total_margin = account_margin->second;
    }

    // The expected risk is in the performance metrics, the VaR in the account detail file (HD 10-13).

    html << "<div class=\"metric\"><strong>Total Notional:</strong> "
         << report_dollars(portfolio_total_notional) << "</div>\n";
    html << "<div class=\"metric\"><strong>Total Margin Posted:</strong> "
         << report_dollars(portfolio_total_margin) << "</div>\n";
    html << "</div>\n";

    return html.str();
}

std::string EmailSender::format_single_strategy_executions_table(
    const std::string& strategy_name, const std::vector<ExecutionReport>& executions,
    bool several_sleeves) {
    std::ostringstream html;

    if (executions.empty()) {
        return std::string();
    }

    // Helper function to format numbers with commas
    auto format_with_commas = [](double value) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << value;
        std::string str = oss.str();

        size_t start_pos = 0;
        if (!str.empty() && str[0] == '-') {
            start_pos = 1;
        }

        size_t decimal_pos = str.find('.');
        if (decimal_pos == std::string::npos) {
            decimal_pos = str.length();
        }

        int insert_pos = static_cast<int>(decimal_pos) - 3;
        while (insert_pos > static_cast<int>(start_pos)) {
            str.insert(static_cast<size_t>(insert_pos), ",");
            insert_pos -= 3;
        }

        return str;
    };

    // Strategy sub-header with styled left border accent
    std::string display_name = format_strategy_display_name(strategy_name);
    html << "<h3 style=\"margin-top: 20px; margin-bottom: 10px; color: #333; "
         << "border-left: 4px solid #2c5aa0; padding-left: 12px;\">" << display_name << "</h3>\n";

    // Executions table
    html << "<table>\n";
    // A book of more than one sleeve shows, beside each row's own cost, the row's netting
    // adjustment (as it acts on the cost) and its cost after netting. A one-sleeve book's table is
    // as it always was.
    if (several_sleeves) {
        html << "<tr><th>Symbol</th><th>Side</th><th>Quantity</th><th>Price</th><th>Notional</"
                "th><th>Transaction Cost</th><th>Netting Adjustment</th><th>Cost After Netting</"
                "th></tr>\n";
    } else {
        html << "<tr><th>Symbol</th><th>Side</th><th>Quantity</th><th>Price</th><th>Notional</"
                "th><th>Transaction Cost</th></tr>\n";
    }

    double total_transaction_costs = 0.0;
    double total_notional_traded = 0.0;
    size_t strategy_trades = 0;
    NettingFooter netting;

    for (const auto& exec : executions) {
        netting.add(exec);
        // Every row's cost after netting: the sleeve's share of what the account was charged.
        total_transaction_costs += transaction_cost::net_cost(exec).as_double();
        if (exec.execution_type != ExecutionType::STRATEGY) continue;        // T-ROLLX
        ++strategy_trades;
        double contract_multiplier = 1.0;
        bool has_multiplier = true;

        try {
            auto& registry = InstrumentRegistry::instance();
            std::string lookup_sym = exec.symbol;
            auto dotpos = lookup_sym.find(".v.");
            if (dotpos != std::string::npos) {
                lookup_sym = lookup_sym.substr(0, dotpos);
            }
            dotpos = lookup_sym.find(".c.");
            if (dotpos != std::string::npos) {
                lookup_sym = lookup_sym.substr(0, dotpos);
            }
            auto instrument = registry.get_instrument(lookup_sym);
            if (instrument) {
                // The multiplier is the registry's (metadata "Contract Size"), the only source.
                contract_multiplier = instrument->get_multiplier();
            } else {
                has_multiplier = false;
            }
        } catch (...) {
            has_multiplier = false;
        }
        if (!has_multiplier) {
            ERROR("Email: " + exec.symbol + " is not in the instrument registry "
                  "(metadata.contract_metadata); its notional is shown as n/a and left out of "
                  "the total, not guessed");
        }

        double notional = has_multiplier ? exec.filled_quantity.as_double() *
                                               exec.fill_price.as_double() * contract_multiplier
                                         : 0.0;
        total_notional_traded += notional;

        std::string side_str = exec.side == Side::BUY ? "BUY" : "SELL";
        std::string side_class = exec.side == Side::BUY ? "positive" : "negative";

        html << "<tr>\n";
        html << "<td>" << exec.symbol << "</td>\n";
        html << "<td class=\"" << side_class << "\">" << side_str << "</td>\n";
        html << "<td>"
             << report_quantity_text(report_instrument_of(exec.symbol),
                                     exec.filled_quantity.as_double())
             << "</td>\n";
        html << "<td>" << report_price_dollars_places(exec.fill_price.as_double(), report_places_of(exec.symbol)) << "</td>\n";
        if (has_multiplier) {
            html << "<td>" << report_dollars(notional) << "</td>\n";
        } else {
            html << "<td>n/a</td>\n";
        }
        html << "<td>" << report_dollars(exec.total_transaction_costs.as_double())
             << "</td>\n";
        if (several_sleeves) {
            html << "<td>"
                 << netting_effect_text(exec.total_transaction_costs.as_double(),
                                        transaction_cost::net_cost(exec).as_double())
                 << "</td>\n";
            html << "<td>" << report_dollars(transaction_cost::net_cost(exec).as_double())
                 << "</td>\n";
        }
        html << "</tr>\n";
    }

    html << "</table>\n";
    html << format_roll_legs_block(executions);

    // Compact strategy-level summary
    if (!g_single_sleeve) {
    html << "<div style=\"font-size: 13px; color: #666; margin: 8px 0 20px 0; padding-left: "
            "16px;\">\n";
    html << "<strong>Trades:</strong> " << strategy_trades << " | <strong>Notional:</strong> "
         << report_dollars(total_notional_traded);
    // Shown only when a row of this sleeve was netted: own costs, the adjustment as it acts on
    // them, then what the sleeve was charged.
    html << " | <strong>Transaction Costs:</strong> " << report_dollars(total_transaction_costs)
         << (netting.any_adjusted ? note_sup(5) : std::string()) << "\n";
    html << "</div>\n";
    }

    return html.str();
}

std::string EmailSender::format_strategy_executions_tables(
    const StrategyExecutionsMap& strategy_executions) {
    std::ostringstream html;
    SingleSleeveScope single_sleeve_scope;
    g_single_sleeve = false;
    {
        int sleeves = 0;
        for (const auto& [name, execs] : strategy_executions) {
            (void)name;
            if (!execs.empty()) ++sleeves;
        }
        g_single_sleeve = sleeves == 1;
    }
    

    if (strategy_executions.empty()) {
        html << "<p>No executions for today.</p>\n";
        return html.str();
    }

    // Helper function to format numbers with commas
    auto format_with_commas = [](double value) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << value;
        std::string str = oss.str();

        size_t start_pos = 0;
        if (!str.empty() && str[0] == '-') {
            start_pos = 1;
        }

        size_t decimal_pos = str.find('.');
        if (decimal_pos == std::string::npos) {
            decimal_pos = str.length();
        }

        int insert_pos = static_cast<int>(decimal_pos) - 3;
        while (insert_pos > static_cast<int>(start_pos)) {
            str.insert(static_cast<size_t>(insert_pos), ",");
            insert_pos -= 3;
        }

        return str;
    };

    // Sort strategies alphabetically for consistent ordering
    std::vector<std::string> strategy_names;
    for (const auto& [name, _] : strategy_executions) {
        strategy_names.push_back(name);
    }
    std::sort(strategy_names.begin(), strategy_names.end());

    // Track portfolio totals
    int portfolio_total_trades = 0;
    double portfolio_total_notional = 0.0;
    double portfolio_total_transaction_costs = 0.0;
    double portfolio_total_roll_costs = 0.0;  // T-ROLLX
    int portfolio_total_roll_fills = 0;
    NettingFooter netting;
    // A printed cent that differs from the footer under it (HD ruling Q3): a column of rows
    // against its sleeve's footer, and the sleeves' footers against the book's.
    bool printed_cent_differs = false;
    email_netting::RoundingCheck book_cost_of_sleeves, book_notional_of_sleeves,
        book_own_of_sleeves;
    // A book of more than one sleeve: each row also shows its adjustment and its cost after netting.
    const bool several_sleeves = strategy_executions.size() > 1;

    // Generate table for each strategy
    for (const auto& strategy_name : strategy_names) {
        const auto& executions = strategy_executions.at(strategy_name);

        if (executions.empty()) {
            continue;
        }

        html << format_single_strategy_executions_table(strategy_name, executions,
                                                        several_sleeves);

        // Accumulate portfolio totals (T-ROLLX: trades and notional over STRATEGY rows; costs over
        // every row, the ROLL part shown beside)
        email_netting::RoundingCheck sleeve_cost, sleeve_notional, sleeve_own;
        for (const auto& exec : executions) {
            netting.add(exec);
            sleeve_cost.add(transaction_cost::net_cost(exec).as_double());
            sleeve_own.add(exec.total_transaction_costs.as_double());
            portfolio_total_transaction_costs += transaction_cost::net_cost(exec).as_double();
            if (exec.execution_type == ExecutionType::ROLL) {  // never netted: its own cost
                portfolio_total_roll_costs += exec.total_transaction_costs.as_double();
                ++portfolio_total_roll_fills;
            }
            if (exec.execution_type != ExecutionType::STRATEGY) continue;
            ++portfolio_total_trades;
            // The sleeve table's rule (T-8D-2 R62): a symbol that is not in the registry has no
            // notional (its cell reads n/a) and is left out of the total, never counted at a
            // multiplier of 1.
            double contract_multiplier = 1.0;
            bool has_multiplier = false;
            try {
                auto& registry = InstrumentRegistry::instance();
                std::string lookup_sym = exec.symbol;
                auto dotpos = lookup_sym.find(".v.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }
                dotpos = lookup_sym.find(".c.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }
                auto instrument = registry.get_instrument(lookup_sym);
                if (instrument) {
                    contract_multiplier = instrument->get_multiplier();
                    has_multiplier = true;
                }
            } catch (...) {
            }
            if (!has_multiplier)
                continue;

            double notional = exec.filled_quantity.as_double() * exec.fill_price.as_double() *
                              contract_multiplier;
            portfolio_total_notional += notional;
            sleeve_notional.add(notional);
        }
        printed_cent_differs = printed_cent_differs || sleeve_cost.differs() ||
                               sleeve_notional.differs() || sleeve_own.differs();
        book_cost_of_sleeves.add(sleeve_cost.exact);
        book_notional_of_sleeves.add(sleeve_notional.exact);
        book_own_of_sleeves.add(sleeve_own.exact);
    }
    printed_cent_differs = printed_cent_differs || book_cost_of_sleeves.differs() ||
                           book_notional_of_sleeves.differs() || book_own_of_sleeves.differs();

    // Portfolio-wide summary
    html << "<div class=\"summary-stats\" style=\"margin-top: 20px; border-top: 2px solid #2c5aa0; "
            "padding-top: 15px;\">\n";
    html << "<div class=\"metric\"><strong>Total Trades:</strong> " << portfolio_total_trades
         << "</div>\n";
    html << "<div class=\"metric\"><strong>Total Notional Traded:</strong> "
         << report_dollars(portfolio_total_notional) << "</div>\n";
    // "Today's": the same words would otherwise name the cumulative figure of the snapshot.
    html << "<div class=\"metric\"><strong>Today's Transaction Costs:</strong> "
         << report_dollars(portfolio_total_transaction_costs)
         << (netting.any_adjusted ? note_sup(5) : std::string()) << "</div>\n";
    if (netting.any_adjusted) {
        g_netting_note = "Netting between strategies: own costs " +
                         report_dollars(netting.own_costs.as_double()) + ", adjustment " +
                         netting_effect_text(netting.own_costs.as_double(),
                                             portfolio_total_transaction_costs) +
                         ", transaction costs " + report_dollars(portfolio_total_transaction_costs) +
                         ".";
    }
    if (portfolio_total_roll_fills > 0) {
        html << "<div class=\"metric\"><strong>Roll Fills:</strong> " << portfolio_total_roll_fills
             << "</div>\n";
    }
    html << "</div>\n";

    return html.str();
}

std::string EmailSender::generate_trading_report_body(
    const StrategyPositionsMap& strategy_positions,
    const std::unordered_map<std::string, Position>& positions,
    const std::optional<RiskResult>& risk_metrics,
    const std::map<std::string, double>& strategy_metrics,
    const StrategyExecutionsMap& strategy_executions, const std::string& date,
    const std::string& portfolio_name, bool is_daily_strategy,
    const std::unordered_map<std::string, double>& current_prices,
    std::shared_ptr<DatabaseInterface> db, const StrategyPositionsMap& yesterday_strategy_positions,
    const std::unordered_map<std::string, double>& yesterday_close_prices,
    const std::unordered_map<std::string, double>& two_days_ago_close_prices,
    const std::map<std::string, double>& yesterday_daily_metrics,
    const std::string& chart_strategy_id, const OverLimitReport& over_limit,
    const EmailReportContext& context) {
    (void)risk_metrics;
    reset_notes();
    std::ostringstream html;

    // Parse the date to check day of week
    std::tm tm = {};
    std::istringstream ss(date);
    ss >> std::get_time(&tm, "%Y-%m-%d");
    std::mktime(&tm);

    // Calculate yesterday's date
    std::string yesterday_date_str;
    std::string yesterday_holiday_name;
    bool is_yesterday_holiday = false;
    try {
        auto time_point = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        auto yesterday = time_point - std::chrono::hours(24);
        auto yesterday_time_t = std::chrono::system_clock::to_time_t(yesterday);
        std::tm yesterday_tm = *std::gmtime(&yesterday_time_t);
        std::ostringstream oss;
        oss << std::put_time(&yesterday_tm, "%Y-%m-%d");
        yesterday_date_str = oss.str();

        INFO("Checking holiday for yesterday's date: " + yesterday_date_str);

        is_yesterday_holiday = holiday_checker_.is_holiday(yesterday_date_str);

        INFO("Is yesterday (" + yesterday_date_str + ") a holiday? " +
             std::string(is_yesterday_holiday ? "YES" : "NO"));

        if (is_yesterday_holiday) {
            yesterday_holiday_name = holiday_checker_.get_holiday_name(yesterday_date_str);
            INFO("Holiday name: " + yesterday_holiday_name);
        }
    } catch (...) {
        ERROR("Exception while calculating yesterday's date");
        yesterday_date_str = "Previous Day";
    }

    // T-8D-2 R61: the settled day is the runner's own T-1 when it names one.
    const std::string settled_date =
        context.settled_date.empty() ? yesterday_date_str : context.settled_date;

    // T-8D-2 R59: what the report says about the day before comes from the runner's own
    // classification of it, never from the weekday. A futures runner that carried its book found
    // no session (context.prior_day_no_session): the settled table is then not shown, and the
    // calendar adds the holiday's name when that day is one. The equity runner's day before is
    // its previous session by construction. The runner's carried-day banner, put at the top of
    // the body by the runner, states the reason.
    const bool show_yesterday_pnl = !context.prior_day_no_session;
    const bool holiday_banner =
        context.prior_day_no_session && is_yesterday_holiday && !context.equities;

    // Generate HTML header and styles
    html << "<!DOCTYPE html>\n";
    html << "<html>\n<head>\n";
    html << "<meta charset=\"UTF-8\" />\n";
    html << "<style>\n";
    html << "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f9f9f9; }\n";
    html << ".container { max-width: 1200px; margin: 0 auto; background-color: white; padding: "
            "30px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n";
    html << "h1, h2, h3 { color: #333; font-family: Arial, sans-serif; }\n";
    html << "h1 { font-size: 24px; margin-bottom: 5px; }\n";
    html << "h2 { font-size: 20px; margin-top: 25px; margin-bottom: 10px; border-bottom: 2px solid "
            "#2c5aa0; padding-bottom: 5px; }\n";
    html << "h3 { font-size: 16px; margin-top: 20px; margin-bottom: 10px; }\n";
    html << "table { border-collapse: collapse; width: 100%; margin: 10px 0; font-size: 14px; "
            "font-family: Arial, sans-serif; }\n";
    html << "th, td { border: 1px solid #ddd; padding: 10px; text-align: left; }\n";
    html << "th { background-color: #f2f2f2; font-weight: bold; }\n";
    html << ".metric { margin: 8px 0; font-size: 14px; line-height: 1.6; font-family: Arial, "
            "sans-serif; }\n";
    html << ".positive { color: #1a7f37; font-weight: 500; }\n";
    html << ".negative { color: #b42318; font-weight: 500; }\n";
    html << ".neutral { color: #0b6efd; font-weight: 500; }\n";
    html << ".header-section { margin-bottom: 30px; display: flex; align-items: center; }\n";
    html << ".header-section img { width: 80px; height: 80px; margin-right: 20px; }\n";
    html << ".header-text { flex: 1; }\n";
    html << ".header-info { color: #666; font-size: 14px; margin-top: 10px; font-family: Arial, "
            "sans-serif; }\n";
    html << ".fund-branding { color: #2c5aa0; font-weight: bold; font-size: 16px; font-family: "
            "Arial, sans-serif; }\n";
    html << ".metrics-section { margin: 20px 0; }\n";
    html << ".metrics-category { background-color: #fff5e6; padding: 15px; border-radius: 5px; "
            "margin-bottom: 20px; }\n";
    html << ".footer-note { background-color: #fff9e6; border-left: 4px solid #ffc107; padding: "
            "15px; margin: 20px 0; font-size: 13px; color: #666; font-family: Arial, sans-serif; "
            "}\n";
    html << ".alert-note { background-color: #fee2e2; border-left: 4px solid #dc2626; padding: "
            "15px; margin: 20px 0; font-size: 13px; color: #991b1b; font-family: Arial, "
            "sans-serif; }\n";
    html << ".summary-stats { background-color: #fff5e6; padding: 15px; margin: 15px 0; "
            "border-radius: 5px; font-family: Arial, sans-serif; font-size: 14px; }\n";
    html << ".chart-container { margin: 20px 0; padding: 20px; background-color: #f8f9fa; "
            "border-radius: 8px; text-align: center; }\n";
    html << ".weekend-message { background-color: #e6f3ff; border-left: 4px solid #2c5aa0; "
            "padding: 20px; margin: 20px 0; font-size: 16px; }\n";
    html << "</style>\n";
    html << "</head>\n<body>\n";
    html << "<div class=\"container\">\n";

    // Header with logo and branding
    html << "<div class=\"header-section\">\n";
    html << "<img src=\"cid:algogators_logo\" alt=\"AlgoGators Logo\">\n";
    html << "<div class=\"header-text\">\n";
    html << "<span class=\"fund-branding\">AlgoGators</span><br>\n";
    html << "<h1>Daily Trading Report</h1>\n";
    html << "<div class=\"header-info\">" << date << " | "
         << format_strategy_display_name(portfolio_name) << "</div>\n";
    html << "</div>\n";
    html << "</div>\n";

    // The banners about the day before (HD 2026-10-14, section 15): the original texts; the settled
    // day's weekday and the calendar choose the title, the prices that arrived choose the form.
    DayBeforeBanner day_banner;
    if (!context.equities) {
        bool some_missing = false;
        for (const auto& [sleeve, sleeve_positions] : yesterday_strategy_positions) {
            (void)sleeve;
            for (const auto& [symbol, position] : sleeve_positions) {
                if (std::abs(position.quantity.as_double()) < 0.0001) continue;
                if (!yesterday_close_prices.count(symbol) ||
                    context.settled_zero_kinds.withheld.count(symbol))
                    some_missing = true;
            }
        }
        std::string settled_holiday;
        try {
            if (holiday_checker_.is_holiday(settled_date))
                settled_holiday = holiday_checker_.get_holiday_name(settled_date);
        } catch (...) {
        }
        day_banner = day_before_banner(settled_date, settled_holiday, context.prior_day_no_session,
                                       some_missing);
    }
    html << day_banner.banner_html;

    // Today's Positions - use per-strategy tables if strategy_positions is provided, otherwise
    // fallback to single table
    html << "<h2>Today's Positions</h2>\n";

    if (!strategy_positions.empty()) {
        html << format_strategy_positions_tables(strategy_positions, current_prices,
                                                 strategy_metrics);
    } else {
        html << format_positions_table(positions, is_daily_strategy, current_prices,
                                       strategy_metrics);
    }
    html << notes_html();

    // Executions (if any) - use per-strategy tables
    bool has_executions = false;
    for (const auto& [_, execs] : strategy_executions) {
        if (!execs.empty()) {
            has_executions = true;
            break;
        }
    }
    if (has_executions) {
        html << "<h2>Daily Executions</h2>\n";
        html << format_strategy_executions_tables(strategy_executions);
        html << notes_html();
    }

    // Yesterday's Finalized Positions (with actual PnL) - use per-strategy breakdown
    bool has_yesterday_positions = false;
    for (const auto& [_, positions_map] : yesterday_strategy_positions) {
        if (!positions_map.empty()) {
            has_yesterday_positions = true;
            break;
        }
    }
    if (show_yesterday_pnl && has_yesterday_positions && !yesterday_close_prices.empty() &&
        !two_days_ago_close_prices.empty()) {
        html << format_yesterday_finalized_positions_table(
            yesterday_strategy_positions, two_days_ago_close_prices, yesterday_close_prices, db,
            yesterday_daily_metrics, yesterday_date_str, settled_date, context.equities,
            context.settled_zero_kinds);
        html << notes_html();
    } else if (!show_yesterday_pnl) {
        // No prices at all: the red box and the yellow note, the table hidden (section 15).
        html << "<div class=\"alert-note\">\n" << day_banner.red_box_text << "</div>\n";
        html << "<div class=\"footer-note\">\n" << day_banner.yellow_note << "</div>\n";
    }

    // Strategy metrics
    if (!strategy_metrics.empty()) {
        html << "<div class=\"metrics-section\">\n";
        html << format_strategy_metrics(strategy_metrics, &over_limit, date, settled_date, context);
        html << "</div>\n";
    }

    html << "<h2>Charts</h2>\n";
    core::ChartLedger chart_ledger;
    if (db) {
        // portfolio_name carries the portfolio_id from the live runner (e.g.
        // "CONSERVATIVE_PORTFOLIO"). Pass it to chart queries so they correctly filter on
        // (strategy_id, portfolio_id) — without this they pulled cross-portfolio rows and
        // the email charts double-rendered every date. The strategy id is the run's own
        // (chart_strategy_id): a book of two sleeves stores its rows under its combined id.
        const std::string& chart_portfolio_id = portfolio_name;

        // Generate equity curve chart
        chart_base64_ = ChartGenerator::generate_equity_curve_chart(db, chart_strategy_id,
                                                                    chart_portfolio_id, date, 30);
        html << chart_ledger.record(std::string("Equity Curve"), !chart_base64_.empty(),
                                          ChartGenerator::last_chart_status(),
                                          ChartGenerator::last_chart_reason(), "no data to plot");
        if (!chart_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Equity Curve</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:equity_chart\" alt=\"Portfolio Equity Curve\" "
                    "style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px "
                    "8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        drawdown_base64_ = ChartGenerator::generate_drawdown_chart(db, chart_strategy_id, chart_portfolio_id, date, 30);
        html << chart_ledger.record(std::string("Drawdown"), !drawdown_base64_.empty(),
                                    ChartGenerator::last_chart_status(),
                                    ChartGenerator::last_chart_reason(), "no data to plot");
        if (!drawdown_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Drawdown</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:drawdown\" alt=\"Drawdown\" style=\"max-width: 100%; height: auto; "
                    "border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        // Generate PnL by symbol chart - ONLY if show_yesterday_pnl is true
        if (show_yesterday_pnl) {
            pnl_by_symbol_base64_ = ChartGenerator::generate_pnl_by_symbol_chart(
                db, chart_strategy_id, chart_portfolio_id, date, context.equities);
            html << chart_ledger.record(std::string(ChartTitles::pnl_by_symbol_heading(context.equities)), !pnl_by_symbol_base64_.empty(),
                                              ChartGenerator::last_chart_status(),
                                              ChartGenerator::last_chart_reason(), "no data to plot");
            if (!pnl_by_symbol_base64_.empty()) {
                html << "<h3 style=\"margin-top: 20px; color: #333;\">"
                     << ChartTitles::pnl_by_symbol_heading(context.equities) << "</h3>\n";
                html << "<div style=\"width: 100%; max-width: 800px; margin: 20px auto; "
                        "text-align: center;\">\n";
                html << "<img src=\"cid:pnl_by_symbol\" alt=\"PnL by Symbol\" style=\"max-width: "
                        "100%; height: auto; border-radius: 8px; box-shadow: 0 2px 8px "
                        "rgba(0,0,0,0.1);\" />\n";
                html << "</div>\n";
            }
        }

        // Generate daily PnL chart
        daily_pnl_base64_ = ChartGenerator::generate_daily_pnl_chart(
            db, chart_strategy_id, chart_portfolio_id, date, 30);
        html << chart_ledger.record(std::string(ChartTitles::daily_pnl_heading(30)), !daily_pnl_base64_.empty(),
                                          ChartGenerator::last_chart_status(),
                                          ChartGenerator::last_chart_reason(), "no data to plot");
        if (!daily_pnl_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">"
                 << ChartTitles::daily_pnl_heading(30) << "</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:daily_pnl\" alt=\"Daily PnL\" style=\"max-width: 100%; height: "
                    "auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        gross_leverage_base64_ = ChartGenerator::generate_gross_leverage_chart(db, chart_strategy_id, chart_portfolio_id, date, 30);
        html << chart_ledger.record(std::string("Leverage"), !gross_leverage_base64_.empty(),
                                    ChartGenerator::last_chart_status(),
                                    ChartGenerator::last_chart_reason(), "no data to plot");
        if (!gross_leverage_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Leverage</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:gross_leverage\" alt=\"Leverage\" style=\"max-width: 100%; height: auto; "
                    "border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        total_transaction_costs_base64_ = ChartGenerator::generate_total_transaction_costs_chart(
            db, chart_strategy_id, chart_portfolio_id, date);
        html << chart_ledger.record(std::string("Cost per $1M Traded"), !total_transaction_costs_base64_.empty(),
                                          ChartGenerator::last_chart_status(),
                                          ChartGenerator::last_chart_reason(), "no data to plot");
        if (!total_transaction_costs_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Cost per $1M Traded</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:total_transaction_costs\" alt=\"Cost per $1M Traded\" "
                    "style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px "
                    "8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        margin_posted_base64_ = ChartGenerator::generate_margin_posted_chart(
            db, chart_strategy_id, chart_portfolio_id, date);
        html << chart_ledger.record(std::string("Margin Posted"), !margin_posted_base64_.empty(),
                                          ChartGenerator::last_chart_status(),
                                          ChartGenerator::last_chart_reason(), "no data to plot");
        if (!margin_posted_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Margin Posted</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:margin_posted\" alt=\"Margin Posted\" style=\"max-width: 100%; "
                    "height: auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" "
                    "/>\n";
            html << "</div>\n";
        }

        portfolio_composition_base64_ =
            ChartGenerator::generate_portfolio_composition_chart(positions, current_prices, date);
        html << chart_ledger.record(std::string("Portfolio Composition"), !portfolio_composition_base64_.empty(),
                                          ChartGenerator::last_chart_status(),
                                          ChartGenerator::last_chart_reason(), "no positions");
        if (!portfolio_composition_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Portfolio Composition</h3>\n";
            html << "<div style=\"width: 100%; max-width: 800px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:portfolio_composition\" alt=\"Portfolio Composition\" "
                    "style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px "
                    "8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        cumulative_pnl_by_symbol_base64_ = ChartGenerator::generate_cumulative_pnl_by_symbol_chart(
            db, chart_strategy_id, chart_portfolio_id, date);
        html << chart_ledger.record(std::string(ChartTitles::cumulative_pnl_heading()), !cumulative_pnl_by_symbol_base64_.empty(),
                                          ChartGenerator::last_chart_status(),
                                          ChartGenerator::last_chart_reason(), "no data to plot");
        if (!cumulative_pnl_by_symbol_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">"
                 << ChartTitles::cumulative_pnl_heading() << "</h3>\n";
            html << "<div style=\"width: 100%; max-width: 800px; margin: 20px auto; text-align: "
                    "center;\">\n";
            html << "<img src=\"cid:cumulative_pnl_by_symbol\" alt=\"Cumulative PnL by Symbol\" "
                    "style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px "
                    "8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }
        INFO(chart_ledger.summary());
    }

    // Symbols Reference
    html << "<h2>Symbols Reference</h2>\n";
    if (db) {
        try {
            // Every contract any sleeve holds, not only those the book holds net (N4).
            const auto held_by_sleeves = held_by_any_sleeve(positions, strategy_positions);
            html << format_symbols_table_for_positions(held_by_sleeves, db, yesterday_date_str);
        } catch (const std::exception& e) {
            html << "<p>Error loading symbols data: " << e.what() << "</p>\n";
        }
    } else {
        html << "<p>Database unavailable; symbols reference not included.</p>\n";
    }

    // Footer note
    if (is_daily_strategy) {
        html << "<div class=\"footer-note\">\n";
        html << report_footer_note_html(context);
        html << "</div>\n";
    }

    html << "<hr style=\"margin-top: 30px; border: none; border-top: 1px solid #ddd;\">\n";
    html << "<p style=\"text-align: center; color: #999; font-size: 12px; margin-top: 20px; "
            "font-family: Arial, sans-serif;\">Generated by AlgoGator's Trade-ngin</p>\n";
    html << "</div>\n";
    html << "</body>\n</html>\n";

    return html.str();
}

}  // namespace trade_ngin
