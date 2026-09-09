// E2-F30 (with E2-F61): what happens after main() returns.
//
// Two defects, one shutdown, one fix each:
//
// E2-F30 -- DatabasePool and StateManager are both function-local statics.
//   Statics are destroyed in reverse order of CONSTRUCTION, and a pooled
//   PostgresDatabase calls StateManager::instance().unregister_component() from
//   its own destructor. Reach the pool first and StateManager is constructed
//   second, so it is destroyed FIRST, and every pooled connection's shutdown
//   then takes a recursive_mutex inside a destroyed object. That is the five
//   "recursive mutex" warnings on every live run.
//
// E2-F61 -- the same shutdown logs "Disconnected from PostgreSQL database"
//   through Logger::format_message, which read `static thread_local std::string
//   current_component_`. On the main thread that string is destroyed alongside
//   the statics, so the log line was prefixed with the freed bytes of a dead
//   std::string.
//
// Neither is observable from an ordinary test: both happen after the last test
// body has run, when gtest can no longer assert anything. So these are DEATH
// tests. The child process reaches the singletons in the order that provokes the
// bug, then calls std::exit(0), which runs exactly the static destruction the
// defects live in; the parent inspects the child's exit status and its output.
//
// Ordering inside the child matters and is the whole point:
//   DatabasePool::instance() is touched BEFORE anything else registers a
//   component. Without the fix that is what puts StateManager second in the
//   construction order. A child that touched StateManager first would shut down
//   cleanly with or without the fix and would prove nothing.
//
// Reachability gate matches the other DB-backed tests: the pool needs a real
// server to hold a connection whose destructor does the unregistering.

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <pqxx/pqxx>

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

std::string dsn() {
    const char* d = std::getenv("TRADE_NGIN_TEST_DSN");
    return (d && *d) ? std::string(d) : std::string();
}

// Where the child re-points its stderr, so the parent can read what static
// destruction printed. gtest's own stderr capture cannot be used here: the death
// test already owns fd 2, and calling CaptureStderr around EXPECT_EXIT aborts.
const char* kChildStderr = "/tmp/trade_ngin_t1_shutdown_output.txt";

// Runs in the re-exec'd child. Everything after the freopen goes to kChildStderr.
//
// The ORDER here is the test. It mirrors a real runner: the logger is set up
// first, as main() does before anything else, and the database pool is the first
// thing to reach StateManager -- because StateManager is constructed lazily by
// PostgresDatabase::connect() inside the pool's own initialize(). That puts the
// pool ahead of StateManager in construction order and therefore behind it in
// destruction order, which is E2-F30 exactly.
void shutdown_through_the_pool() {
    // 1. Logger first, as every runner's main() does. Without this the runner
    //    takes a "Logger not initialized" path that never calls
    //    format_message(), so E2-F61's destroyed-thread_local read never
    //    happens and its test would pass vacuously. Console destination keeps
    //    output on stderr and writes no files.
    LoggerConfig lc;
    lc.destination = LogDestination::CONSOLE;
    lc.min_level = LogLevel::INFO;
    Logger::instance().initialize(lc);
    Logger::register_component("SHUTDOWN_PROBE_COMPONENT");

    // 2. The pool is the first thing to touch StateManager, via the
    //    PostgresDatabase objects it builds.
    auto init = DatabasePool::instance().initialize(dsn(), 2);
    if (init.is_error()) {
        std::fprintf(stderr, "POOL_INIT_FAILED\n");
        std::exit(2);
    }

    // 3. Take and return a connection so the pool really holds live
    //    PostgresDatabase objects with component registrations to undo.
    {
        auto guard = DatabasePool::instance().acquire_connection();
        if (!guard.get()) {
            std::fprintf(stderr, "ACQUIRE_FAILED\n");
            std::exit(3);
        }
    }

    // 4. Re-point the child's output at a file the parent can read AFTER the
    //    child is gone. BOTH streams: Logger::write_to_console_unsafe uses
    //    std::cout (logger.cpp:236) while the runtime's own destruction
    //    complaints go to stderr, and this test needs both halves of the
    //    shutdown in one place.
    std::cout.flush();
    std::cerr.flush();
    if (std::freopen(kChildStderr, "w", stdout) == nullptr) std::exit(4);
    if (dup2(fileno(stdout), fileno(stderr)) == -1) std::exit(5);
    std::fprintf(stdout, "CHILD_REACHED_EXIT\n");
    std::fflush(stdout);

    // 5. std::exit runs static destructors. Everything above was setup; this
    //    line is the test.
    std::exit(0);
}

std::string read_child_stderr() {
    std::ifstream in(kChildStderr, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void clear_child_stderr() { std::remove(kChildStderr); }

// Deliberately raw libpqxx, NOT PostgresDatabase.
//
// PostgresDatabase's constructor calls Logger::register_component and its
// connect() registers with StateManager -- so probing with one would construct
// StateManager before the test body ever reaches DatabasePool, which is exactly
// the ordering the test exists to get wrong. Reachability has to be established
// without touching either singleton.
bool db_available() {
    if (dsn().empty()) return false;
    try {
        pqxx::connection c(dsn());
        return c.is_open();
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

class StaticDestructionOrderTest : public ::testing::Test {
protected:
    void SetUp() override {
        // "threadsafe" makes each death test RE-EXEC this binary rather than
        // fork(). That matters here and nowhere else: a forked child inherits
        // the parent's already-constructed statics, so StateManager would
        // already exist and the broken construction order could never be
        // reproduced -- the test would pass with or without the fix. A fresh
        // exec builds every singleton from nothing, in the order the test body
        // chooses.
        ::testing::FLAGS_gtest_death_test_style = "threadsafe";

        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();
        if (!db_available()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but no reachable TRADE_NGIN_TEST_DSN, so the "
                          "E2-F30 / E2-F61 shutdown goes unverified";
            }
            GTEST_SKIP() << "no reachable TRADE_NGIN_TEST_DSN; skipping";
        }
    }
};

// E2-F30: the process must reach the end of static destruction and exit 0.
TEST_F(StaticDestructionOrderTest, PoolShutdownDoesNotTouchADestroyedStateManager) {
    clear_child_stderr();
    EXPECT_EXIT(shutdown_through_the_pool(), ::testing::ExitedWithCode(0), ".*");
    EXPECT_NE(read_child_stderr().find("CHILD_REACHED_EXIT"), std::string::npos)
        << "the child never reached the exit that runs static destruction";
}

// E2-F30, sharper: the recursive-mutex warning must be gone from the shutdown.
// This is the line the audit counted five of on every live run.
TEST_F(StaticDestructionOrderTest, ShutdownEmitsNoRecursiveMutexWarning) {
    clear_child_stderr();
    EXPECT_EXIT(shutdown_through_the_pool(), ::testing::ExitedWithCode(0), ".*");
    const std::string err = read_child_stderr();
    EXPECT_EQ(err.find("recursive"), std::string::npos)
        << "static destruction still reports a recursive-mutex problem:\n" << err;
    EXPECT_EQ(err.find("mutex lock failed"), std::string::npos)
        << "static destruction still fails a mutex lock:\n" << err;
}

// E2-F61: the shutdown log line's component prefix must be a LIVE registered
// component, not a fragment of freed memory.
//
// The first shape of this test asked only that the prefix held printable bytes,
// and it passed with the defect in place -- a destroyed std::string does not
// reliably read back as unprintable rubbish. Measured pre-fix output was:
//
//   [INFO] [Disconnected fro] Disconnected from PostgreSQL database
//
// Sixteen perfectly printable characters: the freed inline buffer, reused by the
// allocator for the message string that the very same log call was building. So
// the tell is not "is it printable" but "is it the message looking at itself".
//
// The expected value is "PostgresDatabase", not the SHUTDOWN_PROBE_COMPONENT
// this test registers. That is not a compromise -- it is the correct answer.
// PostgresDatabase's constructor calls Logger::register_component("Postgres\
// Database") (postgres_database.cpp:29), and the pool builds its connections
// after this test registers its own name, so the last live value on the thread
// is that one. What matters is that it is a value something actually
// registered, still readable after main() has returned.
TEST_F(StaticDestructionOrderTest, ShutdownLogPrefixIsALiveRegisteredComponent) {
    clear_child_stderr();
    EXPECT_EXIT(shutdown_through_the_pool(), ::testing::ExitedWithCode(0), ".*");
    const std::string out = read_child_stderr();

    const std::string msg = "Disconnected from PostgreSQL database";
    ASSERT_NE(out.find(msg), std::string::npos)
        << "static destruction never logged the disconnect, so this test would pass "
           "vacuously; child output was:\n" << out;

    size_t at = out.find(msg);
    int checked = 0;
    while (at != std::string::npos) {
        size_t line_start = out.rfind('\n', at);
        line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
        const std::string prefix = out.substr(line_start, at - line_start);

        EXPECT_NE(prefix.find("[PostgresDatabase]"), std::string::npos)
            << "the component prefix is not the component that registered last -- E2-F61, "
               "Logger read its thread_local after that string was destroyed. Prefix was: '"
            << prefix << "'";

        // The specific corruption seen pre-fix: the prefix becomes the head of
        // the message itself, because the allocator reused the freed buffer.
        EXPECT_EQ(prefix.find(msg.substr(0, 8)), std::string::npos)
            << "the component prefix contains the head of the message it is prefixing, "
               "which is freed memory reused by the message string (E2-F61). Prefix was: '"
            << prefix << "'";
        ++checked;
        at = out.find(msg, at + 1);
    }
    EXPECT_GT(checked, 0);
}
