#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

#include "trade_ngin/core/email_sender.hpp"

using namespace trade_ngin;

namespace {

class ScopedDeliveryPolicy {
public:
    explicit ScopedDeliveryPolicy(std::optional<std::string> value) {
        if (const char* current = std::getenv("QT_EMAIL_DELIVERY_ENABLED")) {
            previous_ = current;
        }

        if (value.has_value()) {
            setenv("QT_EMAIL_DELIVERY_ENABLED", value->c_str(), 1);
        } else {
            unsetenv("QT_EMAIL_DELIVERY_ENABLED");
        }
    }

    ~ScopedDeliveryPolicy() {
        if (previous_.has_value()) {
            setenv("QT_EMAIL_DELIVERY_ENABLED", previous_->c_str(), 1);
        } else {
            unsetenv("QT_EMAIL_DELIVERY_ENABLED");
        }
    }

private:
    std::optional<std::string> previous_;
};

class FakeTransportEmailSender final : public EmailSender {
public:
    FakeTransportEmailSender() : EmailSender(EmailSenderConfig{}) {}

    int delivery_calls() const { return delivery_calls_; }

protected:
    Result<void> deliver_email(const std::string&, const std::string&, bool,
                               const std::vector<std::string>&) override {
        ++delivery_calls_;
        return Result<void>();
    }

private:
    int delivery_calls_{0};
};

class EmailDeliveryPolicyTest : public ::testing::TestWithParam<std::optional<std::string>> {};

TEST_P(EmailDeliveryPolicyTest, DisabledPolicyRefusesBeforeTransportBoundary) {
    ScopedDeliveryPolicy policy(GetParam());
    FakeTransportEmailSender sender;

    const auto result = sender.send_email("test subject", "test body", true,
                                          {"report.csv"});

    ASSERT_TRUE(result.is_error());
    ASSERT_NE(result.error(), nullptr);
    EXPECT_EQ(result.error()->code(), ErrorCode::PERMISSION_ERROR);
    EXPECT_NE(std::string(result.error()->what()).find("disabled"), std::string::npos);
    EXPECT_EQ(sender.delivery_calls(), 0);
}

INSTANTIATE_TEST_SUITE_P(UnsetEmptyFalseAndMalformed, EmailDeliveryPolicyTest,
                         ::testing::Values(std::nullopt, std::optional<std::string>{""},
                                           std::optional<std::string>{"false"},
                                           std::optional<std::string>{"TRUE"},
                                           std::optional<std::string>{"1"},
                                           std::optional<std::string>{"garbage"}));

TEST(EmailDeliveryPolicy, ExplicitTrueDelegatesOnlyToFakeTransport) {
    ScopedDeliveryPolicy policy(std::string{"true"});
    FakeTransportEmailSender sender;

    const auto send_result = sender.send_email("test subject", "test body");
    EXPECT_TRUE(send_result.is_ok());
    EXPECT_EQ(sender.delivery_calls(), 1);
}

TEST(EmailDeliveryPolicy, DisabledPolicyRefusesInitializationBeforeCredentialLookup) {
    ScopedDeliveryPolicy policy(std::nullopt);
    FakeTransportEmailSender sender;

    const auto result = sender.initialize();

    ASSERT_TRUE(result.is_error());
    ASSERT_NE(result.error(), nullptr);
    EXPECT_EQ(result.error()->code(), ErrorCode::PERMISSION_ERROR);
    EXPECT_NE(std::string(result.error()->what()).find("disabled"), std::string::npos);
    EXPECT_EQ(sender.delivery_calls(), 0);
}

}  // namespace
