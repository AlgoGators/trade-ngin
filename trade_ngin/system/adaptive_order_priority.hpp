#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <functional>
#include <unordered_map>
#include <utility>

// Mock configuration of mocking into IB
static const char* LOCALHOST = "127.0.0.1";
static const int PORT = 7497;
static const int CLIENT_ID = 0;

// AdaptiveOrderPriority enum
enum class AdaptiveOrderPriority {
    NORMAL,
    URGENT,
    SLOW
};

inline std::string to_string(AdaptiveOrderPriority p) {
    switch(p) {
        case AdaptiveOrderPriority::NORMAL: return "NORMAL";
        case AdaptiveOrderPriority::URGENT: return "URGENT";
        case AdaptiveOrderPriority::SLOW:   return "SLOW";
    }
    throw std::runtime_error("Unknown AdaptiveOrderPriority");
}

// Contract class
class Contract {
public:
    Contract(std::string symbol, std::string multiplier, std::string exchange, 
             std::string currency, std::string secType)
    : symbol_(std::move(symbol)), multiplier_(std::move(multiplier)), exchange_(std::move(exchange)),
      currency_(std::move(currency)), secType_(std::move(secType)) {}

    const std::string& symbol() const { return symbol_; }
    const std::string& multiplier() const { return multiplier_; }
    const std::string& exchange() const { return exchange_; }
    const std::string& currency() const { return currency_; }
    const std::string& secType() const { return secType_; }

private:
    std::string symbol_;
    std::string multiplier_;
    std::string exchange_;
    std::string currency_;
    std::string secType_;
};

// Position class
class Position {
public:
    Position(Contract contract, std::string quantity)
        : contract_(std::move(contract)), quantity_(std::move(quantity)) {}

    const Contract& contract() const { return contract_; }
    const std::string& quantity() const { return quantity_; }

    double quantity_as_double() const {
        return std::stod(quantity_);
    }

private:
    Contract contract_;
    std::string quantity_; 
};

// Account class
class Account {
public:
    Account() = default;
    Account(std::vector<Position> positions) : positions_(std::move(positions)) {}

    const std::vector<Position>& positions() const { return positions_; }

    Account operator-(const Account& other) const {
        std::unordered_map<std::string, std::pair<double, Contract>> other_map;
        for (auto &pos : other.positions_) {
            other_map[pos.contract().symbol()] = {pos.quantity_as_double(), pos.contract()};
        }

        std::vector<Position> result_positions;

        for (auto &pos : positions_) {
            double target_qty = pos.quantity_as_double();
            double current_qty = 0.0;
            Contract c = pos.contract();
            auto it = other_map.find(c.symbol());
            if (it != other_map.end()) {
                current_qty = it->second.first;
                it->second.first = 0.0;
            }
            double diff = target_qty - current_qty;
            if (diff != 0.0) {
                result_positions.emplace_back(c, std::to_string(diff));
            }
        }

        // Close out any leftover positions in other
        for (auto &kv : other_map) {
            double remaining = kv.second.first;
            if (remaining != 0.0) {
                double diff = -remaining;
                result_positions.emplace_back(kv.second.second, std::to_string(diff));
            }
        }

        return Account(std::move(result_positions));
    }

private:
    std::vector<Position> positions_;
};

// TradingAlgorithm class
class TradingAlgorithm {
public:
    TradingAlgorithm(AdaptiveOrderPriority priority) : priority_(priority) {}

    std::function<void(const std::vector<Position>&)> adaptive_market_order() {
        return [this](const std::vector<Position>& trades) {
            for (auto &pos : trades) {
                std::cout << "[AdaptiveMarketOrder-" << to_string(priority_) << "] "
                          << "Contract: " << pos.contract().symbol()
                          << " Qty: " << pos.quantity()
                          << "\n";
            }
        };
    }

private:
    AdaptiveOrderPriority priority_;
};

// ApiHandler class
class ApiHandler {
public:
    ApiHandler(const std::string& host, int port, int client_id) 
        : host_(host), port_(port), client_id_(client_id) {
        std::cout << "Connecting to IB at " << host << ":" << port << " with client_id=" << client_id << "\n";
    }

    ~ApiHandler() {
        std::cout << "Disconnecting from IB\n";
    }

    void cancel_outstanding_orders() {
        std::cout << "Cancelling outstanding orders...\n";
    }

    Account current_positions() {
        // Mock: return empty account
        return Account();
    }

    void initialize_desired_contracts(const Account& account, int some_val) {
        (void)account; (void)some_val;
        std::cout << "Initializing desired contracts...\n";
    }

    void place_orders(const Account& trades, std::function<void(const std::vector<Position>&)> order_fn) {
        order_fn(trades.positions());
    }

private:
    std::string host_;
    int port_;
    int client_id_;
};

// ApiHandlerContextGuard to simulate context manager behavior
class ApiHandlerContextGuard {
public:
    ApiHandlerContextGuard(const std::string& host, int port, int client_id)
        : handler_(host, port, client_id) {}

    ApiHandler& operator()() {
        return handler_;
    }

private:
    ApiHandler handler_;
};

// update_account function
void update_account(Account account, AdaptiveOrderPriority order_priority) {
    ApiHandlerContextGuard ctx(LOCALHOST, PORT, CLIENT_ID);
    ApiHandler& api_handler = ctx();

    api_handler.cancel_outstanding_orders();

    Account held_account = api_handler.current_positions();

    api_handler.initialize_desired_contracts(account, 5);

    Account trades = account - held_account;

    TradingAlgorithm algo(order_priority);
    api_handler.place_orders(trades, algo.adaptive_market_order());
}

int main() {
    std::vector<Position> new_positions = {
        Position(Contract("MES", "5", "CME", "USD", "FUT"), "2"),
        Position(Contract("MYM", "0.5", "CBOT", "USD", "FUT"), "-2")
    };
    Account new_account(new_positions);

    update_account(new_account, AdaptiveOrderPriority::NORMAL);

    return 0;
}
