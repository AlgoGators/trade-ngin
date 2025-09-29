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

Result<void> EmailSender::send_email(const std::string& subject, const std::string& body, bool is_html) {
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

        // Create multipart email payload with embedded image
        std::string boundary = "----=_Part_0_" + std::to_string(std::time(nullptr));
        std::string content_type = is_html ? "text/html; charset=UTF-8" : "text/plain";

        g_email_payload =
            "From: " + config_.from_email + "\r\n"
            "To: " + config_.to_emails[0] + "\r\n"
            "Subject: " + subject + "\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: multipart/related; boundary=\"" + boundary + "\"\r\n"
            "\r\n"
            "--" + boundary + "\r\n"
            "Content-Type: " + content_type + "\r\n"
            "Content-Transfer-Encoding: 7bit\r\n"
            "\r\n"
            + body + "\r\n";

        if (!logo_base64.empty()) {
            g_email_payload +=
                "\r\n--" + boundary + "\r\n"
                "Content-Type: image/png\r\n"
                "Content-Transfer-Encoding: base64\r\n"
                "Content-ID: <algogators_logo>\r\n"
                "Content-Disposition: inline; filename=\"Algo.png\"\r\n"
                "\r\n"
                + logo_base64 + "\r\n";
        }

        g_email_payload += "\r\n--" + boundary + "--\r\n";

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
    bool is_daily_strategy) {

    std::ostringstream html;

    html << "<!DOCTYPE html>\n";
    html << "<html>\n<head>\n";
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

    // Position summary
    html << "<h2>Positions</h2>\n";
    html << format_positions_table(positions, is_daily_strategy);

    // Executions for the day
    if (!executions.empty()) {
        html << "<h2>Daily Executions</h2>\n";
        html << format_executions_table(executions);
    }

    // Strategy metrics organized by category
    if (!strategy_metrics.empty() || risk_metrics.has_value()) {
        html << "<div class=\"metrics-section\">\n";
        html << format_strategy_metrics(strategy_metrics);
        html << "</div>\n";
    }

    // Footer notes
    if (is_daily_strategy) {
        html << "<div class=\"footer-note\">\n";
        html << "<strong>Note:</strong> This strategy is based on daily OHLCV data. For execution, it assumes that all orders are filled at the day's opening price, meaning the average execution price is treated as equal to the daily market open.\n";
        html << "</div>\n";
    }

    html << "<hr style=\"margin-top: 30px; border: none; border-top: 1px solid #ddd;\">\n";
    html << "<p style=\"text-align: center; color: #999; font-size: 12px; margin-top: 20px; font-family: Arial, sans-serif;\">Generated by AlgoGator's Trade-ngin</p>\n";
    html << "</div>\n";
    html << "</body>\n</html>\n";

    return html.str();
}

std::string EmailSender::generate_trading_report_from_db(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    const std::string& date) {
    
    // This would query the trading.results table for the given strategy and date
    // For now, return a placeholder
    std::ostringstream html;
    
    html << "<!DOCTYPE html>\n";
    html << "<html>\n<head>\n";
    html << "<style>\n";
    html << "body { font-family: Arial, sans-serif; margin: 20px; }\n";
    html << "h1, h2 { color: #333; }\n";
    html << "table { border-collapse: collapse; width: 100%; margin: 10px 0; }\n";
    html << "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }\n";
    html << "th { background-color: #f2f2f2; }\n";
    html << "</style>\n";
    html << "</head>\n<body>\n";
    
    html << "<h1>Daily Trading Report - " << date << "</h1>\n";
    html << "<h2>Strategy: " << strategy_id << "</h2>\n";
    
    // TODO: Query trading.results table and format the data
    html << "<p>Results from trading.results table will be displayed here.</p>\n";
    
    html << "<hr>\n";
    html << "<p><small>Generated by Trade-ngin Live Pipeline</small></p>\n";
    html << "</body>\n</html>\n";
    
    return html.str();
}

std::string EmailSender::format_positions_table(const std::unordered_map<std::string, Position>& positions, bool is_daily_strategy) {
    std::ostringstream html;

    html << "<table>\n";
    html << "<tr><th>Symbol</th><th>Quantity</th>";

    // Conditionally show columns based on strategy type
    if (is_daily_strategy) {
        html << "<th>Market Price</th>";
    } else {
        html << "<th>Average Price</th><th>Market Price</th>";
    }

    html << "<th>Notional</th><th>Total P&L</th></tr>\n";

    double total_notional = 0.0;
    double total_margin_posted = 0.0;
    int active_positions = 0;

    for (const auto& [symbol, position] : positions) {
        if (position.quantity.as_double() != 0.0) {
            active_positions++;
            double notional = position.quantity.as_double() * position.average_price.as_double();
            total_notional += std::abs(notional);

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
                    double contracts_abs = std::abs(position.quantity.as_double());
                    double initial_margin_per_contract = instrument->get_margin_requirement();
                    total_margin_posted += contracts_abs * initial_margin_per_contract;
                }
            } catch (...) {
                // Fall back: keep previous estimate behavior if registry not available
                double margin_estimate = std::abs(notional) * 0.10;
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

            // Use average price as market price (could be enhanced with current market prices)
            double market_price = position.average_price.as_double();

            html << "<tr>\n";
            html << "<td>" << symbol << "</td>\n";
            html << "<td>" << std::fixed << std::setprecision(0) << position.quantity.as_double() << "</td>\n";

            // Conditionally show price columns
            if (is_daily_strategy) {
                html << "<td>$" << std::fixed << std::setprecision(2) << market_price << "</td>\n";
            } else {
                html << "<td>$" << std::fixed << std::setprecision(2) << position.average_price.as_double() << "</td>\n";
                html << "<td>$" << std::fixed << std::setprecision(2) << market_price << "</td>\n";
            }

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
        } else if (key.find("Margin") != std::string::npos ||
                   key.find("Cash") != std::string::npos || key.find("Cushion") != std::string::npos) {
            // Risk & Liquidity metrics - handle separately
            continue;
        }
        // Store everything else for performance metrics
        ordered_metrics[key] = value;
    }

    // Extract leverage metrics
    std::map<std::string, double> leverage_metrics;
    for (const auto& [key, value] : strategy_metrics) {
        if (key.find("Leverage") != std::string::npos) {
            leverage_metrics[key] = value;
        }
    }

    // Extract risk & liquidity metrics
    std::map<std::string, double> risk_liquidity_metrics;
    for (const auto& [key, value] : strategy_metrics) {
        if (key.find("Margin") != std::string::npos ||
            key.find("Cash") != std::string::npos || key.find("Cushion") != std::string::npos) {
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
            }
        }

        html << "</div>\n";
    }

    // Render Leverage Metrics Section with proper labels
    if (!leverage_metrics.empty()) {
        html << "<h2>Leverage Metrics</h2>\n";
        html << "<div class=\"metrics-category\">\n";

        // Define the proper order and labels for leverage metrics
        std::vector<std::pair<std::string, std::string>> leverage_order = {
            {"Gross Leverage", "Gross Leverage (Risk Management)"},
            {"Net Leverage", "Net Leverage (Risk Management)"},
            {"Portfolio Leverage (Gross)", "Portfolio Leverage"},
            {"Portfolio Leverage", "Portfolio Leverage"},
            {"Margin Leverage", "Implied Leverage from Margin"}
        };

        for (const auto& [search_key, display_label] : leverage_order) {
            auto it = leverage_metrics.find(search_key);
            if (it != leverage_metrics.end()) {
                html << format_metric(display_label, it->second);
            }
        }

        html << "</div>\n";
    }

    // Render Risk & Liquidity Metrics Section
    if (!risk_liquidity_metrics.empty()) {
        html << "<h2>Risk & Liquidity Metrics</h2>\n";
        html << "<div class=\"metrics-category\">\n";
        for (const auto& [key, value] : risk_liquidity_metrics) {
            html << format_metric(key, value);
        }
        html << "</div>\n";
    }

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
        double notional = exec.filled_quantity.as_double() * exec.fill_price.as_double();
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

    html << "<div class=\"summary-stats\">\n";
    html << "<strong>Trades:</strong> " << executions.size() << "<br>\n";
    html << "<strong>Notional Traded:</strong> $" << format_with_commas(total_notional_traded) << "<br>\n";
    html << "<strong>Commissions:</strong> $" << format_with_commas(total_commission) << "\n";
    html << "</div>\n";

    return html.str();
}

} // namespace trade_ngin
