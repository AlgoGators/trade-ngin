// tests/live/test_mode_branch_guard.cpp
//
// T-DATED safeguard S2 (HD 2026-10-10). THIS TEST READS SOURCE, on purpose: it is a guard, not a
// behaviour test.
//
// A live runner has three ways of being run: with no date, with a date equal to the host's date
// (production), and with a past date (every test run). Stage 3 found protections that existed in
// only one of them. This guard fails when a code line of the apps, or of the library they call,
// reads one of the things a run's mode can be told from, outside the lists below:
//
//   the date flag            use_override_date, dated_run
//   the host-date test       run_date_is_host_date
//   a clock                  system_clock, steady_clock, time(), localtime, localtime_r,
//                            gettimeofday, clock_gettime, and the DATABASE's clock in SQL text
//                            (now(), CURRENT_DATE, CURRENT_TIMESTAMP)
//   the environment          getenv
//   in the apps, also        send_email, email_plan.send, write_body_file (production always
//                            mails and a test run never does, so the mail flag is a mode flag in
//                            practice) and the two refusal rulings (hole_ruling.refuse,
//                            freshness_ruling.refuse)
//
// The lists hold the LINES, in order, not counts: a read added and another removed in the same
// file is seen. Every file under apps/ is scanned, not only the three live runners. Comment lines
// are left out by following the comment markers; a code line that merely starts with `*` is code.
// What it cannot see: a decision that reads none of these words (a helper that wraps them under
// another name is caught where the helper is defined, in the library list).
//
// TO ADD A SITE: add its line here with what it is for, and, when it DECIDES anything, name the
// test that EXECUTES both of its arms. A site that only stamps a row or a log line says so.
// Whoever adds a new mode branch has to write that test first.
//
// The second half of the file plants each form in a copy of the real text and requires the guard
// to fail on it.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::filesystem::path repo_root() {
    auto dir = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
        if (std::filesystem::exists(dir / "apps/strategies/live_portfolio_conservative.cpp")) return dir;
        if (!dir.has_parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_text(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trimmed(const std::string& line) {
    const auto a = line.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    const auto b = line.find_last_not_of(" \t\r");
    return line.substr(a, b - a + 1);
}

// The code lines of a text that match the pattern, trimmed, in order. A line is a comment when it
// starts with `//`, or lies inside a block comment opened by a line that starts with `/*`.
std::vector<std::string> matching_lines(const std::string& text, const std::regex& what) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    bool in_block = false;
    while (std::getline(in, line)) {
        const std::string t = trimmed(line);
        if (in_block) {
            if (t.find("*/") != std::string::npos) in_block = false;
            continue;
        }
        if (t.empty() || t.rfind("//", 0) == 0) continue;
        if (t.rfind("/*", 0) == 0) {
            if (t.find("*/") == std::string::npos) in_block = true;
            continue;
        }
        // std::regex is slow: a line holding none of the patterns' literal stems cannot match.
        bool candidate = false;
        for (const char* stem : {"now", "NOW", "time", "date", "DATE", "TIMESTAMP", "getenv",
                                 "send", "refuse", "body_file", "dated_run"}) {
            if (t.find(stem) != std::string::npos) {
                candidate = true;
                break;
            }
        }
        if (candidate && std::regex_search(t, what)) out.push_back(t);
    }
    return out;
}

const std::regex kLibrarySites(
    R"RX(use_override_date|dated_run|run_date_is_host_date\(|system_clock::now\(|steady_clock::now\(|high_resolution_clock::now\(|localtime(_r)?\(|gettimeofday\(|clock_gettime\(|(^|[^_a-zA-Z0-9])time\(|CURRENT_DATE|CURRENT_TIMESTAMP|(^|[^_a-zA-Z0-9:.>])(now|NOW)\(\)|getenv\()RX");
const std::regex kAppSites(
    R"RX(use_override_date|dated_run|run_date_is_host_date\(|system_clock::now\(|steady_clock::now\(|high_resolution_clock::now\(|localtime(_r)?\(|gettimeofday\(|clock_gettime\(|(^|[^_a-zA-Z0-9])time\(|CURRENT_DATE|CURRENT_TIMESTAMP|(^|[^_a-zA-Z0-9:.>])(now|NOW)\(\)|getenv\(|send_email|email_plan\.send|write_body_file|hole_ruling\.refuse|freshness_ruling\.refuse)RX");

struct Site {
    const char* line;  ///< the trimmed code line
    const char* what;  ///< what it is for; a DECISION names the test that executes both arms
};

// --- the futures twins (the same lines, in the same order, in both) ---------------------------
const std::vector<Site> kFuturesSites = {
    {"bool use_override_date = false;",
     "the flag: a date argument was given"},
    {"bool send_email = false;  // Default to false for historical runs",
     "the flag: mail the report"},
    {"send_email = true;",
     "set by --send-email, or by a run with no date"},
    {"use_override_date = true;",
     "set by the argument parse"},
    {"if (!use_override_date) {",
     "DECISION: a run with no date is refused: usage, exit 2, nothing run and nothing sent "
     "(live/run_args.hpp, RunArgs.* in tests/live/test_run_args.cpp)"},
    {"if (!use_override_date) {",
     "QT (rule 2 of the rejoin): --finalize-system needs a date; unreachable after the "
     "dateless refusal above"},
    {"send_email = false;",
     "QT ruling 17: --finalize-system never e-mails"},
    {"if (!use_override_date || qt_audit_id > 0) {",
     "QT contract C7: --send needs a date and no --audit-id"},
    {"send_email = true;",
     "QT contract C7: --send e-mails a published day from its stored rows"},
    {"if (!use_override_date || qt_audit_id <= 0) {",
     "QT: --desk, --override, --publish, --fallback need a date and an --audit-id"},
    {"send_email =",
     "QT ruling 15 and contract C7: a desk run never e-mails; --publish or --fallback only "
     "with --send-now"},
    {"if (send_email && use_override_date) {",
     "a console line"},
    {"auto now = use_override_date ? target_date : std::chrono::system_clock::now();",
     "THE RUN CLOCK: the date given, or the wall clock on a run with no date"},
    {"core::safe_localtime(&now_time_t, &now_tm_fields);",
     "the run date's local-time fields, copied (console date, email date, a Sunday "
     "log line)"},
    {"auto end_date = use_override_date ? (now - std::chrono::hours(24)) : now;",
     "the bar window's end: T-1 on a dated run"},
    {"INFO(\"DEBUG: Run type: \" + std::string(use_override_date ? \"HISTORICAL\" : \"LIVE\"));",
     "a log line"},
    {"if (qt_desk_editable && qt_mode == qt::Mode::MODEL) send_email = false;",
     "QT ruling 15: on a desk-editable portfolio the daily e-mail is publish's; our books are "
     "not desk-editable"},
    {"send_email = false;",
     "QT contract C7: a published day's e-mail already sent is not sent again"},
    {"use_override_date, \"Futures\");",
     "DECISION: rule_feed_freshness(dated_run). Both arms executed by "
     "FeedFreshnessRuling.* (tests/live/test_held_feed_ruling.cpp)"},
    {"if (freshness_ruling.refuse) {",
     "acts on the ruling above: return 1, nothing of the day stored"},
    {"held_holes, hole_tolerance_days, now, std::chrono::system_clock::now());",
     "DECISION: the held-hole refusal is for the host's own date "
     "(rule_held_feed_holes). Both arms executed by HeldFeedRuling.* "
     "(tests/live/test_held_feed_ruling.cpp) and, on the real binary, by the "
     "production-mode chain of scripts/live_mode_gate/gate.sh"},
    {"if (hole_ruling.refuse) {",
     "acts on the ruling above: ERROR or WARN lines, then return 1 with nothing of "
     "the day stored"},
    {"if (hole_ruling.refuse) {",
     "acts on the ruling above: ERROR or WARN lines, then return 1 with nothing of "
     "the day stored"},
    {"if (!use_override_date) {",
     "the processing-time print: a console line"},
    {"std::chrono::system_clock::now() - now)",
     "the processing-time print"},
    {"live::plan_email_report(send_email, std::getenv(live::kEmailBodyDirEnv));",
     "the report plan (send, or write the body to a file, or neither): "
     "EmailReportPlan.* (tests/live/test_email_body_file.cpp)"},
    {"if (email_plan.send) {",
     "DECISION: mail, on --send-email only. The plan: EmailReportPlan.*; the send "
     "arm itself is never run by a test, by rule (it mails)"},
    {"auto send_result = email_sender->send_email(mail_subject, mail_body,",
     "the send itself, handed to deliver_email_report as its sender; the file-or-send "
     "choice is made there: EmailDelivery.* (tests/live/test_email_delivery.cpp)"},
};

// --- the equity runner ------------------------------------------------------------------------
const std::vector<Site> kEquitySites = {
    {"if (const char* env = std::getenv(\"TRADE_NGIN_STATE_DIR\")) {",
     "where the state files are; the same in every mode"},
    {"bool use_override_date = false;",
     "the flag: a date argument was given"},
    {"bool send_email = false;  // Default to false for historical runs",
     "the flag: mail the report"},
    {"send_email = true;",
     "set by --send-email, or by a run with no date"},
    {"use_override_date = true;",
     "set by the argument parse"},
    {"if (!use_override_date) {",
     "DECISION: a run with no date is refused: usage, exit 2, nothing run and nothing sent "
     "(live/run_args.hpp, RunArgs.* in tests/live/test_run_args.cpp)"},
    {"if (send_email && use_override_date) {",
     "a console line"},
    {"if (use_override_date) {",
     "the log directory of a dated run (logs/<date>/): a file name"},
    {"auto now = use_override_date ? target_date : std::chrono::system_clock::now();",
     "THE RUN CLOCK: the date given, or the wall clock on a run with no date"},
    {"auto end_date = use_override_date ? (now - std::chrono::hours(24)) : now;",
     "the bar window's end: T-1 on a dated run"},
    {"DEBUG(\"Run type: \" + std::string(use_override_date ? \"HISTORICAL\" : \"LIVE\"));",
     "a log line"},
    {"use_override_date, \"Equity\");",
     "DECISION: rule_feed_freshness(dated_run). Both arms executed by "
     "FeedFreshnessRuling.* (tests/live/test_held_feed_ruling.cpp)"},
    {"if (freshness_ruling.refuse) {",
     "acts on the ruling above: return 1, nothing of the day stored"},
    {"feed_holes.held, hole_tolerance_days, now, std::chrono::system_clock::now());",
     "DECISION: the held-hole refusal is for the host's own date "
     "(rule_held_feed_holes). Both arms executed by HeldFeedRuling.* "
     "(tests/live/test_held_feed_ruling.cpp) on a LOCAL-midnight run instant; the "
     "equity runner parses its date as UTC midnight, so on a host west of UTC its "
     "refusing arm is not reached on the run's own date (recorded, its own issue)"},
    {"if (hole_ruling.refuse) {",
     "acts on the ruling above: ERROR or WARN lines, then return 1 with nothing of "
     "the day stored"},
    {"if (hole_ruling.refuse) {",
     "acts on the ruling above: ERROR or WARN lines, then return 1 with nothing of "
     "the day stored"},
    {"if (!use_override_date) {",
     "the processing-time print: a console line"},
    {"std::chrono::system_clock::now() - now).count() << \"ms\" << std::endl;",
     "the processing-time print"},
    {"live::plan_email_report(send_email, std::getenv(live::kEmailBodyDirEnv));",
     "the report plan (send, or write the body to a file, or neither): "
     "EmailReportPlan.* (tests/live/test_email_body_file.cpp)"},
    {"if (email_plan.send) {",
     "DECISION: mail, on --send-email only. The plan: EmailReportPlan.*; the send "
     "arm itself is never run by a test, by rule (it mails)"},
    {"email_sender->send_email(mail_subject, mail_body, true, attachments);",
     "the send itself, handed to deliver_email_report as its sender; the file-or-send "
     "choice is made there: EmailDelivery.* (tests/live/test_email_delivery.cpp)"},
};

// --- every other file under apps/ (none is a live runner) --------------------------------------
const std::map<std::string, std::vector<std::string>> kOtherAppSites = {
    {"apps/backtest/bt_equity_mean_reversion.cpp", {"auto now = std::chrono::system_clock::now();"}},
    {"apps/backtest/bt_equity_validation.cpp", {"auto now = std::chrono::system_clock::now();"}},
    {"apps/backtest/bt_portfolio.cpp", {"auto now = std::chrono::system_clock::now();"}},
    {"apps/backtest/bt_portfolio_conservative.cpp", {"auto now = std::chrono::system_clock::now();"}},
    {"apps/test_database_extensions.cpp", {"auto now = std::chrono::system_clock::now();", "std::chrono::system_clock::now().time_since_epoch())"}},
};

// --- the library: the lines of each file, and what they are ------------------------------------
// A file not listed must hold none. A list that no longer matches means a read was added, removed
// or changed: look at it, and say here what it is.
const std::map<std::string, std::pair<const char*, std::vector<std::string>>> kLibrarySiteLines = {
    {"include/trade_ngin/backtest/backtest_types.hpp",
     {"formats a timestamp it is given; backtest only",
      {"core::safe_localtime(&time_t, &tm);"}}},
    {"include/trade_ngin/core/holiday_checker.hpp",
     {"where the holiday calendar file is; the same in every mode",
      {"if (const char* env = std::getenv(\"TRADE_NGIN_HOLIDAYS_JSON\")) {"}}},
    {"include/trade_ngin/core/time_utils.hpp",
     {"safe_localtime itself (localtime_r), and a formatting helper's default "
      "argument",
      {"inline std::tm* safe_localtime(const std::time_t* time, std::tm* result) {",
       "return localtime_r(time, result);",
       "auto now = std::chrono::system_clock::now();",
       "safe_localtime(&now_c, &result);"}}},
    {"include/trade_ngin/core/timed_process.hpp",
     {"the chart process's time limit (a steady clock, a duration). Executed by "
      "TimedProcess.*",
      {"const auto deadline = std::chrono::steady_clock::now() + timeout;",
       "deadline - std::chrono::steady_clock::now());",
       "if (std::chrono::steady_clock::now() >= until) return false;",
       "reap_until(std::chrono::steady_clock::now() + std::chrono::seconds(2));"}}},
    {"include/trade_ngin/live/data_freshness.hpp",
     {"DECISION: rule_feed_freshness(dated_run), two arms: a run with no date refuses "
      "on an absent or stale symbol, a dated run goes on. Both arms executed by "
      "FeedFreshnessRuling.* (tests/live/test_held_feed_ruling.cpp)",
      {"const std::string& as_of_ymd, bool dated_run,",
       "if (!dated_run) {",
       "if (dated_run) {"}}},
    {"include/trade_ngin/live/live_price_manager.hpp",
     {"a default argument no live runner uses",
      {"const Timestamp& reference_date = std::chrono::system_clock::now(),"}}},
    {"include/trade_ngin/live/session_book_gate.hpp",
     {"DECISION: run_date_is_host_date (host-local dates, localtime_r) and its one "
      "caller rule_held_feed_holes; the host clock is passed IN by the runner. Both "
      "arms executed by HeldFeedRuling.* and "
      "SessionBookGate.TheRefusalIsForTheHostsOwnDateOnly, and on the real binary by "
      "scripts/live_mode_gate/gate.sh",
      {"localtime_r(&tt, &tm);",
       "inline bool run_date_is_host_date(const Timestamp& run_now, const Timestamp& host_now) {",
       "ruling.refuse = run_date_is_host_date(run_now, host_now);"}}},
    {"include/trade_ngin/live/settled_stamp.hpp",
     {"the DATABASE's clock stamps settled_at when a day is finalised; a stamp, the "
      "same in every mode",
      {"return \"UPDATE trading.live_results SET settled_at = now() \""}}},
    {"include/trade_ngin/optimization/one_pass_record.hpp",
     {"TRADE_NGIN_SERIES_DUMP_DIR: a diagnostic dump, no decision",
      {"const char* dir = std::getenv(\"TRADE_NGIN_SERIES_DUMP_DIR\");",
       "const char* dir = std::getenv(\"TRADE_NGIN_SERIES_DUMP_DIR\");"}}},
    {"include/trade_ngin/strategy/trend_estimator_record.hpp",
     {"TRADE_NGIN_SERIES_DUMP_DIR: a diagnostic dump, no decision",
      {"const char* dir = std::getenv(\"TRADE_NGIN_SERIES_DUMP_DIR\");"}}},
    {"src/backtest/backtest_coordinator.cpp",
     {"backtest only",
      {"if (const char* dir = std::getenv(\"TRADE_NGIN_SERIES_DUMP_DIR\"); dir != nullptr && *dir) {",
       "strategy_names, std::chrono::system_clock::now());"}}},
    {"src/backtest/backtest_metrics_calculator.cpp",
     {"formats a timestamp it is given; backtest only",
      {"core::safe_localtime(&time_t, &tm);"}}},
    {"src/core/config_loader.cpp",
     {"the backtest window; no live runner calls it",
      {"std::tm* local_tm = std::localtime(&now_time_t);"}}},
    {"src/core/email_sender.cpp",
     {"the MIME boundary of a sent mail; a chart query's column stamp",
      {"std::string outer_boundary = \"----=_Outer_\" + std::to_string(std::time(nullptr));",
       "std::string inner_boundary = \"----=_Inner_\" + std::to_string(std::time(nullptr));",
       "\"  CURRENT_TIMESTAMP AS \\\"time\\\",\""}}},
    {"src/core/logger.cpp",
     {"log file names and line stamps",
      {"auto now = std::chrono::system_clock::now();",
       "core::safe_localtime(&now_c, &time_info);",
       "auto now = std::chrono::system_clock::now();",
       "core::safe_localtime(&now_c, &time_info);"}}},
    {"src/core/state_manager.cpp",
     {"in-memory component stamps",
      {"it->second.last_update = std::chrono::system_clock::now();",
       "it->second.last_update = std::chrono::system_clock::now();"}}},
    {"src/data/credential_store.cpp",
     {"where the config and its key are; the same in every mode",
      {"const char* env_config = std::getenv(\"TRADING_CONFIG_PATH\");",
       "const char* env_key = std::getenv(\"TRADING_ENCRYPTION_KEY\");"}}},
    {"src/data/database_pooling.cpp",
     {"connection-pool wait timing",
      {"auto start_time = std::chrono::steady_clock::now();",
       "auto elapsed = std::chrono::steady_clock::now() - start_time;"}}},
    {"src/data/postgres_database.cpp",
     {"a component stamp, and the fallback when a stored last_update does not parse "
      "(WARN)",
      {"std::chrono::system_clock::now(),",
       "last_update = std::chrono::system_clock::now();",
       "last_update = std::chrono::system_clock::now();"}}},
    {"src/execution/execution_engine.cpp",
     {"the execution engine; no live runner uses it",
      {"std::chrono::system_clock::now(),",
       "std::chrono::system_clock::now(),  // start_time",
       "std::chrono::system_clock::now() - job.start_time);",
       "job.end_time = std::chrono::system_clock::now();",
       "active_job.end_time = std::chrono::system_clock::now();",
       "std::chrono::system_clock::now() - current_job.start_time);",
       "std::chrono::system_clock::now() - job.start_time);",
       "auto now = std::chrono::system_clock::now();"}}},
    {"src/instruments/equity.cpp",
     {"formats a timestamp it is given",
      {"std::tm* local_time = trade_ngin::core::safe_localtime(&time, &local_time_buffer);"}}},
    {"src/instruments/futures.cpp",
     {"is_tradeable(); no live caller",
      {"if (is_expired(std::chrono::system_clock::now())) {",
       "std::tm* local_time = trade_ngin::core::safe_localtime(&time, &local_time_buffer);"}}},
    {"src/instruments/option.cpp",
     {"options; no live runner uses them",
      {"auto now = std::chrono::system_clock::now();",
       "std::tm* local_time = trade_ngin::core::safe_localtime(&time, &local_time_buffer);",
       "auto now = std::chrono::system_clock::now();",
       "auto now = std::chrono::system_clock::now();"}}},
    {"src/live/qt_desk.cpp",
     {"QT (rule 2 of the rejoin): the desk command log's own timestamps (started_at, finished_at, "
      "email_sent_at, published_at, sent_at) are the database's clock, the same in every mode; "
      "QT_EMAIL_DISABLED keeps a QT send's e-mail in the command row when the engine has no SMTP. "
      "None of it is read by a model run of a book that is not desk-editable",
      {"\"started_at = COALESCE(started_at, now()) WHERE id = \" +",
       "\"COALESCE(started_at, now()) WHERE id = \" + std::to_string(id) +",
       "\", started_at = COALESCE(started_at, now()), finished_at = now() WHERE id = \" +",
       "\"jsonb_build_object('email_sent_at', now()::text) WHERE id = \" + std::to_string(id) +",
       "sql_literal(published_by) + \", published_at = now(), publish_source = \" +",
       "db, \"WITH m AS (UPDATE trading.live_run_metadata SET sent_at = now()\" +",
       "\"now()::text) WHERE id = \" + std::to_string(row_id) +",
       "const char* flag = std::getenv(\"QT_EMAIL_DISABLED\");"}}},
    {"src/live/live_pnl_manager.cpp",
     {"a snapshot stamp nothing stores",
      {"snapshot.timestamp = std::chrono::system_clock::now();"}}},
    {"src/order/order_manager.cpp",
     {"the order manager; no live runner uses it",
      {"std::chrono::system_clock::now(),",
       "order_id,   order, OrderStatus::NEW, 0.0, 0.0, \"\", \"\", std::chrono::system_clock::now(),",
       "it->second.last_update = std::chrono::system_clock::now();",
       "std::chrono::system_clock::now(),"}}},
    {"src/portfolio/portfolio_manager.cpp",
     {"a component stamp, and the fill-time fallback when no time is passed and no "
      "bar is loaded (not reached with bars loaded)",
      {"std::chrono::system_clock::now(),",
       ": (data.empty() ? std::chrono::system_clock::now()",
       ": (data.empty() ? std::chrono::system_clock::now()",
       ": (data.empty() ? std::chrono::system_clock::now()"}}},
    {"src/storage/backtest_results_manager.cpp",
     {"backtest only",
      {"start_date_(std::chrono::system_clock::now()),",
       "end_date_(std::chrono::system_clock::now()) {"}}},
    {"src/strategy/base_strategy.cpp",
     {"in-memory component stamps",
      {"id_ + \"_\" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());",
       "std::chrono::system_clock::now(),"}}},
    {"src/strategy/mean_reversion.cpp",
     {"the initial last_update of a position, overwritten with the run date before a "
      "store",
      {"pos.last_update = std::chrono::system_clock::now();"}}},
    {"src/strategy/regime_detector.cpp",
     {"not on a live runner's path",
      {"std::chrono::system_clock::now(),"}}},
    {"src/strategy/trend_following.cpp",
     {"the initial last_update of a position, overwritten with the run date before a "
      "store",
      {"pos.last_update = std::chrono::system_clock::now();"}}},
};

const char* const kFuturesRunner = "apps/strategies/live_portfolio_conservative.cpp";
const char* const kBaseRunner = "apps/strategies/live_portfolio.cpp";
const char* const kEquityRunner = "apps/strategies/live_equity_mean_reversion.cpp";

std::vector<std::string> lines_of(const std::vector<Site>& allowed) {
    std::vector<std::string> want;
    for (const auto& s : allowed) want.emplace_back(s.line);
    return want;
}

// THE GUARD ON A RUNNER: the matching lines of its text are exactly the listed ones, in order.
bool runner_is_clean(const std::string& text, const std::vector<Site>& allowed) {
    return matching_lines(text, kAppSites) == lines_of(allowed);
}

// The matching lines of every .cpp and .hpp under `tops`, by repository-relative path. The tree
// is read once per (tops, pattern) and kept. `planted` then replaces (or adds) the text of a
// path, so a form can be planted without touching a file and without reading the tree again.
std::map<std::string, std::vector<std::string>> scan_tree(
    const std::filesystem::path& root, const std::vector<std::string>& tops, const std::regex& what,
    const std::map<std::string, std::string>& planted = {}) {
    static std::map<std::string, std::map<std::string, std::vector<std::string>>> kept;
    std::string key;
    for (const auto& top : tops) key += top + "|";
    key += (&what == &kLibrarySites) ? "library" : "apps";
    auto it = kept.find(key);
    if (it == kept.end()) {
        std::map<std::string, std::vector<std::string>> scanned;
        for (const auto& top : tops) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root / top)) {
                if (!entry.is_regular_file()) continue;
                const auto ext = entry.path().extension().string();
                if (ext != ".cpp" && ext != ".hpp") continue;
                auto lines = matching_lines(read_text(entry.path()), what);
                if (!lines.empty()) {
                    scanned[std::filesystem::relative(entry.path(), root).generic_string()] =
                        std::move(lines);
                }
            }
        }
        it = kept.emplace(key, std::move(scanned)).first;
    }
    auto found = it->second;
    for (const auto& [relative, text] : planted) {
        auto lines = matching_lines(text, what);
        if (lines.empty()) {
            found.erase(relative);
        } else {
            found[relative] = std::move(lines);
        }
    }
    return found;
}

// THE GUARD ON THE LIBRARY.
bool library_is_clean(const std::filesystem::path& root,
                      const std::map<std::string, std::string>& planted = {}) {
    std::map<std::string, std::vector<std::string>> want;
    for (const auto& [file, entry] : kLibrarySiteLines) want[file] = entry.second;
    return scan_tree(root, {"src", "include"}, kLibrarySites, planted) == want;
}

// THE GUARD ON THE OTHER APPS: every file under apps/ but the three live runners.
bool other_apps_are_clean(const std::filesystem::path& root,
                          const std::map<std::string, std::string>& planted = {}) {
    auto found = scan_tree(root, {"apps"}, kAppSites, planted);
    for (const char* runner : {kFuturesRunner, kBaseRunner, kEquityRunner}) found.erase(runner);
    return found == kOtherAppSites;
}

// A copy of `text` with `line` put in after the first line that holds `anchor`.
std::string planted_after(const std::string& text, const std::string& anchor,
                          const std::string& line) {
    const auto at = text.find(anchor);
    if (at == std::string::npos) return {};
    const auto eol = text.find('\n', at);
    return text.substr(0, eol + 1) + line + "\n" + text.substr(eol + 1);
}

void expect_sites(const std::string& relative, const std::vector<Site>& allowed) {
    const auto root = repo_root();
    if (root.empty()) GTEST_SKIP() << "repository root not found from the test working directory";
    EXPECT_EQ(matching_lines(read_text(root / relative), kAppSites), lines_of(allowed))
        << relative
        << ": a line that reads the date flag, the host-date test, a clock, the environment, "
           "the mail flag or a refusal ruling was added, removed or changed. Add it to the list "
           "in tests/live/test_mode_branch_guard.cpp with what it is for and the test that "
           "executes both of its arms.";
}

}  // namespace

TEST(ModeBranchGuard, TheConservativeFuturesRunnerReadsTheModeOnlyAtTheListedSites) {
    expect_sites(kFuturesRunner, kFuturesSites);
}

TEST(ModeBranchGuard, TheBaseFuturesRunnerReadsTheModeOnlyAtTheListedSites) {
    expect_sites(kBaseRunner, kFuturesSites);
}

TEST(ModeBranchGuard, TheEquityRunnerReadsTheModeOnlyAtTheListedSites) {
    expect_sites(kEquityRunner, kEquitySites);
}

TEST(ModeBranchGuard, NoLiveRunnerKeepsThePointerStdLocaltimeReturns) {
    const auto root = repo_root();
    if (root.empty()) GTEST_SKIP() << "repository root not found";
    const std::regex pointer(R"(=\s*std::localtime\()");
    for (const char* app : {kFuturesRunner, kBaseRunner, kEquityRunner}) {
        EXPECT_TRUE(matching_lines(read_text(root / app), pointer).empty())
            << app << " keeps a pointer into the C library's static struct";
    }
}

TEST(ModeBranchGuard, TheLibraryReadsTheModeOnlyAtTheListedLines) {
    const auto root = repo_root();
    if (root.empty()) GTEST_SKIP() << "repository root not found";
    std::map<std::string, std::vector<std::string>> want;
    for (const auto& [file, entry] : kLibrarySiteLines) want[file] = entry.second;
    EXPECT_EQ(scan_tree(root, {"src", "include"}, kLibrarySites), want)
        << "a line that reads the date flag, the host-date test, a clock or the environment was "
           "added to, removed from or changed in the library. Say what it is in kLibrarySiteLines "
           "(tests/live/test_mode_branch_guard.cpp); if it DECIDES anything on a live run, name "
           "the test that executes both of its arms.";
}

TEST(ModeBranchGuard, NoOtherFileUnderAppsReadsTheModeOutsideItsListedLines) {
    const auto root = repo_root();
    if (root.empty()) GTEST_SKIP() << "repository root not found";
    auto found = scan_tree(root, {"apps"}, kAppSites);
    for (const char* runner : {kFuturesRunner, kBaseRunner, kEquityRunner}) found.erase(runner);
    EXPECT_EQ(found, kOtherAppSites)
        << "a file under apps/ other than the three live runners reads the mode: list its lines "
           "in kOtherAppSites, or, if it is a live runner, give it its own list";
}

// ---------------------------------------------------------------------------------------------
// The guard fails on each form it claims to catch: the form is planted in a copy of the real
// text and the guard's own comparison is run on the copy.
// ---------------------------------------------------------------------------------------------

TEST(ModeBranchGuardPlanted, EveryRunnerFormPlantedInARunnerFailsTheGuard) {
    const auto root = repo_root();
    if (root.empty()) GTEST_SKIP() << "repository root not found";
    const std::vector<std::pair<const char*, const char*>> forms = {
        {"the wall clock", "if (std::chrono::system_clock::now() > cutoff) return 1;"},
        {"the date flag", "if (use_override_date && held_holes.empty()) return 1;"},
        {"the mail flag", "if (send_email) { return 1; }"},
        {"the report plan's send arm", "if (email_plan.send && !positions.empty()) return 1;"},
        {"the report plan's file arm", "bool test_run = email_plan.write_body_file;"},
        {"a new decision on the held-hole ruling", "if (hole_ruling.refuse && is_sunday) {"},
        {"a new decision on the freshness ruling", "skip_checks = !freshness_ruling.refuse;"},
        {"the environment", "if (std::getenv(\"TRADE_NGIN_TEST_MODE\")) return 0;"},
        {"localtime_r", "localtime_r(&host_t, &host_tm);"},
        {"gettimeofday", "gettimeofday(&tv, nullptr);"},
        {"clock_gettime", "clock_gettime(CLOCK_REALTIME, &ts);"},
        {"time(&t)", "time(&host_t);"},
        {"std::time", "const auto host_t = std::time(nullptr);"},
        {"the database's date", "\" AND date = CURRENT_DATE\""},
        {"the database's clock", "\" AND created_at > now() - interval '1 day'\""},
        {"a code line that starts with a star", "*refuse_today = std::chrono::system_clock::now() > cutoff;"},
    };
    for (const auto& [runner, allowed] :
         {std::pair{kFuturesRunner, &kFuturesSites}, std::pair{kBaseRunner, &kFuturesSites},
          std::pair{kEquityRunner, &kEquitySites}}) {
        const std::string text = read_text(root / runner);
        ASSERT_TRUE(runner_is_clean(text, *allowed)) << runner;
        for (const auto& [what, line] : forms) {
            const std::string planted =
                planted_after(text, "bool use_override_date = false;", std::string("        ") + line);
            ASSERT_FALSE(planted.empty()) << runner;
            EXPECT_FALSE(runner_is_clean(planted, *allowed))
                << runner << ": the guard does not see " << what << ": " << line;
        }
        // and a planted COMMENT changes nothing
        EXPECT_TRUE(runner_is_clean(
            planted_after(text, "bool use_override_date = false;",
                          "        // if (send_email) return 1;\n        /*\n         * "
                          "std::chrono::system_clock::now()\n         */"),
            *allowed))
            << runner;
    }
}

TEST(ModeBranchGuardPlanted, EveryLibraryFormPlantedInTheLibraryFailsTheGuard) {
    const auto root = repo_root();
    if (root.empty()) GTEST_SKIP() << "repository root not found";
    ASSERT_TRUE(library_is_clean(root));
    const std::string freshness = "include/trade_ngin/live/data_freshness.hpp";
    const std::string logger = "src/core/logger.cpp";
    const std::string clean_file = "src/live/margin_manager.cpp";  // holds no site today
    const std::string freshness_text = read_text(root / freshness);
    const std::string logger_text = read_text(root / logger);

    // a second decision on the date flag inside the shared ruling
    EXPECT_FALSE(library_is_clean(
        root, {{freshness, planted_after(freshness_text, "FeedFreshnessRuling ruling;",
                                         "    if (dated_run) return ruling;")}}));
    // each clock, the environment and the database's clock, in a file that holds none
    for (const char* line :
         {"if (std::getenv(\"TRADE_NGIN_TEST_MODE\")) return;", "localtime_r(&t, &tm);",
          "gettimeofday(&tv, nullptr);", "clock_gettime(CLOCK_REALTIME, &ts);", "time(&t);",
          "auto t = std::chrono::system_clock::now();",
          "\"SELECT 1 FROM trading.live_results WHERE date = CURRENT_DATE\"",
          "\"DELETE FROM trading.executions WHERE created_at < now()\"",
          "*out = run_date_is_host_date(a, b);"}) {
        EXPECT_FALSE(library_is_clean(root, {{clean_file, std::string("void f() {\n") + line + "\n}\n"}}))
            << "the guard does not see: " << line;
    }
    // one clock read added and another removed in the same file: the count is unchanged
    const std::string was = "core::safe_localtime(&now_c, &time_info);";
    const auto at = logger_text.find(was);
    ASSERT_NE(at, std::string::npos);
    std::string swapped = logger_text;
    swapped.replace(at, was.size(), "gmtime_r(&now_c, &time_info); if (std::time(nullptr) > 0) {}");
    ASSERT_EQ(matching_lines(swapped, kLibrarySites).size(),
              matching_lines(logger_text, kLibrarySites).size());
    EXPECT_FALSE(library_is_clean(root, {{logger, swapped}}));
}

TEST(ModeBranchGuardPlanted, AModeReadInAnotherFileUnderAppsFailsTheGuard) {
    const auto root = repo_root();
    if (root.empty()) GTEST_SKIP() << "repository root not found";
    ASSERT_TRUE(other_apps_are_clean(root));
    EXPECT_FALSE(other_apps_are_clean(
        root, {{"apps/strategies/live_new_book.cpp",
                "int main() {\n    if (send_email) return 1;\n    return 0;\n}\n"}}));
    EXPECT_FALSE(other_apps_are_clean(
        root, {{"apps/common/run_mode.hpp",
                "inline bool production() { return std::getenv(\"PROD\") != nullptr; }\n"}}));
}
