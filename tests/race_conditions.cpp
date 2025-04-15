#include <gtest/gtest.h>
#include <thread>
#include <mutex>
#include <vector>
#include <atomic>

class UnsafeCounter {
    int value = 0;
public:
    void increment() { value++; }  // Race condition here
    int get() const { return value; }
};

class SafeCounter {
    std::atomic<int> value{0};
public:
    void increment() { value++; }
    int get() const { return value.load(); }
};

TEST(ThreadTest, UnsafeIncrement) {
    UnsafeCounter counter;
    std::vector<std::thread> threads;
    
    // Create multiple threads that increment the counter
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&counter]() {
            for (int j = 0; j < 1000; ++j) {
                counter.increment();
            }
        });
    }
    
    // Join all threads
    for (auto& thread : threads) {
        thread.join();
    }
    
    // The actual value might be less than expected due to race conditions
    EXPECT_NE(counter.get(), 10000);
}

TEST(ThreadTest, SafeIncrement) {
    SafeCounter counter;
    std::vector<std::thread> threads;
    
    // Create multiple threads that increment the counter
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&counter]() {
            for (int j = 0; j < 1000; ++j) {
                counter.increment();
            }
        });
    }
    
    // Join all threads
    for (auto& thread : threads) {
        thread.join();
    }
    
    // The value should be exactly as expected
    EXPECT_EQ(counter.get(), 10000);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 