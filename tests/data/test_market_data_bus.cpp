#include <gtest/gtest.h>
#include <atomic>
#include <map>
#include <thread>
#include "../core/test_base.hpp"
#include "trade_ngin/data/market_data_bus.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class MarketDataBusTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        bus_ = &MarketDataBus::instance();
    }

    void TearDown() override {
        // Clear any subscriptions
        for (const auto& id : subscriber_ids_) {
            bus_->unsubscribe(id);
        }
        subscriber_ids_.clear();
        TestBase::TearDown();
    }

    MarketDataEvent create_test_event(const std::string& symbol,
                                      MarketDataEventType type = MarketDataEventType::BAR,
                                      double price = 100.0) {
        MarketDataEvent event;
        event.type = type;
        event.symbol = symbol;
        event.timestamp = std::chrono::system_clock::now();

        // Set numeric fields based on price
        event.numeric_fields = {{"open", price},       {"high", price * 1.01},
                                {"low", price * 0.99}, {"close", price * 1.005},
                                {"volume", 10000.0},   {"vwap", price * 1.002}};

        event.string_fields = {{"exchange", "NYSE"}, {"condition", "Regular"}};

        return event;
    }

    MarketDataBus* bus_;
    std::vector<std::string> subscriber_ids_;
};

TEST_F(MarketDataBusTest, BasicSubscription) {
    std::atomic<int> callback_count{0};

    MarketDataCallback callback = [&callback_count](const MarketDataEvent& event) {
        callback_count++;
    };

    SubscriberInfo info{"test_subscriber", {MarketDataEventType::BAR}, {"AAPL"}, callback};

    auto result = bus_->subscribe(info);
    ASSERT_TRUE(result.is_ok());
    subscriber_ids_.push_back("test_subscriber");

    // Publish matching event
    auto event = create_test_event("AAPL");
    bus_->publish(event);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(callback_count, 1);

    // Publish non-matching symbol
    auto other_event = create_test_event("MSFT");
    bus_->publish(other_event);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(callback_count, 1);  // Should not increment
}

TEST_F(MarketDataBusTest, MultipleEventTypes) {
    std::atomic<int> bar_count{0};
    std::atomic<int> trade_count{0};

    MarketDataCallback callback = [&](const MarketDataEvent& event) {
        if (event.type == MarketDataEventType::BAR) {
            bar_count++;
        } else if (event.type == MarketDataEventType::TRADE) {
            trade_count++;
        }
    };

    SubscriberInfo info{"multi_type_subscriber",
                        {MarketDataEventType::BAR, MarketDataEventType::TRADE},
                        {"AAPL"},
                        callback};

    ASSERT_TRUE(bus_->subscribe(info).is_ok());
    subscriber_ids_.push_back("multi_type_subscriber");

    // Publish different event types
    bus_->publish(create_test_event("AAPL", MarketDataEventType::BAR));
    bus_->publish(create_test_event("AAPL", MarketDataEventType::TRADE));
    bus_->publish(create_test_event("AAPL", MarketDataEventType::QUOTE));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(bar_count, 1);
    EXPECT_EQ(trade_count, 1);
}

TEST_F(MarketDataBusTest, UnsubscribeTest) {
    std::atomic<int> callback_count{0};

    MarketDataCallback callback = [&callback_count](const MarketDataEvent& event) {
        callback_count++;
    };

    SubscriberInfo info{"temp_subscriber", {MarketDataEventType::BAR}, {"AAPL"}, callback};

    ASSERT_TRUE(bus_->subscribe(info).is_ok());

    // Initial event
    bus_->publish(create_test_event("AAPL"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(callback_count, 1);

    // Unsubscribe
    ASSERT_TRUE(bus_->unsubscribe("temp_subscriber").is_ok());

    // Should not receive after unsubscribe
    bus_->publish(create_test_event("AAPL"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(callback_count, 1);  // Should not increment
}

TEST_F(MarketDataBusTest, MultipleSubscribers) {
    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    SubscriberInfo info1{
        "subscriber1", {MarketDataEventType::BAR}, {"AAPL"}, [&count1](const MarketDataEvent&) {
            count1++;
        }};

    SubscriberInfo info2{
        "subscriber2", {MarketDataEventType::BAR}, {"AAPL"}, [&count2](const MarketDataEvent&) {
            count2++;
        }};

    ASSERT_TRUE(bus_->subscribe(info1).is_ok());
    ASSERT_TRUE(bus_->subscribe(info2).is_ok());
    subscriber_ids_.push_back("subscriber1");
    subscriber_ids_.push_back("subscriber2");

    bus_->publish(create_test_event("AAPL"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(count1, 1);
    EXPECT_EQ(count2, 1);
}

TEST_F(MarketDataBusTest, EmptySymbolListSubscription) {
    std::atomic<int> callback_count{0};

    SubscriberInfo info{"wildcard_subscriber",
                        {MarketDataEventType::BAR},
                        {},  // Empty symbol list means subscribe to all
                        [&callback_count](const MarketDataEvent&) { callback_count++; }};

    ASSERT_TRUE(bus_->subscribe(info).is_ok());
    subscriber_ids_.push_back("wildcard_subscriber");

    // Should receive events for all symbols
    bus_->publish(create_test_event("AAPL"));
    bus_->publish(create_test_event("MSFT"));
    bus_->publish(create_test_event("GOOG"));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(callback_count, 3);
}

TEST_F(MarketDataBusTest, InvalidSubscriptions) {
    MarketDataCallback dummy_callback = [](const MarketDataEvent&) {};

    // Empty subscriber ID
    SubscriberInfo info1{"", {MarketDataEventType::BAR}, {"AAPL"}, dummy_callback};
    EXPECT_TRUE(bus_->subscribe(info1).is_error());

    // No event types
    SubscriberInfo info2{"sub2", {}, {"AAPL"}, dummy_callback};
    EXPECT_TRUE(bus_->subscribe(info2).is_error());

    // Null callback
    SubscriberInfo info3{"sub3", {MarketDataEventType::BAR}, {"AAPL"}, nullptr};
    EXPECT_TRUE(bus_->subscribe(info3).is_error());
}

TEST_F(MarketDataBusTest, ConcurrentOperations) {
    const int num_publishers = 5;
    const int events_per_publisher = 100;
    std::atomic<int> total_callbacks{0};

    // Subscribe to all events
    SubscriberInfo info{"concurrent_test",
                        {MarketDataEventType::BAR},
                        {},  // All symbols
                        [&total_callbacks](const MarketDataEvent&) { total_callbacks++; }};

    ASSERT_TRUE(bus_->subscribe(info).is_ok());
    subscriber_ids_.push_back("concurrent_test");

    // Launch multiple publisher threads
    std::vector<std::thread> publishers;
    for (int i = 0; i < num_publishers; ++i) {
        publishers.emplace_back([this, i, events_per_publisher]() {
            for (int j = 0; j < events_per_publisher; ++j) {
                auto event = create_test_event("SYM" + std::to_string(i), MarketDataEventType::BAR,
                                               100.0 + j);
                bus_->publish(event);
            }
        });
    }

    // Wait for publishers
    for (auto& thread : publishers) {
        thread.join();
    }

    // Allow time for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(total_callbacks, num_publishers * events_per_publisher);
}

TEST_F(MarketDataBusTest, ExceptionHandling) {
    std::atomic<int> successful_callbacks{0};

    SubscriberInfo info{"exception_test",
                        {MarketDataEventType::BAR},
                        {"AAPL"},
                        [&successful_callbacks](const MarketDataEvent&) {
                            successful_callbacks++;
                            throw std::runtime_error("Intentional test exception");
                        }};

    ASSERT_TRUE(bus_->subscribe(info).is_ok());
    subscriber_ids_.push_back("exception_test");

    // Exception in callback shouldn't crash the bus
    EXPECT_NO_THROW(bus_->publish(create_test_event("AAPL")));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(successful_callbacks, 1);

    // Should still be able to publish after exception
    EXPECT_NO_THROW(bus_->publish(create_test_event("AAPL")));
}

TEST_F(MarketDataBusTest, HighVolumeTest) {
    std::atomic<int> processed_count{0};
    const int num_events = 10000;

    SubscriberInfo info{"high_volume_test",
                        {MarketDataEventType::BAR},
                        {"AAPL"},
                        [&processed_count](const MarketDataEvent&) { processed_count++; }};

    ASSERT_TRUE(bus_->subscribe(info).is_ok());
    subscriber_ids_.push_back("high_volume_test");

    // Publish many events rapidly
    for (int i = 0; i < num_events; ++i) {
        bus_->publish(create_test_event("AAPL", MarketDataEventType::BAR, 100.0 + (i % 100)));
    }

    // Allow time for processing
    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_EQ(processed_count, num_events);
}
