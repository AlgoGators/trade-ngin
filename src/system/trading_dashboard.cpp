#include "trading_dashboard.hpp"
#include <spdlog/spdlog.h>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <iomanip>

namespace websocket = boost::beast::websocket;

TradingDashboard::TradingDashboard(
    std::shared_ptr<IBKRInterface> ibkr,
    std::shared_ptr<DatabaseClient> db,
    std::shared_ptr<PerformanceAnalytics> analytics,
    const DashboardConfig& config
) : ibkr_(ibkr), db_(db), analytics_(analytics), config_(config), running_(false) {
    portfolio_ = std::make_unique<PortfolioSection>();
    risk_ = std::make_unique<RiskSection>();
    performance_ = std::make_unique<PerformanceSection>();
    trades_ = std::make_unique<TradeSection>();

    if (config_.enable_websocket) {
        ws_server_ = std::make_unique<WebSocketServer>();
    }
}

TradingDashboard::~TradingDashboard() {
    if (running_) {
        stop();
    }
}

void TradingDashboard::start() {
    if (running_) return;
    running_ = true;

    // Start update thread
    update_thread_ = std::make_unique<std::thread>([this]() {
        while (running_) {
            try {
                updateMetrics();
                broadcastUpdate();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.update_interval_ms)
                );
            } catch (const std::exception& e) {
                spdlog::error("Error in dashboard update: {}", e.what());
            }
        }
    });

    // Start websocket server if enabled
    if (config_.enable_websocket) {
        initializeWebSocket();
    }

    spdlog::info("Trading dashboard started");
}

void TradingDashboard::stop() {
    if (!running_) return;
    running_ = false;

    if (update_thread_ && update_thread_->joinable()) {
        update_thread_->join();
    }

    if (websocket_thread_ && websocket_thread_->joinable()) {
        websocket_thread_->join();
    }

    spdlog::info("Trading dashboard stopped");
}

void TradingDashboard::updateMetrics() {
    try {
        // Get current metrics
        auto current_metrics = analytics_->getCurrentMetrics();
        auto historical_stats = analytics_->getHistoricalStats();

        // Convert metrics to JSON
        json metrics_json = {
            {"current_equity", current_metrics.current_equity},
            {"cash_balance", current_metrics.cash_balance},
            {"buying_power", current_metrics.buying_power},
            {"margin_used", current_metrics.margin_used},
            {"current_var", current_metrics.current_var},
            {"current_leverage", current_metrics.current_leverage},
            {"net_exposure", current_metrics.net_exposure},
            {"gross_exposure", current_metrics.gross_exposure},
            {"today", {
                {"trades_today", current_metrics.today.trades_today},
                {"today_pnl", current_metrics.today.today_pnl},
                {"today_turnover", current_metrics.today.today_turnover},
                {"today_fees", current_metrics.today.today_fees}
            }}
        };

        json historical_json = {
            {"sharpe_ratio", historical_stats.sharpe_ratio},
            {"sortino_ratio", historical_stats.sortino_ratio},
            {"win_rate", historical_stats.win_rate},
            {"total_return", historical_stats.total_return}
        };

        // Update portfolio section
        json portfolio_data = {
            {"total_equity", current_metrics.current_equity},
            {"cash_balance", current_metrics.cash_balance},
            {"buying_power", current_metrics.buying_power},
            {"margin_used", current_metrics.margin_used}
        };
        portfolio_->update(portfolio_data);

        // Update risk section
        json risk_data = {
            {"var", current_metrics.current_var},
            {"leverage", current_metrics.current_leverage},
            {"net_exposure", current_metrics.net_exposure},
            {"gross_exposure", current_metrics.gross_exposure}
        };
        risk_->update(risk_data);

        // Update performance section
        json performance_data = {
            {"sharpe_ratio", historical_stats.sharpe_ratio},
            {"sortino_ratio", historical_stats.sortino_ratio},
            {"win_rate", historical_stats.win_rate},
            {"total_return", historical_stats.total_return}
        };
        performance_->update(performance_data);

        // Update trade section
        json trade_data = {
            {"trades_today", current_metrics.today.trades_today},
            {"today_pnl", current_metrics.today.today_pnl},
            {"today_turnover", current_metrics.today.today_turnover}
        };
        trades_->update(trade_data);

        // Check alerts
        checkAlerts(metrics_json);

        // Save to database if enabled
        if (config_.save_to_database) {
            saveMetricsToDatabase(metrics_json);
        }

    } catch (const std::exception& e) {
        spdlog::error("Error updating metrics: {}", e.what());
    }
}

void TradingDashboard::broadcastUpdate() {
    if (!config_.enable_websocket) return;

    try {
        json update = {
            {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()},
            {"portfolio", portfolio_->getData()},
            {"risk", risk_->getData()},
            {"performance", performance_->getData()},
            {"trades", trades_->getData()}
        };

        std::string message = update.dump();
        std::lock_guard<std::mutex> lock(ws_server_->clients_mutex);

        for (auto it = ws_server_->clients.begin(); it != ws_server_->clients.end();) {
            try {
                websocket::stream<asio::ip::tcp::socket> ws(std::move(**it));
                ws.write(asio::buffer(message));
                ++it;
            } catch (const std::exception& e) {
                spdlog::warn("Error broadcasting to client: {}", e.what());
                it = ws_server_->clients.erase(it);
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("Error broadcasting update: {}", e.what());
    }
}

void TradingDashboard::initializeWebSocket() {
    try {
        ws_server_->acceptor = std::make_unique<asio::ip::tcp::acceptor>(
            ws_server_->io_context,
            asio::ip::tcp::endpoint(asio::ip::tcp::v4(), config_.websocket_port)
        );

        websocket_thread_ = std::make_unique<std::thread>([this]() {
            while (running_) {
                try {
                    auto socket = std::make_shared<asio::ip::tcp::socket>(
                        ws_server_->io_context
                    );
                    ws_server_->acceptor->accept(*socket);
                    handleWebSocketConnection(socket);
                } catch (const std::exception& e) {
                    spdlog::error("Websocket error: {}", e.what());
                }
            }
        });

        spdlog::info("Websocket server started on port {}", config_.websocket_port);
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize websocket server: {}", e.what());
    }
}

json TradingDashboard::getCurrentState() const {
    json state = {
        {"portfolio", portfolio_->getData()},
        {"risk", risk_->getData()},
        {"performance", performance_->getData()},
        {"trades", trades_->getData()},
        {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()}
    };
    return state;
}

json TradingDashboard::getHistoricalMetrics() const {
    try {
        auto historical_stats = analytics_->getHistoricalStats();
        return {
            {"sharpe_ratio", historical_stats.sharpe_ratio},
            {"sortino_ratio", historical_stats.sortino_ratio},
            {"win_rate", historical_stats.win_rate},
            {"total_return", historical_stats.total_return},
            {"max_drawdown", historical_stats.max_drawdown},
            {"max_drawdown_duration", historical_stats.max_drawdown_duration},
            {"profit_factor", historical_stats.profit_factor},
            {"monthly_returns", historical_stats.monthly_returns},
            {"daily_returns", historical_stats.daily_returns}
        };
    } catch (const std::exception& e) {
        spdlog::error("Error getting historical metrics: {}", e.what());
        return json::object();
    }
}

json TradingDashboard::getPositionDetails() const {
    try {
        auto current_metrics = analytics_->getCurrentMetrics();
        json positions = json::array();
        
        for (const auto& [symbol, metrics] : current_metrics.positions) {
            positions.push_back({
                {"symbol", symbol},
                {"quantity", metrics.quantity},
                {"avg_price", metrics.avg_price},
                {"current_price", metrics.current_price},
                {"unrealized_pnl", metrics.unrealized_pnl},
                {"realized_pnl", metrics.realized_pnl},
                {"position_var", metrics.position_var},
                {"position_beta", metrics.position_beta}
            });
        }
        
        return {
            {"positions", positions},
            {"total_exposure", current_metrics.gross_exposure},
            {"net_exposure", current_metrics.net_exposure}
        };
    } catch (const std::exception& e) {
        spdlog::error("Error getting position details: {}", e.what());
        return json::object();
    }
}

json TradingDashboard::getRiskMetrics() const {
    try {
        auto current_metrics = analytics_->getCurrentMetrics();
        auto historical_stats = analytics_->getHistoricalStats();
        
        return {
            {"var_95", historical_stats.var_95},
            {"cvar_95", historical_stats.cvar_95},
            {"current_var", current_metrics.current_var},
            {"portfolio_beta", historical_stats.portfolio_beta},
            {"rolling_var", historical_stats.rolling_var},
            {"rolling_sharpe", historical_stats.rolling_sharpe},
            {"leverage", current_metrics.current_leverage},
            {"exposure", current_metrics.gross_exposure}
        };
    } catch (const std::exception& e) {
        spdlog::error("Error getting risk metrics: {}", e.what());
        return json::object();
    }
}

void TradingDashboard::setAlert(const std::string& metric, double threshold, 
                               std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    
    // Remove existing alert for this metric if it exists
    removeAlert(metric);
    
    // Add new alert
    alerts_.push_back(Alert{metric, threshold, callback});
    spdlog::info("Alert set for {} with threshold {}", metric, threshold);
}

void TradingDashboard::removeAlert(const std::string& metric) {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    
    auto it = std::remove_if(alerts_.begin(), alerts_.end(),
        [&metric](const Alert& alert) { return alert.metric == metric; });
    
    if (it != alerts_.end()) {
        alerts_.erase(it, alerts_.end());
        spdlog::info("Alert removed for {}", metric);
    }
}

void TradingDashboard::handleWebSocketConnection(std::shared_ptr<asio::ip::tcp::socket> socket) {
    try {
        websocket::stream<asio::ip::tcp::socket> ws(std::move(*socket));
        ws.accept();

        std::lock_guard<std::mutex> lock(ws_server_->clients_mutex);
        ws_server_->clients.push_back(socket);

        // Send initial state
        json initial_state = getCurrentState();
        ws.write(asio::buffer(initial_state.dump()));

    } catch (const std::exception& e) {
        spdlog::error("Error handling websocket connection: {}", e.what());
    }
}

void TradingDashboard::checkAlerts(const json& metrics) {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    
    for (const auto& alert : alerts_) {
        try {
            if (metrics.contains(alert.metric)) {
                double value = metrics[alert.metric];
                if (value > alert.threshold) {
                    alert.callback(fmt::format(
                        "Alert: {} exceeded threshold {} (current: {})",
                        alert.metric, alert.threshold, value
                    ));
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("Error checking alert {}: {}", alert.metric, e.what());
        }
    }
}

void TradingDashboard::saveMetricsToDatabase(const json& metrics) {
    try {
        std::string timestamp = std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count()
        );

        std::string query = fmt::format(
            "INSERT INTO metrics_history (timestamp, metrics) VALUES ('{}', '{}')",
            timestamp, metrics.dump()
        );

        db_->executeQuery(query);
    } catch (const std::exception& e) {
        spdlog::error("Error saving metrics to database: {}", e.what());
    }
}

// Section implementations
json TradingDashboard::PortfolioSection::getData() const {
    return {
        {"total_equity", total_equity},
        {"daily_pnl", daily_pnl},
        {"unrealized_pnl", unrealized_pnl},
        {"positions", positions}
    };
}

void TradingDashboard::PortfolioSection::update(const json& data) {
    total_equity = data["total_equity"];
    daily_pnl = data["daily_pnl"];
    unrealized_pnl = data["unrealized_pnl"];
    positions.clear();
    for (const auto& pos : data["positions"]) {
        positions.emplace_back(pos["symbol"], pos["value"]);
    }
}

json TradingDashboard::RiskSection::getData() const {
    return {
        {"var", var},
        {"leverage", leverage},
        {"exposure", exposure},
        {"risk_allocation", risk_allocation}
    };
}

void TradingDashboard::RiskSection::update(const json& data) {
    var = data["var"];
    leverage = data["leverage"];
    exposure = data["exposure"];
    risk_allocation.clear();
    for (const auto& alloc : data["risk_allocation"]) {
        risk_allocation.emplace_back(alloc["symbol"], alloc["value"]);
    }
}

json TradingDashboard::PerformanceSection::getData() const {
    return {
        {"sharpe_ratio", sharpe_ratio},
        {"sortino_ratio", sortino_ratio},
        {"win_rate", win_rate},
        {"returns_distribution", returns_distribution}
    };
}

void TradingDashboard::PerformanceSection::update(const json& data) {
    sharpe_ratio = data["sharpe_ratio"];
    sortino_ratio = data["sortino_ratio"];
    win_rate = data["win_rate"];
    returns_distribution = data["returns_distribution"].get<std::vector<double>>();
}

json TradingDashboard::TradeSection::getData() const {
    return {
        {"trades_today", trades_today},
        {"avg_trade_pnl", avg_trade_pnl},
        {"recent_trades", recent_trades}
    };
}

void TradingDashboard::TradeSection::update(const json& data) {
    trades_today = data["trades_today"];
    avg_trade_pnl = data["avg_trade_pnl"];
    recent_trades.clear();
    for (const auto& trade : data["recent_trades"]) {
        recent_trades.emplace_back(trade["symbol"], trade["pnl"]);
    }
}
