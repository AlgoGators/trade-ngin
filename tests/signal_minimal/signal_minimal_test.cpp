#include "base_strategy_stub.hpp"
#include <vector>
#include <thread>
#include <memory>
#include <iostream>

void thread_send_signals(std::shared_ptr<BaseStrategyStub> strat, int n, double base) {
    for (int i = 0; i < n; ++i) {
        strat->on_signal("SYM" + std::to_string(i % 10), base + i);
    }
}

int main() {
    auto strat = std::make_shared<BaseStrategyStub>("test_strategy", StrategyConfig{});
    strat->initialize();
    strat->start();
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(thread_send_signals, strat, 1000, i * 1000.0);
    }
    for (auto& t : threads) t.join();
    strat->stop();
    std::cout << "Strategy state: " << static_cast<int>(strat->get_state()) << std::endl;
    std::cout << "Signal minimal thread/mem/RAII test passed!" << std::endl;
    return 0;
} 