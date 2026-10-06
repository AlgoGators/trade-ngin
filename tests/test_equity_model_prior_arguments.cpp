#include <gtest/gtest.h>
#include "trade_ngin/apps/equity_model_prior.hpp"
using namespace trade_ngin;
namespace {
const std::string decision="40000000-0000-4000-8000-000000000001";
const std::string finalization="50000000-0000-4000-8000-000000000001";
std::vector<std::string> selected(){return {"--verified-desk-prior","--prior-decision",decision,"--prior-finalization",finalization};}
}
TEST(EquityModelPriorArguments,NoFlagsPreservesSystemReferenceAndExistingArguments){
    const std::vector<std::string> existing{"2026-09-25","--send-email"};
    auto r=parse_equity_model_prior_arguments(existing);ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().mode,EquityModelPriorMode::SystemReference);
    EXPECT_EQ(r.value().runner_arguments,existing);EXPECT_TRUE(r.value().decision_id.empty());
}
TEST(EquityModelPriorArguments,ExplicitModeRequiresAndRetainsExactPairedUUIDs){
    auto arguments=selected();arguments.insert(arguments.begin(),"2026-09-25");
    auto r=parse_equity_model_prior_arguments(arguments);ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().mode,EquityModelPriorMode::VerifiedDeskPrior);
    EXPECT_EQ(r.value().decision_id,decision);EXPECT_EQ(r.value().finalization_id,finalization);
    EXPECT_EQ(r.value().runner_arguments,std::vector<std::string>{"2026-09-25"});
}
TEST(EquityModelPriorArguments,MissingExplicitModeRefuses){auto a=selected();a.erase(a.begin());EXPECT_TRUE(parse_equity_model_prior_arguments(a).is_error());}
TEST(EquityModelPriorArguments,MissingPairRefuses){for(const auto& flag:{"--prior-decision","--prior-finalization"})EXPECT_TRUE(parse_equity_model_prior_arguments({"--verified-desk-prior",flag,decision}).is_error());}
TEST(EquityModelPriorArguments,ModeWithoutIdentitiesRefuses){EXPECT_TRUE(parse_equity_model_prior_arguments({"--verified-desk-prior"}).is_error());}
TEST(EquityModelPriorArguments,DuplicateModeRefuses){auto a=selected();a.push_back("--verified-desk-prior");EXPECT_TRUE(parse_equity_model_prior_arguments(a).is_error());}
TEST(EquityModelPriorArguments,DuplicateIdentityRefusesEvenIdentical){for(const auto& flag:{"--prior-decision","--prior-finalization"}){auto a=selected();a.push_back(flag);a.push_back(decision);EXPECT_TRUE(parse_equity_model_prior_arguments(a).is_error());}}
TEST(EquityModelPriorArguments,MissingArgumentValueRefuses){auto a=selected();a.push_back("--prior-decision");EXPECT_TRUE(parse_equity_model_prior_arguments(a).is_error());}
TEST(EquityModelPriorArguments,UppercaseAliasIsNotCanonical){auto a=selected();a[2]="AAAAAAAA-0000-4000-8000-000000000001";EXPECT_TRUE(parse_equity_model_prior_arguments(a).is_error());}
TEST(EquityModelPriorArguments,NullIdentityRefuses){auto a=selected();a[2]="00000000-0000-0000-0000-000000000000";EXPECT_TRUE(parse_equity_model_prior_arguments(a).is_error());}
TEST(EquityModelPriorArguments,ArgumentInjectionRefuses){auto a=selected();a[2]=decision+"'::uuid;SELECT 1;--";EXPECT_TRUE(parse_equity_model_prior_arguments(a).is_error());}
TEST(EquityModelPriorArguments,UnknownModeIsLeftToExistingRunnerRejection){auto a=selected();a.push_back("--implicit-desk-prior");auto r=parse_equity_model_prior_arguments(a);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().runner_arguments,std::vector<std::string>{"--implicit-desk-prior"});}
TEST(EquityModelPriorArguments,EqualsSyntaxCannotSilentlySelectSystemReference){EXPECT_TRUE(parse_equity_model_prior_arguments({"--prior-decision="+decision}).is_error());EXPECT_TRUE(parse_equity_model_prior_arguments({"--verified-desk-prior=true"}).is_error());}
TEST(EquityModelPriorArguments,MisspelledReservedIdentityCannotSilentlySelectSystemReference){EXPECT_TRUE(parse_equity_model_prior_arguments({"--prior-finalisation",finalization}).is_error());}
TEST(EquityModelPriorArguments,UnknownReservedFlagRefusesEvenWithCompleteValidPair){auto a=selected();a.push_back("--verified-desk-prior-fallback");EXPECT_TRUE(parse_equity_model_prior_arguments(a).is_error());}
