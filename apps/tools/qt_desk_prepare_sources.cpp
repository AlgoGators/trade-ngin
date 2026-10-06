// Prepare immutable inputs for one confirmed decision; processing is separate.
#include "qt_desk_connection.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/qt_desk_market_capture.hpp"
#include "trade_ngin/data/qt_desk_upstream.hpp"

#include <cstdlib>
#include <ctime>
#include <iostream>

namespace {
using Json = nlohmann::json;
using namespace trade_ngin::qt_desk_cli;

int fail(int code, const char* status) {
    std::cout << Json({{"schema", "qt-desk-prepare/v1"}, {"status", status}}).dump() << '\n';
    return code;
}

bool utc_timestamp(const std::string& value) {
    if (value.size() < 20 || value.size() > 27 || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value.back() != 'Z' ||
        !canonical_day(value.substr(0, 10))) return false;
    for (const auto offset : {11, 14, 17})
        for (int i = offset; i < offset + 2; ++i)
            if (value[i] < '0' || value[i] > '9') return false;
    if (std::stoi(value.substr(11, 2)) > 23 || std::stoi(value.substr(14, 2)) > 59 ||
        std::stoi(value.substr(17, 2)) > 59) return false;
    if (value.size() == 20) return true;
    return value.size() >= 22 && value[19] == '.' &&
        std::all_of(value.begin() + 20, value.end() - 1,
            [](char c) { return c >= '0' && c <= '9'; });
}

bool parse(int argc, char** argv, std::map<std::string, std::string>& options, int& fd) {
    if (argc != 17 && argc != 15) return false;
    for (int i = 1; i < argc; i += 2) {
        const std::string name(argv[i]);
        if (name != "--desk" && name != "--decision" && name != "--prior-decision" && name != "--first-day-anchor" &&
            name != "--market-source" && name != "--finalization" && name != "--as-of" &&
            name != "--valid-until" && name != "--connection-fd") return false;
        if (!options.emplace(name, argv[i + 1]).second) return false;
    }
    if (!canonical_day(options.at("--desk")) || !utc_timestamp(options.at("--as-of")) ||
        !utc_timestamp(options.at("--valid-until"))) return false;
    for (const auto name : {"--decision", "--market-source"})
        if (!canonical_uuid(options.at(name))) return false;
    const bool first=options.contains("--first-day-anchor");
    if(first){if(options.contains("--prior-decision")||options.contains("--finalization")||options.size()!=7||!canonical_uuid(options.at("--first-day-anchor")))return false;}
    else {if(!options.contains("--prior-decision")||!options.contains("--finalization")||options.size()!=8||!canonical_uuid(options.at("--prior-decision"))||!canonical_uuid(options.at("--finalization")))return false;}
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
        std::cout << "qt_desk_prepare_sources --desk YYYY-MM-DD --decision UUID --prior-decision UUID\n"
                     "  --market-source UUID --finalization UUID --as-of UTC --valid-until UTC --connection-fd N\n"
                     "or --desk YYYY-MM-DD --decision UUID --first-day-anchor UUID --market-source UUID\n"
                     "  --as-of UTC --valid-until UTC --connection-fd N\n"
                     "Prepare immutable sources for a confirmed decision; current accounting is a separate step.\n";
        return 0;
    }
    std::map<std::string, std::string> options;
    int fd = -1;
    if (!parse(argc, argv, options, fd)) return fail(2, "invalid_arguments");
    std::string material, dsn;
    if (!read_connection(fd, material) || !explicit_local_connection(material, dsn))
        return fail(3, "connection_input_unavailable");
    std::fill(material.begin(), material.end(), '\0');
    if (clearenv() != 0) return fail(3, "connection_input_unavailable");
    // The existing market Arrow conversion uses local-time mktime twice.
    // Establish our explicit UTC convention after removing inherited inputs.
    if (setenv("TZ", "UTC", 1) != 0) return fail(3, "connection_input_unavailable");
    tzset();
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
        const bool first=options.contains("--first-day-anchor");
        const Json request{{"source_id", options.at("--market-source")},
            {"decision_id", options.at("--decision")}, {"prior_decision_id", first?Json(nullptr):Json(options.at("--prior-decision"))},
            {"as_of", options.at("--as-of")}, {"valid_until", options.at("--valid-until")}};
        auto captured = trade_ngin::capture_qt_desk_market_source(connection, request);
        if (captured.is_error()) return fail(5, "source_capture_refused");
        Json answer{{"schema", "qt-desk-prepare/v1"}, {"status", "sources_prepared"},
            {"decision_id", options.at("--decision")}, {"source_day", options.at("--desk")}, {"market_source_id", options.at("--market-source")}};
        if(first){auto anchored=trade_ngin::create_qt_first_day_anchor(connection,options.at("--decision"),options.at("--first-day-anchor"));if(anchored.is_error())return fail(6,"first_day_anchor_refused");answer["first_day_anchor_id"]=options.at("--first-day-anchor");}
        else {auto finalized=trade_ngin::finalize_qt_desk_accounting(connection,options.at("--prior-decision"),options.at("--finalization"),options.at("--market-source"));if(finalized.is_error())return fail(6,"prior_finalization_refused");answer["prior_decision_id"]=options.at("--prior-decision");answer["finalization_source_id"]="qt-finalization/"+options.at("--finalization");}
        std::cout << answer.dump() << '\n';
        return 0;
    } catch (const std::exception&) {
        return fail(7, "preparation_unavailable");
    }
}
