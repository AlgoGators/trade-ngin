//===== test_base.hpp =====
#pragma once

#ifndef TESTING
#define TESTING
#endif

#include <gtest/gtest.h>
#include "trade_ngin/core/state_manager.hpp"

namespace trade_ngin {
namespace testing {
    
class TestBase : public ::testing::Test {
protected:
    void SetUp() override {
        StateManager::reset_instance();
    }

    void TearDown() override {
        StateManager::instance().reset_instance();
    }
};

}  // namespace testing
}  // namespace trade_ngin