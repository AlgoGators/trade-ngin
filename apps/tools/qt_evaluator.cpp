#include "trade_ngin/portfolio/qt_evaluation.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/portfolio/qt_equity_proof.hpp"
#include "trade_ngin/core/logger.hpp"

#include <array>
#include <iostream>
#include <string>

namespace {

trade_ngin::Result<std::string> read_bounded_stdin(size_t maximum) {
    std::string input;
    input.reserve(8192);
    std::array<char, 8192> buffer{};
    while (std::cin) {
        std::cin.read(buffer.data(), buffer.size());
        const auto count = static_cast<size_t>(std::cin.gcount());
        if (count > maximum - input.size())
            return trade_ngin::make_error<std::string>(trade_ngin::ErrorCode::INVALID_ARGUMENT,
                                                        "qt_request_too_large", "qt_evaluator");
        input.append(buffer.data(), count);
        if (!count) break;
    }
    if (!std::cin.eof() && std::cin.bad())
        return trade_ngin::make_error<std::string>(trade_ngin::ErrorCode::FILE_IO_ERROR,
                                                    "qt_request_read_failed", "qt_evaluator");
    if (input.empty())
        return trade_ngin::make_error<std::string>(trade_ngin::ErrorCode::INVALID_ARGUMENT,
                                                    "qt_request_empty", "qt_evaluator");
    return input;
}

int write_qt_error(const trade_ngin::TradeError& error) {
    const auto code = error.code() == trade_ngin::ErrorCode::FILE_IO_ERROR ? "input_error" :
                      error.code() == trade_ngin::ErrorCode::ENCRYPTION_ERROR ? "hash_unavailable" :
                      "invalid_qt_evaluation_request";
    nlohmann::json output = {
        {"schema", "qt-eval/v1"},
        {"error", {{"code", code}, {"message", error.what()}, {"retryable", false}}}
    };
    std::cout << output.dump() << '\n';
    return 2;
}

int write_qt_success(const trade_ngin::QtEvaluation& evaluation) {
    auto result = trade_ngin::serialize_qt_evaluation(evaluation);
    if (result.is_error()) return write_qt_error(*result.error());
    std::cout << result.value() << '\n';
    return 0;
}

}  // namespace

int main() {
    // This offline tool has one structured response channel. Kernel outcomes
    // remain in that response; incidental application logging must not write
    // files or contaminate the strict process protocol.
    trade_ngin::LoggerConfig logging;
    logging.destination = trade_ngin::LogDestination::NONE;
    trade_ngin::Logger::instance().initialize(logging);
    constexpr size_t kMaxRequestBytes = 8 * 1024 * 1024;
    const auto input = read_bounded_stdin(kMaxRequestBytes);
    if (input.is_error()) return write_qt_error(*input.error());
    const auto envelope = trade_ngin::parse_qt_offline_envelope(input.value());
    if (envelope.is_error()) return write_qt_error(*envelope.error());
    if (envelope.value().at("schema") == "qt-equity-finalization-proof/v1") {
        const auto proof = trade_ngin::recompute_qt_equity_finalization_proof(envelope.value());
        if (proof.is_error()) return write_qt_error(*proof.error());
        std::cout << proof.value().dump() << '\n';
        return 0;
    }
    if (envelope.value().at("schema") == "qt-equity-proof/v1") {
        const auto proof = trade_ngin::recompute_qt_equity_proof(envelope.value());
        if (proof.is_error()) return write_qt_error(*proof.error());
        std::cout << proof.value().dump() << '\n';
        return 0;
    }
    const auto request = trade_ngin::parse_qt_evaluation_request(input.value());
    if (request.is_error()) return write_qt_error(*request.error());
    const auto result = trade_ngin::evaluate_qt_request(request.value());
    if (result.is_error()) return write_qt_error(*result.error());
    return write_qt_success(result.value());
}
