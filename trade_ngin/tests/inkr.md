
# IBKR Paper Trading API Integration Design Document

This document outlines the complete design (with pseudo-code, detailed comments, and bullet-pointed steps) for integrating the IBKR Web API for paper trading into the existing system. The integration covers the full cycle of order execution (submission, modification, cancellation, status monitoring) and includes session management with automatic order reply suppression.

---

## 1. Overview & Objectives

- **Goal:**  
  Integrate IBKR web API paper trading into our trading system as a drop-in replacement for order execution. This integration will:
  - Process new orders, order modifications, cancellations, and monitor order status.
  - Automatically suppress common order reply messages (futures warnings) when the session starts.
  - Use our existing real-time market data from Databento (via PostgreSQL on our EC2 instance) for strategy execution.
  - Allow toggling between the mock IBKR interface and the real (paper trading) IBKR interface via a configuration option.
  - Log all API interactions verbosely for troubleshooting.

- **Paper Trading Focus:**  
  At this stage, target paper trading orders only. Live trading integration might be considered later as a toggle.

---

## 2. Architecture & Components

### A. IBKRInterface Abstraction
- **Existing Abstraction:**  
  There is already an IBKR interface abstraction (including a mock interface). We will add a new implementation (e.g., `RealIBKRInterface`) that uses IBKR’s Web API endpoints.
  
- **Key Methods to Implement:**
  - `initializeSession()`: Establish session, perform authentication, and automatically suppress order reply messages.
  - `submitOrder(order)`: Submit a new order using POST `/iserver/account/{accountId}/orders`.
  - `modifyOrder(orderId, updatedOrder)`: Modify an existing order using POST `/iserver/account/{accountId}/order/{orderId}`.
  - `cancelOrder(orderId)`: Cancel an order using DELETE `/iserver/account/{accountId}/order/{orderId}`.
  - `getOrderStatus(orderId)`: Query the status of a given order.
  - `confirmOrderReplyMessage(messageId)`: Confirm any order reply messages when required.
  - `suppressOrderReplyMessages(listOfMessageIds)`: Call the suppression endpoint `/iserver/questions/suppress` to automatically suppress common futures warning messages.

### B. Configuration Module
- Create a configuration wrapper that reads:
  - API endpoint URLs (base URL)
  - API credentials (test credentials for paper trading)
  - Mode (real vs. mock)
- **Implementation Options:**  
  Either use environment variables or a config file (whichever fits best with our system). There are no strict preferences, so choose the most systematic approach.

### C. HTTP Library
- **Library:**  
  Use the system’s existing libcurl integration for HTTP requests.
  
- **WebSocket:**  
  Not required for order execution in paper trading. (Since our data comes from Databento, we only use HTTP for IBKR API.)

### D. Logging
- All interactions with IBKR (requests, responses, errors) should use the system-wide Logger (spdlog) with verbose logging enabled.

---

## 3. Detailed Components & Pseudo-Code

### A. Session Management & Initialization

**Purpose:**  
Establish an authenticated session, storing tokens/cookies and automatically suppressing order reply messages (futures warnings).  
  
**Pseudo-code:**

```cpp
// IBKRInterface.hpp (new methods in our RealIBKRInterface)
class RealIBKRInterface : public IBKRInterface {
public:
    // Constructor accepts API endpoint and credentials from config
    RealIBKRInterface(const std::string& api_url, const std::string& account, const std::string& token);
    
    // Initialize session (authentication, suppression)
    bool initializeSession();
    
    // New Order methods – see section below
    json submitOrder(const Order& order);
    json modifyOrder(const std::string& orderId, const Order& updatedOrder);
    json cancelOrder(const std::string& orderId);
    json getOrderStatus(const std::string& orderId);
    json confirmOrderReplyMessage(const std::string& messageId);
    
private:
    std::string baseUrl;
    std::string accountId;
    std::string authToken; // might be OAuth token or similar
    // Additional session tokens or cookies if needed

    // Helper method to perform HTTP POST, GET, DELETE using libcurl
    json performHttpRequest(const std::string& method,
                            const std::string& endpoint,
                            const json& body = {});
    
    // Suppress common order reply messages on startup.
    bool suppressOrderReplyMessages(const std::vector<std::string>& messageIds);
};
```

**In `initializeSession()`:**

```cpp
bool RealIBKRInterface::initializeSession() {
    // 1. Perform authentication (if required) using authToken/credentials.
    //    Setup necessary headers using authToken.
    // 2. Automatically call suppression API.
    std::vector<std::string> commonMessageIds = { "o163" }; // add more if required
    bool suppressResult = suppressOrderReplyMessages(commonMessageIds);
    Logger::getInstance().info("Order reply messages suppressed: {}", suppressResult);
    
    return suppressResult; // or return true if other setup steps pass
}
```

**Suppress Order Reply Messages Implementation:**

```cpp
bool RealIBKRInterface::suppressOrderReplyMessages(const std::vector<std::string>& messageIds) {
    // Build JSON body: { "messageIds": [ "o163", ... ] }
    json body;
    body["messageIds"] = messageIds;

    std::string endpoint = "/iserver/questions/suppress";
    json response = performHttpRequest("POST", endpoint, body);
    
    // Check response for status "submitted"
    if (response.contains("status") && response["status"] == "submitted") {
      return true;
    }
    Logger::getInstance().error("Failed to suppress order reply messages: {}", response.dump());
    return false;
}
```

### B. Order Lifecycle Methods

Each method will use the helper method `performHttpRequest()`.  
  
#### 1. Submit New Order

- **Endpoint:** `/iserver/account/{accountId}/orders`  
- **HTTP Method:** POST  
- **Required Data:**  
  - `conid`
  - `side` ("BUY" or "SELL")
  - `orderType` ("LMT", "MKT", etc.)
  - `price`
  - `quantity`
  - `tif` ("DAY", etc.)  

**Pseudo-Code:**

```cpp
json RealIBKRInterface::submitOrder(const Order& order) {
    // Build JSON order ticket as an array with one object:
    json orderTicket = {
      { "conid", order.conid },
      { "side", order.side },
      { "orderType", order.orderType },
      { "price", order.price },
      { "quantity", order.quantity },
      { "tif", order.tif }
      // You may include additional fields as needed.
    };

    json body = json::array();
    body.push_back(orderTicket);

    std::string endpoint = "/iserver/account/" + accountId + "/orders";
    json response = performHttpRequest("POST", endpoint, body);
    
    Logger::getInstance().info("Submitted order: {}", response.dump());
    return response;
}
```

#### 2. Modify Order

- **Endpoint:** `/iserver/account/{accountId}/order/{orderId}`  
- **HTTP Method:** POST  
- **Note:** Include all original fields, only change the ones needed (e.g., price).

```cpp
json RealIBKRInterface::modifyOrder(const std::string& orderId, const Order& updatedOrder) {
    // Build modified order ticket – ensure all keys are same as original.
    json orderTicket = {
      { "conid", updatedOrder.conid },
      { "side", updatedOrder.side },
      { "orderType", updatedOrder.orderType },
      { "price", updatedOrder.price },  // Updated value (e.g., new limit price)
      { "quantity", updatedOrder.quantity },
      { "tif", updatedOrder.tif }
    };

    std::string endpoint = "/iserver/account/" + accountId + "/order/" + orderId;
    json response = performHttpRequest("POST", endpoint, orderTicket);
    
    Logger::getInstance().info("Modified order {}: {}", orderId, response.dump());
    return response;
}
```

#### 3. Cancel Order

- **Endpoint:** `/iserver/account/{accountId}/order/{orderId}`  
- **HTTP Method:** DELETE

```cpp
json RealIBKRInterface::cancelOrder(const std::string& orderId) {
    std::string endpoint = "/iserver/account/" + accountId + "/order/" + orderId;
    json response = performHttpRequest("DELETE", endpoint);
    
    Logger::getInstance().info("Canceled order {}: {}", orderId, response.dump());
    return response;
}
```

#### 4. Get Order Status

- **Endpoint:** Use appropriate endpoint (e.g., `/iserver/account/{accountId}/order/status/{orderId}`)
- **HTTP Method:** GET

```cpp
json RealIBKRInterface::getOrderStatus(const std::string& orderId) {
    std::string endpoint = "/iserver/account/" + accountId + "/order/status/" + orderId;
    json response = performHttpRequest("GET", endpoint);
    
    Logger::getInstance().info("Order status for {}: {}", orderId, response.dump());
    return response;
}
```

#### 5. Confirm Order Reply Message (if needed)

```cpp
json RealIBKRInterface::confirmOrderReplyMessage(const std::string& messageId) {
    std::string endpoint = "/iserver/reply/" + messageId;
    json body = { {"confirmed", true} };
    json response = performHttpRequest("POST", endpoint, body);
    
    Logger::getInstance().info("Confirmed order reply message {}: {}", messageId, response.dump());
    return response;
}
```

### C. Helper HTTP Request Method

This helper abstracts the HTTP calls (using libcurl). Pseudo-code below:

```cpp
json RealIBKRInterface::performHttpRequest(const std::string& method,
                                             const std::string& endpoint,
                                             const json& body) {
    // Pseudo-code using libcurl:
    // 1. Initialize CURL
    // 2. Set URL: baseUrl + endpoint
    // 3. Set HTTP method (GET, POST, DELETE)
    // 4. Set headers for authentication (using authToken)
    // 5. If body is non-empty, convert JSON to string and attach as request body
    // 6. Perform the request and capture response
    // 7. Parse response string to JSON object and return.
    // 8. Log any errors.
    
    json dummyResponse; // This is a placeholder for the parsed JSON response.
    // ... Implementation goes here ...
    return dummyResponse;
}
```

*Note: In production, this method would handle timeouts, retries, and response code checks, logging errors accordingly.*

---

## 4. Integration with Trading Strategies

- **Strategy Execution Layer:**  
  The TrendStrategy should call the IBKR interface’s `submitOrder()` (and possibly `modifyOrder()` or `cancelOrder()` based on signals) when generating trading signals.
  
- **Callbacks & Order Status:**  
  When an order is submitted, the IBKR integration logs the response. Optionally, you could set callbacks to update the portfolio module based on the order status (e.g., filled, rejected).  
  - For now, logging these statuses with the system logger is sufficient.

- **No Major Changes in TrendStrategy:**  
  The integration should act as a drop-in replacement for the execution layer. Only minor modifications may be needed if asynchronous status updates require additional handling.

---

## 5. Testing & Simulation

### A. Lightweight Fake Server for Testing

- **Purpose:**  
  Simulate IBKR API responses so that integration tests can run without connecting to real endpoints.

- **Implementation:**  
  Write a simple HTTP server (using a lightweight framework or even a simple stub in C++) that listens on a configurable port and returns canned JSON responses for endpoints such as:
  - `/iserver/account/{accountId}/orders` (for POST, return a response with order_id and order_status)
  - `/iserver/account/{accountId}/order/{orderId}` for modification or deletion.
  - `/iserver/questions/suppress` returning `{ "status": "submitted" }`
  
- **Integration Test File:**  
  Create a new test file (e.g., `tests/test_ibkr_paper_trader.cpp`) that:
  - Spins up the fake server.
  - Configures the IBKR interface to connect to the fake server.
  - Tests submission, modification, cancellation, and status retrieval.
  - Uses assertions to verify the JSON responses are formatted as expected.

### B. Test Cases to Include:
- **Order Submission Test:**  
  Submit a sample order and check that the returned order status and order_id are correct.
  
- **Modify Order Test:**  
  Modify the order price and verify the update response.
  
- **Cancel Order Test:**  
  Cancel an order and verify that the response indicates that the request was received.
  
- **Order Reply Suppression Test:**  
  Ensure that on session initialization, the suppression endpoint is called and logged appropriately.

---

## 6. Logging and Error Handling

- **Verbose Logging:**  
  All HTTP requests/responses, including errors and status codes, must be logged via our Logger.
  
- **Error Handling:**  
  If a request fails, log the error; then either retry or return an error status to the calling strategy module.
  
- **Order Reply Messages:**  
  If the IBKR API returns an order reply message that requires confirmation, the interface should either:
  - Automatically call `confirmOrderReplyMessage()` if configured, or
  - Log the message and wait for manual command-line confirmation (for paper trading, automated approaches are preferred).

---

## 7. Summary of Workflow

1. **Session Initialization:**  
   - Read configuration (base URL, account credentials, etc.).  
   - Call `initializeSession()`, performing authentication and automatically suppressing common reply messages.

2. **Order Execution Flow:**  
   - When a trading signal is generated (e.g., in TrendStrategy), create an order object.  
   - Call `submitOrder(order)` on the IBKR interface.
   - Optionally, if order parameters change, call `modifyOrder()`.
   - To cancel active orders, call `cancelOrder()`.
   - Poll `getOrderStatus()` to monitor order execution and update portfolio accordingly.
   
3. **Testing:**  
   - Use the lightweight fake server to simulate responses.  
   - Run integration tests that verify the correctness of each API method.

4. **Logging:**  
   - Each step logs the actions taken and details of the API responses for troubleshooting.

---

## 8. Final Notes

- This design document provides a step-by-step integration path with pseudo-code and detailed comments.
- Adjustments may be needed based on real API behavior, but the structure here should enable a seamless drop-in replacement for our existing execution layer.
- Future enhancements might include a more robust authentication flow (e.g., full OAuth 2.0) and expanded handling of asynchronous updates from IBKR.

---

This concludes the detailed design document. Please review and let me know if any changes or further details need to be added.
```

---
