#include "bt_trend_engine_stub.hpp"
#include <vector>
#include <thread>
#include <memory>
#include <iostream>

class DummyStrategy : public StrategyInterface {};
class DummyPortfolio : public PortfolioManager {};

void thread_run_engine(std::shared_ptr<BtTrendEngineStub> engine, int n) {
    for (int i = 0; i < n; ++i) {
        engine->run(std::make_shared<DummyStrategy>());
        engine->run_portfolio(std::make_shared<DummyPortfolio>());
    }
}

int main() {
    BacktestConfig config;
    auto engine = std::make_shared<BtTrendEngineStub>(config);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(thread_run_engine, engine, 1000);
    }
    for (auto& t : threads) t.join();
    std::cout << "BtTrendEngine minimal thread/mem/RAII test passed!" << std::endl;
    return 0;
} 