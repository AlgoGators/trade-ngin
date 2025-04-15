#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>

// Simple RAII class
template<typename T>
class ScopedValue {
    T& ref;
    T old;
public:
    ScopedValue(T& r, T v) : ref(r), old(r) { ref = v; }
    ~ScopedValue() { ref = old; }
};

void thread_increment(std::atomic<int>& counter, int times) {
    for (int i = 0; i < times; ++i) {
        counter.fetch_add(1, std::memory_order_relaxed);
    }
}

int main() {
    std::atomic<int> counter{0};
    int value = 42;
    {
        ScopedValue<int> sv(value, 100);
        assert(value == 100);
    }
    assert(value == 42);

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(thread_increment, std::ref(counter), 10000);
    }
    for (auto& t : threads) t.join();
    std::cout << "Final counter: " << counter << std::endl;
    assert(counter == 40000);
    std::cout << "Minimal thread/RAII test passed!" << std::endl;
    return 0;
} 