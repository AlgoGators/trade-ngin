#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/chart_generator.hpp"
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/core/chart_generator.hpp"
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
#include <cstdlib>
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
    : credentials_(credentials), initialized_(false), holiday_checker_("include/trade_ngin/core/holidays.json") {
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

        if(!total_transaction_costs_base64_.empty()){
            g_email_payload += "\r\n--" + inner_boundary + "\r\n";
            g_email_payload += "Content-Type: image/png\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-ID: <total_transaction_costs>\r\n";
            g_email_payload += "Content-Disposition: inline; filename=\"total_transaction_costs.png\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += total_transaction_costs_base64_ + "\r\n";
        }

        if(!margin_posted_base64_.empty()){
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
            g_email_payload += "Content-Disposition: inline; filename=\"portfolio_composition.png\"\r\n";
            g_email_payload += "\r\n";
            g_email_payload += portfolio_composition_base64_ + "\r\n";
        }

        if (!cumulative_pnl_by_symbol_base64_.empty()) {
            g_email_payload += "\r\n--" + inner_boundary + "\r\n";
            g_email_payload += "Content-Type: image/png\r\n";
            g_email_payload += "Content-Transfer-Encoding: base64\r\n";
            g_email_payload += "Content-ID: <cumulative_pnl_by_symbol>\r\n";
            g_email_payload += "Content-Disposition: inline; filename=\"cumulative_pnl_by_symbol.png\"\r\n";
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

namespace {
    // Helper function to check if a symbol is an agricultural future
    bool is_agricultural_future(const std::string& symbol) {
        // Extract base symbol (remove .v.0, .c.0 suffixes)
        std::string base_symbol = symbol;
        auto dot_pos = base_symbol.find('.');
        if (dot_pos != std::string::npos) {
            base_symbol = base_symbol.substr(0, dot_pos);
        }
        
        // Agricultural futures list
        static const std::set<std::string> ag_futures = {
            "ZC",  // Corn
            "ZS",  // Soybeans
            "ZW",  // Wheat
            "ZL",  // Soybean Oil
            "ZM",  // Soybean Meal
            "ZR",  // Rough Rice
            "KE",  // KC HRW Wheat
            "HE",  // Lean Hogs
            "LE",  // Live Cattle
            "GF"   // Feeder Cattle
        };
        
        return ag_futures.find(base_symbol) != ag_futures.end();
    }
    
    // Helper function to filter out agricultural positions
    std::unordered_map<std::string, Position> filter_non_agricultural_positions(
        const std::unordered_map<std::string, Position>& positions)
    {
        std::unordered_map<std::string, Position> filtered;
        for (const auto& [symbol, position] : positions) {
            if (!is_agricultural_future(symbol)) {
                filtered[symbol] = position;
            }
        }
        return filtered;
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

   // Parse the date to check day of week
   std::tm tm = {};
   std::istringstream ss(date);
   ss >> std::get_time(&tm, "%Y-%m-%d");
   std::mktime(&tm);
   int day_of_week = tm.tm_wday;  // 0=Sunday, 1=Monday, ..., 6=Saturday

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
       
       INFO("Is yesterday (" + yesterday_date_str + ") a holiday? " + std::string(is_yesterday_holiday ? "YES" : "NO"));
       
       if (is_yesterday_holiday) {
           yesterday_holiday_name = holiday_checker_.get_holiday_name(yesterday_date_str);
           INFO("Holiday name: " + yesterday_holiday_name);
       }
   } catch (...) {
       ERROR("Exception while calculating yesterday's date");
       yesterday_date_str = "Previous Day";
   }

   // Check if today is Monday
   bool is_monday = (day_of_week == 1);
   bool is_sunday = (day_of_week == 0);

   // Note: If yesterday was a holiday, we'll show a banner but continue with full report
   // The show_yesterday_pnl flag below will hide the Yesterday's PnL table

   // Determine if we should show yesterday's PnL table AND chart
   bool show_yesterday_pnl = true;
   
   // Hide yesterday's PnL if:
   // 1. Today is Sunday (Saturday has no data)
   // 2. Yesterday was a holiday
   if (is_sunday || is_yesterday_holiday) {
       show_yesterday_pnl = false;
   }

   // Generate HTML header and styles
   html << "<!DOCTYPE html>\n";
   html << "<html>\n<head>\n";
   html << "<meta charset=\"UTF-8\" />\n";
   html << "<style>\n";
   html << "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f9f9f9; }\n";
   html << ".container { max-width: 1200px; margin: 0 auto; background-color: white; padding: 30px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n";
   html << "h1, h2, h3 { color: #333; font-family: Arial, sans-serif; }\n";
   html << "h1 { font-size: 24px; margin-bottom: 5px; }\n";
   html << "h2 { font-size: 20px; margin-top: 25px; margin-bottom: 10px; border-bottom: 2px solid #2c5aa0; padding-bottom: 5px; }\n";
   html << "h3 { font-size: 16px; margin-top: 20px; margin-bottom: 10px; }\n";
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
   html << ".alert-note { background-color: #fee2e2; border-left: 4px solid #dc2626; padding: 15px; margin: 20px 0; font-size: 13px; color: #991b1b; font-family: Arial, sans-serif; }\n";
   html << ".summary-stats { background-color: #fff5e6; padding: 15px; margin: 15px 0; border-radius: 5px; font-family: Arial, sans-serif; font-size: 14px; }\n";
   html << ".chart-container { margin: 20px 0; padding: 20px; background-color: #f8f9fa; border-radius: 8px; text-align: center; }\n";
   html << ".weekend-message { background-color: #e6f3ff; border-left: 4px solid #2c5aa0; padding: 20px; margin: 20px 0; font-size: 16px; }\n";
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

   // Weekend/Holiday banners
   if (is_sunday) {
       // Sunday banner (yesterday was Saturday)
       html << "<div style=\"background: linear-gradient(135deg, #fef3c7 0%, #fde68a 100%); border: 2px solid #f59e0b; border-radius: 8px; padding: 20px 30px; margin: 20px 0 30px 0; box-shadow: 0 4px 6px rgba(0,0,0,0.1);\">\n";
       html << "<h2 style=\"margin: 0 0 10px 0; color: #92400e; font-size: 20px; border-bottom: 2px solid #92400e; padding-bottom: 8px; display: inline-block;\">Yesterday was Saturday</h2>\n";
       html << "<p style=\"margin: 15px 0 5px 0; color: #78350f; font-size: 15px; line-height: 1.6;\">The latest futures settlement prices are not available, as futures markets were closed yesterday (" << yesterday_date_str << ") due to it being a Saturday. The PnL for these contracts will be updated in the next report once settlement data is released.</p>\n";
       html << "<p style=\"margin: 5px 0 0 0; color: #92400e; font-weight: 600; font-size: 14px;\">Please continue to monitor your positions closely.</p>\n";
       html << "</div>\n";
   }
   else if (is_monday) {
       // Monday banner (agricultural futures issue)
       html << "<div style=\"background: linear-gradient(135deg, #fef3c7 0%, #fde68a 100%); border: 2px solid #f59e0b; border-radius: 8px; padding: 20px 30px; margin: 20px 0 30px 0; box-shadow: 0 4px 6px rgba(0,0,0,0.1);\">\n";
       html << "<h2 style=\"margin: 0 0 10px 0; color: #92400e; font-size: 20px; border-bottom: 2px solid #92400e; padding-bottom: 8px; display: inline-block;\">Yesterday was Sunday</h2>\n";
       html << "<p style=\"margin: 15px 0 5px 0; color: #78350f; font-size: 15px; line-height: 1.6;\">Agricultural futures settlement prices for Sunday (" << yesterday_date_str << ") are not yet available, as these contracts begin trading Sunday evening. The PnL for these contracts will be updated in the next report once settlement data is released.</p>\n";
       html << "<p style=\"margin: 5px 0 0 0; color: #92400e; font-weight: 600; font-size: 14px;\">Please monitor these positions closely.</p>\n";
       html << "</div>\n";
   }
   else if (is_yesterday_holiday) {
       // Holiday banner
       html << "<div style=\"background: linear-gradient(135deg, #fef3c7 0%, #fde68a 100%); border: 2px solid #f59e0b; border-radius: 8px; padding: 20px 30px; margin: 20px 0 30px 0; box-shadow: 0 4px 6px rgba(0,0,0,0.1);\">\n";
       html << "<h2 style=\"margin: 0 0 10px 0; color: #92400e; font-size: 20px; border-bottom: 2px solid #92400e; padding-bottom: 8px; display: inline-block;\">Yesterday was " << yesterday_holiday_name << "</h2>\n";
       html << "<p style=\"margin: 15px 0 5px 0; color: #78350f; font-size: 15px; line-height: 1.6;\">The latest futures settlement prices are not available, as futures markets were closed yesterday (" << yesterday_date_str << ") due to a federal holiday. The PnL for these contracts will be updated in the next report once settlement data is released.</p>\n";
       html << "<p style=\"margin: 5px 0 0 0; color: #92400e; font-weight: 600; font-size: 14px;\">Please continue to monitor your positions closely.</p>\n";
       html << "</div>\n";
   }

   // Today's Positions (Forward-Looking - no PnL shown as it will be zero)
   html << "<h2>Today's Positions</h2>\n";
      
   html << format_positions_table(positions, is_daily_strategy, current_prices, strategy_metrics);

   // Executions (if any)
   if (!executions.empty()) {
       html << "<h2>Daily Executions</h2>\n";
       html << format_executions_table(executions);
   }

   // Yesterday's Finalized Positions (with actual PnL) - show on all days except Sunday and after holidays
   if (show_yesterday_pnl && !yesterday_positions.empty() && !yesterday_close_prices.empty() && !two_days_ago_close_prices.empty()) {
       html << format_yesterday_finalized_positions_table(
           yesterday_positions,
           two_days_ago_close_prices,  // Entry prices (Day T-2)
           yesterday_close_prices,      // Exit prices (Day T-1)
           db,
           yesterday_daily_metrics,    // Yesterday's daily metrics
           yesterday_date_str
       );
   }
   else if (!show_yesterday_pnl) {
       // Add a red alert explaining no executions due to non-trading day
       html << "<div class=\"alert-note\">\n";
       if (is_sunday) {
           html << "<strong>No Executions:</strong> Futures markets were closed yesterday, so no executions were generated and portfolio holdings are unchanged. Updates will resume with the next trading session’s data.\n";
       }
       else if (is_yesterday_holiday) {
           html << "<strong>No Executions:</strong> Since yesterday (" << yesterday_date_str << ") was a market holiday, no new market data is available. Positions remain unchanged from the previous trading day, and no executions were generated.\n";
       }
       html << "</div>\n";
       
       // Add a yellow note explaining why yesterday's PnL is not shown
       html << "<div class=\"footer-note\">\n";
       if (is_sunday) {
           html << "<strong>Note:</strong> Yesterday's PnL data is not available.\n";
       }
       else if (is_monday) {
           html << "<strong>Note:</strong> Yesterday's PnL data is not available for agricultural contracts.\n";
       }
       else if (is_yesterday_holiday) {
           html << "<strong>Note:</strong> Yesterday's PnL data is not available.\n";
       }
       html << "</div>\n";
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

   html << "<h2>Charts</h2>\n";
   if (db) {
       // Generate equity curve chart
       chart_base64_ = ChartGenerator::generate_equity_curve_chart(db, "LIVE_TREND_FOLLOWING", 30);
       if (!chart_base64_.empty()) {
           html << "<h3 style=\"margin-top: 20px; color: #333;\">Equity Curve</h3>\n";
           html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: center;\">\n";
           html << "<img src=\"cid:equity_chart\" alt=\"Portfolio Equity Curve\" style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
           html << "</div>\n";
       }

       // Generate PnL by symbol chart - ONLY if show_yesterday_pnl is true
       if (show_yesterday_pnl) {
           pnl_by_symbol_base64_ = ChartGenerator::generate_pnl_by_symbol_chart(db, "LIVE_TREND_FOLLOWING", date);
           if (!pnl_by_symbol_base64_.empty()) {
               html << "<h3 style=\"margin-top: 20px; color: #333;\">Yesterday's PnL by Symbol</h3>\n";
               html << "<div style=\"width: 100%; max-width: 800px; margin: 20px auto; text-align: center;\">\n";
               html << "<img src=\"cid:pnl_by_symbol\" alt=\"PnL by Symbol\" style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
               html << "</div>\n";
           }
       }

       // Generate daily PnL chart
       daily_pnl_base64_ = ChartGenerator::generate_daily_pnl_chart(db, "LIVE_TREND_FOLLOWING", date, 30);
       if (!daily_pnl_base64_.empty()) {
           html << "<h3 style=\"margin-top: 20px; color: #333;\">Daily PnL (Last 30 Days)</h3>\n";
           html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: center;\">\n";
           html << "<img src=\"cid:daily_pnl\" alt=\"Daily PnL\" style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
           html << "</div>\n";
       }

        total_transaction_costs_base64_ =
            ChartGenerator::generate_total_transaction_costs_chart(
                db, "LIVE_TREND_FOLLOWING", date);
        if (!total_transaction_costs_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Cost per $1M Traded (Efficiency Metric)</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: center;\">\n";
            html << "<img src=\"cid:total_transaction_costs\" alt=\"Cost per $1M Traded\" style=\"max-width: 100%; height: auto; ""border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        margin_posted_base64_ = ChartGenerator::generate_margin_posted_chart(db, "LIVE_TREND_FOLLOWING", date);

        if (!margin_posted_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Margin Posted</h3>\n";
            html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: center;\">\n";
            html << "<img src=\"cid:margin_posted\" alt=\"Margin Posted\" "
                    "style=\"max-width: 100%; height: auto; border-radius: 8px; "
                    "box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        portfolio_composition_base64_ = ChartGenerator::generate_portfolio_composition_chart(
        positions,
        current_prices,
        date
        );
        if (!portfolio_composition_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Portfolio Composition</h3>\n";
            html << "<div style=\"width: 100%; max-width: 800px; margin: 20px auto; text-align: center;\">\n";
            html << "<img src=\"cid:portfolio_composition\" alt=\"Portfolio Composition\" "
                    "style=\"max-width: 100%; height: auto; border-radius: 8px; "
                    "box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }

        cumulative_pnl_by_symbol_base64_ = ChartGenerator::generate_cumulative_pnl_by_symbol_chart(
            db,
            "LIVE_TREND_FOLLOWING",
            date
        );
        if (!cumulative_pnl_by_symbol_base64_.empty()) {
            html << "<h3 style=\"margin-top: 20px; color: #333;\">Cumulative PnL by Symbol (All-Time)</h3>\n";
            html << "<div style=\"width: 100%; max-width: 800px; margin: 20px auto; text-align: center;\">\n";
            html << "<img src=\"cid:cumulative_pnl_by_symbol\" alt=\"Cumulative PnL by Symbol\" "
                    "style=\"max-width: 100%; height: auto; border-radius: 8px; "
                    "box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
            html << "</div>\n";
        }
   }

   // Symbols Reference (moved after Portfolio Snapshot)
   html << "<h2>Symbols Reference</h2>\n";
   if (db) {
       try {
           html << format_symbols_table_for_positions(positions, db, yesterday_date_str);
       } catch (const std::exception& e) {
           html << "<p>Error loading symbols data: " << e.what() << "</p>\n";
       }
   } else {
       html << "<p>Database unavailable; symbols reference not included.</p>\n";
   }

   // Rollover Warning (if applicable)
   if (db) {
       // Check for test date override via environment variable
       const char* test_date_env = std::getenv("ROLLOVER_TEST_DATE");
       std::string test_date = test_date_env ? std::string(test_date_env) : "";
       html << format_rollover_warning(positions, date, db, test_date);
   }

   // Footer note
   if (is_daily_strategy) {
       html << "<div class=\"footer-note\">\n";
       html << "<strong>Note:</strong> This strategy is based on daily OHLCV data. We currently only provide data for the front-month contract.<br><br>\n";
       html << "All values reflect a trading start date of October 5th, 2025.<br><br>\n";
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
    html << "<tr><th>Symbol</th><th>Side</th><th>Quantity</th><th>Price</th><th>Notional</th><th>Transaction Cost</th></tr>\n";

    double total_transaction_cost = 0.0;
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
        total_transaction_cost += exec.total_transaction_costs.as_double();

        std::string side_str = exec.side == Side::BUY ? "BUY" : "SELL";
        std::string side_class = exec.side == Side::BUY ? "positive" : "negative";

        html << "<tr>\n";
        html << "<td>" << exec.symbol << "</td>\n";
        html << "<td class=\"" << side_class << "\">" << side_str << "</td>\n";
        html << "<td>" << std::fixed << std::setprecision(0) << exec.filled_quantity.as_double() << "</td>\n";
        html << "<td>$" << std::fixed << std::setprecision(2) << exec.fill_price.as_double() << "</td>\n";
        html << "<td>$" << std::fixed << std::setprecision(2) << notional << "</td>\n";
        html << "<td>$" << std::fixed << std::setprecision(2) << exec.total_transaction_costs.as_double() << "</td>\n";
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
    html << "<strong>Transaction Costs:</strong> $" << format_with_commas(total_transaction_cost) << "\n";
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

    // Helper to check if a symbol is an agricultural future
    auto is_ag_future = [](const std::string& sym) -> bool {
        std::string base = sym;
        auto dotpos = base.find(".v.");
        if (dotpos != std::string::npos) base = base.substr(0, dotpos);
        dotpos = base.find(".c.");
        if (dotpos != std::string::npos) base = base.substr(0, dotpos);
        
        static const std::set<std::string> ag_futures = {
            "ZC", "ZS", "ZW", "ZL", "ZM", "ZR", "KE", "HE", "LE", "GF"
        };
        return ag_futures.find(base) != ag_futures.end();
    };

    // Check if yesterday was Sunday (meaning today is Monday)
    bool is_monday = false;
    if (!yesterday_date.empty()) {
        std::tm tm = {};
        std::istringstream ss(yesterday_date);
        ss >> std::get_time(&tm, "%Y-%m-%d");
        if (!ss.fail()) {
            std::mktime(&tm);
            is_monday = (tm.tm_wday == 0);  // 0 = Sunday
        }
    }

    // Render table rows
    for (const auto& [symbol, qty, entry_price, exit_price, realized_pnl] : position_data) {

        html << "<tr>";
        html << "<td>" << symbol << "</td>";
        html << "<td>" << std::fixed << std::setprecision(2) << qty << "</td>";
        html << "<td>" << format_with_commas(entry_price, 2) << "</td>";
        
        // Show N/A for exit price if ag future on Monday
        if (is_monday && is_ag_future(symbol) && exit_price == 0.0) {
            html << "<td>N/A</td>";
        } else {
            html << "<td>" << format_with_commas(exit_price, 2) << "</td>";
        }

        // Show N/A for realized PnL if ag future on Monday with zero PnL
        if (is_monday && is_ag_future(symbol) && std::abs(realized_pnl) < 0.01) {
            html << "<td>N/A</td>";
        } else {
            // Realized PnL with color (GROSS PnL from positions table)
            std::string realized_pnl_class = (realized_pnl >= 0) ? "positive" : "negative";
            html << "<td class=\"" << realized_pnl_class << "\">";
            html << "$" << format_with_commas(realized_pnl, 2);
            html << "</td>";
        }

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

        // Add Total Positions line
        html << "<div class=\"metric\"><strong>Total Positions:</strong> " << position_data.size() << "</div>\n";

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
            html << format_metric_display("Daily Unrealized PnL (Gross)", daily_unrealized_it->second, false);
        }

        auto daily_realized_it = strategy_metrics.find("Daily Realized PnL");
        if (daily_realized_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Realized PnL (Gross)", daily_realized_it->second, false);
        }

        auto daily_transaction_costs_it = strategy_metrics.find("Daily Transaction Costs");
        if (daily_transaction_costs_it != strategy_metrics.end()) {
            // Display transaction costs as positive value in black font (no color coding)
            double transaction_cost_value = std::abs(daily_transaction_costs_it->second);
            std::string formatted_transaction_cost = "$" + format_with_commas(transaction_cost_value, 2);
            html << "<div class=\"metric\"><strong>Daily Transaction Costs:</strong> <span>" << formatted_transaction_cost << "</span></div>\n";
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
    std::shared_ptr<DatabaseInterface> db,
    const std::map<std::string, double>& strategy_metrics,
    const std::string& yesterday_date
) {
    std::ostringstream html;

    // Check if there are any positions across all strategies
    bool has_positions = false;
    for (const auto& [_, positions] : strategy_positions) {
        for (const auto& [__, pos] : positions) {
            if (std::abs(pos.quantity.as_double()) >= 0.0001) {
                has_positions = true;
                break;
            }
        }
        if (has_positions) break;
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

    // Helper to check if a symbol is an agricultural future
    auto is_ag_future = [](const std::string& sym) -> bool {
        std::string base = sym;
        auto dotpos = base.find(".v.");
        if (dotpos != std::string::npos) base = base.substr(0, dotpos);
        dotpos = base.find(".c.");
        if (dotpos != std::string::npos) base = base.substr(0, dotpos);

        static const std::set<std::string> ag_futures = {
            "ZC", "ZS", "ZW", "ZL", "ZM", "ZR", "KE", "HE", "LE", "GF"
        };
        return ag_futures.find(base) != ag_futures.end();
    };

    // Check if yesterday was Sunday (meaning today is Monday)
    bool is_monday = false;
    if (!yesterday_date.empty()) {
        std::tm tm = {};
        std::istringstream ss(yesterday_date);
        ss >> std::get_time(&tm, "%Y-%m-%d");
        if (!ss.fail()) {
            std::mktime(&tm);
            is_monday = (tm.tm_wday == 0);  // 0 = Sunday
        }
    }

    // Sort strategies alphabetically for consistent ordering
    std::vector<std::string> strategy_names;
    for (const auto& [name, _] : strategy_positions) {
        strategy_names.push_back(name);
    }
    std::sort(strategy_names.begin(), strategy_names.end());

    // Track total positions across all strategies
    int total_positions_count = 0;

    // Generate table for each strategy
    for (const auto& strategy_name : strategy_names) {
        const auto& positions = strategy_positions.at(strategy_name);

        // Check if strategy has any active positions
        std::vector<std::tuple<std::string, double, double, double, double>> position_data;
        double strategy_total_pnl = 0.0;

        for (const auto& [symbol, position] : positions) {
            double qty = position.quantity.as_double();
            if (std::abs(qty) < 0.0001) continue;

            double entry_price = 0.0;
            double exit_price = 0.0;

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
             << "border-left: 4px solid #2c5aa0; padding-left: 12px;\">"
             << display_name << "</h3>\n";

        // Table for this strategy
        html << "<table>\n";
        html << "<tr>";
        html << "<th>Symbol</th>";
        html << "<th>Quantity</th>";
        html << "<th>Entry Price</th>";
        html << "<th>Exit Price</th>";
        html << "<th>Realized PnL</th>";
        html << "</tr>\n";

        for (const auto& [symbol, qty, entry_price, exit_price, realized_pnl] : position_data) {
            html << "<tr>";
            html << "<td>" << symbol << "</td>";
            html << "<td>" << std::fixed << std::setprecision(2) << qty << "</td>";
            html << "<td>" << format_with_commas(entry_price, 2) << "</td>";

            if (is_monday && is_ag_future(symbol) && exit_price == 0.0) {
                html << "<td>N/A</td>";
            } else {
                html << "<td>" << format_with_commas(exit_price, 2) << "</td>";
            }

            if (is_monday && is_ag_future(symbol) && std::abs(realized_pnl) < 0.01) {
                html << "<td>N/A</td>";
            } else {
                std::string realized_pnl_class = (realized_pnl >= 0) ? "positive" : "negative";
                html << "<td class=\"" << realized_pnl_class << "\">";
                html << "$" << format_with_commas(realized_pnl, 2);
                html << "</td>";
            }

            html << "</tr>\n";
        }

        html << "</table>\n";

        // Compact strategy-level summary
        std::string pnl_class = (strategy_total_pnl >= 0) ? "positive" : "negative";
        html << "<div style=\"font-size: 13px; color: #666; margin: 8px 0 20px 0; padding-left: 16px;\">\n";
        html << "<strong>Positions:</strong> " << position_data.size()
             << " | <strong>Total Realized PnL (Gross):</strong> <span class=\"" << pnl_class << "\">$"
             << format_with_commas(strategy_total_pnl, 2) << "</span>\n";
        html << "</div>\n";
    }

    // Add daily metrics section
    if (!strategy_metrics.empty() && !yesterday_date.empty()) {
        html << "<h2>" << yesterday_date << " Metrics</h2>\n";
        html << "<div class=\"metrics-category\">\n";

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

        // Add Total Positions line
        html << "<div class=\"metric\"><strong>Total Positions:</strong> " << total_positions_count << "</div>\n";

        auto daily_return_it = strategy_metrics.find("Daily Return");
        if (daily_return_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Return", daily_return_it->second, true);
        }

        auto daily_unrealized_it = strategy_metrics.find("Daily Unrealized PnL");
        if (daily_unrealized_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Unrealized PnL (Gross)", daily_unrealized_it->second, false);
        }

        auto daily_realized_it = strategy_metrics.find("Daily Realized PnL");
        if (daily_realized_it != strategy_metrics.end()) {
            html << format_metric_display("Daily Realized PnL (Gross)", daily_realized_it->second, false);
        }

        auto daily_commissions_it = strategy_metrics.find("Daily Commissions");
        if (daily_commissions_it != strategy_metrics.end()) {
            double commission_value = std::abs(daily_commissions_it->second);
            std::string formatted_commission = "$" + format_with_commas(commission_value, 2);
            html << "<div class=\"metric\"><strong>Daily Commissions:</strong> <span>" << formatted_commission << "</span></div>\n";
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
            key.find("Transaction Costs") != std::string::npos) {
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

    // Total Cumulative Return
    auto total_cumulative_return = strategy_metrics.find("Total Cumulative Return");
    if (total_cumulative_return != strategy_metrics.end()) {
        html << format_metric("Total Cumulative Return", total_cumulative_return->second);
    }


    // Total Annualized Return
    auto total_annualized_return = strategy_metrics.find("Total Annualized Return");
    if (total_annualized_return != strategy_metrics.end()) {
        html << format_metric("Total Annualized Return", total_annualized_return->second);
    }
    
    html << "<br>\n";

    // Total Unrealized PnL (Gross)
    auto total_unrealized = strategy_metrics.find("Total Unrealized PnL");
    if (total_unrealized != strategy_metrics.end()) {
        html << format_metric("Total Unrealized PnL (Gross)", total_unrealized->second);
    }

    // Total Realized PnL (Gross)
    auto total_realized = strategy_metrics.find("Total Realized PnL");
    if (total_realized != strategy_metrics.end()) {
        html << format_metric("Total Realized PnL (Gross)", total_realized->second);
    }

    // Total Transaction Costs
    auto total_comm = strategy_metrics.find("Total Transaction Costs");
    if (total_comm != strategy_metrics.end()) {
        html << format_metric("Total Transaction Costs", total_comm->second);
    }

    // Total PnL (Net)
    auto total_pnl = strategy_metrics.find("Total PnL");
    if (total_pnl != strategy_metrics.end()) {
        html << format_metric("Total PnL (Net)", total_pnl->second);
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
   std::shared_ptr<DatabaseInterface> db,
   const std::string& date) {


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

      // Helper function to get front month contract symbol (advances on/after expiry)
      auto get_front_month_symbol = [](const std::string& ib_symbol, const std::string& contract_months, const std::string& date_str) -> std::string {
           // Parse the date
           std::tm current_tm = {};
           std::istringstream ss(date_str);
           ss >> std::get_time(&current_tm, "%Y-%m-%d");
           if (ss.fail()) {
               return "";
           }
           
         // Helpers for business day calculations
         auto is_business_day = [](const std::tm& tm) { return tm.tm_wday != 0 && tm.tm_wday != 6; };
         auto get_previous_business_day = [&is_business_day](std::tm tm) { do { tm.tm_mday--; std::mktime(&tm); } while (!is_business_day(tm)); return tm; };
         auto get_next_business_day = [&is_business_day](std::tm tm) { do { tm.tm_mday++; std::mktime(&tm); } while (!is_business_day(tm)); return tm; };
         auto get_last_day_of_month = [](int year, int month){ std::tm r{}; r.tm_year=year-1900; r.tm_mon=month; r.tm_mday=0; std::mktime(&r); return r; };
         auto get_nth_weekday = [](int year,int month,int weekday,int n){ std::tm r{}; r.tm_year=year-1900; r.tm_mon=month-1; r.tm_mday=1; std::mktime(&r); int first=r.tm_wday; int delta=(weekday-first+7)%7; r.tm_mday=1+delta+(n-1)*7; std::mktime(&r); return r; };
         auto get_nth_business_day = [&get_next_business_day](int year,int month,int n){ std::tm r{}; r.tm_year=year-1900; r.tm_mon=month-1; r.tm_mday=0; int c=0; while(c<n){ r=get_next_business_day(r); c++; } return r; };

         // Compute exchange-specific expiry for a given symbol/month/year
         auto compute_expiry = [&](const std::string& sym,int y,int m){
             std::tm e{};
             if (sym=="MES" || sym=="MYM" || sym=="MNQ" || sym=="RTY" || sym=="ES" || sym=="YM" || sym=="NQ") {
                 e = get_nth_weekday(y,m,5,3); // 3rd Friday
             } else if (sym=="ZC"||sym=="ZW"||sym=="ZM"||sym=="ZL"||sym=="ZS"||sym=="ZR"||sym=="KE") {
                 e.tm_year=y-1900; e.tm_mon=m-1; e.tm_mday=14; std::mktime(&e); if (!is_business_day(e)) e=get_previous_business_day(e);
             } else if (sym=="GC"||sym=="PL"||sym=="SI") {
                 e=get_last_day_of_month(y,m); e=get_previous_business_day(e); e=get_previous_business_day(e); e=get_previous_business_day(e);
             } else if (sym=="6B"||sym=="6E"||sym=="6J"||sym=="6M"||sym=="6N"||sym=="6S") {
                 e=get_nth_weekday(y,m,3,3); e=get_previous_business_day(e); e=get_previous_business_day(e);
             } else if (sym=="6C") {
                 e=get_nth_weekday(y,m,3,3); e=get_previous_business_day(e);
             } else if (sym=="ZN"||sym=="UB") {
                 e=get_last_day_of_month(y,m); if (!is_business_day(e)) e=get_previous_business_day(e); for(int j=0;j<7;++j) e=get_previous_business_day(e);
             } else if (sym=="HE") {
                 e=get_nth_business_day(y,m,10);
             } else if (sym=="LE") {
                 e=get_last_day_of_month(y,m); if (!is_business_day(e)) e=get_previous_business_day(e);
             } else if (sym=="GF") {
                 e=get_last_day_of_month(y,m); while(e.tm_wday!=4){ e.tm_mday--; std::mktime(&e); }
             } else {
                 e=get_nth_weekday(y,m,5,3);
             }
             return e;
         };

          // Check for monthly contracts (all months or consecutive)
          if (contract_months.find("All Months") != std::string::npos ||
              contract_months.find("consecutive") != std::string::npos) {
             int month = current_tm.tm_mon + 1;
             int year = current_tm.tm_year + 1900;
             // Advance only AFTER actual expiry for this symbol/month
             std::tm expiry = compute_expiry(ib_symbol, year, month);
             if (std::difftime(std::mktime(&current_tm), std::mktime(&expiry)) > 0) {
                 month += 1;
                 if (month > 12) { month = 1; year += 1; }
             }

              char month_code = '\0';
              switch (month) {
                  case 1: month_code = 'F'; break; // January
                  case 2: month_code = 'G'; break; // February
                  case 3: month_code = 'H'; break; // March
                  case 4: month_code = 'J'; break; // April
                  case 5: month_code = 'K'; break; // May
                  case 6: month_code = 'M'; break; // June
                  case 7: month_code = 'N'; break; // July
                  case 8: month_code = 'Q'; break; // August
                  case 9: month_code = 'U'; break; // September
                  case 10: month_code = 'V'; break; // October
                  case 11: month_code = 'X'; break; // November
                  case 12: month_code = 'Z'; break; // December
              }
              int year_digit = year % 10;
              return ib_symbol + month_code + std::to_string(year_digit);
          }
           
           // For quarterly contracts, find the next contract month
           std::vector<int> month_codes;
           if (contract_months.find("MAR") != std::string::npos) month_codes.push_back(3);
           if (contract_months.find("JUN") != std::string::npos) month_codes.push_back(6);
           if (contract_months.find("SEP") != std::string::npos) month_codes.push_back(9);
           if (contract_months.find("DEC") != std::string::npos) month_codes.push_back(12);
           if (contract_months.find("JAN") != std::string::npos && month_codes.empty()) month_codes.push_back(1);
           if (contract_months.find("FEB") != std::string::npos) month_codes.push_back(2);
           if (contract_months.find("APR") != std::string::npos) month_codes.push_back(4);
           if (contract_months.find("MAY") != std::string::npos) month_codes.push_back(5);
           if (contract_months.find("JULY") != std::string::npos) month_codes.push_back(7);
           if (contract_months.find("AUG") != std::string::npos) month_codes.push_back(8);
           if (contract_months.find("OCT") != std::string::npos) month_codes.push_back(10);
           if (contract_months.find("NOV") != std::string::npos) month_codes.push_back(11);
           
           if (month_codes.empty()) return "";
           
           int current_month = current_tm.tm_mon + 1;
           int current_year = current_tm.tm_year + 1900;
           
           std::sort(month_codes.begin(), month_codes.end());
          int next_month = -1, next_year = current_year;
           for (int m : month_codes) {
               if (m >= current_month) {
                   next_month = m;
                   break;
               }
           }
           if (next_month == -1) {
               next_month = month_codes[0];
               next_year++;
           }
          
          // compute_expiry already defined above

          // If current date is after expiry of the candidate month, advance to the next listed month
          std::tm expiry_candidate = compute_expiry(ib_symbol, next_year, next_month);
          if (std::difftime(std::mktime(&current_tm), std::mktime(&expiry_candidate)) > 0) {
              // advance to next contract in list
              auto it = std::find(month_codes.begin(), month_codes.end(), next_month);
              if (it != month_codes.end()) {
                  ++it;
                  if (it == month_codes.end()) { it = month_codes.begin(); next_year++; }
                  next_month = *it;
              }
          }

          char month_code = '\0';
           switch (next_month) {
               case 1: month_code = 'F'; break;
               case 2: month_code = 'G'; break;
               case 3: month_code = 'H'; break;
               case 4: month_code = 'J'; break;
               case 5: month_code = 'K'; break;
               case 6: month_code = 'M'; break;
               case 7: month_code = 'N'; break;
               case 8: month_code = 'Q'; break;
               case 9: month_code = 'U'; break;
               case 10: month_code = 'V'; break;
               case 11: month_code = 'X'; break;
               case 12: month_code = 'Z'; break;
           }
           
          int year_digit = next_year % 10;
          return ib_symbol + month_code + std::to_string(year_digit);
       };

       // Render
      html << "<div class=\"summary-stats\" style=\"margin-top:6px;\">"
              "<strong>Month Codes:</strong> F=Jan, G=Feb, H=Mar, J=Apr, K=May, M=Jun, N=Jul, Q=Aug, U=Sep, V=Oct, X=Nov, Z=Dec"
              "</div>\n";
      html << "<table>\n";
      html << "<tr><th>Databento Symbol</th><th>IB Symbol</th>"
              "<th>Name</th><th>Contract Months</th><th>Front Month</th></tr>\n";


       // Track matched symbols to detect gaps
       std::set<std::string> matched;


       // helper to uppercase once, reuse
       auto up = [](std::string s){
           std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
           return s;
       };


       // --- HARD-CODED ROWS: always render first ---
       struct Row { std::string db, ib, name, months; };
       const std::vector<Row> hardcoded = {
           {"NQ", "NQ", "E-mini Nasdaq - 100 Index", "MAR, JUN, SEP, DEC"},
           {"YM", "YM", "E-mini Dow Jones Industrial Average Index", "MAR, JUN, SEP, DEC"}
       };


       for (const auto& r : hardcoded) {
           std::string front_month = get_front_month_symbol(r.ib, r.months, date);
           html << "<tr>\n"
               << "<td>" << r.db     << "</td>\n"
               << "<td>" << r.ib     << "</td>\n"
               << "<td>" << r.name   << "</td>\n"
               << "<td>" << r.months << "</td>\n"
               << "<td>" << front_month << "</td>\n"
               << "</tr>\n";
           matched.insert(up(r.db));
           matched.insert(up(r.ib));
       }


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


           std::string front_month = get_front_month_symbol(ib_sym, months, date);
           html << "<tr>\n";
           html << "<td>" << db_sym << "</td>\n";
           html << "<td>" << ib_sym << "</td>\n";
           html << "<td>" << name   << "</td>\n";
           html << "<td>" << months << "</td>\n";
           html << "<td>" << front_month << "</td>\n";
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

std::string EmailSender::format_rollover_warning(
    const std::unordered_map<std::string, Position>& positions,
    const std::string& date,
    std::shared_ptr<DatabaseInterface> db,
    const std::string& date_override_for_testing) {

    std::ostringstream html;

    // Use override date for testing if provided, otherwise use actual date
    std::string effective_date = date_override_for_testing.empty() ? date : date_override_for_testing;

    // Log if using test date override
    if (!date_override_for_testing.empty()) {
        INFO("TESTING MODE: Using date override for rollover warning: " + date_override_for_testing +
             " (actual date: " + date + ")");
    }

    // Parse the date string to get current date
    std::tm current_tm = {};
    std::istringstream ss(effective_date);
    ss >> std::get_time(&current_tm, "%Y-%m-%d");
    if (ss.fail()) {
        WARN("Failed to parse date for rollover warning: " + effective_date);
        return std::string();
    }

    // Helper: Calculate nth weekday of month (e.g., 3rd Friday)
    auto get_nth_weekday = [](int year, int month, int weekday, int n) -> std::tm {
        std::tm result = {};
        result.tm_year = year - 1900;
        result.tm_mon = month - 1;
        result.tm_mday = 1;
        std::mktime(&result); // Normalize

        // Find first occurrence of target weekday
        int first_weekday = result.tm_wday;
        int days_until_target = (weekday - first_weekday + 7) % 7;
        result.tm_mday = 1 + days_until_target + (n - 1) * 7;
        std::mktime(&result); // Normalize
        return result;
    };

    // Helper: Check if date is business day (not Sat/Sun)
    auto is_business_day = [](const std::tm& tm) -> bool {
        return tm.tm_wday != 0 && tm.tm_wday != 6;
    };

    // Helper: Get previous business day
    auto get_previous_business_day = [&is_business_day](std::tm tm) -> std::tm {
        do {
            tm.tm_mday--;
            std::mktime(&tm); // Normalize
        } while (!is_business_day(tm));
        return tm;
    };

    // Helper: Get next business day
    auto get_next_business_day = [&is_business_day](std::tm tm) -> std::tm {
        do {
            tm.tm_mday++;
            std::mktime(&tm); // Normalize
        } while (!is_business_day(tm));
        return tm;
    };

    // Helper: Get last day of month
    auto get_last_day_of_month = [](int year, int month) -> std::tm {
        std::tm result = {};
        result.tm_year = year - 1900;
        result.tm_mon = month; // Next month
        result.tm_mday = 0; // Day 0 = last day of previous month
        std::mktime(&result);
        return result;
    };

    // Helper: Get nth business day of month
    auto get_nth_business_day = [&is_business_day, &get_next_business_day](int year, int month, int n) -> std::tm {
        std::tm result = {};
        result.tm_year = year - 1900;
        result.tm_mon = month - 1;
        result.tm_mday = 0; // Will increment to first day

        int count = 0;
        while (count < n) {
            result = get_next_business_day(result);
            count++;
        }
        return result;
    };

    // Helper: Calculate days between two dates
    auto days_between = [](const std::tm& from, const std::tm& to) -> int {
        std::time_t from_time = std::mktime(const_cast<std::tm*>(&from));
        std::time_t to_time = std::mktime(const_cast<std::tm*>(&to));
        return static_cast<int>((to_time - from_time) / (60 * 60 * 24));
    };

    // Helper: Convert month and year to IB contract format (e.g., 12, 2025 -> "Z5")
    auto month_year_to_contract = [](int month, int year) -> std::string {
        char month_code = '\0';
        switch (month) {
            case 1: month_code = 'F'; break;  // January
            case 2: month_code = 'G'; break;  // February
            case 3: month_code = 'H'; break;  // March
            case 4: month_code = 'J'; break;  // April
            case 5: month_code = 'K'; break;  // May
            case 6: month_code = 'M'; break;  // June
            case 7: month_code = 'N'; break;  // July
            case 8: month_code = 'Q'; break;  // August
            case 9: month_code = 'U'; break;  // September
            case 10: month_code = 'V'; break; // October
            case 11: month_code = 'X'; break; // November
            case 12: month_code = 'Z'; break; // December
        }
        int year_digit = year % 10;
        return std::string(1, month_code) + std::to_string(year_digit);
    };

    // Get contract month info from database
    std::set<std::string> active_symbols;
    for (const auto& [symbol, pos] : positions) {
        if (pos.quantity.as_double() != 0.0) {
            std::string base_sym = symbol;
            auto dotpos = base_sym.find(".v.");
            if (dotpos != std::string::npos) base_sym = base_sym.substr(0, dotpos);
            dotpos = base_sym.find(".c.");
            if (dotpos != std::string::npos) base_sym = base_sym.substr(0, dotpos);
            active_symbols.insert(base_sym);
        }
    }

    if (active_symbols.empty()) return std::string();

    // Build SQL query - also fetch IB Symbol for display
    std::ostringstream sql;
    sql << "SELECT \"Databento Symbol\", \"IB Symbol\", \"Contract Months\" FROM metadata.contract_metadata WHERE \"Databento Symbol\" IN (";
    bool first = true;
    for (const auto& sym : active_symbols) {
        if (!first) sql << ", ";
        sql << "'" << sym << "'";
        first = false;
    }
    sql << ")";

    try {
        auto query_result = db->execute_query(sql.str());
        if (query_result.is_error()) {
            WARN("Failed to query contract metadata for rollover warning: " + query_result.error()->to_string());
            return std::string();
        }

        auto table = query_result.value();
        if (!table || table->num_rows() == 0) return std::string();

        auto combined_result = table->CombineChunks();
        if (!combined_result.ok()) return std::string();
        auto combined = combined_result.ValueOrDie();

        // Find column indices
        int idx_symbol = -1, idx_ib_symbol = -1, idx_months = -1;
        for (int i = 0; i < combined->schema()->num_fields(); ++i) {
            std::string field_name = combined->schema()->field(i)->name();
            if (field_name == "Databento Symbol") idx_symbol = i;
            if (field_name == "IB Symbol") idx_ib_symbol = i;
            if (field_name == "Contract Months") idx_months = i;
        }

        if (idx_symbol < 0 || idx_ib_symbol < 0 || idx_months < 0) return std::string();

        // Create mapping from Databento symbol to IB symbol for display
        std::unordered_map<std::string, std::string> databento_to_ib;

        // Unified rollover info: (ib_symbol, current_front_month, next_front_month)
        std::vector<std::tuple<std::string, std::string, std::string>> rollover_info;

        // Process each contract
        for (int64_t i = 0; i < combined->num_rows(); ++i) {
            auto symbol_arr = combined->column(idx_symbol)->chunk(0);
            auto ib_symbol_arr = combined->column(idx_ib_symbol)->chunk(0);
            auto months_arr = combined->column(idx_months)->chunk(0);

            if (symbol_arr->IsNull(i) || ib_symbol_arr->IsNull(i) || months_arr->IsNull(i)) continue;

            std::string symbol = std::static_pointer_cast<arrow::StringArray>(symbol_arr)->GetString(i);
            std::string ib_symbol = std::static_pointer_cast<arrow::StringArray>(ib_symbol_arr)->GetString(i);
            std::string contract_months = std::static_pointer_cast<arrow::StringArray>(months_arr)->GetString(i);

            // Store mapping for display later
            databento_to_ib[symbol] = ib_symbol;

            // Check for monthly contracts
            if (contract_months.find("All Months") != std::string::npos ||
                contract_months.find("consecutive") != std::string::npos) {
                for (const auto& [pos_symbol, pos] : positions) {
                    if (pos.quantity.as_double() != 0.0 && pos_symbol.find(symbol) == 0) {
                        // Compute this month's expiry using symbol-specific rules
                        int disp_month = current_tm.tm_mon + 1;
                        int disp_year  = current_tm.tm_year + 1900;
                        std::tm expiry_date = {};
                        // Reuse the same expiry mapping as below defaults
                        // Equity index style default: 3rd Friday; Metals/Agriculture/FX handled below in quarterly branch, but here monthly energy defaults apply as 3rd Friday
                        // We'll use a simplified mapping consistent with compute_expiry behavior in get_front_month_symbol
                        auto is_business_day_local = [](const std::tm& tm) { return tm.tm_wday != 0 && tm.tm_wday != 6; };
                        auto get_previous_business_day_local = [&is_business_day_local](std::tm tm) { do { tm.tm_mday--; std::mktime(&tm); } while (!is_business_day_local(tm)); return tm; };
                        auto get_last_day_of_month_local = [](int year, int month){ std::tm r{}; r.tm_year=year-1900; r.tm_mon=month; r.tm_mday=0; std::mktime(&r); return r; };
                        auto get_nth_weekday_local = [](int year,int month,int weekday,int n){ std::tm r{}; r.tm_year=year-1900; r.tm_mon=month-1; r.tm_mday=1; std::mktime(&r); int first=r.tm_wday; int delta=(weekday-first+7)%7; r.tm_mday=1+delta+(n-1)*7; std::mktime(&r); return r; };
                        auto get_next_business_day_local = [&is_business_day_local](std::tm tm) { do { tm.tm_mday++; std::mktime(&tm); } while (!is_business_day_local(tm)); return tm; };
                        auto get_nth_business_day_local = [&get_next_business_day_local](int year,int month,int n){ std::tm r{}; r.tm_year=year-1900; r.tm_mon=month-1; r.tm_mday=0; int c=0; while(c<n){ r=get_next_business_day_local(r); c++; } return r; };

                        if (symbol == "GC" || symbol == "PL" || symbol == "SI") {
                            expiry_date = get_last_day_of_month_local(disp_year, disp_month);
                            expiry_date = get_previous_business_day_local(expiry_date);
                            expiry_date = get_previous_business_day_local(expiry_date);
                            expiry_date = get_previous_business_day_local(expiry_date);
                        } else if (symbol == "HE") {
                            expiry_date = get_nth_business_day_local(disp_year, disp_month, 10);
                        } else if (symbol == "LE") {
                            expiry_date = get_last_day_of_month_local(disp_year, disp_month);
                            if (!is_business_day_local(expiry_date)) expiry_date = get_previous_business_day_local(expiry_date);
                        } else if (symbol == "GF") {
                            expiry_date = get_last_day_of_month_local(disp_year, disp_month);
                            while (expiry_date.tm_wday != 4) { expiry_date.tm_mday--; std::mktime(&expiry_date); }
                        } else if (symbol == "ZN" || symbol == "UB") {
                            expiry_date = get_last_day_of_month_local(disp_year, disp_month);
                            if (!is_business_day_local(expiry_date)) expiry_date = get_previous_business_day_local(expiry_date);
                            for (int j = 0; j < 7; ++j) expiry_date = get_previous_business_day_local(expiry_date);
                        } else if (symbol == "6B" || symbol == "6E" || symbol == "6J" || symbol == "6M" || symbol == "6N" || symbol == "6S") {
                            expiry_date = get_nth_weekday_local(disp_year, disp_month, 3, 3);
                            expiry_date = get_previous_business_day_local(expiry_date);
                            expiry_date = get_previous_business_day_local(expiry_date);
                        } else if (symbol == "6C") {
                            expiry_date = get_nth_weekday_local(disp_year, disp_month, 3, 3);
                            expiry_date = get_previous_business_day_local(expiry_date);
                        } else {
                            // Default monthly (e.g., energy like CL, RB): 3rd Friday
                            expiry_date = get_nth_weekday_local(disp_year, disp_month, 5, 3);
                        }

                        int days_to_expiry = days_between(current_tm, expiry_date);
                        if (days_to_expiry > 0 && days_to_expiry <= 15) {
                            auto to_code = [](int month){ switch(month){case 1:return 'F';case 2:return 'G';case 3:return 'H';case 4:return 'J';case 5:return 'K';case 6:return 'M';case 7:return 'N';case 8:return 'Q';case 9:return 'U';case 10:return 'V';case 11:return 'X';default:return 'Z';} };
                            std::string current_front = ib_symbol + std::string(1, to_code(disp_month)) + std::to_string(disp_year % 10);
                            int next_m = disp_month + 1; int next_y = disp_year; if (next_m > 12){ next_m = 1; next_y++; }
                            std::string next_front = ib_symbol + std::string(1, to_code(next_m)) + std::to_string(next_y % 10);
                            rollover_info.push_back(std::make_tuple(ib_symbol, current_front, next_front));
                        }
                        break;
                    }
                }
                continue;
            }

            // Determine current contract month and expiry date
            int current_year = current_tm.tm_year + 1900;
            int current_month = current_tm.tm_mon + 1;

            // Parse contract months (MAR, JUN, SEP, DEC, etc.)
            std::vector<int> month_codes; // 1=JAN, 3=MAR, etc.
            if (contract_months.find("JAN") != std::string::npos) month_codes.push_back(1);
            if (contract_months.find("FEB") != std::string::npos) month_codes.push_back(2);
            if (contract_months.find("MAR") != std::string::npos) month_codes.push_back(3);
            if (contract_months.find("APR") != std::string::npos) month_codes.push_back(4);
            if (contract_months.find("MAY") != std::string::npos) month_codes.push_back(5);
            if (contract_months.find("JUN") != std::string::npos) month_codes.push_back(6);
            if (contract_months.find("JULY") != std::string::npos) month_codes.push_back(7);
            if (contract_months.find("AUG") != std::string::npos) month_codes.push_back(8);
            if (contract_months.find("SEP") != std::string::npos) month_codes.push_back(9);
            if (contract_months.find("OCT") != std::string::npos) month_codes.push_back(10);
            if (contract_months.find("NOV") != std::string::npos) month_codes.push_back(11);
            if (contract_months.find("DEC") != std::string::npos) month_codes.push_back(12);

            if (month_codes.empty()) continue;

            // Find next contract month (this is the candidate - may need to check expiry)
            std::sort(month_codes.begin(), month_codes.end());
            int next_month = -1, next_year = current_year;
            for (int m : month_codes) {
                if (m >= current_month) {
                    next_month = m;
                    break;
                }
            }
            if (next_month == -1) {
                next_month = month_codes[0];
                next_year++;
            }
            
            // Store the initially found month for potential rollover warning display
            int expiring_month = next_month;
            int expiring_year = next_year;

            // Calculate expiry date based on contract-specific rules
            std::tm expiry_date = {};

            // Equity index futures: 3rd Friday (MES, MYM, MNQ, RTY)
            if (symbol == "MES" || symbol == "MYM" || symbol == "MNQ" || symbol == "RTY") {
                expiry_date = get_nth_weekday(next_year, next_month, 5, 3); // Friday=5, 3rd occurrence
            }
            // Ag futures: Business day prior to 15th (ZC, ZW, ZM, ZL, ZS, ZR, KE)
            else if (symbol == "ZC" || symbol == "ZW" || symbol == "ZM" || symbol == "ZL" ||
                     symbol == "ZS" || symbol == "ZR" || symbol == "KE") {
                expiry_date.tm_year = next_year - 1900;
                expiry_date.tm_mon = next_month - 1;
                expiry_date.tm_mday = 14;
                std::mktime(&expiry_date);
                if (!is_business_day(expiry_date)) {
                    expiry_date = get_previous_business_day(expiry_date);
                }
            }
            // Metals: 3rd last business day (GC, PL, SI)
            else if (symbol == "GC" || symbol == "PL" || symbol == "SI") {
                expiry_date = get_last_day_of_month(next_year, next_month);
                expiry_date = get_previous_business_day(expiry_date);
                expiry_date = get_previous_business_day(expiry_date);
                expiry_date = get_previous_business_day(expiry_date);
            }
            // FX: 2 business days prior to 3rd Wednesday (6B, 6E, 6J, 6M, 6N, 6S)
            else if (symbol == "6B" || symbol == "6E" || symbol == "6J" ||
                     symbol == "6M" || symbol == "6N" || symbol == "6S") {
                expiry_date = get_nth_weekday(next_year, next_month, 3, 3); // Wednesday=3, 3rd occurrence
                expiry_date = get_previous_business_day(expiry_date);
                expiry_date = get_previous_business_day(expiry_date);
            }
            // CAD: 1 business day prior to 3rd Wednesday
            else if (symbol == "6C") {
                expiry_date = get_nth_weekday(next_year, next_month, 3, 3);
                expiry_date = get_previous_business_day(expiry_date);
            }
            // Treasuries: 7 business days prior to last business day (ZN, UB)
            else if (symbol == "ZN" || symbol == "UB") {
                expiry_date = get_last_day_of_month(next_year, next_month);
                if (!is_business_day(expiry_date)) {
                    expiry_date = get_previous_business_day(expiry_date);
                }
                for (int j = 0; j < 7; ++j) {
                    expiry_date = get_previous_business_day(expiry_date);
                }
            }
            // Lean Hog: 10th business day (HE)
            else if (symbol == "HE") {
                expiry_date = get_nth_business_day(next_year, next_month, 10);
            }
            // Live Cattle: Last business day (LE)
            else if (symbol == "LE") {
                expiry_date = get_last_day_of_month(next_year, next_month);
                if (!is_business_day(expiry_date)) {
                    expiry_date = get_previous_business_day(expiry_date);
                }
            }
            // Feeder Cattle: Last Thursday (GF)
            else if (symbol == "GF") {
                expiry_date = get_last_day_of_month(next_year, next_month);
                while (expiry_date.tm_wday != 4) { // Thursday=4
                    expiry_date.tm_mday--;
                    std::mktime(&expiry_date);
                }
            }
            else {
                // Default: Use 3rd Friday as conservative estimate
                expiry_date = get_nth_weekday(next_year, next_month, 5, 3);
            }

            // Calculate days to expiry
            int days_to_expiry = days_between(current_tm, expiry_date);

            // Check if within 15-day rollover window
            if (days_to_expiry > 0 && days_to_expiry <= 15) {
                // Check if we're holding this contract, if yes calculate and store contract info
                for (const auto& [pos_symbol, pos] : positions) {
                    if (pos.quantity.as_double() != 0.0 && pos_symbol.find(symbol) == 0) {
                        // Current front month: the contract that's expiring (use expiring_month, not next_month)
                        std::string current_contract = ib_symbol + month_year_to_contract(expiring_month, expiring_year);
                        
                        // Next front month: the contract after expiring_month in the cycle
                        int next_contract_month = -1, next_contract_year = expiring_year;
                        auto it = std::find(month_codes.begin(), month_codes.end(), expiring_month);
                        if (it != month_codes.end()) {
                            ++it;
                            if (it == month_codes.end()) {
                                // Wrap to first month of next year
                                next_contract_month = month_codes[0];
                                next_contract_year++;
                            } else {
                                next_contract_month = *it;
                            }
                        } else {
                            // Fallback: should not happen, but use next month in list
                            next_contract_month = month_codes[0];
                            if (expiring_month >= month_codes.back()) {
                                next_contract_year++;
                            }
                        }
                        
                        std::string next_contract = ib_symbol + month_year_to_contract(next_contract_month, next_contract_year);
                        
                        rollover_info.push_back(std::make_tuple(ib_symbol, current_contract, next_contract));
                        break;
                    }
                }
            }
        }

        // Generate unified warnings
        if (!rollover_info.empty()) {
            std::sort(rollover_info.begin(), rollover_info.end(),
                     [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
            html << "<div style=\"background-color: #fff5f5; border-left: 4px solid #dc2626; padding: 15px; margin: 20px 0; font-size: 13px; font-family: Arial, sans-serif;\">\n";
            html << "<p style=\"color: #991b1b; margin: 0 0 10px 0;\"><strong>Rollover Notice:</strong> ";
            html << "These securities contracts are approaching their rollover period. ";
            html << "Please consider rolling over to the next contract month unless you intend to take delivery.</p>\n";
            html << "<ul style=\"color: #991b1b; margin: 5px 0 0 20px; padding: 0;\">\n";
            for (const auto& info : rollover_info) {
                const std::string& ib_symbol = std::get<0>(info);
                const std::string& current_contract = std::get<1>(info);
                const std::string& next_contract = std::get<2>(info);
                html << "<li><strong>" << ib_symbol << ":</strong> Currently holding <strong>" << current_contract 
                     << "</strong> → Switch to <strong>" << next_contract << "</strong></li>\n";
            }
            html << "</ul>\n";
            html << "</div>\n";
        }

    } catch (const std::exception& e) {
        WARN("Exception in format_rollover_warning: " + std::string(e.what()));
        return std::string();
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

std::string EmailSender::format_single_strategy_table(
    const std::string& strategy_name,
    const std::unordered_map<std::string, Position>& positions,
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

    // Strategy sub-header with styled left border accent
    std::string display_name = format_strategy_display_name(strategy_name);
    html << "<h3 style=\"margin-top: 20px; margin-bottom: 10px; color: #333; "
         << "border-left: 4px solid #2c5aa0; padding-left: 12px;\">"
         << display_name << "</h3>\n";

    // Position table
    html << "<table>\n";
    html << "<tr><th>Symbol</th><th>Quantity</th><th>Market Price</th><th>Notional</th><th>% of Total</th></tr>\n";

    double total_notional = 0.0;
    double total_margin_posted = 0.0;
    int active_positions = 0;

    // First pass: calculate total notional for this strategy
    std::vector<std::tuple<std::string, double, double, double, double>> position_data;

    for (const auto& [symbol, position] : positions) {
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
                    ERROR("CRITICAL: Instrument " + lookup_sym + " not found in registry for email generation!");
                    throw std::runtime_error("Missing instrument in registry: " + lookup_sym);
                }

                contract_multiplier = instrument->get_multiplier();
                if (contract_multiplier <= 0) {
                    ERROR("CRITICAL: Invalid multiplier " + std::to_string(contract_multiplier) + " for " + lookup_sym);
                    throw std::runtime_error("Invalid multiplier for: " + lookup_sym);
                }

                double contracts_abs = std::abs(position.quantity.as_double());
                double initial_margin_per_contract = instrument->get_margin_requirement();
                if (initial_margin_per_contract <= 0) {
                    ERROR("CRITICAL: Invalid margin requirement " + std::to_string(initial_margin_per_contract) + " for " + lookup_sym);
                    throw std::runtime_error("Invalid margin requirement for: " + lookup_sym);
                }
                margin_for_position = contracts_abs * initial_margin_per_contract;
                total_margin_posted += margin_for_position;

            } catch (const std::exception& e) {
                ERROR("CRITICAL: Failed to get instrument data for " + position.symbol + ": " + e.what());
                throw;
            }

            notional = position.quantity.as_double() * position.average_price.as_double() * contract_multiplier;
            total_notional += std::abs(notional);

            double market_price = position.average_price.as_double();
            auto price_it = current_prices.find(symbol);
            if (price_it != current_prices.end()) {
                market_price = price_it->second;
            }

            position_data.push_back(std::make_tuple(symbol, position.quantity.as_double(), market_price, notional, margin_for_position));
        }
    }

    // Second pass: render rows with % of total (relative to this strategy's total)
    for (const auto& [symbol, qty, market_price, notional, margin] : position_data) {
        double pct_of_total = (total_notional > 0) ? (std::abs(notional) / total_notional * 100.0) : 0.0;

        html << "<tr>\n";
        html << "<td>" << symbol << "</td>\n";
        html << "<td>" << std::fixed << std::setprecision(0) << qty << "</td>\n";
        html << "<td>$" << std::fixed << std::setprecision(2) << market_price << "</td>\n";
        html << "<td>$" << format_with_commas(std::abs(notional)) << "</td>\n";
        html << "<td>" << std::fixed << std::setprecision(2) << pct_of_total << "%</td>\n";
        html << "</tr>\n";
    }

    html << "</table>\n";

    // Compact strategy-level summary
    html << "<div style=\"font-size: 13px; color: #666; margin: 8px 0 20px 0; padding-left: 16px;\">\n";
    html << "<strong>Positions:</strong> " << active_positions
         << " | <strong>Notional:</strong> $" << format_with_commas(total_notional)
         << " | <strong>Margin:</strong> $" << format_with_commas(total_margin_posted) << "\n";
    html << "</div>\n";

    return html.str();
}

std::string EmailSender::format_strategy_positions_tables(
    const StrategyPositionsMap& strategy_positions,
    const std::unordered_map<std::string, double>& current_prices,
    const std::map<std::string, double>& strategy_metrics) {

    std::ostringstream html;

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
        for (const auto& [symbol, position] : positions) {
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
                        double notional = position.quantity.as_double() * position.average_price.as_double() * contract_multiplier;
                        portfolio_total_notional += std::abs(notional);

                        double contracts_abs = std::abs(position.quantity.as_double());
                        double initial_margin = instrument->get_margin_requirement();
                        portfolio_total_margin += contracts_abs * initial_margin;
                    }
                } catch (...) {
                    // Already logged in format_single_strategy_table
                }
            }
        }
    }

    // Portfolio-wide summary
    html << "<div class=\"summary-stats\" style=\"margin-top: 20px; border-top: 2px solid #2c5aa0; padding-top: 15px;\">\n";
    html << "<h3 style=\"margin: 0 0 10px 0; color: #333;\">Portfolio Summary</h3>\n";
    html << "<div class=\"metric\"><strong>Active Positions:</strong> " << portfolio_total_positions << "</div>\n";

    // Add volatility if available in strategy metrics
    auto volatility_it = strategy_metrics.find("Volatility");
    if (volatility_it != strategy_metrics.end()) {
        html << "<div class=\"metric\"><strong>Volatility:</strong> " << std::fixed << std::setprecision(2)
             << volatility_it->second << "%</div>\n";
    }

    html << "<div class=\"metric\"><strong>Total Notional:</strong> $" << format_with_commas(portfolio_total_notional) << "</div>\n";
    html << "<div class=\"metric\"><strong>Total Margin Posted:</strong> $" << format_with_commas(portfolio_total_margin) << "</div>\n";
    html << "</div>\n";

    return html.str();
}

std::string EmailSender::format_single_strategy_executions_table(
    const std::string& strategy_name,
    const std::vector<ExecutionReport>& executions) {

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
         << "border-left: 4px solid #2c5aa0; padding-left: 12px;\">"
         << display_name << "</h3>\n";

    // Executions table
    html << "<table>\n";
    html << "<tr><th>Symbol</th><th>Side</th><th>Quantity</th><th>Price</th><th>Notional</th><th>Commission</th></tr>\n";

    double total_commission = 0.0;
    double total_notional_traded = 0.0;

    for (const auto& exec : executions) {
        double contract_multiplier = 1.0;

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
            } else {
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
        html << "<td>$" << format_with_commas(notional) << "</td>\n";
        html << "<td>$" << std::fixed << std::setprecision(2) << exec.commission.as_double() << "</td>\n";
        html << "</tr>\n";
    }

    html << "</table>\n";

    // Compact strategy-level summary
    html << "<div style=\"font-size: 13px; color: #666; margin: 8px 0 20px 0; padding-left: 16px;\">\n";
    html << "<strong>Trades:</strong> " << executions.size()
         << " | <strong>Notional:</strong> $" << format_with_commas(total_notional_traded)
         << " | <strong>Commissions:</strong> $" << format_with_commas(total_commission) << "\n";
    html << "</div>\n";

    return html.str();
}

std::string EmailSender::format_strategy_executions_tables(
    const StrategyExecutionsMap& strategy_executions) {

    std::ostringstream html;

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
    double portfolio_total_commission = 0.0;

    // Generate table for each strategy
    for (const auto& strategy_name : strategy_names) {
        const auto& executions = strategy_executions.at(strategy_name);

        if (executions.empty()) {
            continue;
        }

        html << format_single_strategy_executions_table(strategy_name, executions);

        // Accumulate portfolio totals
        portfolio_total_trades += executions.size();
        for (const auto& exec : executions) {
            double contract_multiplier = 1.0;
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
                }
            } catch (...) {}

            double notional = exec.filled_quantity.as_double() * exec.fill_price.as_double() * contract_multiplier;
            portfolio_total_notional += notional;
            portfolio_total_commission += exec.commission.as_double();
        }
    }

    // Portfolio-wide summary
    html << "<div class=\"summary-stats\" style=\"margin-top: 20px; border-top: 2px solid #2c5aa0; padding-top: 15px;\">\n";
    html << "<h3 style=\"margin: 0 0 10px 0; color: #333;\">Portfolio Summary</h3>\n";
    html << "<div class=\"metric\"><strong>Total Trades:</strong> " << portfolio_total_trades << "</div>\n";
    html << "<div class=\"metric\"><strong>Total Notional Traded:</strong> $" << format_with_commas(portfolio_total_notional) << "</div>\n";
    html << "<div class=\"metric\"><strong>Total Commissions:</strong> $" << format_with_commas(portfolio_total_commission) << "</div>\n";
    html << "</div>\n";

    return html.str();
}

std::string EmailSender::generate_trading_report_body(
   const StrategyPositionsMap& strategy_positions,
   const std::unordered_map<std::string, Position>& positions,
   const std::optional<RiskResult>& risk_metrics,
   const std::map<std::string, double>& strategy_metrics,
   const StrategyExecutionsMap& strategy_executions,
   const std::string& date,
   const std::string& portfolio_name,
   bool is_daily_strategy,
   const std::unordered_map<std::string, double>& current_prices,
   std::shared_ptr<DatabaseInterface> db,
   const StrategyPositionsMap& yesterday_strategy_positions,
   const std::unordered_map<std::string, double>& yesterday_close_prices,
   const std::unordered_map<std::string, double>& two_days_ago_close_prices,
   const std::map<std::string, double>& yesterday_daily_metrics)
{
   std::ostringstream html;

   // Parse the date to check day of week
   std::tm tm = {};
   std::istringstream ss(date);
   ss >> std::get_time(&tm, "%Y-%m-%d");
   std::mktime(&tm);
   int day_of_week = tm.tm_wday;  // 0=Sunday, 1=Monday, ..., 6=Saturday

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

       INFO("Is yesterday (" + yesterday_date_str + ") a holiday? " + std::string(is_yesterday_holiday ? "YES" : "NO"));

       if (is_yesterday_holiday) {
           yesterday_holiday_name = holiday_checker_.get_holiday_name(yesterday_date_str);
           INFO("Holiday name: " + yesterday_holiday_name);
       }
   } catch (...) {
       ERROR("Exception while calculating yesterday's date");
       yesterday_date_str = "Previous Day";
   }

   bool is_monday = (day_of_week == 1);
   bool is_sunday = (day_of_week == 0);

   bool show_yesterday_pnl = true;
   if (is_sunday || is_yesterday_holiday) {
       show_yesterday_pnl = false;
   }

   // Generate HTML header and styles
   html << "<!DOCTYPE html>\n";
   html << "<html>\n<head>\n";
   html << "<meta charset=\"UTF-8\" />\n";
   html << "<style>\n";
   html << "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f9f9f9; }\n";
   html << ".container { max-width: 1200px; margin: 0 auto; background-color: white; padding: 30px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n";
   html << "h1, h2, h3 { color: #333; font-family: Arial, sans-serif; }\n";
   html << "h1 { font-size: 24px; margin-bottom: 5px; }\n";
   html << "h2 { font-size: 20px; margin-top: 25px; margin-bottom: 10px; border-bottom: 2px solid #2c5aa0; padding-bottom: 5px; }\n";
   html << "h3 { font-size: 16px; margin-top: 20px; margin-bottom: 10px; }\n";
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
   html << ".alert-note { background-color: #fee2e2; border-left: 4px solid #dc2626; padding: 15px; margin: 20px 0; font-size: 13px; color: #991b1b; font-family: Arial, sans-serif; }\n";
   html << ".summary-stats { background-color: #fff5e6; padding: 15px; margin: 15px 0; border-radius: 5px; font-family: Arial, sans-serif; font-size: 14px; }\n";
   html << ".chart-container { margin: 20px 0; padding: 20px; background-color: #f8f9fa; border-radius: 8px; text-align: center; }\n";
   html << ".weekend-message { background-color: #e6f3ff; border-left: 4px solid #2c5aa0; padding: 20px; margin: 20px 0; font-size: 16px; }\n";
   html << "</style>\n";
   html << "</head>\n<body>\n";
   html << "<div class=\"container\">\n";

   // Header with logo and branding
   html << "<div class=\"header-section\">\n";
   html << "<img src=\"cid:algogators_logo\" alt=\"AlgoGators Logo\">\n";
   html << "<div class=\"header-text\">\n";
   html << "<span class=\"fund-branding\">AlgoGators</span><br>\n";
   html << "<h1>Daily Trading Report</h1>\n";
   html << "<div class=\"header-info\">" << date << " | " << format_strategy_display_name(portfolio_name) << "</div>\n";
   html << "</div>\n";
   html << "</div>\n";

   // Weekend/Holiday banners
   if (is_sunday) {
       html << "<div style=\"background: linear-gradient(135deg, #fef3c7 0%, #fde68a 100%); border: 2px solid #f59e0b; border-radius: 8px; padding: 20px 30px; margin: 20px 0 30px 0; box-shadow: 0 4px 6px rgba(0,0,0,0.1);\">\n";
       html << "<h2 style=\"margin: 0 0 10px 0; color: #92400e; font-size: 20px; border-bottom: 2px solid #92400e; padding-bottom: 8px; display: inline-block;\">Yesterday was Saturday</h2>\n";
       html << "<p style=\"margin: 15px 0 5px 0; color: #78350f; font-size: 15px; line-height: 1.6;\">The latest futures settlement prices are not available, as futures markets were closed yesterday (" << yesterday_date_str << ") due to it being a Saturday. The PnL for these contracts will be updated in the next report once settlement data is released.</p>\n";
       html << "<p style=\"margin: 5px 0 0 0; color: #92400e; font-weight: 600; font-size: 14px;\">Please continue to monitor your positions closely.</p>\n";
       html << "</div>\n";
   }
   else if (is_monday) {
       html << "<div style=\"background: linear-gradient(135deg, #fef3c7 0%, #fde68a 100%); border: 2px solid #f59e0b; border-radius: 8px; padding: 20px 30px; margin: 20px 0 30px 0; box-shadow: 0 4px 6px rgba(0,0,0,0.1);\">\n";
       html << "<h2 style=\"margin: 0 0 10px 0; color: #92400e; font-size: 20px; border-bottom: 2px solid #92400e; padding-bottom: 8px; display: inline-block;\">Yesterday was Sunday</h2>\n";
       html << "<p style=\"margin: 15px 0 5px 0; color: #78350f; font-size: 15px; line-height: 1.6;\">Agricultural futures settlement prices for Sunday (" << yesterday_date_str << ") are not yet available, as these contracts begin trading Sunday evening. The PnL for these contracts will be updated in the next report once settlement data is released.</p>\n";
       html << "<p style=\"margin: 5px 0 0 0; color: #92400e; font-weight: 600; font-size: 14px;\">Please monitor these positions closely.</p>\n";
       html << "</div>\n";
   }
   else if (is_yesterday_holiday) {
       html << "<div style=\"background: linear-gradient(135deg, #fef3c7 0%, #fde68a 100%); border: 2px solid #f59e0b; border-radius: 8px; padding: 20px 30px; margin: 20px 0 30px 0; box-shadow: 0 4px 6px rgba(0,0,0,0.1);\">\n";
       html << "<h2 style=\"margin: 0 0 10px 0; color: #92400e; font-size: 20px; border-bottom: 2px solid #92400e; padding-bottom: 8px; display: inline-block;\">Yesterday was " << yesterday_holiday_name << "</h2>\n";
       html << "<p style=\"margin: 15px 0 5px 0; color: #78350f; font-size: 15px; line-height: 1.6;\">The latest futures settlement prices are not available, as futures markets were closed yesterday (" << yesterday_date_str << ") due to a federal holiday. The PnL for these contracts will be updated in the next report once settlement data is released.</p>\n";
       html << "<p style=\"margin: 5px 0 0 0; color: #92400e; font-weight: 600; font-size: 14px;\">Please continue to monitor your positions closely.</p>\n";
       html << "</div>\n";
   }

   // Today's Positions - use per-strategy tables if strategy_positions is provided, otherwise fallback to single table
   html << "<h2>Today's Positions</h2>\n";

   if (!strategy_positions.empty()) {
       html << format_strategy_positions_tables(strategy_positions, current_prices, strategy_metrics);
   } else {
       html << format_positions_table(positions, is_daily_strategy, current_prices, strategy_metrics);
   }

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
   }

   // Yesterday's Finalized Positions (with actual PnL) - use per-strategy breakdown
   bool has_yesterday_positions = false;
   for (const auto& [_, positions_map] : yesterday_strategy_positions) {
       if (!positions_map.empty()) {
           has_yesterday_positions = true;
           break;
       }
   }
   if (show_yesterday_pnl && has_yesterday_positions && !yesterday_close_prices.empty() && !two_days_ago_close_prices.empty()) {
       html << format_yesterday_finalized_positions_table(
           yesterday_strategy_positions,
           two_days_ago_close_prices,
           yesterday_close_prices,
           db,
           yesterday_daily_metrics,
           yesterday_date_str
       );
   }
   else if (!show_yesterday_pnl) {
       // Add a red alert explaining no executions due to non-trading day
       html << "<div class=\"alert-note\">\n";
       if (is_sunday) {
           html << "<strong>No Executions:</strong> Since yesterday (Saturday) was not a trading day, no new market data is available. Positions remain unchanged from the previous trading day, and no executions were generated.\n";
       }
       else if (is_yesterday_holiday) {
           html << "<strong>No Executions:</strong> Since yesterday (" << yesterday_date_str << ") was a market holiday, no new market data is available. Positions remain unchanged from the previous trading day, and no executions were generated.\n";
       }
       html << "</div>\n";
       
       // Add a yellow note explaining why yesterday's PnL is not shown
       html << "<div class=\"footer-note\">\n";
       if (is_sunday) {
           html << "<strong>Note:</strong> Yesterday's PnL data is not available.\n";
       }
       else if (is_monday) {
           html << "<strong>Note:</strong> Yesterday's PnL data is not available for agricultural contracts.\n";
       }
       else if (is_yesterday_holiday) {
           html << "<strong>Note:</strong> Yesterday's PnL data is not available.\n";
       }
       html << "</div>\n";
   }

   // Strategy metrics
   if (!strategy_metrics.empty()) {
       html << "<div class=\"metrics-section\">\n";
       html << format_strategy_metrics(strategy_metrics);
       html << "</div>\n";
   }

   html << "<h2>Charts</h2>\n";
   if (db) {
       // Generate equity curve chart
       chart_base64_ = ChartGenerator::generate_equity_curve_chart(db, "LIVE_TREND_FOLLOWING", 30);
       if (!chart_base64_.empty()) {
           html << "<h3 style=\"margin-top: 20px; color: #333;\">Equity Curve</h3>\n";
           html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: center;\">\n";
           html << "<img src=\"cid:equity_chart\" alt=\"Portfolio Equity Curve\" style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
           html << "</div>\n";
       }

       // Generate PnL by symbol chart - ONLY if show_yesterday_pnl is true
       if (show_yesterday_pnl) {
           pnl_by_symbol_base64_ = ChartGenerator::generate_pnl_by_symbol_chart(db, "LIVE_TREND_FOLLOWING", date);
           if (!pnl_by_symbol_base64_.empty()) {
               html << "<h3 style=\"margin-top: 20px; color: #333;\">Yesterday's PnL by Symbol</h3>\n";
               html << "<div style=\"width: 100%; max-width: 800px; margin: 20px auto; text-align: center;\">\n";
               html << "<img src=\"cid:pnl_by_symbol\" alt=\"PnL by Symbol\" style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
               html << "</div>\n";
           }
       }

       // Generate daily PnL chart
       daily_pnl_base64_ = ChartGenerator::generate_daily_pnl_chart(db, "LIVE_TREND_FOLLOWING", date, 30);
       if (!daily_pnl_base64_.empty()) {
           html << "<h3 style=\"margin-top: 20px; color: #333;\">Daily PnL (Last 30 Days)</h3>\n";
           html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: center;\">\n";
           html << "<img src=\"cid:daily_pnl\" alt=\"Daily PnL\" style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
           html << "</div>\n";
       }

       total_commissions_base64_ = ChartGenerator::generate_total_commissions_chart(db, "LIVE_TREND_FOLLOWING", date);
       if (!total_commissions_base64_.empty()) {
           html << "<h3 style=\"margin-top: 20px; color: #333;\">Cost per $1M Traded (Efficiency Metric)</h3>\n";
           html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: center;\">\n";
           html << "<img src=\"cid:total_commissions\" alt=\"Cost per $1M Traded\" style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
           html << "</div>\n";
       }

       margin_posted_base64_ = ChartGenerator::generate_margin_posted_chart(db, "LIVE_TREND_FOLLOWING", date);
       if (!margin_posted_base64_.empty()) {
           html << "<h3 style=\"margin-top: 20px; color: #333;\">Margin Posted</h3>\n";
           html << "<div style=\"width: 100%; max-width: 1000px; margin: 20px auto; text-align: center;\">\n";
           html << "<img src=\"cid:margin_posted\" alt=\"Margin Posted\" style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
           html << "</div>\n";
       }

       portfolio_composition_base64_ = ChartGenerator::generate_portfolio_composition_chart(
           positions,
           current_prices,
           date
       );
       if (!portfolio_composition_base64_.empty()) {
           html << "<h3 style=\"margin-top: 20px; color: #333;\">Portfolio Composition</h3>\n";
           html << "<div style=\"width: 100%; max-width: 800px; margin: 20px auto; text-align: center;\">\n";
           html << "<img src=\"cid:portfolio_composition\" alt=\"Portfolio Composition\" style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
           html << "</div>\n";
       }

       cumulative_pnl_by_symbol_base64_ = ChartGenerator::generate_cumulative_pnl_by_symbol_chart(
           db,
           "LIVE_TREND_FOLLOWING",
           date
       );
       if (!cumulative_pnl_by_symbol_base64_.empty()) {
           html << "<h3 style=\"margin-top: 20px; color: #333;\">Cumulative PnL by Symbol (All-Time)</h3>\n";
           html << "<div style=\"width: 100%; max-width: 800px; margin: 20px auto; text-align: center;\">\n";
           html << "<img src=\"cid:cumulative_pnl_by_symbol\" alt=\"Cumulative PnL by Symbol\" style=\"max-width: 100%; height: auto; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1);\" />\n";
           html << "</div>\n";
       }
   }

   // Symbols Reference
   html << "<h2>Symbols Reference</h2>\n";
   if (db) {
       try {
           html << format_symbols_table_for_positions(positions, db, yesterday_date_str);
       } catch (const std::exception& e) {
           html << "<p>Error loading symbols data: " << e.what() << "</p>\n";
       }
   } else {
       html << "<p>Database unavailable; symbols reference not included.</p>\n";
   }

   // Rollover Warning (if applicable)
   if (db) {
       const char* test_date_env = std::getenv("ROLLOVER_TEST_DATE");
       std::string test_date = test_date_env ? std::string(test_date_env) : "";
       html << format_rollover_warning(positions, date, db, test_date);
   }

   // Footer note
   if (is_daily_strategy) {
       html << "<div class=\"footer-note\">\n";
       html << "<strong>Note:</strong> This strategy is based on daily OHLCV data. We currently only provide data for the front-month contract.<br><br>\n";
       html << "All values reflect a trading start date of October 5th, 2025.<br><br>\n";
       html << "The ES, NQ, and YM positions are micro contracts (MES, MNQ, and MYM), not the standard mini or full-size contracts. All values reflect this accurately, and this is only a mismatch in representation, which we are currently working on fixing.\n";
       html << "</div>\n";
   }

   html << "<hr style=\"margin-top: 30px; border: none; border-top: 1px solid #ddd;\">\n";
   html << "<p style=\"text-align: center; color: #999; font-size: 12px; margin-top: 20px; font-family: Arial, sans-serif;\">Generated by AlgoGator's Trade-ngin</p>\n";
   html << "</div>\n";
   html << "</body>\n</html>\n";

   return html.str();
}

} //namespace trade ngin
