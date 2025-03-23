#include "mock_ibkr_server.hpp"
#include <random>
#include <sstream>
#include <boost/beast/http.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace http = boost::beast::http;

MockIBKRServer::MockIBKRServer(int port) 
    : port(port), running(false) {
    logger = spdlog::get("mock_ibkr_server");
    if (!logger) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        logger = std::make_shared<spdlog::logger>("mock_ibkr_server", console_sink);
    }
}

MockIBKRServer::~MockIBKRServer() {
    if (running) {
        stop();
    }
}

void MockIBKRServer::start() {
    if (running) return;

    try {
        acceptor = std::make_unique<tcp::acceptor>(
            io_context, 
            tcp::endpoint(tcp::v4(), port)
        );
        running = true;
        startAccept();
        
        // Run the io_context in a separate thread
        std::thread([this]() {
            io_context.run();
        }).detach();

        logger->info("Mock IBKR server started on port {}", port);
    } catch (const std::exception& e) {
        logger->error("Failed to start server: {}", e.what());
        throw;
    }
}

void MockIBKRServer::stop() {
    if (!running) return;

    try {
        io_context.stop();
        acceptor->close();
        running = false;
        logger->info("Mock IBKR server stopped");
    } catch (const std::exception& e) {
        logger->error("Error stopping server: {}", e.what());
    }
}

void MockIBKRServer::startAccept() {
    auto socket = std::make_shared<tcp::socket>(io_context);
    acceptor->async_accept(*socket,
        [this, socket](const boost::system::error_code& error) {
            if (!error) {
                handleConnection(socket);
            }
            startAccept();
        });
}

void MockIBKRServer::handleConnection(std::shared_ptr<tcp::socket> socket) {
    auto buffer = std::make_shared<boost::beast::flat_buffer>();
    auto parser = std::make_shared<http::request_parser<http::string_body>>();
    
    http::async_read(*socket, *buffer, *parser,
        [this, socket, buffer, parser](const boost::system::error_code& error, std::size_t) {
            if (!error) {
                const auto& request = parser->get();
                
                logger->debug("Received {} request for {}", std::string(request.method_string()), std::string(request.target()));
                logger->debug("Request headers:");
                for(auto const& field : request) {
                    logger->debug("  {}: {}", std::string(field.name_string()), std::string(field.value()));
                }
                logger->debug("Request body: {}", request.body());
                
                json request_json;
                
                if (!request.body().empty()) {
                    try {
                        request_json = json::parse(request.body());
                        logger->debug("Parsed request body: {}", request_json.dump());
                    } catch (const json::parse_error& e) {
                        logger->error("Failed to parse request body: {}", e.what());
                    }
                }
                
                request_json["endpoint"] = std::string(request.target());
                request_json["method"] = std::string(request.method_string());
                
                std::string response;
                processRequest(request_json, response);
                
                logger->debug("Sending response: {}", response);
                
                http::response<http::string_body> http_response;
                http_response.version(request.version());
                http_response.result(http::status::ok);
                http_response.set(http::field::server, "MockIBKRServer");
                http_response.set(http::field::content_type, "application/json");
                http_response.set(http::field::access_control_allow_origin, "*");
                http_response.set(http::field::access_control_allow_methods, "GET, POST, PUT, DELETE");
                http_response.set(http::field::access_control_allow_headers, "Content-Type");
                http_response.body() = response;
                http_response.prepare_payload();
                
                http::async_write(*socket, http_response,
                    [socket](const boost::system::error_code& error, std::size_t) {
                        if (!error) {
                            socket->shutdown(tcp::socket::shutdown_send);
                        }
                    });
            }
        });
}

void MockIBKRServer::processRequest(const json& request, std::string& response) {
    logger->info("Processing request: {}", request.dump());
    
    std::string endpoint = request["endpoint"];
    std::string method = request["method"];
    
    logger->info("Endpoint: {}, Method: {}", endpoint, method);
    
    json response_json;
    
    if (endpoint == "/iserver/authenticate") {
        handleAuthentication(request, response_json);
    } else if (endpoint == "/iserver/auth/status") {
        handleAuthStatus(request, response_json);
    } else if (endpoint == "/iserver/marketdata/snapshot") {
        handleMarketData(request, response_json);
    } else if (endpoint == "/iserver/account") {
        handleAccountData(request, response_json);
    } else if (endpoint == "/iserver/account/positions") {
        response_json = positions_data;
        response_json["status"] = 200;
    } else {
        logger->warn("Unknown endpoint: {}", endpoint);
        response_json["error"] = "Unknown endpoint";
        response_json["status"] = 404;
    }
    
    response = response_json.dump();
    logger->info("Sending response: {}", response);
}

void MockIBKRServer::handleAuthentication(const json& request, json& response) {
    logger->info("Authentication request received: {}", request.dump());
    
    // Always authenticate for testing
    response["authenticated"] = true;
    response["status"] = 200;
    response["session_id"] = "test_session_123";
    response["message"] = "Authentication successful";
    
    logger->info("Sending authentication response: {}", response.dump());
}

void MockIBKRServer::handleAuthStatus(const json& request, json& response) {
    logger->info("Auth status request received: {}", request.dump());
    
    response["authenticated"] = true;
    response["status"] = 200;
    response["message"] = "Already authenticated";
    response["session_id"] = "test_session_123";
    
    logger->info("Sending auth status response: {}", response.dump());
}

void MockIBKRServer::handleMarketData(const json& request, json& response) {
    std::string symbol = request["symbol"];
    
    if (market_data.find(symbol) != market_data.end()) {
        response = market_data[symbol];
        response["status"] = 200;
    } else {
        response["error"] = "Symbol not found";
        response["status"] = 404;
    }
}

void MockIBKRServer::handleHistoricalData(const json& request, json& response) {
    std::string symbol = request["symbol"];
    
    if (historical_data.find(symbol) != historical_data.end()) {
        response = historical_data[symbol];
        response["status"] = 200;
    } else {
        response["error"] = "Historical data not found";
        response["status"] = 404;
    }
}

void MockIBKRServer::handleOrder(const json& request, json& response) {
    // Generate a random order ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(10000, 99999);
    
    json order = request;
    order["order_id"] = std::to_string(dis(gen));
    order["status"] = "submitted";
    order["timestamp"] = std::chrono::system_clock::now().time_since_epoch().count();
    
    order_history.push_back(order);
    
    response = order;
    response["status"] = 200;
    response["message"] = "Order submitted successfully";
}

void MockIBKRServer::handleAccountData(const json& request, json& response) {
    response = account_data;
    response["status"] = 200;
}

std::string MockIBKRServer::generateSessionId() {
    boost::uuids::random_generator gen;
    boost::uuids::uuid uuid = gen();
    return boost::uuids::to_string(uuid);
}

void MockIBKRServer::setMarketData(const std::string& symbol, const json& data) {
    market_data[symbol] = data;
}

void MockIBKRServer::setHistoricalData(const std::string& symbol, const json& data) {
    historical_data[symbol] = data;
}

void MockIBKRServer::setAccountData(const json& data) {
    account_data = data;
}

void MockIBKRServer::setPositions(const json& data) {
    positions_data = data;
}

bool MockIBKRServer::isRunning() const {
    return running;
}
