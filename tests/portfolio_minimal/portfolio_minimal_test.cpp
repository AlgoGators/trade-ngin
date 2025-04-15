#include "portfolio_manager_stub.hpp"
#include <vector>
#include <thread>
#include <memory>
#include <iostream>

class DummyStrategy : public StrategyInterface {};

void thread_add_strategy(std::shared_ptr<PortfolioManagerStub> pm, int n) {
    for (int i = 0; i < n; ++i) {
        pm->add_strategy(std::make_shared<DummyStrategy>(), 0.1);
    }
}

int main() {
    PortfolioConfig config;
    auto pm = std::make_shared<PortfolioManagerStub>(config);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(thread_add_strategy, pm, 1000);
    }
    for (auto& t : threads) t.join();
    std::cout << "Portfolio config type: " << pm->get_config().get_config_type() << std::endl;
    std::cout << "Portfolio config version: " << pm->get_config().get_config_version() << std::endl;
    std::cout << "Portfolio minimal thread/mem/RAII test passed!" << std::endl;
    return 0;
} 