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

Result<void> EmailSender::send_email(const std::string& subject, const std::string& body, bool is_html, const std::vector<std::string>& attachment_paths) {
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

        // Read and encode CSV attachments if provided
        std::vector<std::pair<std::string, std::string>> attachments; // filename, base64_data
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
        std::string top_content_type = attachments.empty() ? "multipart/related" : "multipart/mixed";
        std::string top_boundary = attachments.empty() ? inner_boundary : outer_boundary;

        g_email_payload =
            "From: " + config_.from_email + "\r\n"
            "To: " + config_.to_emails[0] + "\r\n"
            "Subject: " + subject + "\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: " + top_content_type + "; boundary=\"" + top_boundary + "\"\r\n"
            "\r\n";

        // If we have attachments, create nested structure
        if (!attachments.empty()) {
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

        // Add CSV attachments if present
        if (!attachments.empty()) {
            for (const auto& [csv_filename, csv_base64] : attachments) {
                g_email_payload += "\r\n--" + outer_boundary + "\r\n";
                g_email_payload += "Content-Type: text/csv; name=\"" + csv_filename + "\"\r\n";
                g_email_payload += "Content-Transfer-Encoding: base64\r\n";
                g_email_payload += "Content-Disposition: attachment; filename=\"" + csv_filename + "\"\r\n";
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
    std::shared_ptr<DatabaseInterface> db,
    const std::unordered_map<std::string, Position>& yesterday_positions,
    const std::unordered_map<std::string, double>& yesterday_close_prices,
    const std::unordered_map<std::string, double>& two_days_ago_close_prices,
    const std::map<std::string, double>& yesterday_daily_metrics)
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

    // Today's Positions (Forward-Looking - no PnL shown as it will be zero)
    html << "<h2>Today's Positions</h2>\n";
    html << format_positions_table(positions, is_daily_strategy, current_prices, strategy_metrics);

    // Executions (if any)
    if (!executions.empty()) {
        html << "<h2>Daily Executions</h2>\n";
        html << format_executions_table(executions);
    }

    // Calculate yesterday's date for display
    std::string yesterday_date_str;
    try {
        // Parse the date string to calculate yesterday
        std::tm tm = {};
        std::istringstream ss(date);
        ss >> std::get_time(&tm, "%Y-%m-%d");
        if (!ss.fail()) {
            auto time_point = std::chrono::system_clock::from_time_t(std::mktime(&tm));
            auto yesterday = time_point - std::chrono::hours(24);
            auto yesterday_time_t = std::chrono::system_clock::to_time_t(yesterday);
            std::ostringstream oss;
            oss << std::put_time(std::gmtime(&yesterday_time_t), "%Y-%m-%d");
            yesterday_date_str = oss.str();
        }
    } catch (...) {
        yesterday_date_str = "Previous Day";
    }

    // Yesterday's Finalized Positions (with actual PnL)
    if (!yesterday_positions.empty() && !yesterday_close_prices.empty() && !two_days_ago_close_prices.empty()) {
        html << format_yesterday_finalized_positions_table(
            yesterday_positions,
            two_days_ago_close_prices,  // Entry prices (Day T-2)
            yesterday_close_prices,      // Exit prices (Day T-1)
            db,
            yesterday_daily_metrics,    // Yesterday's daily metrics (not today's strategy_metrics)
            yesterday_date_str
        );
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
        html << format_strategy_metrics(strategy_metrics);
        html << "</div>\n";
    }

    // Symbols Reference (moved after Portfolio Snapshot)
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

    // Footer note
    if (is_daily_strategy) {
        html << "<div class=\"footer-note\">\n";
        html << "<strong>Note:</strong> This strategy is based on daily OHLCV data. All values reflect a trading start date of October 5th, 2025. ";
        html << "The ES, NQ, and YM positions are micro contracts (MES, MNQ, and MYM), not the standard mini or full-size contracts. All values reflect this accurately, and this is only a mismatch in representation, which we are currently working on fixing.\n";
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
                // Get multiplier from registry (primary source)
                contract_multiplier = instrument->get_multiplier();
            } else {
                // NOTE: This fallback uses static values as EmailSender doesn't have access to TrendFollowingStrategy
                // Future improvement: pass strategy reference or create shared utility for fallback multipliers

                // Fallback multipliers for common contracts (kept minimal for robustness)
                static const std::unordered_map<std::string, double> fallback_multipliers = {
                    {"NQ", 20.0}, {"MNQ", 2.0}, {"ES", 50.0}, {"MES", 5.0},
                    {"YM", 5.0}, {"MYM", 0.5}, {"RTY", 50.0},
                    {"6A", 100000.0}, {"6B", 62500.0}, {"6C", 100000.0}, {"6E", 125000.0},
                    {"6J", 12500000.0}, {"6S", 125000.0}, {"6N", 100000.0}, {"6M", 500000.0},
                    {"CL", 1000.0}, {"GC", 100.0}, {"HG", 25000.0}, {"PL", 50.0}, {"SI", 5000.0},
                    {"ZC", 5000.0}, {"ZS", 5000.0}, {"ZW", 5000.0}, {"ZL", 60000.0},
                    {"ZM", 100.0}, {"ZN", 100000.0}, {"ZB", 100000.0}, {"UB", 100000.0},
                    {"ZR", 2000.0}, {"RB", 42000.0}, {"HO", 42000.0}, {"NG", 10000.0},
                    {"HE", 40000.0}, {"LE", 40000.0}, {"GF", 50000.0}, {"KE", 5000.0}
                };

                auto it = fallback_multipliers.find(lookup_sym);
                if (it != fallback_multipliers.end()) {
                    contract_multiplier = it->second;
                } else {
                    WARN("Unknown contract multiplier for " + lookup_sym + " in email formatting, using 1.0");
                }
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
        
        // Handle negative numbers by processing the absolute value
        size_t start_pos = 0;
        if (!s.empty() && s[0] == '-') {
            start_pos = 1;
        }
        
        size_t dp = s.find('.');
        if (dp == std::string::npos) dp = s.size();
        int pos = static_cast<int>(dp) - 3;
        while (pos > static_cast<int>(start_pos)) {
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
                                                const std::unordered_map<std::string, double>& current_prices,
                                                const std::map<std::string, double>& strategy_metrics) {
    std::ostringstream html;

    html << "<table>\n";
    html << "<tr><th>Symbol</th><th>Quantity</th>";

    // Show only market price for forward-looking positions (no PnL - will be zero)
    html << "<th>Market Price</th>";

    html << "<th>Notional</th><th>% of Total</th></tr>\n";

    double total_notional = 0.0;
    double total_margin_posted = 0.0;
    int active_positions = 0;

    // First pass: calculate total notional
    std::vector<std::tuple<std::string, double, double, double, double, double>> position_data;  // symbol, qty, entry, market, notional, margin

    for (const auto& [symbol, position] : positions) {
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
                    ERROR("CRITICAL: Instrument " + lookup_sym + " not found in registry for email generation!");
                    throw std::runtime_error("Missing instrument in registry: " + lookup_sym);
                }

                // Get contract multiplier for notional calculation from registry (ONLY source)
                contract_multiplier = instrument->get_multiplier();
                if (contract_multiplier <= 0) {
                    ERROR("CRITICAL: Invalid multiplier " + std::to_string(contract_multiplier) +
                          " for " + lookup_sym);
                    throw std::runtime_error("Invalid multiplier for: " + lookup_sym);
                }

                double contracts_abs = std::abs(position.quantity.as_double());
                double initial_margin_per_contract = instrument->get_margin_requirement();
                if (initial_margin_per_contract <= 0) {
                    ERROR("CRITICAL: Invalid margin requirement " + std::to_string(initial_margin_per_contract) +
                          " for " + lookup_sym);
                    throw std::runtime_error("Invalid margin requirement for: " + lookup_sym);
                }
                total_margin_posted += contracts_abs * initial_margin_per_contract;

            } catch (const std::exception& e) {
                ERROR("CRITICAL: Failed to get instrument data for " + position.symbol + ": " + e.what());
                throw;  // Re-throw - don't hide the error
            }

            // Calculate notional with proper contract multiplier
            notional = position.quantity.as_double() * position.average_price.as_double() * contract_multiplier;
            total_notional += std::abs(notional);

            // Use current market price if available, otherwise fall back to average price
            double market_price = position.average_price.as_double();
            auto price_it = current_prices.find(symbol);
            if (price_it != current_prices.end()) {
                market_price = price_it->second;
            }

            // Entry price (same as market price for forward-looking positions)
            double entry_price = position.average_price.as_double();

            // Store data for second pass
            position_data.push_back(std::make_tuple(
                symbol,
                position.quantity.as_double(),
                entry_price,
                market_price,
                notional,
                total_margin_posted
            ));
        }
    }

    // Second pass: render rows with % of total
    for (const auto& [symbol, qty, entry_price, market_price, notional, margin] : position_data) {
        double pct_of_total = (total_notional > 0) ? (std::abs(notional) / total_notional * 100.0) : 0.0;

        html << "<tr>\n";
        html << "<td>" << symbol << "</td>\n";
        html << "<td>" << std::fixed << std::setprecision(0) << qty << "</td>\n";

        // Show only market price (entry price removed from display)
        html << "<td>$" << std::fixed << std::setprecision(2) << market_price << "</td>\n";

        html << "<td>$" << std::fixed << std::setprecision(2) << std::abs(notional) << "</td>\n";
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

    // Add volatility if available in strategy metrics
    auto volatility_it = strategy_metrics.find("Volatility");
    if (volatility_it != strategy_metrics.end()) {
        html << "<strong>Volatility:</strong> " << std::fixed << std::setprecision(2)
             << volatility_it->second << "%<br>\n";
    }

    html << "<strong>Total Notional:</strong> $" << format_with_commas(total_notional) << "<br>\n";
    html << "<strong>Total Margin Posted:</strong> $" << format_with_commas(total_margin_posted) << "\n";
    html << "</div>\n";

    return html.str();
}

std::string EmailSender::format_yesterday_finalized_positions_table(
    const std::unordered_map<std::string, Position>& yesterday_positions,
    const std::unordered_map<std::string, double>& entry_prices,  // Day T-2 close
    const std::unordered_map<std::string, double>& exit_prices,   // Day T-1 close
    std::shared_ptr<DatabaseInterface> db,
    const std::map<std::string, double>& strategy_metrics,
    const std::string& yesterday_date
) {
    std::ostringstream html;

    if (yesterday_positions.empty()) {
        return std::string();
    }

    html << "<h2>Yesterday's Finalized Position Results</h2>\n";
    html << "<table>\n";
    html << "<tr>";
    html << "<th>Symbol</th>";
    html << "<th>Quantity</th>";
    html << "<th>Entry Price</th>";
    html << "<th>Exit Price</th>";
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

        // Skip positions with zero quantity
        if (std::abs(qty) < 0.0001) continue;

        // Get entry and exit prices
        double entry_price = 0.0;
        double exit_price = 0.0;

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

    // Sort by symbol
    std::sort(position_data.begin(), position_data.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });

    // Render table rows
    for (const auto& [symbol, qty, entry_price, exit_price, realized_pnl] : position_data) {

        html << "<tr>";
        html << "<td>" << symbol << "</td>";
        html << "<td>" << std::fixed << std::setprecision(2) << qty << "</td>";
        html << "<td>" << format_with_commas(entry_price, 2) << "</td>";
        html << "<td>" << format_with_commas(exit_price, 2) << "</td>";

        // Realized PnL with color (GROSS PnL from positions table)
        std::string realized_pnl_class = (realized_pnl >= 0) ? "positive" : "negative";
        html << "<td class=\"" << realized_pnl_class << "\">";
        html << "$" << format_with_commas(realized_pnl, 2);
        html << "</td>";

        html << "</tr>\n";
    }

    html << "</table>\n";

    // Add daily metrics as a section heading after the table - use passed yesterday_daily_metrics
    if (!strategy_metrics.empty() && !yesterday_date.empty()) {
        html << "<h2>" << yesterday_date << " Metrics</h2>\n";
        html << "<div class=\"metrics-category\">\n";

        // Helper to format metrics with proper color
        auto format_metric_display = [&format_with_commas](const std::string& label, double value, bool is_percentage) -> std::string {
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
                formatted_value = "$" + format_with_commas(value, 2);
            }

            return "<div class=\"metric\"><strong>" + label + ":</strong> <span" + value_class + ">" + formatted_value + "</span></div>\n";
        };

        // Extract and display metrics in the specified order:
        // 1. Daily Return (as percentage)
        // 2. Daily Unrealized PnL (as dollar amount)
        // 3. Daily Realized PnL (as dollar amount)
        // 4. Daily Total PnL (as dollar amount)

        auto daily_return_it = strategy_metrics.find("Daily Return");
        if (daily_return_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Return", daily_return_it->second, true);
        }

        auto daily_unrealized_it = strategy_metrics.find("Daily Unrealized PnL");
        if (daily_unrealized_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Unrealized PnL", daily_unrealized_it->second, false);
        }

        auto daily_realized_it = strategy_metrics.find("Daily Realized PnL");
        if (daily_realized_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Realized PnL", daily_realized_it->second, false);
        }

        auto daily_total_it = strategy_metrics.find("Daily Total PnL");
        if (daily_total_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Total PnL", daily_total_it->second, false);
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

std::string EmailSender::format_strategy_metrics(const std::map<std::string, double>& strategy_metrics) {
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
        } else if (key.find("Leverage") != std::string::npos || key.find("Ratio") != std::string::npos) {
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

    // Single Portfolio Snapshot section
    html << "<h2>Portfolio Snapshot</h2>\n";
    html << "<div class=\"metrics-category\">\n";

    // Define the exact order for Portfolio Snapshot
    std::vector<std::string> portfolio_order = {
        "Total Annualized Return",
        "Total Unrealized PnL",
        "Total Realized PnL",
        "Total PnL"
    };

    // Add total metrics
    for (const auto& metric_name : portfolio_order) {
        auto it = strategy_metrics.find(metric_name);
        if (it != strategy_metrics.end()) {
            html << format_metric(it->first, it->second);
        }
    }

    html << "<br>\n";

    // Leverage metrics
    auto gross_lev = strategy_metrics.find("Gross Leverage");
    if (gross_lev != strategy_metrics.end()) {
        html << format_metric("Gross Leverage", gross_lev->second);
    }

    auto net_lev = strategy_metrics.find("Net Leverage");
    if (net_lev != strategy_metrics.end()) {
        html << format_metric("Net Leverage", net_lev->second);
    }

    auto port_lev = strategy_metrics.find("Portfolio Leverage");
    if (port_lev != strategy_metrics.end()) {
        html << format_metric("Portfolio Leverage", port_lev->second);
    } else {
        auto port_lev_gross = strategy_metrics.find("Portfolio Leverage (Gross)");
        if (port_lev_gross != strategy_metrics.end()) {
            html << format_metric("Portfolio Leverage", port_lev_gross->second);
        }
    }

    html << "<br>\n";

    // Total Commissions
    auto total_comm = strategy_metrics.find("Total Commissions");
    if (total_comm != strategy_metrics.end()) {
        html << format_metric("Total Commissions", total_comm->second);
    }

    html << "<br>\n";

    // Margin metrics
    auto margin_posted = strategy_metrics.find("Margin Posted");
    if (margin_posted != strategy_metrics.end()) {
        html << format_metric("Margin Posted", margin_posted->second);
    }

    auto equity_margin = strategy_metrics.find("Equity-to-Margin Ratio");
    if (equity_margin != strategy_metrics.end()) {
        html << format_metric("Equity-to-Margin Ratio", equity_margin->second);
    }

    auto margin_cushion = strategy_metrics.find("Margin Cushion");
    if (margin_cushion != strategy_metrics.end()) {
        html << format_metric("Margin Cushion", margin_cushion->second);
    }

    html << "<br>\n";

    // Cash and Portfolio Value
    auto portfolio_value = strategy_metrics.find("Current Portfolio Value");
    if (portfolio_value != strategy_metrics.end()) {
        html << format_metric("Current Portfolio Value", portfolio_value->second);
    }

    auto cash_available = strategy_metrics.find("Cash Available");
    if (cash_available != strategy_metrics.end()) {
        html << format_metric("Cash Available", cash_available->second);
    }

    html << "</div>\n";

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
