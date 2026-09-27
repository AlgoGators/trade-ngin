#include <gtest/gtest.h>
#include "trade_ngin/core/logger.hpp"
#include <cstdlib>
#include <sstream>
#include <thread>

// Existing logging macros name these types without namespace qualification.
using trade_ngin::Logger;
using trade_ngin::LogLevel;

namespace {
constexpr const char* component =
    "PostgresDatabase_shutdown_component_0123456789_0123456789_0123456789";

void configure() {
    trade_ngin::Logger::reset_for_tests();
    trade_ngin::LoggerConfig config;
    config.destination = trade_ngin::LogDestination::CONSOLE;
    config.include_timestamp = false;
    trade_ngin::Logger::instance().initialize(config);
}

// Constructed before the worker's logger TLS value, or after the main logger
// singleton. Its log therefore occurs after nontrivial component TLS teardown.
// The expected string remains owned by this still-live object during the check.
struct VerifyLateLog {
    std::string expected_component;
    ~VerifyLateLog() noexcept {
        std::ostringstream captured;
        auto* original = std::cout.rdbuf(captured.rdbuf());
        try {
            INFO("Disconnected from PostgreSQL database");
            INFO("Disconnected from PostgreSQL database");
            std::cout.rdbuf(original);
            const auto line = "[INFO] [" + expected_component +
                "] Disconnected from PostgreSQL database\n";
            if (captured.str() != line + line) {
                std::cerr << "late_shutdown_context_mismatch\n";
                std::_Exit(41);
            }
            std::cerr << "late_shutdown_context_exact=1\n";
        } catch (...) {
            std::cout.rdbuf(original);
            std::_Exit(42);
        }
    }
};
}  // namespace

TEST(LoggerShutdownDeathTest, MainTlsTeardownPreservesBothStaticDisconnectLogs) {
    ASSERT_EXIT({
        configure();
        trade_ngin::Logger::register_component(component);
        static VerifyLateLog late{component};
        (void)late;
        // Returning from main and std::exit both destroy the calling thread's
        // nontrivial TLS before static-storage objects. No DB or delivery code.
        std::exit(0);
    }, ::testing::ExitedWithCode(0), "late_shutdown_context_exact=1");
}

TEST(LoggerShutdownDeathTest, WorkerTlsTeardownPreservesBothLateDisconnectLogs) {
    ASSERT_EXIT({
        configure();
        std::thread worker([] {
            thread_local VerifyLateLog late{component};
            (void)late;
            trade_ngin::Logger::register_component(component);
        });
        worker.join();
        std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "late_shutdown_context_exact=1");
}

TEST(LoggerShutdown, NormalThreadsNeverInheritRetiredThreadComponentNames) {
    configure();
    trade_ngin::Logger::register_component("main-component");
    std::ostringstream captured;
    auto* original = std::cout.rdbuf(captured.rdbuf());
    std::string expected;
    // Sequential short threads exercise implementations that reuse thread ids.
    // Each must start without the last worker's component, then keep its own.
    for (int index = 0; index < 8; ++index) {
        const std::string name = "worker-" + std::to_string(index);
        std::thread worker([name] {
            INFO("new worker starts without a component");
            trade_ngin::Logger::register_component(name);
            INFO("worker component recorded");
        });
        worker.join();
        expected += "[INFO] new worker starts without a component\n";
        expected += "[INFO] [" + name + "] worker component recorded\n";
    }
    INFO("main component remains independent");
    std::cout.rdbuf(original);
    expected += "[INFO] [main-component] main component remains independent\n";
    EXPECT_EQ(captured.str(), expected);
    trade_ngin::Logger::register_component("");
}
