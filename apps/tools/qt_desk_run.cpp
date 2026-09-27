#include "qt_desk_connection.hpp"
#include "qt_desk_csv.hpp"
// Explicit local accounting for one already-confirmed immutable QT decision.
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/qt_desk_processor.hpp"

#include <libpq-fe.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace {
using Json = nlohmann::json;
using namespace trade_ngin::qt_desk_cli;

int fail(int code, const char* status) {
    std::cout << Json({{"schema", "qt-desk-run/v1"}, {"status", status}}).dump() << '\n';
    return code;
}

bool parse(int argc, char** argv, std::map<std::string, std::string>& options, int& fd) {
    if (argc < 11 || argc > 17 || argc % 2 == 0) return false;
    for (int i = 1; i < argc; i += 2) {
        const std::string name(argv[i]);
        if (name != "--desk" && name != "--decision" && name != "--attempt" &&
            name != "--input" && name != "--connection-fd" &&
            name != "--market-source" && name != "--finalization-source" && name != "--csv-output") return false;
        if (!options.emplace(name, argv[i + 1]).second) return false;
    }
    for (const char* name : {"--desk", "--decision", "--attempt", "--input", "--connection-fd"})
        if (!options.contains(name)) return false;
    const bool market = options.contains("--market-source");
    if (market != options.contains("--finalization-source") ||
        options.size() != (market ? 7u : 5u) + (options.contains("--csv-output") ? 1u : 0u)) return false;
    if (market) {
        const std::string prefix("qt-finalization/");
        const auto& finalization = options.at("--finalization-source");
        if (!canonical_uuid(options.at("--market-source")) ||
            !finalization.starts_with(prefix) ||
            !canonical_uuid(finalization.substr(prefix.size()))) return false;
    }
    if (!canonical_day(options.at("--desk")) ||
        !canonical_uuid(options.at("--decision")) || !canonical_uuid(options.at("--attempt")) ||
        !canonical_uuid(options.at("--input"))) return false;
    const auto& number = options.at("--connection-fd");
    if (number.empty() || number.front() == '0' ||
        !std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), fd);
    return parsed.ec == std::errc() && parsed.ptr == number.data() + number.size() && fd >= 3;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "qt_desk_run --desk YYYY-MM-DD --decision UUID --attempt UUID --input UUID --connection-fd N\n"
                     "  [--market-source UUID --finalization-source qt-finalization/UUID]\n"
                     "  [--csv-output /absolute/new-file.csv]\n"
                     "Local accounting for an already-confirmed QT decision; internal receipt only.\n";
        return 0;
    }
    std::map<std::string, std::string> options;
    int fd = -1;
    if (!parse(argc, argv, options, fd)) return fail(2, "invalid_arguments");
    std::optional<trade_ngin::Result<std::unique_ptr<QtDeskCsvOutput>>> csv_output;
    if (options.contains("--csv-output")) {
        auto admitted = QtDeskCsvOutput::admit(options.at("--csv-output"));
        if (admitted.is_error()) return fail(2, "invalid_arguments");
        csv_output.emplace(std::move(admitted));
    }
    std::string material, dsn;
    if (!read_connection(fd, material) || !explicit_local_connection(material, dsn))
        return fail(3, "connection_input_unavailable");
    std::fill(material.begin(), material.end(), '\0');
    // libpq must never source omitted options or credentials from inherited env.
    if (clearenv() != 0) return fail(3, "connection_input_unavailable");
    trade_ngin::LoggerConfig logging;
    logging.destination = trade_ngin::LogDestination::NONE;
    trade_ngin::Logger::instance().initialize(logging);
    try {
        pqxx::connection connection(dsn);
        std::fill(dsn.begin(), dsn.end(), '\0');
        {
            pqxx::read_transaction tx(connection);
            const auto decision = tx.exec("SELECT source_day::text,status FROM trading.qt_decisions WHERE decision_id=" +
                                          tx.quote(options.at("--decision")) + "::uuid");
            if (decision.size() != 1 || decision[0][0].as<std::string>() != options.at("--desk") ||
                decision[0][1].as<std::string>() != "confirmed_decision")
                return fail(4, "decision_day_unavailable");
        }
        auto processed = options.contains("--market-source")
            ? trade_ngin::process_qt_desk_sourced_decision(
                connection, options.at("--decision"), options.at("--attempt"), options.at("--input"),
                options.at("--market-source"), options.at("--finalization-source"))
            : trade_ngin::process_qt_desk_accounting_decision(
                connection, options.at("--decision"), options.at("--attempt"), options.at("--input"));
        if (processed.is_error()) return fail(5, "accounting_refused");
        const auto& receipt = processed.value();
        Json answer{{"schema", "qt-desk-run/v1"}, {"status", "accounted"},
            {"decision_id", receipt.decision_id}, {"attempt_id", receipt.attempt_id},
            {"source_day", options.at("--desk")}, {"book_digest", receipt.published_book_digest},
            {"replayed", receipt.replayed}};
        int exit_code = 0;
        if (csv_output) {
            // The processor has already committed. An export refusal must retain
            // that truthful receipt status, including unexpected writer exceptions.
            answer["csv_status"] = "refused"; exit_code = 7;
            try {
                auto written = export_committed_qt_desk_csv(connection, receipt,
                    options.at("--input"), options.at("--desk"), *csv_output->value());
                if (written.is_ok()) {
                    answer["csv_status"] = "written"; answer["csv_sha256"] = written.value(); exit_code = 0;
                }
            } catch (const std::exception&) {}
        }
        std::cout << answer.dump() << '\n';
        return exit_code;
    } catch (const std::exception&) {
        return fail(6, "accounting_unavailable");
    }
}
