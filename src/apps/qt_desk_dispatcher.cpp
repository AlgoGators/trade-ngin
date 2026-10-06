#include "trade_ngin/apps/qt_desk_dispatcher.hpp"
#include <array>
#include <stdexcept>
#include <string_view>
#include <uuid/uuid.h>

namespace trade_ngin {
namespace {

bool canonical_uuid(std::string_view value) {
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        } else if (std::string_view("0123456789abcdef").find(value[index]) ==
                   std::string_view::npos) return false;
    }
    return true;
}

std::string stable_uuid(const std::string& decision_id, std::string_view purpose) {
    static const uuid_t dispatch_namespace = {
        0x1d, 0x31, 0x6c, 0x47, 0x84, 0x72, 0x5a, 0x9f,
        0x9b, 0x23, 0x68, 0xd2, 0x4c, 0x53, 0xd1, 0x72};
    const auto name = std::string(purpose) + "/" + decision_id;
    uuid_t generated{};
    uuid_generate_sha1(generated, dispatch_namespace, name.data(), name.size());
    std::array<char, 37> rendered{};
    uuid_unparse_lower(generated, rendered.data());
    return rendered.data();
}

}  // namespace

QtDispatchIds qt_dispatch_ids(const std::string& decision_id) {
    if (!canonical_uuid(decision_id)) throw std::invalid_argument("qt_dispatch_decision_id_invalid");
    return {stable_uuid(decision_id, "attempt"), stable_uuid(decision_id, "input"),
            stable_uuid(decision_id, "market"), stable_uuid(decision_id, "finalization")};
}

bool qt_dispatch_scope_supported(const QtDispatchScope& scope) {
    return scope.registry_id == "trendfollowing" &&
           scope.strategy_type == "LIVE_TREND_FOLLOWING" &&
           scope.book_id == "CONSERVATIVE_PORTFOLIO" &&
           scope.lifecycle == "live" && scope.is_active;
}

std::chrono::seconds qt_dispatch_retry_delay(unsigned attempt_number) {
    constexpr std::array<int, 8> delays{1, 2, 5, 10, 30, 60, 120, 300};
    const auto index = attempt_number == 0 ? 0u :
        std::min<std::size_t>(attempt_number - 1, delays.size() - 1);
    return std::chrono::seconds(delays[index]);
}

QtDispatchDisposition qt_dispatch_classify_exit(
    QtDispatchPhase phase, int exit_code, unsigned attempt_number, unsigned max_attempts) {
    if (exit_code == 0) return QtDispatchDisposition::Succeeded;
    const bool deterministic_refusal = exit_code == 2 || exit_code == 4 ||
        (phase == QtDispatchPhase::Prepare && exit_code == 6) ||
        (phase == QtDispatchPhase::Run && exit_code == 5);
    if (deterministic_refusal || attempt_number >= max_attempts)
        return QtDispatchDisposition::DeadLetter;
    return QtDispatchDisposition::Retry;
}

std::vector<std::string> qt_dispatch_prepare_arguments(
    const QtDispatchJob& job, const std::string& prior_decision_id,
    const std::string& as_of, const std::string& valid_until, int connection_fd) {
    return {"--desk", job.source_day, "--decision", job.decision_id,
            "--prior-decision", prior_decision_id,
            "--market-source", job.market_source_id,
            "--finalization", job.finalization_id,
            "--as-of", as_of, "--valid-until", valid_until,
            "--connection-fd", std::to_string(connection_fd)};
}

std::vector<std::string> qt_dispatch_run_arguments(
    const QtDispatchJob& job, int connection_fd) {
    return {"--desk", job.source_day, "--decision", job.decision_id,
            "--attempt", job.attempt_id, "--input", job.input_id,
            "--connection-fd", std::to_string(connection_fd),
            "--market-source", job.market_source_id,
            "--finalization-source", "qt-finalization/" + job.finalization_id};
}

}  // namespace trade_ngin
