// tests/live/test_python_mail.cpp
//
// The book e-mail sent by scripts/send_book_email.py (live/python_mail.hpp). A stand-in shell
// script plays the mail script, so these cases EXECUTE the real process path (manifest, files,
// time limit, exit status) without any SMTP. POSIX only: the stand-in is a shell script.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

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

PythonMailRequest request() {
    PythonMailRequest r;
    r.smtp_host = "smtp.example.com";
    r.smtp_port = 587;
    r.username = "u@example.com";
    r.password = "s3cret-pw";
    r.from_email = "fund@example.com";
    r.to = {"a@x.org", "b@x.org"};
    r.cc = {"vp@x.org"};
    r.subject = "[QT p] Daily Trading Report - 2026-10-23";
    r.body_html = "<html>body</html>";
    r.inline_images = {{"equity_chart", "QUJD", ""}, {"algogators_logo", "", "assets/Algo.png"}};
    return r;
}

class PythonMail : public ::testing::Test {
protected:
    void SetUp() override {
#ifdef _WIN32
        GTEST_SKIP() << "the stand-in mail script is a POSIX shell script";
#endif
        dir_ = fs::temp_directory_path() / ("tn_pymail_test_" + std::to_string(::getpid()));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // A stand-in `send_book_email.py`: copies the manifest to captured.json, then runs `body`.
    PythonMailSettings stub(const std::string& body, std::chrono::milliseconds timeout =
                                                         std::chrono::seconds(10)) {
        const fs::path script = dir_ / "stub.sh";
        std::ofstream(script) << "cp \"$2\" '" << (dir_ / "captured.json").string() << "'\n"
                              << body << "\n";
        PythonMailSettings s;
        s.python = "sh";
        s.script = script.string();
        s.timeout = timeout;
        return s;
    }

    nlohmann::json captured() {
        std::ifstream f(dir_ / "captured.json");
        return nlohmann::json::parse(f);
    }

    fs::path dir_;
};

}  // namespace

TEST(PythonMailPure, SplitAddressesTrimsAndDropsBlanks) {
    EXPECT_EQ(split_addresses(" a@x.org, b@x.org;;  ,c@x.org "),
              (std::vector<std::string>{"a@x.org", "b@x.org", "c@x.org"}));
    EXPECT_TRUE(split_addresses("").empty());
}

TEST(PythonMailPure, TheBackendIsSelectedOnlyByTheExactValue) {
    setenv("QT_MAIL_BACKEND", "python", 1);
    EXPECT_TRUE(python_mail_selected());
    setenv("QT_MAIL_BACKEND", "curl", 1);
    EXPECT_FALSE(python_mail_selected());
    unsetenv("QT_MAIL_BACKEND");
    EXPECT_FALSE(python_mail_selected());
}

TEST_F(PythonMail, ExitZeroIsDeliveredAndTheManifestCarriesTheMessage) {
    const auto why = send_via_python(request(), stub("exit 0"));
    EXPECT_FALSE(why.has_value()) << *why;

    const auto m = captured();
    EXPECT_EQ(m["subject"], "[QT p] Daily Trading Report - 2026-10-23");
    EXPECT_EQ(m["to"], (std::vector<std::string>{"a@x.org", "b@x.org"}));
    EXPECT_EQ(m["cc"], (std::vector<std::string>{"vp@x.org"}));
    EXPECT_EQ(m["smtp"]["host"], "smtp.example.com");
    EXPECT_EQ(m["smtp"]["port"], 587);
    ASSERT_EQ(m["inline"].size(), 2u);
    EXPECT_EQ(m["inline"][0]["cid"], "equity_chart");
    EXPECT_TRUE(m["inline"][0]["base64"].get<bool>());
    EXPECT_EQ(m["inline"][1]["path"], "assets/Algo.png");
    EXPECT_FALSE(m["inline"][1]["base64"].get<bool>());
}

TEST_F(PythonMail, ANonZeroExitIsAFailureWithTheScriptsReasonAndNeverThePassword) {
    const auto why = send_via_python(request(), stub("echo 'SMTPAuthenticationError: 535' >&2; exit 1"));
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("SMTPAuthenticationError: 535"), std::string::npos) << *why;
    EXPECT_EQ(why->find("s3cret-pw"), std::string::npos);
}

TEST_F(PythonMail, AScriptThatNeverFinishesIsKilledAtTheLimit) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto why = send_via_python(request(), stub("sleep 30", std::chrono::milliseconds(300)));
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("did not finish"), std::string::npos) << *why;
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5));
}

TEST_F(PythonMail, TheScratchDirectoryIsRemovedAfterTheSend) {
    // the stub records the manifest's directory; after the call it must be gone
    const auto settings = stub("dirname \"$2\" > '" + (dir_ / "scratch.txt").string() + "'");
    ASSERT_FALSE(send_via_python(request(), settings).has_value());
    std::ifstream f(dir_ / "scratch.txt");
    std::string scratch;
    std::getline(f, scratch);
    ASSERT_FALSE(scratch.empty());
    EXPECT_FALSE(fs::exists(scratch));
}
