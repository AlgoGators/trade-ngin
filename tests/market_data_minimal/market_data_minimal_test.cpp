#include "market_data_manager_stub.cpp"
#include <vector>
#include <thread>
#include <memory>
#include <iostream>

void thread_add_data(std::shared_ptr<MarketDataManagerStub> mgr, int n, double base) {
    for (int i = 0; i < n; ++i) {
        mgr->add_data(base + i);
    }
}

int main() {
    auto mgr = std::make_shared<MarketDataManagerStub>();
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(thread_add_data, mgr, 1000, i * 1000.0);
    }
    for (auto& t : threads) t.join();
    std::cout << "Final data size: " << mgr->size() << std::endl;
    std::cout << "Sample value: " << mgr->get_data(0) << std::endl;
    std::cout << "Market data minimal thread/mem/RAII test passed!" << std::endl;
    return 0;
} 