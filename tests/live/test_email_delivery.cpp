// tests/live/test_email_delivery.cpp
//
// T-EMAIL P2-4 (HD ruling Q2b). The three live runners finish the daily report through
// deliver_email_report: the day's flags, then the body file or the one send, then a result. These
// cases RUN that function with a stand-in sender; they replace the source-text test that used to
// check the runners' text for the same order.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "trade_ngin/live/email_delivery.hpp"

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
               ("tn_email_delivery_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

struct Sender {
    int calls{0};
    std::string subject, body;
    std::optional<std::string> error;
    EmailSendFn fn() {
        return [this](const std::string& s, const std::string& b) {
            ++calls;
            subject = s;
            body = b;
            return error;
        };
    }
};

const std::string kBody = "<html><div class=\"container\">\n<h1>Report</h1></div></html>";

nlohmann::json risk_failure() {
    nlohmann::json j;
    j["module"] = "portfolio_risk";
    j["error"] = "boom";
    return j;
}
}  // namespace

TEST(EmailDelivery, ASendMailsTheBodyOnceAndNothingIsWritten) {
    Sender s;
    const auto plan = plan_email_report(true, nullptr);
    const auto d = deliver_email_report(plan, "BOOK", "2026-04-28", "subject", kBody,
                                        std::nullopt, false, "", s.fn());
    EXPECT_TRUE(d.ok);
    EXPECT_EQ(s.calls, 1);
    EXPECT_EQ(s.body, kBody);
    EXPECT_EQ(s.subject, "subject");
}

TEST(EmailDelivery, TheBodyFileIsTheBodyAMailWouldHaveCarriedAndNothingIsMailed) {
    TempDir dir;
    Sender s;
    const std::string dirpath = dir.path.string();
    const auto plan = plan_email_report(false, dirpath.c_str());
    const auto d = deliver_email_report(plan, "BOOK", "2026-04-28", "subject", kBody,
                                        risk_failure(), true, "no session on 2026-04-27", s.fn());
    ASSERT_TRUE(d.ok) << d.message;
    EXPECT_EQ(s.calls, 0);
    const std::string written = slurp(dirpath + "/email_body_BOOK_2026-04-28.html");
    EXPECT_EQ(written, d.body);

    // the same day sent: the file equals the mailed body byte for byte
    Sender mailed;
    const auto sent = deliver_email_report(plan_email_report(true, nullptr), "BOOK", "2026-04-28",
                                           "subject", kBody, risk_failure(), true,
                                           "no session on 2026-04-27", mailed.fn());
    EXPECT_EQ(mailed.body, written);
    EXPECT_EQ(sent.subject, d.subject);
}

TEST(EmailDelivery, ARiskModuleFailureAndACarriedDayFlagTheBodyAndTheSubject) {
    Sender s;
    const auto d = deliver_email_report(plan_email_report(true, nullptr), "BOOK", "2026-04-28",
                                        "subject", kBody, risk_failure(), true, "no session",
                                        s.fn());
    EXPECT_NE(d.subject, "subject");
    EXPECT_NE(d.subject.find("subject"), std::string::npos);
    // HD 10-14: no carried-day banner is added; the report's own plain note says it
    EXPECT_EQ(s.body.find("NO SESSION - BOOK CARRIED"), std::string::npos);
}

TEST(EmailDelivery, AnUnflaggedDayIsSentAsBuilt) {
    Sender s;
    deliver_email_report(plan_email_report(true, nullptr), "BOOK", "2026-04-28", "subject", kBody,
                         std::nullopt, false, "", s.fn());
    EXPECT_EQ(s.body, kBody);
}

TEST(EmailDelivery, AFailedSendIsAFailureWithTheSendersText) {
    Sender s;
    s.error = "connection refused";
    const auto d = deliver_email_report(plan_email_report(true, nullptr), "BOOK", "2026-04-28",
                                        "subject", kBody, std::nullopt, false, "", s.fn());
    EXPECT_FALSE(d.ok);
    EXPECT_NE(d.message.find("connection refused"), std::string::npos);
}

TEST(EmailDelivery, AnEmptyBodyIsAFailureAndNothingIsSentOrWritten) {
    Sender s;
    const auto d = deliver_email_report(plan_email_report(true, nullptr), "BOOK", "2026-04-28",
                                        "subject", "", std::nullopt, false, "", s.fn());
    EXPECT_FALSE(d.ok);
    EXPECT_EQ(s.calls, 0);
}

TEST(EmailDelivery, ABodyFileThatCannotBeWrittenIsAFailure) {
    Sender s;
    const auto plan = plan_email_report(false, "/nonexistent_tn_dir_for_test");
    const auto d = deliver_email_report(plan, "BOOK", "2026-04-28", "subject", kBody, std::nullopt,
                                        false, "", s.fn());
    EXPECT_FALSE(d.ok);
    EXPECT_EQ(s.calls, 0);
}

TEST(EmailDelivery, AFailedReportExitsFourAndARiskModuleFailureKeepsItsOwnCode) {
    EXPECT_EQ(kEmailFailureExitCode, 4);
    EXPECT_EQ(futures_run_exit_code(std::nullopt, false), 0);
    EXPECT_EQ(futures_run_exit_code(std::nullopt, true), 4);
    EXPECT_EQ(futures_run_exit_code(risk_failure(), false), 3);
    EXPECT_EQ(futures_run_exit_code(risk_failure(), true), 3);
}
