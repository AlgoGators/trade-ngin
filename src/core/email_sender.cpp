#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/logger.hpp"
#include <curl/curl.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>

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
        
        // Create email payload
        std::string content_type = is_html ? "text/html" : "text/plain";
        g_email_payload = 
            "From: " + config_.from_email + "\r\n"
            "To: " + config_.to_emails[0] + "\r\n"
            "Subject: " + subject + "\r\n"
            "Content-Type: " + content_type + "\r\n"
            "\r\n"
            + body;
        
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
    const std::string& date) {
    
    std::ostringstream html;
    
    html << "<!DOCTYPE html>\n";
    html << "<html>\n<head>\n";
    html << "<style>\n";
    html << "body { font-family: Arial, sans-serif; margin: 20px; }\n";
    html << "h1, h2 { color: #333; }\n";
    html << "table { border-collapse: collapse; width: 100%; margin: 10px 0; }\n";
    html << "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }\n";
    html << "th { background-color: #f2f2f2; }\n";
    html << ".metric { margin: 10px 0; }\n";
    html << ".positive { color: #1a7f37; }\n"; // professional green
    html << ".negative { color: #b42318; }\n"; // professional red
    html << ".neutral { color: #0b6efd; }\n";  // blue for zero
    html << ".header-info { color: #666; font-size: 14px; margin-bottom: 20px; }\n";
    html << ".fund-branding { color: #2c5aa0; font-weight: bold; }\n";
    html << "</style>\n";
    html << "</head>\n<body>\n";
    
    html << "<h1>Daily Trading Report - " << date << "</h1>\n";
    html << "<div class=\"header-info\">\n";
    html << "<span class=\"fund-branding\">AlgoGators</span> - Trend Following Strategy<br>\n";
    html << "Strategy: LIVE_TREND_FOLLOWING<br>\n";
    html << "</div>\n";
    
    // Position summary
    html << "<h2>Position Summary</h2>\n";
    html << format_positions_table(positions);
    
    // Executions for the day
    if (!executions.empty()) {
        html << "<h2>Daily Executions</h2>\n";
        html << format_executions_table(executions);
    }
    
    // Risk metrics (excluding duplicates that will be in strategy metrics)
    if (risk_metrics.has_value()) {
        html << "<h2>Risk Metrics</h2>\n";
        html << format_risk_metrics(risk_metrics.value());
    }
    
    // Strategy metrics (consolidated, no duplicates)
    if (!strategy_metrics.empty()) {
        html << "<h2>Strategy Performance</h2>\n";
        html << format_strategy_metrics(strategy_metrics);
    }
    
    html << "<hr>\n";
    html << "<p><small>Generated by Trade-ngin Live Pipeline for <span class=\"fund-branding\">AlgogAtors</span></small></p>\n";
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

std::string EmailSender::format_positions_table(const std::unordered_map<std::string, Position>& positions) {
    std::ostringstream html;
    
    html << "<table>\n";
    html << "<tr><th>Symbol</th><th>Quantity</th><th>Avg Price</th><th>Market Price</th><th>Notional</th><th>Total P&L</th></tr>\n";
    
    double total_notional = 0.0;
    double total_margin_posted = 0.0;
    int active_positions = 0;
    
    for (const auto& [symbol, position] : positions) {
        if (position.quantity.as_double() != 0.0) {
            active_positions++;
            double notional = position.quantity.as_double() * position.average_price.as_double();
            total_notional += std::abs(notional);
            
            // Estimate margin as 10% of notional value for futures
            double margin_estimate = std::abs(notional) * 0.10;
            total_margin_posted += margin_estimate;
            
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
            html << "<td>$" << std::fixed << std::setprecision(2) << position.average_price.as_double() << "</td>\n";
            html << "<td>$" << std::fixed << std::setprecision(2) << market_price << "</td>\n";
            html << "<td>$" << std::fixed << std::setprecision(0) << std::abs(notional) << "</td>\n";
            html << "<td class=\"" << pnl_class << "\">$" << std::fixed << std::setprecision(2) 
                 << total_pnl << "</td>\n";
            html << "</tr>\n";
        }
    }
    
    html << "</table>\n";
    html << "<div class=\"metric\">\n";
    html << "<strong>Active Positions:</strong> " << active_positions << "<br>\n";
    html << "<strong>Total Notional:</strong> $" << std::fixed << std::setprecision(0) << total_notional << "<br>\n";
    html << "<strong>Total Margin Posted:</strong> $" << std::fixed << std::setprecision(0) << total_margin_posted << "\n";
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
    
    html << "<div class=\"metric\">\n";
    for (const auto& [key, value] : strategy_metrics) {
        std::string formatted_value;
        
        // Add appropriate units based on the metric name with proper decimal formatting
        if (key.find("P&L") != std::string::npos || key.find("Portfolio Value") != std::string::npos || key.find("Notional") != std::string::npos) {
            std::ostringstream oss;
            oss << "$" << std::fixed << std::setprecision(2) << value;
            formatted_value = oss.str();
        } else if (key.find("Return") != std::string::npos || key.find("Volatility") != std::string::npos) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << value << "%";
            formatted_value = oss.str();
        } else if (key.find("Leverage") != std::string::npos) {
            // Display leverage in 0.53x format, not as percentage
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << value << "x";
            formatted_value = oss.str();
        } else if (key.find("Positions") != std::string::npos) {
            // Count values don't need units
            formatted_value = std::to_string(static_cast<int>(std::round(value)));
        } else {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << value;
            formatted_value = oss.str();
        }
        
        // Color code P&L and Return values
        std::string value_class = "";
        bool colorize = (key.find("P&L") != std::string::npos) || (key.find("Return") != std::string::npos);
        if (colorize) {
            std::string cls;
            if (std::fabs(value) < 1e-9) {
                cls = "neutral";
            } else if (value > 0) {
                cls = "positive";
            } else {
                cls = "negative";
            }
            value_class = " class=\"" + cls + "\"";
        }
        
        html << "<strong>" << key << ":</strong> <span" << value_class << ">" << formatted_value << "</span><br>\n";
    }
    html << "</div>\n";
    
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
        html << "<td>$" << std::fixed << std::setprecision(0) << notional << "</td>\n";
        html << "<td>$" << std::fixed << std::setprecision(2) << exec.commission.as_double() << "</td>\n";
        html << "</tr>\n";
    }
    
    html << "</table>\n";
    html << "<div class=\"metric\">\n";
    html << "<strong>Total Trades:</strong> " << executions.size() << "<br>\n";
    html << "<strong>Total Notional Traded:</strong> $" << std::fixed << std::setprecision(0) << total_notional_traded << "<br>\n";
    html << "<strong>Total Commission:</strong> $" << std::fixed << std::setprecision(2) << total_commission << "\n";
    html << "</div>\n";
    
    return html.str();
}

} // namespace trade_ngin
