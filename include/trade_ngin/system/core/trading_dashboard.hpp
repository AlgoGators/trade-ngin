#pragma once

#include <memory>
#include <thread>
#include <atomic>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include "performance_analytics.hpp"
#include "../data/database_client.hpp"
#include "ibkr_interface.hpp"

using json = nlohmann::json;
namespace asio = boost::asio;

class TradingDashboard {
public:
    struct DashboardConfig {
        int update_interval_ms;
        bool save_to_database;
        bool enable_alerts;
        bool enable_websocket;
        int websocket_port;
        std::string log_level;

        DashboardConfig()
            : update_interval_ms(1000)
            , save_to_database(true)
            , enable_alerts(true)
            , enable_websocket(true)
            , websocket_port(8081)
            , log_level("info")
        {}
    };

    TradingDashboard(
        std::shared_ptr<IBKRInterface> ibkr,
        std::shared_ptr<DatabaseClient> db,
        std::shared_ptr<PerformanceAnalytics> analytics,
        const DashboardConfig& config = DashboardConfig()
    );
    
    ~TradingDashboard();

    // Dashboard control
    void start();
    void stop();
    bool isRunning() const;

    // Real-time updates
    void updateMetrics();
    void broadcastUpdate();
    
    // Data access
    json getCurrentState() const;
    json getHistoricalMetrics() const;
    json getPositionDetails() const;
    json getRiskMetrics() const;

    // Alert configuration
    void setAlert(const std::string& metric, double threshold, 
                 std::function<void(const std::string&)> callback);
    void removeAlert(const std::string& metric);

private:
    // Components
    std::shared_ptr<IBKRInterface> ibkr_;
    std::shared_ptr<DatabaseClient> db_;
    std::shared_ptr<PerformanceAnalytics> analytics_;
    DashboardConfig config_;

    // Websocket server
    struct WebSocketServer {
        asio::io_context io_context;
        std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
        std::vector<std::shared_ptr<asio::ip::tcp::socket>> clients;
        std::mutex clients_mutex;
    };
    std::unique_ptr<WebSocketServer> ws_server_;

    // Threading
    std::atomic<bool> running_;
    std::unique_ptr<std::thread> update_thread_;
    std::unique_ptr<std::thread> websocket_thread_;

    // Alert system
    struct Alert {
        std::string metric;
        double threshold;
        std::function<void(const std::string&)> callback;
    };
    std::vector<Alert> alerts_;
    std::mutex alerts_mutex_;

    // Dashboard sections
    struct DashboardSection {
        virtual ~DashboardSection() = default;
        virtual json getData() const = 0;
        virtual void update(const json& data) = 0;
    };

    struct PortfolioSection : DashboardSection {
        double total_equity;
        double daily_pnl;
        double unrealized_pnl;
        std::vector<std::pair<std::string, double>> positions;
        
        json getData() const override;
        void update(const json& data) override;
    };

    struct RiskSection : DashboardSection {
        double var;
        double leverage;
        double exposure;
        std::vector<std::pair<std::string, double>> risk_allocation;
        
        json getData() const override;
        void update(const json& data) override;
    };

    struct PerformanceSection : DashboardSection {
        double sharpe_ratio;
        double sortino_ratio;
        double win_rate;
        std::vector<double> returns_distribution;
        
        json getData() const override;
        void update(const json& data) override;
    };

    struct TradeSection : DashboardSection {
        int trades_today;
        double avg_trade_pnl;
        std::vector<std::pair<std::string, double>> recent_trades;
        
        json getData() const override;
        void update(const json& data) override;
    };

    // Dashboard state
    std::unique_ptr<PortfolioSection> portfolio_;
    std::unique_ptr<RiskSection> risk_;
    std::unique_ptr<PerformanceSection> performance_;
    std::unique_ptr<TradeSection> trades_;

    // Helper methods
    void initializeWebSocket();
    void handleWebSocketConnection(std::shared_ptr<asio::ip::tcp::socket> socket);
    void checkAlerts(const json& metrics);
    void saveMetricsToDatabase(const json& metrics);
    void updateDashboardSections();
};
