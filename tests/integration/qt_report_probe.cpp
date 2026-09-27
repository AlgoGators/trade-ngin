// Disposable-database integration probe for the QT reporting pipeline.
// Deliberately not part of trade_ngin_tests and never invokes the live runner
// or EmailSender::send_email (or any other transport).
#include <chrono>
#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <nlohmann/json.hpp>

#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#undef private
// Include the helper after the test-only access seam: it includes both headers transitively.
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/live/csv_exporter.hpp"

namespace {
constexpr const char* kResultTag = "QT_PIPELINE_RESULT=";
constexpr const char* kErrorTag = "QT_REPORT_PROBE_ERROR=";

void fail(const std::string& message) {
    std::cerr << kErrorTag << nlohmann::json{{"error", message}}.dump() << '\n';
}

bool loopback(std::string host) {
    if (host.size() > 1 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool owned_socket(const std::string& host, const std::string& database) {
    constexpr const char* prefix = "/tmp/qt_pipeline_test_";
    constexpr const char* suffix = "/socket";
    if (host.rfind(prefix, 0) != 0 || host.size() <= std::char_traits<char>::length(prefix) +
        std::char_traits<char>::length(suffix) ||
        host.compare(host.size() - std::char_traits<char>::length(suffix),
                     std::char_traits<char>::length(suffix), suffix) != 0) return false;
    const auto id = host.substr(std::char_traits<char>::length(prefix),
        host.size() - std::char_traits<char>::length(prefix) -
        std::char_traits<char>::length(suffix));
    if (id.size() != 12 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return std::isdigit(c) || (c >= 'a' && c <= 'f');
        })) return false;
    return database == std::string("qt_pipeline_test_") + id;
}

std::unordered_map<std::string, std::string> conninfo_fields(const std::string& text) {
    std::unordered_map<std::string, std::string> fields;
    std::istringstream input(text);
    std::string token;
    while (input >> token) {
        const auto equals = token.find('=');
        if (equals != std::string::npos && equals != 0) fields[token.substr(0, equals)] = token.substr(equals + 1);
    }
    return fields;
}

bool safe_test_dsn(const std::string& dsn, std::string& reason) {
    if (dsn.rfind("postgres://", 0) == 0 || dsn.rfind("postgresql://", 0) == 0) {
        reason = "QT_PIPELINE_TEST_DSN must use host= and dbname= conninfo";
        return false;
    }
    std::istringstream tokens(dsn);
    std::string token;
    while (tokens >> token) {
        if (token.find('=') == std::string::npos) {
            reason = "QT_PIPELINE_TEST_DSN values must be unquoted and space-free";
            return false;
        }
    }
    const auto fields = conninfo_fields(dsn);
    static const std::unordered_set<std::string> allowed{
        "host", "hostaddr", "port", "dbname", "user", "password", "connect_timeout"};
    for (const auto& [key, value] : fields) {
        if (!allowed.contains(key) || value.empty() ||
            value.find_first_of("'\\\"\\\\") != std::string::npos) {
            reason = "QT_PIPELINE_TEST_DSN must use only simple disposable conninfo fields";
            return false;
        }
    }
    const auto database = fields.find("dbname");
    if (database == fields.end() || database->second.rfind("qt_pipeline_test", 0) != 0) {
        reason = "QT_PIPELINE_TEST_DSN dbname must begin qt_pipeline_test";
        return false;
    }
    const auto host = fields.find("host");
    const auto hostaddr = fields.find("hostaddr");
    if (host == fields.end() && hostaddr == fields.end()) {
        reason = "QT_PIPELINE_TEST_DSN needs an explicit disposable endpoint";
        return false;
    }
    const bool unix_socket = host != fields.end() && owned_socket(host->second, database->second);
    if (unix_socket && hostaddr != fields.end()) {
        reason = "QT_PIPELINE_TEST_DSN Unix socket mode forbids hostaddr";
        return false;
    }
    if (!unix_socket && ((host != fields.end() && !loopback(host->second)) ||
        (hostaddr != fields.end() && !loopback(hostaddr->second)))) {
        reason = "QT_PIPELINE_TEST_DSN endpoint must be loopback or the exact owned Unix socket";
        return false;
    }
    const auto port = fields.find("port");
    if (port == fields.end() || port->second.empty() ||
        !std::all_of(port->second.begin(), port->second.end(),
                     [](unsigned char c) { return std::isdigit(c); })) {
        reason = "QT_PIPELINE_TEST_DSN must specify a numeric loopback port";
        return false;
    }
    if (unix_socket && port->second != "5432") {
        reason = "QT_PIPELINE_TEST_DSN owned Unix socket must use port 5432";
        return false;
    }
    return true;
}

nlohmann::json quantities(const trade_ngin::StrategyPositionRows& rows) {
    nlohmann::json output = nlohmann::json::object();
    for (const auto& [strategy, positions] : rows)
        for (const auto& [symbol, position] : positions) output[strategy][symbol] = position.quantity.as_double();
    return output;
}

nlohmann::json quantities(const std::unordered_map<std::string, trade_ngin::Position>& rows) {
    nlohmann::json output = nlohmann::json::object();
    for (const auto& [symbol, position] : rows) output[symbol] = position.quantity.as_double();
    return output;
}

bool snapshot_barrier(const std::filesystem::path& output_dir, std::string& reason) {
#ifdef _WIN32
    reason = "snapshot barrier requires POSIX FIFOs";
    return false;
#else
    const char* ready_raw = std::getenv("QT_PIPELINE_READY_FIFO");
    const char* release_raw = std::getenv("QT_PIPELINE_RELEASE_FIFO");
    if (ready_raw == nullptr || release_raw == nullptr) {
        reason = "snapshot barrier FIFO environment is required";
        return false;
    }
    const auto safe_fifo = [&](const char* raw, const char* label)
        -> std::optional<std::filesystem::path> {
        const std::filesystem::path candidate(raw);
        std::error_code error;
        const auto parent = std::filesystem::weakly_canonical(candidate.parent_path(), error);
        const auto expected = std::filesystem::weakly_canonical(output_dir, error);
        if (error || parent != expected) {
            reason = std::string(label) + " FIFO must be directly inside the output directory";
            return std::nullopt;
        }
        struct stat metadata {};
        if (::stat(candidate.c_str(), &metadata) != 0 || !S_ISFIFO(metadata.st_mode)) {
            reason = std::string(label) + " path must be an existing POSIX FIFO";
            return std::nullopt;
        }
        return candidate;
    };
    const auto ready = safe_fifo(ready_raw, "ready");
    const auto release = safe_fifo(release_raw, "release");
    if (!ready || !release) return false;

    // Opening/writing the ready FIFO is the deterministic proof that the
    // snapshot has been captured. Reading the release FIFO blocks rendering
    // until the Python harness commits its concurrent update. No polling or
    // sleeps are involved.
    std::ofstream ready_stream(*ready);
    if (!ready_stream) { reason = "could not open ready FIFO"; return false; }
    ready_stream << "snapshot-loaded\n";
    ready_stream.close();
    std::ifstream release_stream(*release);
    std::string command;
    if (!release_stream || !std::getline(release_stream, command) || command != "render") {
        reason = "snapshot barrier release was invalid";
        return false;
    }
    return true;
#endif
}

std::string utc_date(trade_ngin::Timestamp timestamp) {
    const auto epoch = std::chrono::system_clock::to_time_t(timestamp);
    std::tm date{};
#ifdef _WIN32
    gmtime_s(&date, &epoch);
#else
    gmtime_r(&epoch, &date);
#endif
    char text[11]{};
    std::strftime(text, sizeof(text), "%Y-%m-%d", &date);
    return text;
}

void install_fixture_instrument(const std::string& symbol, double multiplier,
                                double initial_margin) {
    auto& registry = trade_ngin::InstrumentRegistry::instance();
    const trade_ngin::FuturesSpec spec{symbol, "TEST", "USD", multiplier, 0.01,
        2.50, initial_margin, initial_margin * 0.9, 1.0, "", std::nullopt, std::nullopt};
    registry.instruments_[symbol] = std::make_shared<trade_ngin::FuturesInstrument>(symbol, spec);
}

trade_ngin::StrategyInstancesMap fixture_strategies(
    const std::vector<std::string>& names) {
    install_fixture_instrument("ES", 50.0, 12000.0);
    install_fixture_instrument("NG", 10000.0, 8000.0);
    install_fixture_instrument("NQ", 20.0, 22000.0);
    static std::vector<std::unique_ptr<trade_ngin::TrendFollowingStrategy>> strategies;
    strategies.clear();
    trade_ngin::StrategyInstancesMap result;
    for (const auto& name : names) {
        auto strategy = std::make_unique<trade_ngin::TrendFollowingStrategy>(
            name, trade_ngin::StrategyConfig{}, trade_ngin::TrendFollowingConfig{}, nullptr);
        // Deliberately include the QT-closed NG in the real strategy universe:
        // strict snapshot mode must still keep it out of the emitted CSV.
        strategy->instrument_data_["ES.v.0"] = trade_ngin::InstrumentData{};
        strategy->instrument_data_["NG.v.0"] = trade_ngin::InstrumentData{};
        strategy->instrument_data_["NQ.v.0"] = trade_ngin::InstrumentData{};
        result[name] = strategy.get();
        strategies.push_back(std::move(strategy));
    }
    return result;
}

int probe(const std::string& mode, const std::string& portfolio, const std::string& strategy_id,
          trade_ngin::Timestamp timestamp, const std::filesystem::path& output_dir,
          const std::vector<std::string>& strategy_names) {
    const char* raw_dsn = std::getenv("QT_PIPELINE_TEST_DSN");
    if (raw_dsn == nullptr || *raw_dsn == '\0') { fail("QT_PIPELINE_TEST_DSN is required"); return 2; }
    std::string reason;
    if (!safe_test_dsn(raw_dsn, reason)) { fail(reason); return 2; }
    if (!std::filesystem::is_directory(output_dir)) { fail("output directory does not exist"); return 2; }

    // An explicit hostaddr defeats PGHOST/PGHOSTADDR defaults in loopback mode.
    // Unix-socket mode forbids hostaddr and already has an exact owned path.
    const auto fields = conninfo_fields(raw_dsn);
    const bool unix_socket = fields.contains("host") &&
        owned_socket(fields.at("host"), fields.at("dbname"));
    const std::string constrained_dsn = unix_socket
        ? std::string(raw_dsn)
        : std::string(raw_dsn) + " hostaddr=127.0.0.1";
    trade_ngin::PostgresDatabase db(constrained_dsn);
    if (const auto connected = db.connect(); connected.is_error()) {
        fail("disposable database connection failed: " + std::string(connected.error()->what()));
        return 3;
    }
    if (mode == "seed") {
        if (const auto seeded = trade_ngin::seed_qt_report_positions(db, strategy_id, strategy_names, portfolio, timestamp);
            seeded.is_error()) {
            fail("QT seed failed: " + std::string(seeded.error()->what()));
            return 4;
        }
        std::cout << kResultTag << nlohmann::json{{"mode", mode}, {"portfolio_id", portfolio},
            {"strategy_id", strategy_id}, {"strategy_names", strategy_names}}.dump() << '\n';
        return 0;
    }

    const auto system = db.load_report_positions_by_date(
        strategy_id, strategy_names, portfolio, timestamp, "system");
    if (system.is_error()) { fail("strict system position read failed: " + std::string(system.error()->what())); return 5; }
    const trade_ngin::StrategyPositionRows system_rows = system.value();
    const auto snapshot = trade_ngin::load_qt_report_position_snapshot(
        db, strategy_id, strategy_names, portfolio, timestamp, system_rows);
    if (snapshot.is_error()) { fail("QT snapshot failed: " + std::string(snapshot.error()->what())); return 6; }
    if (mode == "snapshot-barrier") {
        std::string barrier_error;
        if (!snapshot_barrier(output_dir, barrier_error)) {
            fail("snapshot barrier failed: " + barrier_error);
            return 6;
        }
    }

    const std::unordered_map<std::string, double> prices{
        {"ES.v.0", 5000.0}, {"NG.v.0", 3.0}, {"NQ.v.0", 18000.0}};
    trade_ngin::CSVExporter exporter(output_dir.string());
    const auto csv = exporter.export_current_positions(timestamp, snapshot.value().by_strategy, prices,
                                                       1000000.0, 1000000.0, 0.0,
                                                       fixture_strategies(strategy_names), true);
    if (csv.is_error()) { fail("CSV export failed: " + std::string(csv.error()->what())); return 7; }

    trade_ngin::EmailSender email(trade_ngin::EmailSenderConfig{});
    const std::string html = email.generate_trading_report_body(
        snapshot.value().by_strategy, snapshot.value().combined, std::nullopt,
        {{"Current Portfolio Value", 1000000.0}, {"Gross Leverage", 1.0}, {"Net Leverage", 0.0}}, {},
        utc_date(timestamp), portfolio, true, prices, nullptr, {}, {}, {}, {});
    const auto html_path = output_dir / "qt_report_probe.html";
    std::ofstream html_file(html_path);
    if (!html_file) { fail("failed to open HTML output"); return 8; }
    html_file << html;
    if (!html_file) { fail("failed to write HTML output"); return 8; }

    std::cout << kResultTag << nlohmann::json{{"mode", mode}, {"portfolio_id", portfolio},
        {"strategy_id", strategy_id}, {"strategy_names", strategy_names},
        {"report_date", utc_date(timestamp)}, {"portfolio_type", "qt"},
        {"by_strategy", quantities(snapshot.value().by_strategy)},
        {"combined", quantities(snapshot.value().combined)}, {"csv_path", csv.value()},
        {"email_path", html_path.string()}}.dump() << '\n';
    return 0;
}
}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 7) { fail("usage: qt_report_probe <seed|snapshot|snapshot-barrier> <portfolio> <combined-strategy> <utc-epoch-seconds> <output-dir> <strategy-name>..."); return 2; }
    const std::string mode = argv[1];
    if (mode != "seed" && mode != "snapshot" && mode != "snapshot-barrier") {
        fail("mode must be seed, snapshot, or snapshot-barrier");
        return 2;
    }
    trade_ngin::Timestamp timestamp;
    try {
        std::size_t parsed = 0;
        const auto seconds = std::stoll(argv[4], &parsed);
        if (argv[4][parsed] != '\0') throw std::invalid_argument("trailing epoch input");
        timestamp = std::chrono::system_clock::from_time_t(seconds);
    } catch (const std::exception&) {
        fail("utc-epoch-seconds must be an integer");
        return 2;
    }

    std::vector<std::string> names;
    for (int index = 6; index < argc; ++index) names.emplace_back(argv[index]);
    try {
        return probe(mode, argv[2], argv[3], timestamp, argv[5], names);
    } catch (const std::exception& error) {
        fail("probe runtime failure: " + std::string(error.what()));
        return 9;
    }
}
