// tests/live/test_python_mail.cpp
//
// The book e-mail sent by the report module (live/python_mail.hpp), the only way a live runner
// reports its day. A stand-in interpreter plays the module, so these cases EXECUTE the real
// process path (command, time limit, exit status) without a database or SMTP. POSIX only: the
// stand-in is a shell script.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "trade_ngin/live/python_mail.hpp"
#include "trade_ngin/live/report_rolls_json.hpp"

namespace fs = std::filesystem;
using namespace trade_ngin::live;

namespace {
PythonMailRequest request() {
    PythonMailRequest r;
    r.portfolio_id = "QT_CONSERVATIVE";
    r.date = "2026-10-23";
    return r;
}

std::string read_source(const std::string& rel) {
    for (fs::path dir = fs::current_path(); !dir.empty(); dir = dir.parent_path()) {
        std::ifstream f(dir / rel);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
        if (dir == dir.parent_path()) break;
    }
    return {};
}
}  // namespace

TEST(PythonMailPure, TheTestAddressIsReadFromTheEnvironment) {
#ifndef _WIN32
    setenv("QT_MAIL_TEST_TO", "someone@example.org", 1);
    EXPECT_EQ(python_mail_test_to(), "someone@example.org");
    unsetenv("QT_MAIL_TEST_TO");
    EXPECT_EQ(python_mail_test_to(), "");
#endif
}

TEST(PythonMailPure, TheCommandNamesTheModuleThePortfolioAndTheDayAndQuotesThem) {
    PythonMailSettings s;
    s.python = "/opt/rpc/venv/bin/python";
    s.app_dir = "/opt/rpc/app";
    PythonMailRequest r = request();
    r.portfolio_id = "QT'X";
    const std::string cmd = python_mail_command(r, s);
    EXPECT_NE(cmd.find("-m algogators_rpc.services.report"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("--date '2026-10-23'"), std::string::npos) << cmd;
    // an ordinary day: no flag, no attachment, the portfolio's own recipients
    for (const char* absent :
         {"--no-session", "--alert", "--attach", "--to", "--strategy-id", "--equities", "--rolls"}) {
        EXPECT_EQ(cmd.find(absent), std::string::npos) << absent << ": " << cmd;
    }
#ifndef _WIN32
    EXPECT_NE(cmd.find("PYTHONPATH='/opt/rpc/app'"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("'QT'\\''X'"), std::string::npos) << cmd;  // an embedded quote is escaped
#endif
}

TEST(PythonMailPure, TheRunsFlagsFilesAndTestAddressAreHandedOver) {
    PythonMailRequest r = request();
    r.no_session = true;
    r.alert = "The portfolio risk module carver could not evaluate today's book (boom).";
    r.attachments = {"/app/results/positions.csv", "", "/app/results/finalized.csv"};
    r.test_to = "someone@example.org";
    r.equities = true;
    r.rolls_json = R"({"lines":["Silver, December to March: it's"]})";
    r.strategy_id = "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST";
    const std::string cmd = python_mail_command(r, PythonMailSettings{});
    EXPECT_NE(cmd.find("--strategy-id"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find(" --equities"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("--rolls"), std::string::npos) << cmd;
#ifndef _WIN32
    // the roll text's apostrophe is escaped for the shell
    EXPECT_NE(cmd.find("it'\''s"), std::string::npos) << cmd;
#endif
    EXPECT_NE(cmd.find("LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find(" --no-session"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("--alert"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("could not evaluate today"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("positions.csv"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("finalized.csv"), std::string::npos) << cmd;
    std::size_t attaches = 0;
    for (auto at = cmd.find("--attach"); at != std::string::npos; at = cmd.find("--attach", at + 1))
        ++attaches;
    EXPECT_EQ(attaches, 2u) << "an empty path (no settled file that day) is not attached: " << cmd;
    EXPECT_NE(cmd.find("--to"), std::string::npos) << cmd;
}

TEST(PythonMailPure, AHeldBookKeepsItsCodeAndAnUnsentEmailIsFour) {
    const nlohmann::json held = {{"module", "carver"}, {"error", "x"}};
    EXPECT_EQ(futures_run_exit_code(std::nullopt, false), 0);
    EXPECT_EQ(futures_run_exit_code(std::nullopt, true), kBookEmailFailureExitCode);
    EXPECT_EQ(futures_run_exit_code(held, true), trade_ngin::kRiskModuleFailureExitCode);
    EXPECT_EQ(futures_run_exit_code(held, false), trade_ngin::kRiskModuleFailureExitCode);
}

TEST(ReportRollsJson, TheSwitchOffSaysNothing) {
    EXPECT_TRUE(report_rolls_json(trade_ngin::ReportRolls{}, {}, {}).empty());
}

TEST(ReportRollsJson, TheRollLinesTheRolloverAndTheReferenceColumnsAreTheEnginesTexts) {
    using namespace trade_ngin;
    ReportRolls rolls;
    rolls.per_contract_rolls = true;
    ReportRollOutlook si;
    si.root = "SI";
    si.label = "SI";
    si.held = "SIZ5";
    si.cycle = "HKNUZ";
    si.in_window = true;
    si.roll_by = "2025-11-20";
    si.deadline = "2025-11-25";
    si.first_notice = true;
    si.target = "SIH6";
    rolls.outlook["SI"] = si;
    ReportRollOutlook es;  // held, outside its window: front month and months, no rollover line
    es.root = "ES";
    es.held = "ESZ5";
    es.cycle = "HMUZ";
    rolls.outlook["ES"] = es;
    rolls.names["EXEC_SI.v.0_20251125_RC"] = ReportRollNames{"Silver", "December", "March", 3};

    ExecutionReport closing;
    closing.symbol = "SI.v.0";
    closing.execution_type = ExecutionType::ROLL;
    closing.exec_id = "EXEC_SI.v.0_20251125_RC";
    closing.total_transaction_costs = Decimal(22.62);
    closing.roll_bar_move = -1075.0;
    closing.roll_gap_points = 0.67;
    closing.roll_gap_value = 3350.0;
    ExecutionReport opening = closing;
    opening.exec_id = "EXEC_SI.v.0_20251125_RO";
    opening.total_transaction_costs = Decimal(22.67);
    opening.roll_bar_move.reset();
    opening.roll_gap_points.reset();
    opening.roll_gap_value.reset();
    ExecutionReport fill;  // a strategy fill: no line
    fill.symbol = "ES.v.0";
    fill.exec_id = "EXEC_ES.v.0_20251125";

    const auto j = report_rolls_json(rolls, {{"TF", {closing, opening, fill}}}, {"SI.v.0"});
    ASSERT_EQ(j.at("lines").size(), 1u);
    EXPECT_EQ(j.at("lines")[0],
              "Silver, December to March: market move booked -$1,075; contract price difference "
              "0.670 ($3,350, not profit or loss); roll cost $45.29.");
    ASSERT_EQ(j.at("rollover").size(), 1u);
    EXPECT_EQ(j.at("rollover")[0].at("held"), "SIZ5");
    EXPECT_EQ(j.at("rollover")[0].at("target"), "SIH6");
    EXPECT_EQ(j.at("rollover")[0].at("by"), "11-20");
    EXPECT_EQ(j.at("rollover")[0].at("deadline_kind"), "first notice");
    EXPECT_EQ(j.at("front_month").at("ES"), "ESZ5");
    EXPECT_EQ(j.at("contract_months").at("ES"), "Mar Jun Sep Dec");
    EXPECT_EQ(j.at("roll_bar_symbols")[0], "SI.v.0");
    EXPECT_EQ(j.at("footer"), kReportRollsFooter);
}

// Every live runner sends the book e-mail through the module, and nothing else sends it.
TEST(PythonMailSource, EveryLiveRunnerSendsThroughTheReportModuleOnly) {
    for (const char* runner :
         {"apps/strategies/live_portfolio_conservative.cpp", "apps/strategies/live_portfolio.cpp",
          "apps/strategies/live_equity_mean_reversion.cpp"}) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_NE(src.find("live::send_via_python(mail, live::python_mail_settings_from_env())"),
                  std::string::npos);
        for (const char* gone : {"send_email(mail_subject", "generate_trading_report_body(",
                                 "EmailSender", "deliver_email_report(", "QT_MAIL_BACKEND"}) {
            EXPECT_EQ(src.find(gone), std::string::npos) << gone;
        }
    }
}

#ifndef _WIN32
class PythonMail : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("tn_pymail_test_" + std::to_string(::getpid()));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // A stand-in interpreter: records its arguments in args.txt, then runs `body`.
    PythonMailSettings stub(const std::string& body,
                            std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        const fs::path script = dir_ / "fakepython.sh";
        std::ofstream(script) << "#!/bin/sh\n"
                              << "echo \"$@\" > '" << (dir_ / "args.txt").string() << "'\n"
                              << body << "\n";
        fs::permissions(script, fs::perms::owner_all);
        PythonMailSettings s;
        s.python = script.string();
        s.app_dir = dir_.string();
        s.timeout = timeout;
        return s;
    }

    std::string args() {
        std::ifstream f(dir_ / "args.txt");
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    fs::path dir_;
};

TEST_F(PythonMail, ExitZeroIsDeliveredAndTheModuleGotThePortfolioAndDay) {
    const auto why = send_via_python(request(), stub("exit 0"));
    EXPECT_FALSE(why.has_value()) << *why;
    EXPECT_NE(args().find("--portfolio-id QT_CONSERVATIVE --date 2026-10-23"), std::string::npos)
        << args();
}

TEST_F(PythonMail, ANonZeroExitIsAFailureWithTheModulesReason) {
    const auto why =
        send_via_python(request(), stub("echo 'report: send failed: SMTPAuthenticationError' >&2; exit 1"));
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("SMTPAuthenticationError"), std::string::npos) << *why;
    EXPECT_NE(why->find("status"), std::string::npos) << *why;
}

TEST_F(PythonMail, AModuleThatNeverFinishesIsKilledAtTheLimit) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto why = send_via_python(request(), stub("sleep 30", std::chrono::milliseconds(300)));
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("did not finish"), std::string::npos) << *why;
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5));
}
#endif
