#pragma once

#include <EWrapper.h>
#include <memory>
#include "ibkr/market_data_handler.hpp"
#include "ibkr/account_handler.hpp"

namespace ibkr {

class IBKRWrapper : public ::EWrapper {
public:
    IBKRWrapper();
    ~IBKRWrapper() override = default;

    // Set callbacks
    void setMarketDataCallback(MarketDataHandler::DataCallback callback) { marketDataCb_ = callback; }
    void setAccountCallback(AccountHandler::AccountUpdateCallback callback) { accountCb_ = callback; }
    void setPositionCallback(AccountHandler::PositionUpdateCallback callback) { positionCb_ = callback; }
    void setOrderCallback(AccountHandler::OrderUpdateCallback callback) { orderCb_ = callback; }

    // Connection and server responses
    void error(int id, int errorCode, const std::string& errorString, const std::string& advancedOrderRejectJson) override;
    void connectionClosed() override;
    void currentTime(long time) override;
    void nextValidId(::OrderId orderId) override;

    // Market Data
    void tickPrice(::TickerId tickerId, ::TickType field, double price, const ::TickAttrib& attrib) override;
    void tickSize(::TickerId tickerId, ::TickType field, ::Decimal size) override;
    void marketDataType(::TickerId reqId, int marketDataType) override;

    // Account and Portfolio
    void updateAccountValue(const std::string& key, const std::string& val, const std::string& currency, 
                          const std::string& accountName) override;
    void updatePortfolio(const ::Contract& contract, ::Decimal position, double marketPrice, 
                        double marketValue, double averageCost, double unrealizedPNL, 
                        double realizedPNL, const std::string& accountName) override;
    void accountDownloadEnd(const std::string& accountName) override;

    // Orders
    void openOrder(::OrderId orderId, const ::Contract& contract, const ::Order& order, 
                  const ::OrderState& orderState) override;
    void orderStatus(::OrderId orderId, const std::string& status, ::Decimal filled,
                    ::Decimal remaining, double avgFillPrice, int permId, int parentId,
                    double lastFillPrice, int clientId, const std::string& whyHeld,
                    double mktCapPrice) override;
    void openOrderEnd() override;

    // Positions
    void position(const std::string& account, const ::Contract& contract, 
                 ::Decimal pos, double avgCost) override;
    void positionEnd() override;

private:
    // Callbacks
    MarketDataHandler::DataCallback marketDataCb_;
    AccountHandler::AccountUpdateCallback accountCb_;
    AccountHandler::PositionUpdateCallback positionCb_;
    AccountHandler::OrderUpdateCallback orderCb_;
};

} // namespace ibkr
