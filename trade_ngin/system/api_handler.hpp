#pragma once
#include <string>

class ApiHandler {
public:
    ApiHandler(const std::string& host, int port, int client_id)
        : host_(host), port_(port), client_id_(client_id) {}
    
    void cancel_outstanding_orders() {
        // Implementation
    }

private:
    std::string host_;
    int port_;
    int client_id_;
}; 