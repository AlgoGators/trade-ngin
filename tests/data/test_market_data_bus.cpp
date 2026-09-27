#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include "../core/test_base.hpp"
#include "../sync_test_deadline.hpp"
#include "market_data_bus_test_peer.hpp"
#include "trade_ngin/data/market_data_bus.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class MarketDataBusTest : public TestBase {
protected:
    void SetUp() override {
        if (std::strcmp(::testing::UnitTest::GetInstance()->current_test_info()->name(),
                        "PublishEnabledSyncConcurrentToggleAndPublishStress") == 0) {
            sync_deadline_ = std::make_unique<SyncTestDeadline>(std::chrono::seconds(15));
        }
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
    std::unique_ptr<SyncTestDeadline> sync_deadline_;
};

TEST_F(MarketDataBusTest, BasicSubscription) {
    std::atomic<int> callback_count{0};

    MarketDataCallback callback = [&callback_count](const MarketDataEvent& event) {
        (void)event;
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
        (void)event;
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
        publishers.emplace_back([this, i]() {
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

namespace {
class LifetimeBusCleanup {
public:
    explicit LifetimeBusCleanup(MarketDataBus& bus) : bus_(bus) {}
    ~LifetimeBusCleanup() {
        for (const auto& id : ids_) (void)bus_.unsubscribe(id);
        MarketDataBusTestPeer::set_contention_hook(bus_, nullptr);
    }
    void add(std::string id) { ids_.push_back(std::move(id)); }
private:
    MarketDataBus& bus_;
    std::vector<std::string> ids_;
};

SubscriberInfo lifetime_info(std::string id, MarketDataCallback callback) {
    return {std::move(id), {MarketDataEventType::BAR}, {"AAPL"}, std::move(callback)};
}

MarketDataEvent lifetime_bar() {
    return {MarketDataEventType::BAR, "AAPL", std::chrono::system_clock::now(),
            {{"open", 100.0}, {"high", 101.0}, {"low", 99.0},
             {"close", 100.5}, {"volume", 10000.0}}, {}};
}
}  // namespace

TEST_F(MarketDataBusTest, ScopedLifetimeResetAndScopeExit) {
    LifetimeBusCleanup cleanup(*bus_);
    cleanup.add("lifetime_reset");
    int calls = 0;
    auto info = lifetime_info("lifetime_reset", [&](const MarketDataEvent&) { ++calls; });
    {
        MarketDataBus::ScopedSubscription owned;
        ASSERT_TRUE(bus_->subscribe_scoped(info, owned).is_ok());
        EXPECT_FALSE(owned.empty());
        bus_->publish(lifetime_bar());
        EXPECT_EQ(calls, 1);
        owned.reset();
        EXPECT_TRUE(owned.empty());
        owned.reset();
        EXPECT_FALSE(MarketDataBusTestPeer::is_active(*bus_, info.id));
        bus_->publish(lifetime_bar());
        EXPECT_EQ(calls, 1);
        ASSERT_TRUE(bus_->subscribe_scoped(info, owned).is_ok());
        bus_->publish(lifetime_bar());
        EXPECT_EQ(calls, 2);
    }
    EXPECT_FALSE(MarketDataBusTestPeer::is_active(*bus_, info.id));
    bus_->publish(lifetime_bar());
    EXPECT_EQ(calls, 2);
}

TEST_F(MarketDataBusTest, ScopedReplacementOldThenNewDestruction) {
    LifetimeBusCleanup cleanup(*bus_);
    cleanup.add("lifetime_replace_old_first");
    int a = 0, b = 0;
    MarketDataBus::ScopedSubscription old_owner, new_owner;
    ASSERT_TRUE(bus_->subscribe_scoped(lifetime_info("lifetime_replace_old_first", [&](const MarketDataEvent&) { ++a; }), old_owner).is_ok());
    ASSERT_TRUE(bus_->subscribe_scoped(lifetime_info("lifetime_replace_old_first", [&](const MarketDataEvent&) { ++b; }), new_owner).is_ok());
    bus_->publish(lifetime_bar());
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 1);
    old_owner.reset();
    bus_->publish(lifetime_bar());
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 2);
    new_owner.reset();
    EXPECT_FALSE(MarketDataBusTestPeer::is_active(*bus_, "lifetime_replace_old_first"));
    bus_->publish(lifetime_bar());
    EXPECT_EQ(b, 2);
}

TEST_F(MarketDataBusTest, ScopedReplacementNewThenOldDestruction) {
    LifetimeBusCleanup cleanup(*bus_);
    cleanup.add("lifetime_replace_new_first");
    int a = 0, b = 0;
    MarketDataBus::ScopedSubscription old_owner, new_owner;
    ASSERT_TRUE(bus_->subscribe_scoped(lifetime_info("lifetime_replace_new_first", [&](const MarketDataEvent&) { ++a; }), old_owner).is_ok());
    ASSERT_TRUE(bus_->subscribe_scoped(lifetime_info("lifetime_replace_new_first", [&](const MarketDataEvent&) { ++b; }), new_owner).is_ok());
    new_owner.reset();
    EXPECT_FALSE(MarketDataBusTestPeer::is_active(*bus_, "lifetime_replace_new_first"));
    bus_->publish(lifetime_bar());
    old_owner.reset();
    bus_->publish(lifetime_bar());
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 0);
}

TEST_F(MarketDataBusTest, ScopedIdentitySurvivesForcedCallbackAddressReuse) {
    LifetimeBusCleanup cleanup(*bus_);
    cleanup.add("lifetime_address_reuse");
    struct Target { int* calls; void on_event(const MarketDataEvent&) { ++*calls; } };
    alignas(Target) unsigned char storage[sizeof(Target)];
    int a = 0, b = 0, c = 0;
    auto* target = new (storage) Target{&a};
    MarketDataBus::ScopedSubscription old_owner, middle_owner, current_owner;
    ASSERT_TRUE(bus_->subscribe_scoped(lifetime_info("lifetime_address_reuse", [target](const MarketDataEvent& event) { target->on_event(event); }), old_owner).is_ok());
    ASSERT_TRUE(bus_->subscribe_scoped(lifetime_info("lifetime_address_reuse", [&](const MarketDataEvent&) { ++b; }), middle_owner).is_ok());
    target->~Target();
    auto* reused = new (storage) Target{&c};
    EXPECT_EQ(static_cast<void*>(target), static_cast<void*>(reused));
    ASSERT_TRUE(bus_->subscribe_scoped(lifetime_info("lifetime_address_reuse", [reused](const MarketDataEvent& event) { reused->on_event(event); }), current_owner).is_ok());
    old_owner.reset();
    bus_->publish(lifetime_bar());
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 0);
    EXPECT_EQ(c, 1);
    current_owner.reset();
    (void)bus_->unsubscribe("lifetime_address_reuse");
    reused->~Target();
    middle_owner.reset();
}

TEST_F(MarketDataBusTest, ScopedAndLegacyReplacementKeepCurrentIdSemantics) {
    LifetimeBusCleanup cleanup(*bus_);
    cleanup.add("lifetime_scoped_to_legacy");
    cleanup.add("lifetime_legacy_to_scoped");
    int scoped_a = 0, legacy_b = 0, legacy_a = 0, scoped_b = 0;
    MarketDataBus::ScopedSubscription a, b;
    ASSERT_TRUE(bus_->subscribe_scoped(lifetime_info("lifetime_scoped_to_legacy", [&](const MarketDataEvent&) { ++scoped_a; }), a).is_ok());
    ASSERT_TRUE(bus_->subscribe(lifetime_info("lifetime_scoped_to_legacy", [&](const MarketDataEvent&) { ++legacy_b; })).is_ok());
    a.reset();
    bus_->publish(lifetime_bar());
    EXPECT_EQ(scoped_a, 0);
    EXPECT_EQ(legacy_b, 1);
    EXPECT_TRUE(bus_->unsubscribe("lifetime_scoped_to_legacy").is_ok());
    bus_->publish(lifetime_bar());
    EXPECT_EQ(legacy_b, 1);

    ASSERT_TRUE(bus_->subscribe(lifetime_info("lifetime_legacy_to_scoped", [&](const MarketDataEvent&) { ++legacy_a; })).is_ok());
    ASSERT_TRUE(bus_->subscribe_scoped(lifetime_info("lifetime_legacy_to_scoped", [&](const MarketDataEvent&) { ++scoped_b; }), b).is_ok());
    bus_->publish(lifetime_bar());
    EXPECT_EQ(legacy_a, 0);
    EXPECT_EQ(scoped_b, 1);
    EXPECT_TRUE(bus_->unsubscribe("lifetime_legacy_to_scoped").is_ok());
    b.reset();
    bus_->publish(lifetime_bar());
    EXPECT_EQ(scoped_b, 1);
}

TEST_F(MarketDataBusTest, ScopedMoveConstructionAssignmentAndSelfMoveTransferOwnership) {
    LifetimeBusCleanup cleanup(*bus_);
    cleanup.add("lifetime_move_source");
    cleanup.add("lifetime_move_destination");
    int source_calls = 0, destination_calls = 0;
    MarketDataBus::ScopedSubscription source;
    ASSERT_TRUE(bus_->subscribe_scoped(lifetime_info("lifetime_move_source", [&](const MarketDataEvent&) { ++source_calls; }), source).is_ok());
    MarketDataBus::ScopedSubscription moved(std::move(source));
    EXPECT_TRUE(source.empty());
    source.reset();
    bus_->publish(lifetime_bar());
    EXPECT_EQ(source_calls, 1);
    MarketDataBus::ScopedSubscription destination;
    ASSERT_TRUE(bus_->subscribe_scoped(lifetime_info("lifetime_move_destination", [&](const MarketDataEvent&) { ++destination_calls; }), destination).is_ok());
    destination = std::move(moved);
    EXPECT_TRUE(moved.empty());
    EXPECT_FALSE(MarketDataBusTestPeer::is_active(*bus_, "lifetime_move_destination"));
    auto* self = &destination;
    destination = std::move(*self);
    bus_->publish(lifetime_bar());
    EXPECT_EQ(source_calls, 2);
    EXPECT_EQ(destination_calls, 0);
    destination.reset();
    EXPECT_FALSE(MarketDataBusTestPeer::is_active(*bus_, "lifetime_move_source"));
}

TEST_F(MarketDataBusTest, ScopedValidationAndConstructorUnwindPreserveDelivery) {
    LifetimeBusCleanup cleanup(*bus_);
    cleanup.add("lifetime_validation");
    cleanup.add("lifetime_unwind");
    int calls = 0, unwound_calls = 0;
    MarketDataBus::ScopedSubscription original, rejected;
    auto valid = lifetime_info("lifetime_validation", [&](const MarketDataEvent&) { ++calls; });
    ASSERT_TRUE(bus_->subscribe_scoped(valid, original).is_ok());
    auto invalid = valid;
    invalid.id.clear();
    EXPECT_TRUE(bus_->subscribe_scoped(invalid, rejected).is_error());
    invalid = valid; invalid.event_types.clear();
    EXPECT_TRUE(bus_->subscribe_scoped(invalid, rejected).is_error());
    invalid = valid; invalid.callback = nullptr;
    EXPECT_TRUE(bus_->subscribe_scoped(invalid, rejected).is_error());
    EXPECT_TRUE(rejected.empty());
    EXPECT_TRUE(bus_->subscribe_scoped(valid, original).is_error());
    EXPECT_FALSE(original.empty());
    bus_->publish(lifetime_bar());
    EXPECT_EQ(calls, 1);

    struct ThrowingOwner {
        MarketDataBus::ScopedSubscription owned;  // Last member: unwinds first.
        ThrowingOwner(MarketDataBus& bus, int& count) {
            auto info = lifetime_info("lifetime_unwind", [&count](const MarketDataEvent&) { ++count; });
            if (bus.subscribe_scoped(info, owned).is_error()) throw std::runtime_error("register");
            throw std::runtime_error("after register");
        }
    };
    EXPECT_THROW(ThrowingOwner owner(*bus_, unwound_calls), std::runtime_error);
    EXPECT_FALSE(MarketDataBusTestPeer::is_active(*bus_, "lifetime_unwind"));
    bus_->publish(lifetime_bar());
    EXPECT_EQ(unwound_calls, 0);
}

TEST_F(MarketDataBusTest, ScopedCallbackCopyExceptionDoesNotCommit) {
    LifetimeBusCleanup cleanup(*bus_);
    cleanup.add("lifetime_copy_exception");
    int old_calls = 0;
    MarketDataBus::ScopedSubscription original, rejected;
    ASSERT_TRUE(bus_->subscribe_scoped(lifetime_info("lifetime_copy_exception", [&](const MarketDataEvent&) { ++old_calls; }), original).is_ok());
    struct ThrowOnCopy {
        ThrowOnCopy() = default;
        ThrowOnCopy(const ThrowOnCopy&) { throw std::runtime_error("callback copy"); }
        ThrowOnCopy(ThrowOnCopy&&) noexcept = default;
        void operator()(const MarketDataEvent&) const {}
    };
    auto info = lifetime_info("lifetime_copy_exception", ThrowOnCopy{});
    EXPECT_THROW(bus_->subscribe_scoped(info, rejected), std::runtime_error);
    EXPECT_TRUE(rejected.empty());
    bus_->publish(lifetime_bar());
    EXPECT_EQ(old_calls, 1);
}

namespace {
std::atomic<bool>* lifetime_contention_flag = nullptr;
std::condition_variable* lifetime_contention_cv = nullptr;
void signal_lifetime_contention() noexcept {
    lifetime_contention_flag->store(true, std::memory_order_release);
    lifetime_contention_cv->notify_all();
}
}  // namespace

TEST_F(MarketDataBusTest, ScopedResetWaitsForActuallyContendedCallback) {
    LifetimeBusCleanup cleanup(*bus_);
    cleanup.add("lifetime_quiescence");
    std::mutex gate_mutex;
    std::condition_variable gate_cv, contention_cv;
    bool entered = false, release = false;
    std::atomic<bool> exited{false}, returned{false}, saw_exit_at_return{false};
    std::atomic<bool> contended{false};
    int calls = 0;
    lifetime_contention_flag = &contended;
    lifetime_contention_cv = &contention_cv;
    MarketDataBusTestPeer::set_contention_hook(*bus_, signal_lifetime_contention);
    MarketDataBus::ScopedSubscription owned;
    auto info = lifetime_info("lifetime_quiescence", [&](const MarketDataEvent&) {
        ++calls;
        std::unique_lock<std::mutex> lock(gate_mutex);
        entered = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release; });
        exited.store(true, std::memory_order_release);
    });
    ASSERT_TRUE(bus_->subscribe_scoped(info, owned).is_ok());
    std::thread publisher([&] { bus_->publish(lifetime_bar()); });
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        if (!gate_cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; })) {
            release = true;
            lock.unlock();
            gate_cv.notify_all();
            publisher.join();
            MarketDataBusTestPeer::set_contention_hook(*bus_, nullptr);
            lifetime_contention_flag = nullptr;
            lifetime_contention_cv = nullptr;
            FAIL() << "Callback did not enter";
        }
    }
    std::thread resetter([&] {
        owned.reset();
        saw_exit_at_return.store(exited.load(std::memory_order_acquire), std::memory_order_release);
        returned.store(true, std::memory_order_release);
    });
    bool witnessed;
    {
        std::mutex wait_mutex;
        std::unique_lock<std::mutex> lock(wait_mutex);
        witnessed = contention_cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return contended.load(std::memory_order_acquire);
        });
    }
    const bool exited_before_release = exited.load(std::memory_order_acquire);
    const bool returned_before_release = returned.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release = true;
    }
    gate_cv.notify_all();
    publisher.join();
    resetter.join();
    MarketDataBusTestPeer::set_contention_hook(*bus_, nullptr);
    lifetime_contention_flag = nullptr;
    lifetime_contention_cv = nullptr;
    EXPECT_TRUE(witnessed) << "Reset never failed try-lock on the callback-held bus mutex";
    EXPECT_FALSE(exited_before_release);
    EXPECT_FALSE(returned_before_release);
    EXPECT_TRUE(saw_exit_at_return.load(std::memory_order_acquire));
    EXPECT_FALSE(MarketDataBusTestPeer::is_active(*bus_, info.id));
    bus_->publish(lifetime_bar());
    EXPECT_EQ(calls, 1);
}

// market-data-bus-sync-brief.md defect 1: publish_enabled_ is read in publish()
// and written by set_publish_enabled() with no synchronization. A data race is
// undefined behavior, not a reliably observable one, so there is no
// deterministic RED for this defect, and ThreadSanitizer is unavailable
// without a new build directory (not authorized in this repair). This is a
// finite-iteration concurrent toggle-and-publish stress test that must complete
// without crashing or hanging; it is evidence the atomic flag remains usable
// under concurrent toggle/read, NOT proof of race freedom.
TEST_F(MarketDataBusTest, PublishEnabledSyncConcurrentToggleAndPublishStress) {
    std::atomic<int> callback_count{0};
    SubscriberInfo info{"publish_enabled_sync_stress",
                        {MarketDataEventType::BAR},
                        {"AAPL"},
                        [&callback_count](const MarketDataEvent&) { callback_count++; }};
    subscriber_ids_.push_back("publish_enabled_sync_stress");
    ASSERT_TRUE(bus_->subscribe(info).is_ok());

    constexpr int kIterations = 2000;
    std::exception_ptr toggle_error;
    std::exception_ptr publish_error;
    std::thread toggler;
    std::thread publisher;
    auto cleanup = sync_test_scope_exit([&] {
        if (toggler.joinable()) toggler.join();
        if (publisher.joinable()) publisher.join();
        bus_->set_publish_enabled(true);
    });
    toggler = std::thread([&] {
        try {
            for (int i = 0; i < kIterations; ++i) {
                bus_->set_publish_enabled(i % 2 == 0);
            }
        } catch (...) {
            toggle_error = std::current_exception();
        }
    });
    publisher = std::thread([&] {
        try {
            for (int i = 0; i < kIterations; ++i) {
                bus_->publish(create_test_event("AAPL", MarketDataEventType::BAR, 100.0 + (i % 50)));
            }
        } catch (...) {
            publish_error = std::current_exception();
        }
    });

    toggler.join();
    publisher.join();
    if (toggle_error) std::rethrow_exception(toggle_error);
    if (publish_error) std::rethrow_exception(publish_error);

    // Restore a known, enabled state and confirm ordinary delivery still works
    // once toggling has stopped. The point of the fix is toggle/read safety
    // for the flag itself, not any particular delivery count during the race
    // window above.
    bus_->set_publish_enabled(true);
    ASSERT_TRUE(bus_->is_publish_enabled());
    int before = callback_count.load();
    bus_->publish(create_test_event("AAPL"));
    EXPECT_EQ(callback_count.load(), before + 1);
}

TEST(SyncTestDeadlineTest, SyncSyntheticBlockedWorkerExitsWithExactTimeoutStatus) {
    ASSERT_EXIT(
        {
            SyncTestDeadline deadline(std::chrono::milliseconds(50));
            std::mutex mutex;
            std::condition_variable cv;
            std::thread worker([&] {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(300), [] { return false; });
            });
            worker.join();
            std::_Exit(0);
        },
        ::testing::ExitedWithCode(SyncTestDeadline::kTimeoutExitCode), "");
}

TEST(SyncTestDeadlineTest, SyncNormalCompletionCancelsWatchdog) {
    {
        SyncTestDeadline deadline(std::chrono::milliseconds(250));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    SUCCEED();
}
