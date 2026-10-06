#include "trade_ngin/apps/qt_desk_dispatcher.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace trade_ngin;

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

void stable_ids_are_canonical_and_decision_bound() {
    const auto first = qt_dispatch_ids("123e4567-e89b-42d3-a456-426614174000");
    const auto replay = qt_dispatch_ids("123e4567-e89b-42d3-a456-426614174000");
    const auto other = qt_dispatch_ids("123e4567-e89b-42d3-a456-426614174001");

    check(first == replay, "same decision must reproduce IDs");
    check(first.attempt_id != other.attempt_id, "different decisions need different IDs");
    check(first.input_id != first.market_source_id, "input and market IDs must differ");
    check(first.market_source_id != first.finalization_id, "market and finalization IDs must differ");
    for (const auto* id : {&first.attempt_id, &first.input_id,
                           &first.market_source_id, &first.finalization_id}) {
        check(id->size() == 36u, "IDs must be canonical UUID length");
        check(id->at(8) == '-' && id->at(13) == '-' && id->at(18) == '-' && id->at(23) == '-',
              "IDs must contain canonical UUID separators");
    }
}

void only_the_approved_live_scope_is_eligible() {
    check(qt_dispatch_scope_supported({"trendfollowing", "LIVE_TREND_FOLLOWING",
          "CONSERVATIVE_PORTFOLIO", "live", true}), "approved scope must be eligible");
    check(!qt_dispatch_scope_supported({"trendfollowing", "LIVE_TREND_FOLLOWING",
          "CONSERVATIVE_PORTFOLIO", "incubating", true}), "incubating scope must refuse");
    check(!qt_dispatch_scope_supported({"trendfollowing", "LIVE_TREND_FOLLOWING",
          "CONSERVATIVE_PORTFOLIO", "live", false}), "inactive scope must refuse");
    check(!qt_dispatch_scope_supported({"other", "LIVE_TREND_FOLLOWING",
          "CONSERVATIVE_PORTFOLIO", "live", true}), "wrong registry must refuse");
    check(!qt_dispatch_scope_supported({"trendfollowing", "LIVE_TREND_FOLLOWING",
          "INVESTOR_A", "live", true}), "investor book must refuse");
}

void retry_schedule_is_bounded_and_deterministic() {
    const int expected[] = {1, 2, 5, 10, 30, 60, 120, 300};
    for (unsigned attempt = 1; attempt <= 8; ++attempt)
        check(qt_dispatch_retry_delay(attempt).count() == expected[attempt - 1], "retry delay mismatch");
    check(qt_dispatch_retry_delay(99).count() == 300, "retry delay must cap at 300 seconds");
}

void exit_classification_is_bounded() {
    check(qt_dispatch_classify_exit(QtDispatchPhase::Prepare, 4, 1) == QtDispatchDisposition::DeadLetter,
          "wrong day must dead-letter");
    check(qt_dispatch_classify_exit(QtDispatchPhase::Run, 2, 1) == QtDispatchDisposition::DeadLetter,
          "invalid invocation must dead-letter");
    check(qt_dispatch_classify_exit(QtDispatchPhase::Run, 5, 1) == QtDispatchDisposition::DeadLetter,
          "deterministic accounting refusal must dead-letter");
    check(qt_dispatch_classify_exit(QtDispatchPhase::Prepare, 6, 1) == QtDispatchDisposition::DeadLetter,
          "deterministic prior-finalization refusal must dead-letter");
    check(qt_dispatch_classify_exit(QtDispatchPhase::Prepare, 5, 1) == QtDispatchDisposition::Retry,
          "source availability refusal may retry before cap");
    check(qt_dispatch_classify_exit(QtDispatchPhase::Run, 6, 7) == QtDispatchDisposition::Retry,
          "transient failure must retry before cap");
    check(qt_dispatch_classify_exit(QtDispatchPhase::Run, 6, 8) == QtDispatchDisposition::DeadLetter,
          "failure at cap must dead-letter");
    check(qt_dispatch_classify_exit(QtDispatchPhase::Run, 0, 8) == QtDispatchDisposition::Succeeded,
          "zero exit must succeed");
}

void configuration_defaults_match_the_operational_contract() {
    const QtDeskDispatcherConfig config;
    check(config.poll_interval.count() == 1, "poll default must be one second");
    check(config.alert_age.count() == 300, "alert default must be five minutes");
    check(config.lease_duration.count() == 300, "lease default must be five minutes");
    check(config.max_attempts == 8u, "retry cap must be eight attempts");
}

void child_arguments_are_explicit_and_internal_only() {
    const QtDispatchJob job{
        "2026-10-06", "20000000-0000-4000-8000-000000000001",
        "30000000-0000-4000-8000-000000000001",
        "40000000-0000-4000-8000-000000000001",
        "50000000-0000-4000-8000-000000000001",
        "60000000-0000-4000-8000-000000000001"};
    const auto prepare = qt_dispatch_prepare_arguments(job,
        "10000000-0000-4000-8000-000000000001",
        "2026-10-06T14:00:00Z", "2026-10-06T14:10:00Z", 3);
    const std::vector<std::string> expected_prepare{
        "--desk","2026-10-06","--decision",job.decision_id,
        "--prior-decision","10000000-0000-4000-8000-000000000001",
        "--market-source",job.market_source_id,"--finalization",job.finalization_id,
        "--as-of","2026-10-06T14:00:00Z","--valid-until","2026-10-06T14:10:00Z",
        "--connection-fd","3"};
    check(prepare == expected_prepare, "prepare invocation must be explicit");
    const auto run = qt_dispatch_run_arguments(job, 3);
    const std::vector<std::string> expected_run{
        "--desk","2026-10-06","--decision",job.decision_id,
        "--attempt",job.attempt_id,"--input",job.input_id,
        "--connection-fd","3","--market-source",job.market_source_id,
        "--finalization-source","qt-finalization/" + job.finalization_id};
    check(run == expected_run, "run invocation must contain no delivery arguments");
}

}  // namespace

int main() {
    stable_ids_are_canonical_and_decision_bound();
    only_the_approved_live_scope_is_eligible();
    retry_schedule_is_bounded_and_deterministic();
    exit_classification_is_bounded();
    configuration_defaults_match_the_operational_contract();
    child_arguments_are_explicit_and_internal_only();
    if (failures == 0) std::cout << "qt-desk-dispatcher tests: PASS\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
