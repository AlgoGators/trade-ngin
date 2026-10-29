// tests/live/test_python_mail.cpp
//
// The book e-mail sent by the report module (live/python_mail.hpp). A stand-in interpreter plays
// the module, so these cases EXECUTE the real process path (manifest, command, time limit, exit
// status) without SMTP. POSIX only: the stand-in is a shell script.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "trade_ngin/live/python_mail.hpp"

namespace fs = std::filesystem;
using namespace trade_ngin::live;

namespace {
PythonMailRequest request() {
    PythonMailRequest r;
    r.portfolio_id = "QT_CONSERVATIVE";
    r.date = "2026-10-23";
    r.subject = "[QT QT_CONSERVATIVE] Daily Trading Report - 2026-10-23";
    r.body_html = "<div class=\"container\">\n<h2>Today's Positions</h2>\n</div>";
    r.images = {{"equity_chart", "iVBORw0KGgo="}};
    r.attachments = {"/tmp/positions.csv"};
    return r;
}
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

TEST(PythonMailPure, TheTestAddressIsReadFromTheEnvironment) {
#ifndef _WIN32
    setenv("QT_MAIL_TEST_TO", "someone@example.org", 1);
    EXPECT_EQ(python_mail_test_to(), "someone@example.org");
    unsetenv("QT_MAIL_TEST_TO");
    EXPECT_EQ(python_mail_test_to(), "");
#endif
}

TEST(PythonMailPure, TheCommandNamesTheModuleThePortfolioTheDayAndTheManifestAndQuotesThem) {
    PythonMailSettings s;
    s.python = "/opt/rpc/venv/bin/python";
    s.app_dir = "/opt/rpc/app";
    PythonMailRequest r = request();
    r.portfolio_id = "QT'X";
    const std::string cmd = python_mail_command(r, s, "/tmp/m/manifest.json");
    EXPECT_NE(cmd.find("-m algogators_rpc.services.report"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("PYTHONPATH='/opt/rpc/app'"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("--date '2026-10-23'"), std::string::npos) << cmd;
    EXPECT_EQ(cmd.find("--to"), std::string::npos) << cmd;  // the portfolio's recipients
#ifndef _WIN32
    EXPECT_NE(cmd.find("--manifest '/tmp/m/manifest.json'"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("'QT'\\''X'"), std::string::npos) << cmd;  // an embedded quote is escaped
#endif
}

TEST(PythonMailPure, ATestSendNamesItsOneAddress) {
    PythonMailRequest r = request();
    r.test_to = "someone@example.org";
    const std::string cmd = python_mail_command(r, PythonMailSettings{}, "/tmp/m.json");
    EXPECT_NE(cmd.find("--to"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("someone@example.org"), std::string::npos) << cmd;
}

TEST(PythonMailPure, TheManifestCarriesTheEnginesReportAsItIs) {
    const auto m = nlohmann::json::parse(python_mail_manifest_json(request()));
    EXPECT_EQ(m.at("version"), 1);
    EXPECT_EQ(m.at("subject"), "[QT QT_CONSERVATIVE] Daily Trading Report - 2026-10-23");
    EXPECT_EQ(m.at("body_html"), request().body_html);
    ASSERT_EQ(m.at("images").size(), 1u);
    EXPECT_EQ(m.at("images")[0].at("cid"), "equity_chart");
    EXPECT_EQ(m.at("images")[0].at("base64"), "iVBORw0KGgo=");
    ASSERT_EQ(m.at("attachments").size(), 1u);
    EXPECT_EQ(m.at("attachments")[0], "/tmp/positions.csv");
}

TEST(PythonMailPure, AnInvalidByteInTheBodyIsReplacedNotAFailure) {
    PythonMailRequest r = request();
    r.body_html = std::string("caf") + static_cast<char>(0xE9);
    EXPECT_NO_THROW(nlohmann::json::parse(python_mail_manifest_json(r)));
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

    // A stand-in interpreter: records its arguments in args.txt; for the argument after
    // --manifest, a copy of the file (manifest.json), its directory's mode (perms.txt) and its
    // path (path.txt); then runs `body`.
    PythonMailSettings stub(const std::string& body,
                            std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        const fs::path script = dir_ / "fakepython.sh";
        std::ofstream(script) << "#!/bin/sh\n"
                              << "echo \"$@\" > '" << (dir_ / "args.txt").string() << "'\n"
                              << "while [ $# -gt 0 ]; do\n"
                              << "  if [ \"$1\" = --manifest ]; then\n"
                              << "    cp \"$2\" '" << (dir_ / "manifest.json").string() << "'\n"
                              << "    ls -ld \"$(dirname \"$2\")\" > '"
                              << (dir_ / "perms.txt").string() << "'\n"
                              << "    printf '%s' \"$2\" > '" << (dir_ / "path.txt").string()
                              << "'\n"
                              << "  fi\n"
                              << "  shift\n"
                              << "done\n"
                              << body << "\n";
        fs::permissions(script, fs::perms::owner_all);
        PythonMailSettings s;
        s.python = script.string();
        s.app_dir = dir_.string();
        s.timeout = timeout;
        return s;
    }

    std::string read(const char* name) {
        std::ifstream f(dir_ / name);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    std::string args() { return read("args.txt"); }

    fs::path dir_;
};

TEST_F(PythonMail, ExitZeroIsDeliveredAndTheModuleGotTheReport) {
    const auto why = send_via_python(request(), stub("exit 0"));
    EXPECT_FALSE(why.has_value()) << *why;
    EXPECT_NE(args().find("--portfolio-id QT_CONSERVATIVE --date 2026-10-23"), std::string::npos)
        << args();
    const auto m = nlohmann::json::parse(read("manifest.json"));
    EXPECT_EQ(m.at("body_html"), request().body_html);
    // The manifest's directory was private, and it is gone after the call.
    EXPECT_EQ(read("perms.txt").rfind("drwx------", 0), 0u) << read("perms.txt");
    EXPECT_FALSE(fs::exists(fs::path(read("path.txt")).parent_path())) << read("path.txt");
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
