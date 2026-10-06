#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace trade_ngin {

struct QtDispatchIds {
    std::string attempt_id;
    std::string input_id;
    std::string market_source_id;
    std::string finalization_id;
    bool operator==(const QtDispatchIds&) const = default;
};

struct QtDispatchScope {
    std::string registry_id;
    std::string strategy_type;
    std::string book_id;
    std::string lifecycle;
    bool is_active = false;
};

struct QtDispatchJob {
    std::string source_day;
    std::string decision_id;
    std::string attempt_id;
    std::string input_id;
    std::string market_source_id;
    std::string finalization_id;
};

enum class QtDispatchPhase { Prepare, Run };
enum class QtDispatchDisposition { Succeeded, Retry, DeadLetter };

struct QtDeskDispatcherConfig {
    std::chrono::seconds poll_interval{1};
    std::chrono::seconds alert_age{300};
    std::chrono::seconds lease_duration{300};
    unsigned max_attempts = 8;
};

QtDispatchIds qt_dispatch_ids(const std::string& decision_id);
bool qt_dispatch_scope_supported(const QtDispatchScope& scope);
std::chrono::seconds qt_dispatch_retry_delay(unsigned attempt_number);
QtDispatchDisposition qt_dispatch_classify_exit(
    QtDispatchPhase phase, int exit_code, unsigned attempt_number,
    unsigned max_attempts = 8);
std::vector<std::string> qt_dispatch_prepare_arguments(
    const QtDispatchJob& job, const std::string& prior_decision_id,
    const std::string& as_of, const std::string& valid_until, int connection_fd);
std::vector<std::string> qt_dispatch_run_arguments(
    const QtDispatchJob& job, int connection_fd);

}  // namespace trade_ngin
