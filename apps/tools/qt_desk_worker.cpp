#include "qt_desk_connection.hpp"
#include "trade_ngin/apps/qt_desk_dispatcher.hpp"

#include <pqxx/pqxx>
#include <uuid/uuid.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using trade_ngin::QtDispatchDisposition;
using trade_ngin::QtDispatchJob;
using trade_ngin::QtDispatchPhase;
using trade_ngin::QtDeskDispatcherConfig;
using trade_ngin::qt_dispatch_classify_exit;
using trade_ngin::qt_dispatch_ids;
using trade_ngin::qt_dispatch_prepare_arguments;
using trade_ngin::qt_dispatch_retry_delay;
using trade_ngin::qt_dispatch_run_arguments;
using trade_ngin::qt_dispatch_first_day_prepare_arguments;
using trade_ngin::qt_dispatch_first_day_run_arguments;
using namespace trade_ngin::qt_desk_cli;

std::atomic_bool stopping{false};
void stop(int) { stopping = true; }

void log(const std::string& status, const std::string& decision = {}) {
    std::cout << "{\"schema\":\"qt-desk-worker/v1\",\"status\":\"" << status << "\"";
    if (!decision.empty()) std::cout << ",\"decision_id\":\"" << decision << "\"";
    std::cout << "}\n" << std::flush;
}

std::string random_uuid() {
    uuid_t value{}; uuid_generate_random(value);
    char rendered[37]{}; uuid_unparse_lower(value, rendered); return rendered;
}

bool pinned_tool(const std::string& value) {
    try {
        const std::filesystem::path path(value);
        return path.is_absolute() && std::filesystem::is_regular_file(path) &&
            ::access(value.c_str(), X_OK) == 0 &&
            std::filesystem::canonical(path) == path.lexically_normal();
    } catch (...) { return false; }
}

struct Options {
    int connection_fd = -1;
    std::string prepare_tool;
    std::string run_tool;
    bool once = false;
    bool healthcheck = false;
};

bool parse(int argc, char** argv, Options& out) {
    for (int index = 1; index < argc; ++index) {
        const std::string name(argv[index]);
        if (name == "--once") out.once = true;
        else if (name == "--healthcheck") out.healthcheck = true;
        else {
            if (++index >= argc) return false;
            const std::string value(argv[index]);
            if (name == "--connection-fd") {
                if (value.empty() || !std::all_of(value.begin(), value.end(),
                        [](unsigned char character) {
                            return character >= '0' && character <= '9';
                        })) return false;
                const auto parsed = std::from_chars(
                    value.data(), value.data() + value.size(), out.connection_fd);
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != value.data() + value.size()) return false;
            } else if (name == "--prepare-tool") out.prepare_tool = value;
            else if (name == "--run-tool") out.run_tool = value;
            else return false;
        }
    }
    if (out.connection_fd < 3) return false;
    return out.healthcheck || (pinned_tool(out.prepare_tool) && pinned_tool(out.run_tool));
}

struct Claimed {
    QtDispatchJob job;
    unsigned attempt_number = 0;
};

void discover(pqxx::connection& connection) {
    pqxx::work tx(connection);
    const auto rows = tx.exec(R"SQL(
      SELECT d.decision_id::text
        FROM trading.qt_decisions d
        JOIN trading.qt_model_seed_publications p ON p.publication_id=d.model_publication_id
        JOIN trading.strategy_registry r ON r.id='trendfollowing'
          AND r.strategy_type='LIVE_TREND_FOLLOWING' AND r.lifecycle='live' AND r.is_active
          AND (r.portfolio_id='CONSERVATIVE_PORTFOLIO' OR EXISTS(
            SELECT 1 FROM trading.strategy_book_memberships m
             WHERE m.strategy_id=r.id AND m.portfolio_id='CONSERVATIVE_PORTFOLIO'))
        LEFT JOIN trading.qt_desk_dispatch_jobs j ON j.decision_id=d.decision_id
        LEFT JOIN trading.qt_desk_receipts x ON x.decision_id=d.decision_id
       WHERE d.status='confirmed_decision' AND d.book_id='CONSERVATIVE_PORTFOLIO'
         AND p.portfolio_id=d.book_id AND p.strategy_id='LIVE_TREND_FOLLOWING'
         AND p.source_day=d.source_day AND j.decision_id IS NULL AND x.decision_id IS NULL
       ORDER BY d.created_at,d.decision_id
    )SQL");
    for (const auto row : rows) {
        const auto id = row[0].as<std::string>();
        const auto ids = qt_dispatch_ids(id);
        tx.exec("SELECT trading.enqueue_qt_desk_dispatch(" + tx.quote(id) + "::uuid," +
            tx.quote(ids.attempt_id) + "::uuid," + tx.quote(ids.input_id) + "::uuid," +
            tx.quote(ids.market_source_id) + "::uuid," + tx.quote(ids.finalization_id) + "::uuid)");
        log("queued", id);
    }
    tx.commit();
}

std::optional<Claimed> claim(pqxx::connection& connection, const std::string& owner,
                             const QtDeskDispatcherConfig& config) {
    pqxx::work tx(connection);
    const auto rows = tx.exec("SELECT decision_id::text,source_day::text,attempt_id::text,input_id::text,"
        "market_source_id::text,finalization_id::text,attempt_number FROM trading.claim_qt_desk_dispatch(" +
        tx.quote(owner) + "::uuid," + tx.quote(std::to_string(config.lease_duration.count()) + " seconds") + "::interval)");
    if (rows.empty()) { tx.commit(); return std::nullopt; }
    if (rows.size() != 1) throw std::runtime_error("qt_dispatch_claim_cardinality");
    Claimed result{{rows[0][1].as<std::string>(), rows[0][0].as<std::string>(),
        rows[0][2].as<std::string>(), rows[0][3].as<std::string>(),
        rows[0][4].as<std::string>(), rows[0][5].as<std::string>()},
        rows[0][6].as<unsigned>()};
    tx.commit(); return result;
}

void renew(pqxx::connection& connection, const Claimed& claimed, const std::string& owner,
           const QtDeskDispatcherConfig& config) {
    pqxx::work tx(connection);
    tx.exec("SELECT trading.renew_qt_desk_dispatch(" + tx.quote(claimed.job.decision_id) + "::uuid," +
        tx.quote(owner) + "::uuid," + tx.quote(std::to_string(config.lease_duration.count()) + " seconds") + "::interval)");
    tx.commit();
}

void finish(pqxx::connection& connection, const Claimed& claimed, const std::string& owner,
            const std::string& phase, QtDispatchDisposition disposition, const std::string& error) {
    const char* outcome = disposition == QtDispatchDisposition::Succeeded ? "succeeded" :
        disposition == QtDispatchDisposition::Retry ? "retry" : "dead_letter";
    pqxx::work tx(connection);
    std::string retry = "NULL";
    if (disposition == QtDispatchDisposition::Retry)
        retry = tx.quote(std::to_string(qt_dispatch_retry_delay(claimed.attempt_number).count()) + " seconds") + "::interval";
    tx.exec("SELECT trading.finish_qt_desk_dispatch(" + tx.quote(claimed.job.decision_id) + "::uuid," +
        tx.quote(owner) + "::uuid," + tx.quote(phase) + "," + tx.quote(outcome) + "," +
        (error.empty() ? "NULL" : tx.quote(error)) + "," + retry + ")");
    tx.commit();
}

std::optional<std::string> prior_decision(pqxx::connection& connection, const Claimed& claimed) {
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec("SELECT d.decision_id::text FROM trading.qt_decisions d "
        "JOIN trading.qt_desk_receipts r ON r.decision_id=d.decision_id AND r.status='processed' "
        "WHERE d.book_id='CONSERVATIVE_PORTFOLIO' AND d.source_day<" + tx.quote(claimed.job.source_day) +
        "::date ORDER BY d.source_day DESC,d.created_at DESC,d.decision_id DESC LIMIT 1");
    if (rows.empty()) return std::nullopt;
    return rows[0][0].as<std::string>();
}

std::pair<std::string,std::string> lease_times(pqxx::connection& connection) {
    pqxx::read_transaction tx(connection);
    const auto row = tx.exec(R"SQL(
      SELECT to_char(clock_timestamp() AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS"Z"'),
             to_char((clock_timestamp()+interval '10 minutes') AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS"Z"')
    )SQL");
    return {row[0][0].as<std::string>(),row[0][1].as<std::string>()};
}

int child(const std::string& tool, const std::vector<std::string>& arguments,
          const std::string& connection_material, pqxx::connection& connection,
          const Claimed& claimed, const std::string& owner,
          const QtDeskDispatcherConfig& config) {
    const int source_fd = memfd_create("qt-worker-connection", MFD_CLOEXEC);
    if (source_fd < 0 || write(source_fd, connection_material.data(), connection_material.size()) !=
        static_cast<ssize_t>(connection_material.size())) {
        if (source_fd >= 0) close(source_fd);
        return 70;
    }
    const int child_fd = fcntl(source_fd, F_DUPFD, 3);
    if (child_fd < 3) { close(source_fd); return 70; }
    fcntl(child_fd, F_SETFD, fcntl(child_fd, F_GETFD) & ~FD_CLOEXEC);
    auto supplied = arguments;
    for (std::size_t i = 0; i + 1 < supplied.size(); ++i)
        if (supplied[i] == "--connection-fd") supplied[i + 1] = std::to_string(child_fd);
    const pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> argv{const_cast<char*>(tool.c_str())};
        for (auto& value : supplied) argv.push_back(value.data());
        argv.push_back(nullptr); execv(tool.c_str(), argv.data()); _exit(127);
    }
    close(child_fd); close(source_fd);
    if (pid < 0) return 70;
    int status = 0;
    unsigned seconds = 0;
    while (true) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0) {
            if (errno == EINTR) continue;
            return 70;
        }
        if (stopping || seconds >= 600) {
            kill(pid, SIGTERM);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return 70;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (++seconds % 60 == 0) {
            try {
                renew(connection, claimed, owner, config);
            } catch (...) {
                kill(pid, SIGTERM);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                throw;
            }
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 70;
}

bool healthy(pqxx::connection& connection) {
    pqxx::read_transaction tx(connection);
    const auto row = tx.exec(R"SQL(
      SELECT to_regclass('trading.qt_desk_dispatch_jobs') IS NOT NULL
         AND to_regclass('trading.qt_desk_dispatch_attempts') IS NOT NULL
         AND to_regprocedure('trading.claim_qt_desk_dispatch(uuid,interval)') IS NOT NULL
         AND (SELECT count(*) FROM trading.strategy_registry r
               WHERE r.id='trendfollowing' AND r.strategy_type='LIVE_TREND_FOLLOWING'
                 AND r.lifecycle='live' AND r.is_active
                 AND (r.portfolio_id='CONSERVATIVE_PORTFOLIO' OR EXISTS(
                   SELECT 1 FROM trading.strategy_book_memberships m
                    WHERE m.strategy_id=r.id
                      AND m.portfolio_id='CONSERVATIVE_PORTFOLIO')))=1
    )SQL");
    return row.size() == 1 && row[0][0].as<bool>();
}

void alert_old_queue(pqxx::connection& connection, const QtDeskDispatcherConfig& config) {
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec("SELECT decision_id::text FROM trading.qt_desk_dispatch_jobs WHERE "
        "state NOT IN ('succeeded','dead_letter') AND first_seen_at<clock_timestamp()-" +
        tx.quote(std::to_string(config.alert_age.count()) + " seconds") + "::interval ORDER BY first_seen_at");
    for (const auto row : rows) log("queue_age_alert", row[0].as<std::string>());
}
}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) { log("invalid_arguments"); return 2; }
    std::string material, dsn;
    if (!read_connection(options.connection_fd, material) || !explicit_local_connection(material, dsn)) {
        log("connection_input_unavailable"); return 3;
    }
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    try {
        pqxx::connection connection(dsn);
        std::fill(dsn.begin(), dsn.end(), '\0');
        {
            pqxx::work identity(connection);
            identity.exec("SET application_name = 'qt_desk_worker'");
            identity.commit();
        }
        if (options.healthcheck) {
            const bool ok = healthy(connection); log(ok ? "healthy" : "unhealthy");
            std::fill(material.begin(), material.end(), '\0'); return ok ? 0 : 1;
        }
        const QtDeskDispatcherConfig config;
        const auto owner = random_uuid();
        while (!stopping) {
            discover(connection); alert_old_queue(connection, config);
            auto claimed = claim(connection, owner, config);
            if (!claimed) {
                if (options.once) break;
                std::this_thread::sleep_for(config.poll_interval); continue;
            }
            log("claimed", claimed->job.decision_id);
            const auto prior = prior_decision(connection, *claimed);
            const auto times = lease_times(connection);
            int exit_code = child(options.prepare_tool,
                prior?qt_dispatch_prepare_arguments(claimed->job,*prior,times.first,times.second,3):
                      qt_dispatch_first_day_prepare_arguments(claimed->job,times.first,times.second,3),
                material, connection, *claimed, owner, config);
            auto disposition = qt_dispatch_classify_exit(
                QtDispatchPhase::Prepare, exit_code, claimed->attempt_number, config.max_attempts);
            if (disposition != QtDispatchDisposition::Succeeded) {
                finish(connection, *claimed, owner, "prepare", disposition,
                       "prepare_exit_" + std::to_string(exit_code));
                log(disposition == QtDispatchDisposition::Retry ? "retry_wait" : "dead_letter",
                    claimed->job.decision_id); continue;
            }
            exit_code = child(options.run_tool, prior?qt_dispatch_run_arguments(claimed->job,3):
                qt_dispatch_first_day_run_arguments(claimed->job,3),
                material, connection, *claimed, owner, config);
            disposition = qt_dispatch_classify_exit(
                QtDispatchPhase::Run, exit_code, claimed->attempt_number, config.max_attempts);
            finish(connection, *claimed, owner, "run", disposition,
                   disposition == QtDispatchDisposition::Succeeded ? std::string() :
                   "run_exit_" + std::to_string(exit_code));
            log(disposition == QtDispatchDisposition::Succeeded ? "succeeded" :
                disposition == QtDispatchDisposition::Retry ? "retry_wait" : "dead_letter",
                claimed->job.decision_id);
        }
        std::fill(material.begin(), material.end(), '\0'); log("stopped"); return 0;
    } catch (const std::exception&) {
        std::fill(material.begin(), material.end(), '\0'); log("worker_unavailable"); return 1;
    }
}
