#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/logger.hpp"
#include <curl/curl.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>
#include <ctime>
#include <set>
#include <cctype>
#include <nlohmann/json.hpp> 
#include "trade_ngin/instruments/instrument_registry.hpp"

namespace trade_ngin {

// Global variables for curl email payload
std::string g_email_payload;
size_t g_payload_pos = 0;

size_t read_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
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
    : credentials_(credentials), initialized_(false) {
}

Result<void> EmailSender::initialize() {
    auto load_result = load_config();
    if (load_result.is_error()) {
        return load_result;
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
            return make_error<void>(ErrorCode::INVALID_DATA,
                                   "Failed to get SMTP host: " + smtp_host_result.error()->to_string(),
                                   "EmailSender");
        }
        config_.smtp_host = smtp_host_result.value();

        // Load SMTP port
        auto smtp_port_result = credentials_->get<int>("email", "smtp_port");
        if (smtp_port_result.is_error()) {
            return make_error<void>(ErrorCode::INVALID_DATA,
                                   "Failed to get SMTP port: " + smtp_port_result.error()->to_string(),
                                   "EmailSender");
        }
        config_.smtp_port = smtp_port_result.value();

        // Load username
        auto username_result = credentials_->get<std::string>("email", "username");
        if (username_result.is_error()) {
            return make_error<void>(ErrorCode::INVALID_DATA,
                                   "Failed to get email username: " + username_result.error()->to_string(),
                                   "EmailSender");
        }
        config_.username = username_result.value();

        // Load password
        auto password_result = credentials_->get<std::string>("email", "password");
        if (password_result.is_error()) {
            return make_error<void>(ErrorCode::INVALID_DATA,
                                   "Failed to get email password: " + password_result.error()->to_string(),
                                   "EmailSender");
        }
        config_.password = password_result.value();

        // Load from email
        auto from_email_result = credentials_->get<std::string>("email", "from_email");
        if (from_email_result.is_error()) {
            return make_error<void>(ErrorCode::INVALID_DATA,
                                   "Failed to get from email: " + from_email_result.error()->to_string(),
                                   "EmailSender");
        }
        config_.from_email = from_email_result.value();

        // Load to emails
        auto to_emails_result = credentials_->get<std::vector<std::string>>("email", "to_emails");
        if (to_emails_result.is_error()) {
            return make_error<void>(ErrorCode::INVALID_DATA,
                                   "Failed to get to emails: " + to_emails_result.error()->to_string(),
                                   "EmailSender");
        }
        config_.to_emails = to_emails_result.value();

        // Load TLS setting
        auto use_tls_result = credentials_->get<bool>("email", "use_tls");
        if (use_tls_result.is_error()) {
            return make_error<void>(ErrorCode::INVALID_DATA,
                                   "Failed to get TLS setting: " + use_tls_result.error()->to_string(),
                                   "EmailSender");
        }
        config_.use_tls = use_tls_result.value();

        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                               "Failed to load email configuration: " + std::string(e.what()),
                               "EmailSender");
    }
}

Result<void> EmailSender::send_email(const std::string& subject, const std::string& body, bool is_html, const std::optional<std::string>& attachment_path) {
    if (!initialized_) {
        return make_error<void>(ErrorCode::NOT_INITIALIZED,
                               "Email sender not initialized",
                               "EmailSender");
    }

    CURL* curl;
    CURLcode res;

    curl = curl_easy_init();
    if (!curl) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                               "Failed to initialize curl",
                               "EmailSender");
    }

    try {
        // Set basic options
        std::string smtp_url = "smtp://" + config_.smtp_host + ":" + std::to_string(config_.smtp_port);
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
        std::string logo_path = "Algo.png";
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
                    char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                    char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                    char_array_4[3] = char_array_3[2] & 0x3f;

                    for(i = 0; i < 4; i++)
                        logo_base64 += base64_chars[char_array_4[i]];
                    i = 0;
                }
            }

            if (i) {
                for(j = i; j < 3; j++)
                    char_array_3[j] = '\0';

                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

                for (j = 0; j < i + 1; j++)
                    logo_base64 += base64_chars[char_array_4[j]];

                while(i++ < 3)
                    logo_base64 += '=';
            }
        }

        // Read and encode CSV attachment if provided
        std::string csv_base64;
        std::string csv_filename;
        if (attachment_path.has_value()) {
            std::ifstream csv_file(attachment_path.value(), std::ios::binary);
            if (csv_file.is_open()) {
                std::vector<unsigned char> csv_data((std::istreambuf_iterator<char>(csv_file)),
                                                     std::istreambuf_iterator<char>());
                csv_file.close();

                // Base64 encode CSV data using same logic as logo
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
                        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                        char_array_4[3] = char_array_3[2] & 0x3f;

                        for(i = 0; i < 4; i++)
                            csv_base64 += base64_chars[char_array_4[i]];
                        i = 0;
                    }
                }

                if (i) {
                    for(j = i; j < 3; j++)
                        char_array_3[j] = '\0';

                    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                    char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                    char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

                    for (j = 0; j < i + 1; j++)
                        csv_base64 += base64_chars[char_array_4[j]];

                    while(i++ < 3)
                        csv_base64 += '=';
                }

                // Extract filename from path
                size_t last_slash = attachment_path.value().find_last_of("/\\");
                csv_filename = (last_slash != std::string::npos)
                    ? attachment_path.value().substr(last_slash + 1)
                    : attachment_path.value();
            } else {
                WARN("Failed to open CSV attachment file: " + attachment_path.value());
            }
        }

        // Create multipart email payload
        // Use multipart/mixed for attachments, with nested multipart/related for HTML+logo
        std::string outer_boundary = "----=_Outer_" + std::to_string(std::time(nullptr));
        std::string inner_boundary = "----=_Inner_" + std::to_string(std::time(nullptr));
        std::string content_type = is_html ? "text/html; charset=UTF-8" : "text/plain";

        // Determine top-level content type based on whether we have attachments
        std::string top_content_type = csv_base64.empty() ? "multipart/related" : "multipart/mixed";
        std::string top_boundary = csv_base64.empty() ? inner_boundary : outer_boundary;

        g_email_payload =
            "From: " + config_.from_email + "\r\n"
            "To: " + config_.to_emails[0] + "\r\n"
            "Subject: " + subject + "\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: " + top_content_type + "; boundary=\"" + top_boundary + "\"\r\n"
            "\r\n";

        // If we have attachments, create nested structure
        if (!csv_base64.empty()) {
            // Start outer multipart/mixed
            g_email_payload += "--" + outer_boundary + "\r\n";
            g_email_payload += "Content-Type: multipart/related; boundary=\"" + inner_boundary + "\"\r\n";
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

        // Close inner multipart/related
        g_email_payload += "\r\n--" + inner_boundary + "--\r\n";

        // Add CSV attachment if present
        if (!csv_base64.empty()) {
            g_email_payload += "\r\n--" + outer_boundary + "\r\n";
            g_email_payload += "Content-Type: text/csv; name=\"" + csv_filename + "\"\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-Disposition: attachment; filename=\"" + csv_filename + "\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += csv_base64 + "\r\n";

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

        INFO("Email sent successfully to " + std::to_string(config_.to_emails.size()) + " recipients");
        return Result<void>();

    } catch (const std::exception& e) {
        curl_easy_cleanup(curl);
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                               "Exception during email sending: " + std::string(e.what()),
                               "EmailSender");
    }
}

std::string EmailSender::generate_trading_report_body(
    const std::unordered_map<std::string, Position>& positions,
    const std::optional<RiskResult>& risk_metrics,
    const std::map<std::string, double>& strategy_metrics,
    const std::vector<ExecutionReport>& executions,
    const std::string& date,
    bool is_daily_strategy,
    const std::unordered_map<std::string, double>& current_prices,
    std::shared_ptr<DatabaseInterface> db)
{
    std::ostringstream html;

    html << "<!DOCTYPE html>\n";
    html << "<html>\n<head>\n";
    html << "<meta charset=\"UTF-8\" />\n";
    html << "<style>\n";
    html << "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f9f9f9; }\n";
    html << ".container { max-width: 1200px; margin: 0 auto; background-color: white; padding: 30px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n";
    html << "h1, h2 { color: #333; font-family: Arial, sans-serif; }\n";
    html << "h1 { font-size: 24px; margin-bottom: 5px; }\n";
    html << "h2 { font-size: 18px; margin-top: 25px; margin-bottom: 10px; border-bottom: 2px solid #2c5aa0; padding-bottom: 5px; }\n";
    html << "table { border-collapse: collapse; width: 100%; margin: 10px 0; font-size: 14px; font-family: Arial, sans-serif; }\n";
    html << "th, td { border: 1px solid #ddd; padding: 10px; text-align: left; }\n";
    html << "th { background-color: #f2f2f2; font-weight: bold; }\n";
    html << ".metric { margin: 8px 0; font-size: 14px; line-height: 1.6; font-family: Arial, sans-serif; }\n";
    html << ".positive { color: #1a7f37; font-weight: 500; }\n";
    html << ".negative { color: #b42318; font-weight: 500; }\n";
    html << ".neutral { color: #0b6efd; font-weight: 500; }\n";
    html << ".header-section { margin-bottom: 30px; display: flex; align-items: center; }\n";
    html << ".header-section img { width: 80px; height: 80px; margin-right: 20px; }\n";
    html << ".header-text { flex: 1; }\n";
    html << ".header-info { color: #666; font-size: 14px; margin-top: 10px; font-family: Arial, sans-serif; }\n";
    html << ".fund-branding { color: #2c5aa0; font-weight: bold; font-size: 16px; font-family: Arial, sans-serif; }\n";
    html << ".metrics-section { margin: 20px 0; }\n";
    html << ".metrics-category { background-color: #fff5e6; padding: 15px; border-radius: 5px; margin-bottom: 20px; }\n";
    html << ".footer-note { background-color: #fff9e6; border-left: 4px solid #ffc107; padding: 15px; margin: 20px 0; font-size: 13px; color: #666; font-family: Arial, sans-serif; }\n";
    html << ".summary-stats { background-color: #fff5e6; padding: 15px; margin: 15px 0; border-radius: 5px; font-family: Arial, sans-serif; font-size: 14px; }\n";
    html << "</style>\n";
    html << "</head>\n<body>\n";
    html << "<div class=\"container\">\n";

    // Header with logo and branding
    html << "<div class=\"header-section\">\n";
    html << "<img src=\"cid:algogators_logo\" alt=\"AlgoGators Logo\">\n";
    html << "<div class=\"header-text\">\n";
    html << "<span class=\"fund-branding\">AlgoGators</span><br>\n";
    html << "<h1>Daily Trading Report</h1>\n";
    html << "<div class=\"header-info\">" << date << " | Trend Following Strategy</div>\n";
    html << "</div>\n";
    html << "</div>\n";

    // Positions
    html << "<h2>Positions</h2>\n";
    html << format_positions_table(positions, is_daily_strategy, current_prices);

    // Executions (if any)
    if (!executions.empty()) {
        html << "<h2>Daily Executions</h2>\n";
        html << format_executions_table(executions);
    }

    /*    // Risk snapshot (if provided)
    if (risk_metrics.has_value()) {
        html << "<h2>Risk Snapshot</h2>\n";
        html << format_risk_metrics(*risk_metrics);
    }
        */

    // Symbols Reference (make sure this exists in the body again)
    html << "<h2>Symbols Reference</h2>\n";
    if (db) {
        try {
            html << format_symbols_table_for_positions(positions, db);
        } catch (const std::exception& e) {
            html << "<p>Error loading symbols data: " << e.what() << "</p>\n";
        }
    } else {
        html << "<p>Database unavailable; symbols reference not included.</p>\n";
    }

    // Strategy metrics
    if (!strategy_metrics.empty()) {
        html << "<div class=\"metrics-section\">\n";
        html << format_strategy_metrics(strategy_metrics);
        html << "</div>\n";
    }

    // Footer note
    if (is_daily_strategy) {
        html << "<div class=\"footer-note\">\n";
        html << "<strong>Note:</strong> This strategy is based on daily OHLCV data.\n";
        html << "</div>\n";
    }

    html << "<hr style=\"margin-top: 30px; border: none; border-top: 1px solid #ddd;\">\n";
    html << "<p style=\"text-align: center; color: #999; font-size: 12px; margin-top: 20px; font-family: Arial, sans-serif;\">Generated by AlgoGator's Trade-ngin</p>\n";
    html << "</div>\n";
    html << "</body>\n</html>\n";

    return html.str();
}

std::string EmailSender::format_executions_table(const std::vector<ExecutionReport>& executions) {
    std::ostringstream html;

    if (executions.empty()) {
        html << "<p>No executions for today.</p>\n";
        return html.str();
    }

    html << "<table>\n";
    html << "<tr><th>Symbol</th><th>Side</th><th>Quantity</th><th>Price</th><th>Notional</th><th>Commission</th></tr>\n";

    double total_commission = 0.0;
    double total_notional_traded = 0.0;

    for (const auto& exec : executions) {
        // Get contract multiplier for proper notional calculation
        double contract_multiplier = 1.0;

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
                contract_multiplier = instrument->get_multiplier();
            } else {
                // Use fallback multipliers for common contracts
                if (lookup_sym == "NQ" || lookup_sym == "MNQ") contract_multiplier = 20.0;
                else if (lookup_sym == "ES" || lookup_sym == "MES") contract_multiplier = 50.0;
                else if (lookup_sym == "YM" || lookup_sym == "MYM") contract_multiplier = 0.5;
                else if (lookup_sym == "6B") contract_multiplier = 62500.0;
                else if (lookup_sym == "6E") contract_multiplier = 125000.0;
                else if (lookup_sym == "6C") contract_multiplier = 100000.0;
                else if (lookup_sym == "6J") contract_multiplier = 12500000.0;
                else if (lookup_sym == "6S") contract_multiplier = 125000.0;
                else if (lookup_sym == "6N") contract_multiplier = 100000.0;
                else if (lookup_sym == "6M") contract_multiplier = 500000.0;
                else if (lookup_sym == "CL") contract_multiplier = 1000.0;
                else if (lookup_sym == "GC") contract_multiplier = 100.0;
                else if (lookup_sym == "HG") contract_multiplier = 25000.0;
                else if (lookup_sym == "PL") contract_multiplier = 50.0;
                else if (lookup_sym == "ZR") contract_multiplier = 2000.0;
                else if (lookup_sym == "RB") contract_multiplier = 42000.0;
                else if (lookup_sym == "RTY") contract_multiplier = 50.0;
                else if (lookup_sym == "SI") contract_multiplier = 5000.0;
                else if (lookup_sym == "UB") contract_multiplier = 100000.0;
                else if (lookup_sym == "ZC") contract_multiplier = 5000.0;
                else if (lookup_sym == "ZL") contract_multiplier = 60000.0;
                else if (lookup_sym == "ZM") contract_multiplier = 100.0;
                else if (lookup_sym == "ZN") contract_multiplier = 100000.0;
                else if (lookup_sym == "ZS") contract_multiplier = 5000.0;
                else if (lookup_sym == "ZW") contract_multiplier = 5000.0;
                else if (lookup_sym == "HE") contract_multiplier = 40000.0;
                else if (lookup_sym == "LE") contract_multiplier = 40000.0;
                else if (lookup_sym == "GF") contract_multiplier = 50000.0;
                else if (lookup_sym == "KE") contract_multiplier = 5000.0;
            }
        } catch (...) {
            // Use default multiplier if exception occurs
        }

        double notional = exec.filled_quantity.as_double() * exec.fill_price.as_double() * contract_multiplier;
        total_notional_traded += notional;
        total_commission += exec.commission.as_double();

        std::string side_str = exec.side == Side::BUY ? "BUY" : "SELL";
        std::string side_class = exec.side == Side::BUY ? "positive" : "negative";

        html << "<tr>\n";
        html << "<td>" << exec.symbol << "</td>\n";
        html << "<td class=\"" << side_class << "\">" << side_str << "</td>\n";
        html << "<td>" << std::fixed << std::setprecision(0) << exec.filled_quantity.as_double() << "</td>\n";
        html << "<td>$" << std::fixed << std::setprecision(2) << exec.fill_price.as_double() << "</td>\n";
        html << "<td>$" << std::fixed << std::setprecision(2) << notional << "</td>\n";
        html << "<td>$" << std::fixed << std::setprecision(2) << exec.commission.as_double() << "</td>\n";
        html << "</tr>\n";
    }

    html << "</table>\n";

    // Helper to format numbers with commas
    auto format_with_commas = [](double value) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << value;
        std::string s = oss.str();
        size_t dp = s.find('.');
        if (dp == std::string::npos) dp = s.size();
        int pos = static_cast<int>(dp) - 3;
        while (pos > 0) {
            s.insert(static_cast<size_t>(pos), ",");
            pos -= 3;
        }
        return s;
    };

    html << "<div class=\"summary-stats\">\n";
    html << "<strong>Trades:</strong> " << executions.size() << "<br>\n";
    html << "<strong>Notional Traded:</strong> $" << format_with_commas(total_notional_traded) << "<br>\n";
    html << "<strong>Commissions:</strong> $" << format_with_commas(total_commission) << "\n";
    html << "</div>\n";

    return html.str();
}

std::string EmailSender::format_positions_table(const std::unordered_map<std::string, Position>& positions,
                                                bool is_daily_strategy,
                                                const std::unordered_map<std::string, double>& current_prices) {
    std::ostringstream html;

    html << "<table>\n";
    html << "<tr><th>Symbol</th><th>Quantity</th>";

    // Show only market price for positions
    html << "<th>Market Price</th>";

    html << "<th>Notional</th><th>Total P&L</th></tr>\n";

    double total_notional = 0.0;
    double total_margin_posted = 0.0;
    int active_positions = 0;

    for (const auto& [symbol, position] : positions) {
        if (position.quantity.as_double() != 0.0) {
            active_positions++;

            // Get contract multiplier for proper notional calculation
            double contract_multiplier = 1.0;
            double notional = 0.0;

            // Compute posted margin accurately using instrument registry margins when available
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
                if (instrument) {
                    // Get contract multiplier for notional calculation
                    contract_multiplier = instrument->get_multiplier();

                    double contracts_abs = std::abs(position.quantity.as_double());
                    double initial_margin_per_contract = instrument->get_margin_requirement();
                    total_margin_posted += contracts_abs * initial_margin_per_contract;
                } else {
                    // Use fallback multipliers for common contracts
                    if (lookup_sym == "NQ" || lookup_sym == "MNQ") contract_multiplier = 20.0;
                    else if (lookup_sym == "ES" || lookup_sym == "MES") contract_multiplier = 50.0;
                    else if (lookup_sym == "YM" || lookup_sym == "MYM") contract_multiplier = 5.0;
                    else if (lookup_sym == "6B") contract_multiplier = 62500.0;
                    else if (lookup_sym == "6E") contract_multiplier = 125000.0;
                    else if (lookup_sym == "6C") contract_multiplier = 100000.0;
                    else if (lookup_sym == "6J") contract_multiplier = 12500000.0;
                    else if (lookup_sym == "6S") contract_multiplier = 125000.0;
                    else if (lookup_sym == "6N") contract_multiplier = 100000.0;
                    else if (lookup_sym == "6M") contract_multiplier = 500000.0;
                    else if (lookup_sym == "CL") contract_multiplier = 1000.0;
                    else if (lookup_sym == "GC") contract_multiplier = 100.0;
                    else if (lookup_sym == "HG") contract_multiplier = 25000.0;
                    else if (lookup_sym == "PL") contract_multiplier = 50.0;
                    else if (lookup_sym == "ZR") contract_multiplier = 2000.0;
                }
            } catch (...) {
                // Use fallback multipliers if exception occurs
            }

            // Calculate notional with proper contract multiplier
            notional = position.quantity.as_double() * position.average_price.as_double() * contract_multiplier;
            total_notional += std::abs(notional);

            // If margin not calculated above, estimate it
            if (total_margin_posted == 0.0) {
                // Fall back: keep previous estimate behavior if registry not available
                double margin_estimate = std::abs(notional) * 0.05;  // More realistic 5% for futures
                total_margin_posted += margin_estimate;
            }

            // Calculate total P&L (realized + unrealized)
            double total_pnl = position.realized_pnl.as_double() + position.unrealized_pnl.as_double();
            std::string pnl_class;
            if (std::fabs(total_pnl) < 1e-9) {
                pnl_class = "neutral"; // zero -> blue
            } else if (total_pnl > 0) {
                pnl_class = "positive";
            } else {
                pnl_class = "negative";
            }

            // Use current market price if available, otherwise fall back to average price
            double market_price = position.average_price.as_double();
            auto price_it = current_prices.find(symbol);
            if (price_it != current_prices.end()) {
                market_price = price_it->second;
            }

            html << "<tr>\n";
            html << "<td>" << symbol << "</td>\n";
            html << "<td>" << std::fixed << std::setprecision(0) << position.quantity.as_double() << "</td>\n";

            // Show only market price
            html << "<td>$" << std::fixed << std::setprecision(2) << market_price << "</td>\n";

            html << "<td>$" << std::fixed << std::setprecision(2) << std::abs(notional) << "</td>\n";
            html << "<td class=\"" << pnl_class << "\">$" << std::fixed << std::setprecision(2)
                 << total_pnl << "</td>\n";
            html << "</tr>\n";
        }
    }

    html << "</table>\n";
    html << "<div class=\"summary-stats\">\n";

    // Helper function to format numbers with commas
    auto format_with_commas = [](double value) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << value;
        std::string str = oss.str();

        // Find decimal point
        size_t decimal_pos = str.find('.');
        if (decimal_pos == std::string::npos) {
            decimal_pos = str.length();
        }

        // Insert commas
        int insert_pos = decimal_pos - 3;
        while (insert_pos > 0) {
            str.insert(insert_pos, ",");
            insert_pos -= 3;
        }

        return str;
    };

    html << "<strong>Active Positions:</strong> " << active_positions << "<br>\n";
    html << "<strong>Total Notional:</strong> $" << format_with_commas(total_notional) << "<br>\n";
    html << "<strong>Total Margin Posted:</strong> $" << format_with_commas(total_margin_posted) << "\n";
    html << "</div>\n";

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

std::string EmailSender::format_strategy_metrics(const std::map<std::string, double>& strategy_metrics) {
    std::ostringstream html;

    // Helper function to format numbers with commas
    auto format_with_commas = [](double value, int precision = 2) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(precision) << value;
        std::string str = oss.str();

        // Find decimal point
        size_t decimal_pos = str.find('.');
        if (decimal_pos == std::string::npos) {
            decimal_pos = str.length();
        }

        // Insert commas
        int insert_pos = decimal_pos - 3;
        while (insert_pos > 0) {
            str.insert(insert_pos, ",");
            insert_pos -= 3;
        }

        return str;
    };

    // Helper function to format value with proper units and color
    auto format_metric = [&format_with_commas](const std::string& key, double value) -> std::string {
        std::ostringstream result;
        std::string formatted_value;
        std::string value_class;

        // Determine formatting based on metric type
        if (key.find("P&L") != std::string::npos || key.find("PnL") != std::string::npos ||
            key.find("Portfolio Value") != std::string::npos || key.find("Notional") != std::string::npos ||
            key.find("Cash Available") != std::string::npos || key.find("Margin Posted") != std::string::npos ||
            key.find("Commissions") != std::string::npos) {
            formatted_value = "$" + format_with_commas(value);
        } else if (key.find("Return") != std::string::npos || key.find("Volatility") != std::string::npos ||
                   key.find("Cushion") != std::string::npos) {
            formatted_value = format_with_commas(value) + "%";
        } else if (key.find("Leverage") != std::string::npos) {
            formatted_value = format_with_commas(value) + "x";
        } else if (key.find("Positions") != std::string::npos) {
            formatted_value = std::to_string(static_cast<int>(std::round(value)));
        } else {
            formatted_value = format_with_commas(value);
        }

        // Apply color coding for P&L and Return values
        bool colorize = (key.find("P&L") != std::string::npos) || (key.find("PnL") != std::string::npos) ||
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

        result << "<div class=\"metric\"><strong>" << key << ":</strong> <span" << value_class << ">"
               << formatted_value << "</span></div>\n";
        return result.str();
    };

    // Extract specific metrics for proper ordering
    std::map<std::string, double> ordered_metrics;

    // Store metrics in specific order
    for (const auto& [key, value] : strategy_metrics) {
        if (key.find("Leverage") != std::string::npos) {
            // Leverage metrics - handle separately
            continue;
        } else if (key.find("Margin Posted") != std::string::npos ||
                   key.find("Cash Available") != std::string::npos ||
                   key.find("Margin Cushion") != std::string::npos) {
            // Risk & Liquidity metrics - handle separately
            continue;
        }
        // Store everything else for performance metrics
        ordered_metrics[key] = value;
    }

    // Extract leverage metrics (avoiding duplicates)
    std::map<std::string, double> leverage_metrics;
    for (const auto& [key, value] : strategy_metrics) {
        if (key.find("Leverage") != std::string::npos) {
            // Only keep the metrics we want to display
            if (key == "Gross Leverage" || key == "Net Leverage" ||
                key == "Portfolio Leverage" || key == "Portfolio Leverage (Gross)" ||
                key == "Margin Leverage" || key == "Implied Leverage from Margin") {
                leverage_metrics[key] = value;
            }
        }
    }

    // Extract risk & liquidity metrics (margin and cash only)
    std::map<std::string, double> risk_liquidity_metrics;
    for (const auto& [key, value] : strategy_metrics) {
        if (key == "Margin Posted" || key == "Cash Available" || key == "Margin Cushion") {
            risk_liquidity_metrics[key] = value;
        }
    }

    // Render Performance Metrics Section with explicit ordering
    if (!ordered_metrics.empty()) {
        html << "<h2>Performance Metrics</h2>\n";
        html << "<div class=\"metrics-category\">\n";

        // Define the exact order we want
        std::vector<std::string> performance_order = {
            "Daily Return",
            "Daily Unrealized PnL",
            "Daily Realized PnL",
            "Daily Total PnL",
            "Total Annualized Return",
            "Total Return",
            "Total Unrealized PnL",
            "Total Realized PnL",
            "Total PnL",
            "Volatility",
            "Total Commissions",
            "Current Portfolio Value"
        };

        for (const auto& metric_name : performance_order) {
            auto it = ordered_metrics.find(metric_name);
            if (it != ordered_metrics.end()) {
                html << format_metric(it->first, it->second);

                // Add line space after Daily Total PnL
                if (metric_name == "Daily Total PnL") {
                    html << "<br>\n";
                }

                // Add line space after Total PnL
                if (metric_name == "Total PnL") {
                    html << "<br>\n";
                }
            }
        }

        html << "</div>\n";
    }

    // Render combined Risk & Liquidity Metrics Section (includes leverage)
    if (!leverage_metrics.empty() || !risk_liquidity_metrics.empty()) {
        html << "<h2>Risk & Liquidity Metrics</h2>\n";
        html << "<div class=\"metrics-category\">\n";

        // First render Gross and Net Leverage
        auto gross_it = leverage_metrics.find("Gross Leverage");
        if (gross_it != leverage_metrics.end()) {
            html << format_metric("Gross Leverage", gross_it->second);
        }

        auto net_it = leverage_metrics.find("Net Leverage");
        if (net_it != leverage_metrics.end()) {
            html << format_metric("Net Leverage", net_it->second);
        }

        // Add line space
        if ((gross_it != leverage_metrics.end() || net_it != leverage_metrics.end()) &&
            (leverage_metrics.find("Portfolio Leverage") != leverage_metrics.end() ||
             leverage_metrics.find("Portfolio Leverage (Gross)") != leverage_metrics.end())) {
            html << "<br>\n";
        }

        // Portfolio Leverage
        auto portfolio_it = leverage_metrics.find("Portfolio Leverage");
        if (portfolio_it != leverage_metrics.end()) {
            html << format_metric("Portfolio Leverage", portfolio_it->second);
        } else {
            portfolio_it = leverage_metrics.find("Portfolio Leverage (Gross)");
            if (portfolio_it != leverage_metrics.end()) {
                html << format_metric("Portfolio Leverage", portfolio_it->second);
            }
        }

        // Add line space before Implied Leverage
        if (portfolio_it != leverage_metrics.end() && leverage_metrics.find("Margin Leverage") != leverage_metrics.end()) {
            html << "<br>\n";
        }

        // Implied Leverage from Margin (using Margin Leverage value)
        auto margin_lev_it = leverage_metrics.find("Margin Leverage");
        if (margin_lev_it != leverage_metrics.end()) {
            html << format_metric("Implied Leverage from Margin", margin_lev_it->second);
        }

        // Add line space before margin and cash metrics
        if (margin_lev_it != leverage_metrics.end() && !risk_liquidity_metrics.empty()) {
            html << "<br>\n";
        }

        // Render margin and cash metrics in specific order
        auto margin_posted_it = risk_liquidity_metrics.find("Margin Posted");
        if (margin_posted_it != risk_liquidity_metrics.end()) {
            html << format_metric("Margin Posted", margin_posted_it->second);
        }

        auto margin_cushion_it = risk_liquidity_metrics.find("Margin Cushion");
        if (margin_cushion_it != risk_liquidity_metrics.end()) {
            html << format_metric("Margin Cushion", margin_cushion_it->second);
        }

        auto cash_available_it = risk_liquidity_metrics.find("Cash Available");
        if (cash_available_it != risk_liquidity_metrics.end()) {
            html << format_metric("Cash Available", cash_available_it->second);
        }

        html << "</div>\n";
    }

    return html.str();
}
std::string EmailSender::format_symbols_table_for_positions(
    const std::unordered_map<std::string, Position>& positions,
    std::shared_ptr<DatabaseInterface> db) {

    std::ostringstream html;

    // 1) Collect normalized base symbols from active positions
    std::set<std::string> base_syms;
    for (const auto& [sym, pos] : positions) {
        if (pos.quantity.as_double() == 0.0) continue;
        std::string b = sym;
        if (auto p = b.find(".v."); p != std::string::npos) b = b.substr(0, p);
        if (auto p = b.find(".c."); p != std::string::npos) b = b.substr(0, p);
        std::transform(b.begin(), b.end(), b.begin(),
                       [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
        b.erase(std::remove_if(b.begin(), b.end(),
             [](unsigned char c){ return !(std::isalnum(c) || c=='/'); }), b.end());
        if (!b.empty()) base_syms.insert(b);
    }

    if (base_syms.empty()) {
        html << "<p>No active positions to display symbol metadata for.</p>\n";
        return html.str();
    }

    auto make_in_list = [](const std::set<std::string>& S){
        std::ostringstream oss; bool first=true;
        for (const auto& s : S) { if (!first) oss << ", "; oss << "'" << s << "'"; first=false; }
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
            "WHERE \"Databento Symbol\" IN (" + in_list + ") "
            "   OR \"IB Symbol\"       IN (" + in_list + ") "
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
            if (schema->field(i)->name() == "symbol") { idx_symbol = i; break; }
        }
        if (idx_symbol < 0) {
            WARN("Symbols table: required metadata fields missing. Got fields: "
                 + schema->ToString());
            html << "<p>Error loading symbols data</p>\n";
            return html.str();
        }

        auto get_str = [](const std::shared_ptr<arrow::Array>& arr, int64_t i) -> std::string {
            if (!arr || arr->IsNull(i)) return "";
            if (arr->type_id() == arrow::Type::STRING)
                return std::static_pointer_cast<arrow::StringArray>(arr)->GetString(i);
            if (arr->type_id() == arrow::Type::LARGE_STRING)
                return std::static_pointer_cast<arrow::LargeStringArray>(arr)->GetString(i);
            return "";
        };

        auto col_symbol = combined->column(idx_symbol)->chunk(0);

        // Render
        html << "<table>\n";
        html << "<tr><th>Databento Symbol</th><th>IB Symbol</th>"
                "<th>Name</th><th>Contract Months</th></tr>\n";

        // Track matched symbols to detect gaps
        std::set<std::string> matched;

        for (int64_t i = 0; i < combined->num_rows(); ++i) {
            std::string json_txt = get_str(col_symbol, i);
            std::string db_sym, ib_sym, name, months;
            try {
                if (!json_txt.empty()) {
                    nlohmann::json j = nlohmann::json::parse(json_txt);
                    db_sym = j.value("db", "");
                    ib_sym = j.value("ib", "");
                    name   = j.value("name", "");
                    months = j.value("months", "");
                }
            } catch (...) {}

            auto up = [](std::string s){
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
                return s;
            };
            if (!db_sym.empty()) matched.insert(up(db_sym));
            if (!ib_sym.empty()) matched.insert(up(ib_sym));

            html << "<tr>\n";
            html << "<td>" << db_sym << "</td>\n";
            html << "<td>" << ib_sym << "</td>\n";
            html << "<td>" << name   << "</td>\n";
            html << "<td>" << months << "</td>\n";
            html << "</tr>\n";
        }

        // Show any active symbols we couldn’t match
        std::vector<std::string> missing;
        for (const auto& s : base_syms) if (!matched.count(s)) missing.push_back(s);
        if (!missing.empty()) {
            html << "<p style=\"color:#b42318\"><strong>Note:</strong> metadata not found for: ";
            for (size_t i = 0; i < missing.size(); ++i) { if (i) html << ", "; html << missing[i]; }
            html << ".</p>\n";
            WARN("Symbols reference: missing metadata for " + std::to_string(missing.size()) + " symbols");
        }

        html << "</table>\n";

    } catch (const std::exception& e) {
        WARN(std::string("Exception while formatting filtered symbols table: ") + e.what());
        html << "<p>Error loading symbols data</p>\n";
    }

    return html.str();
}

} // namespace trade_ngin
