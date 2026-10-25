// tests/live/test_python_mail.cpp
//
// The book e-mail sent by the report module (live/python_mail.hpp). A stand-in interpreter plays
// the module, so these cases EXECUTE the real process path (command, time limit, exit status)
// without a database or SMTP. POSIX only: the stand-in is a shell script.

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

namespace fs = std::filesystem;
using namespace trade_ngin::live;

namespace {
PythonMailRequest request() { return {"QT_CONSERVATIVE", "2026-10-23"}; }
}  // namespace

TEST(PythonMailPure, TheBackendIsSelectedOnlyByTheExactValue) {
#ifndef _WIN32
    setenv("QT_MAIL_BACKEND", "python", 1);
    EXPECT_TRUE(python_mail_selected());
    setenv("QT_MAIL_BACKEND", "curl", 1);
    EXPECT_FALSE(python_mail_selected());
    unsetenv("QT_MAIL_BACKEND");
    EXPECT_FALSE(python_mail_selected());
#endif
}

TEST(PythonMailPure, TheCommandNamesTheModuleThePortfolioAndTheDayAndQuotesThem) {
    PythonMailSettings s;
    s.python = "/opt/rpc/venv/bin/python";
    s.app_dir = "/opt/rpc/app";
    const std::string cmd = python_mail_command({"QT'X", "2026-10-23"}, s);
    EXPECT_NE(cmd.find("-m algogators_rpc.services.report"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("PYTHONPATH='/opt/rpc/app'"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("--date '2026-10-23'"), std::string::npos) << cmd;
#ifndef _WIN32
    EXPECT_NE(cmd.find("'QT'\\''X'"), std::string::npos) << cmd;  // an embedded quote is escaped
#endif
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
