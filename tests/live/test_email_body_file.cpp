// tests/live/test_email_body_file.cpp
//
// T-NETTING fix round 2 (HD 2026-10-10: --send-email wins). The futures runners decide what to do
// about the daily report through plan_email_report, name the body file through
// email_body_file_path and write it through write_email_body_file (email_body_file.hpp); the
// runners' use of the three is pinned by EmailBodyFileSource in tests/portfolio/test_pm_netting.cpp.
//
//   | --send-email | TRADE_NGIN_EMAIL_BODY_DIR | built | mailed | file written | warning |
//   | yes          | not set                   | yes   | yes    | no           | no      |
//   | yes          | set                       | yes   | yes    | no           | yes     |
//   | no           | set                       | yes   | no     | yes          | no      |
//   | no           | not set                   | no    | no     | no           | no      |

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "trade_ngin/live/email_body_file.hpp"

using namespace trade_ngin::live;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("tn_email_body_" + std::to_string(std::rand()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

}  // namespace

TEST(EmailReportPlan, TheVariableIsCalledWhatTheRunnersAndTheHarnessCallIt) {
    EXPECT_STREQ(kEmailBodyDirEnv, "TRADE_NGIN_EMAIL_BODY_DIR");
}

// Production: --send-email, the variable not set. Built and mailed, as before the variable existed.
TEST(EmailReportPlan, SendWithoutTheVariableMails) {
    for (const char* unset : {static_cast<const char*>(nullptr), ""}) {
        const auto plan = plan_email_report(true, unset);
        EXPECT_TRUE(plan.build());
        EXPECT_TRUE(plan.send);
        EXPECT_FALSE(plan.write_body_file);
        EXPECT_FALSE(plan.variable_ignored) << "nothing to warn about";
        EXPECT_TRUE(plan.body_dir.empty());
    }
}

// --send-email WINS: with the variable set the report is still mailed, no file is written, and the
// run warns once. RED if the variable can silence a requested send (the behaviour of 4398b2c6).
TEST(EmailReportPlan, SendWinsOverTheVariableWhichIsIgnoredWithAWarning) {
    const auto plan = plan_email_report(true, "/var/tmp/bodies");
    EXPECT_TRUE(plan.build());
    EXPECT_TRUE(plan.send) << "a requested send is never suppressed by the variable";
    EXPECT_FALSE(plan.write_body_file) << "no body file on a run that sends";
    EXPECT_TRUE(plan.body_dir.empty());
    EXPECT_TRUE(plan.variable_ignored);
    const std::string warning = email_body_dir_ignored_warning(plan);
    EXPECT_NE(warning.find("TRADE_NGIN_EMAIL_BODY_DIR"), std::string::npos) << warning;
    EXPECT_NE(warning.find("/var/tmp/bodies"), std::string::npos) << warning;
    EXPECT_NE(warning.find("IGNORED"), std::string::npos) << warning;
    EXPECT_NE(warning.find("--send-email"), std::string::npos) << warning;
    EXPECT_NE(warning.find("mailed as usual"), std::string::npos) << warning;
}

// A test run: no --send-email, the variable set. Built, written to the file, never mailed.
TEST(EmailReportPlan, NoSendWithTheVariableWritesTheBodyAndMailsNothing) {
    const auto plan = plan_email_report(false, "/var/tmp/bodies");
    EXPECT_TRUE(plan.build());
    EXPECT_FALSE(plan.send) << "nothing is mailed without --send-email";
    EXPECT_TRUE(plan.write_body_file);
    EXPECT_EQ(plan.body_dir, "/var/tmp/bodies");
    EXPECT_FALSE(plan.variable_ignored);
}

// Neither: nothing is built. RED if the gate's default were anything but "not set" (a non-empty
// default would build a body on every run, and before this round would have stopped every mail).
TEST(EmailReportPlan, NoSendWithoutTheVariableBuildsNothing) {
    for (const char* unset : {static_cast<const char*>(nullptr), ""}) {
        const auto plan = plan_email_report(false, unset);
        EXPECT_FALSE(plan.build());
        EXPECT_FALSE(plan.send);
        EXPECT_FALSE(plan.write_body_file);
        EXPECT_FALSE(plan.variable_ignored);
        EXPECT_TRUE(plan.body_dir.empty());
    }
}

// Whatever the inputs, a plan never both mails and writes a file, and never writes without a
// directory.
TEST(EmailReportPlan, NeverBothMailsAndWritesAFile) {
    for (const bool send : {true, false}) {
        for (const char* dir : {static_cast<const char*>(nullptr), "", "/x", "relative/dir"}) {
            const auto plan = plan_email_report(send, dir);
            EXPECT_FALSE(plan.send && plan.write_body_file);
            EXPECT_EQ(plan.send, send) << "the variable never changes whether the report is mailed";
            if (plan.write_body_file) EXPECT_FALSE(plan.body_dir.empty());
        }
    }
}

TEST(EmailBodyFile, TheFileIsNamedByPortfolioAndDate) {
    EXPECT_EQ(email_body_file_path("/var/tmp/bodies", "BASE_PORTFOLIO", "2026-05-02"),
              "/var/tmp/bodies/email_body_BASE_PORTFOLIO_2026-05-02.html");
    EXPECT_EQ(email_body_file_path("out", "CONSERVATIVE_PORTFOLIO", "2026-04-24"),
              "out/email_body_CONSERVATIVE_PORTFOLIO_2026-04-24.html");
}

// The file holds the body byte for byte (line ends, a NUL, non-ASCII bytes), and a second write of
// the same date replaces the first.
TEST(EmailBodyFile, TheFileHoldsTheBodyByteForByte) {
    TempDir dir;
    const std::string path = email_body_file_path(dir.path.string(), "BASE_PORTFOLIO", "2026-05-02");
    std::string body = "<html>\r\n<td>-$2.55</td>\n caf\xC3\xA9 ";
    body.push_back('\0');
    body += "tail without a newline";
    ASSERT_TRUE(write_email_body_file(path, body));
    EXPECT_EQ(slurp(path), body);
    EXPECT_EQ(std::filesystem::file_size(path), body.size());

    ASSERT_TRUE(write_email_body_file(path, "short"));
    EXPECT_EQ(slurp(path), "short") << "the earlier, longer body is gone";

    ASSERT_TRUE(write_email_body_file(path, ""));
    EXPECT_EQ(std::filesystem::file_size(path), 0u);
}

// A directory that does not exist is not created: the write reports failure (the runner logs an
// ERROR and mails nothing).
TEST(EmailBodyFile, AMissingDirectoryIsAFailedWriteNotACreatedOne) {
    TempDir dir;
    const auto missing = dir.path / "not_there";
    const std::string path = email_body_file_path(missing.string(), "BASE_PORTFOLIO", "2026-05-02");
    EXPECT_FALSE(write_email_body_file(path, "body"));
    EXPECT_FALSE(std::filesystem::exists(missing));
}
