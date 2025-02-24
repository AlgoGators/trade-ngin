#include "ibkr_wrapper.hpp"
#include <spdlog/spdlog.h>

namespace ibkr {

IBKRWrapper::IBKRWrapper() {
    spdlog::info("Initializing IBKRWrapper");
}

void IBKRWrapper::error(int id, int errorCode, const std::string& errorString, 
                       const std::string& advancedOrderRejectJson) {
    spdlog::error("IBKR Error {}: {} (Code: {})", id, errorString, errorCode);
    if (!advancedOrderRejectJson.empty()) {
        spdlog::error("Advanced reject info: {}", advancedOrderRejectJson);
    }
}

void IBKRWrapper::connectionClosed() {
    spdlog::warn("Connection to IBKR closed");
}

void IBKRWrapper::currentTime(long time) {
    spdlog::debug("IBKR server time: {}", time);
}

void IBKRWrapper::nextValidId(OrderId orderId) {
    spdlog::info("Next valid order ID: {}", orderId);
}

void IBKRWrapper::tickPrice(TickerId tickerId, ::TickType field, double price, 
                          const ::TickAttrib& attrib) {
    if (!marketDataCb_) return;

    MarketDataUpdate update;
    update.tickType = convertTickType(field);
    update.price = price;
    update.size = 0;
    marketDataCb_(update);
}

void IBKRWrapper::tickSize(TickerId tickerId, ::TickType field, ::Decimal size) {
    if (!marketDataCb_) return;

    MarketDataUpdate update;
    update.tickType = convertTickType(field);
    update.price = 0.0;
    update.size = size.value();
    marketDataCb_(update);
}

void IBKRWrapper::marketDataType(TickerId reqId, int marketDataType) {
    spdlog::debug("Market data type changed for request {}: {}", reqId, marketDataType);
}

void IBKRWrapper::updateAccountValue(const std::string& key, const std::string& val,
                                   const std::string& currency, const std::string& accountName) {
    if (!accountCb_) return;

    AccountUpdate update;
    update.key = key;
    update.value = val;
    update.currency = currency;
    update.accountName = accountName;
    accountCb_(update);
}

void IBKRWrapper::updatePortfolio(const ::Contract& contract, ::Decimal position,
                                double marketPrice, double marketValue, double averageCost,
                                double unrealizedPNL, double realizedPNL,
                                const std::string& accountName) {
    if (!positionCb_) return;

    PositionUpdate update;
    update.contract = convertContract(contract);
    update.position = position.value();
    update.marketPrice = marketPrice;
    update.marketValue = marketValue;
    update.averageCost = averageCost;
    update.unrealizedPNL = unrealizedPNL;
    update.realizedPNL = realizedPNL;
    update.accountName = accountName;
    positionCb_(update);
}

void IBKRWrapper::accountDownloadEnd(const std::string& accountName) {
    spdlog::debug("Account download completed for {}", accountName);
}

void IBKRWrapper::openOrder(OrderId orderId, const ::Contract& contract,
                          const ::Order& order, const ::OrderState& orderState) {
    if (!orderCb_) return;

    OrderUpdate update;
    update.orderId = orderId;
    update.contract = convertContract(contract);
    update.order = convertOrder(order);
    update.status = orderState.status;
    orderCb_(update);
}

void IBKRWrapper::orderStatus(OrderId orderId, const std::string& status,
                            ::Decimal filled, ::Decimal remaining,
                            double avgFillPrice, int permId, int parentId,
                            double lastFillPrice, int clientId,
                            const std::string& whyHeld, double mktCapPrice) {
    if (!orderCb_) return;

    OrderUpdate update;
    update.orderId = orderId;
    update.status = status;
    update.filled = filled.value();
    update.remaining = remaining.value();
    update.avgFillPrice = avgFillPrice;
    update.lastFillPrice = lastFillPrice;
    orderCb_(update);
}

void IBKRWrapper::openOrderEnd() {
    spdlog::debug("Open orders download completed");
}

void IBKRWrapper::position(const std::string& account, const ::Contract& contract,
                         ::Decimal pos, double avgCost) {
    if (!positionCb_) return;

    PositionUpdate update;
    update.contract = convertContract(contract);
    update.position = pos.value();
    update.averageCost = avgCost;
    update.accountName = account;
    positionCb_(update);
}

void IBKRWrapper::positionEnd() {
    spdlog::debug("Position download completed");
}

TickType IBKRWrapper::convertTickType(::TickType type) {
    switch (type) {
        case ::TickType::BID:
            return TickType::BID;
        case ::TickType::ASK:
            return TickType::ASK;
        case ::TickType::LAST:
            return TickType::LAST;
        case ::TickType::HIGH:
            return TickType::HIGH;
        case ::TickType::LOW:
            return TickType::LOW;
        case ::TickType::VOLUME:
            return TickType::VOLUME;
        default:
            return TickType::UNKNOWN;
    }
}

Contract IBKRWrapper::convertContract(const ::Contract& contract) {
    Contract result;
    result.symbol = contract.symbol;
    result.secType = contract.secType;
    result.currency = contract.currency;
    result.exchange = contract.exchange;
    result.primaryExchange = contract.primaryExch;
    result.localSymbol = contract.localSymbol;
    result.multiplier = contract.multiplier;
    result.strike = contract.strike;
    result.right = contract.right;
    result.lastTradeDateOrContractMonth = contract.lastTradeDateOrContractMonth;
    result.includeExpired = contract.includeExpired;
    return result;
}

Order IBKRWrapper::convertOrder(const ::Order& order) {
    Order result;
    result.orderId = order.orderId;
    result.clientId = order.clientId;
    result.permId = order.permId;
    result.action = order.action;
    result.totalQuantity = order.totalQuantity;
    result.orderType = order.orderType;
    result.lmtPrice = order.lmtPrice;
    result.auxPrice = order.auxPrice;
    return result;
}

} // namespace ibkr
