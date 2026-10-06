#include <gtest/gtest.h>
#include "trade_ngin/data/qt_empty_model_owner_publication.hpp"
#include "empty_owner_vectors.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "qt_test_build_identity.hpp"
using namespace trade_ngin;
namespace {
using J=nlohmann::json;
J vectors(){return trade_ngin::test::fixture_for_compiled_build(J::parse(empty_owner_test::fixture));}
QtEmptyModelOwnerPublication sample(){
    auto v=vectors();QtEmptyModelOwnerPublication value;
    value.publication.publication_id="f0000000-0000-4000-8000-000000000001";
    value.publication.portfolio_id="EQ_BOOK";value.publication.strategy_id="LIVE_EQUITY_MEAN_REVERSION";
    value.publication.source_day="2026-09-26";value.publication.producer_version=TRADE_NGIN_GIT_SHA;
    value.configuration_snapshot=v.at("configuration_snapshot");value.configured_owner_names={"empty-alpha"};
    value.fresh_empty_batches={{"EQ_BOOK","LIVE_EQUITY_MEAN_REVERSION","empty-alpha","2026-09-26"}};
    return value;
}
TEST(QtEmptyModelOwner, ActualEmptyOwnerProducesNoSentinelAndExactClosedVector){
    auto result=qt_empty_model_owner_document(sample());ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(),vectors().at("document"));
    EXPECT_TRUE(result.value().at("system_components").empty());
}
TEST(QtEmptyModelOwner, DedicatedCanonicalBytesMatchIndependentApiVector){
    auto v=vectors();auto bytes=canonical_qt_empty_model_owner_bytes(v.at("document"));ASSERT_TRUE(bytes.is_ok());
    EXPECT_EQ(bytes.value(),v.at("owner_canonical_bytes").get<std::string>());auto hash=qt_sha256_hex(bytes.value());ASSERT_TRUE(hash.is_ok());
    EXPECT_EQ(hash.value(),v.at("owner_digest").get<std::string>());
}
TEST(QtEmptyModelOwner, TwoActualConfiguredEmptyMembersAreRetained){
    auto value=sample();value.configured_owner_names.push_back("empty-beta");
    value.configuration_snapshot["strategies"]["empty-beta"]={{"enabled_live",true}};
    value.fresh_empty_batches.push_back({"EQ_BOOK","LIVE_EQUITY_MEAN_REVERSION","empty-beta","2026-09-26"});
    auto result=qt_empty_model_owner_document(value);ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().at("configured_owner_names"),J::array({"empty-alpha","empty-beta"}));
}
TEST(QtEmptyModelOwner, ExistingActualQtFractionalHoldingIsPreservedUnderFlatModel){
    auto value=sample();value.qt_components.push_back({{"EQ_BOOK","LIVE_EQUITY_MEAN_REVERSION","empty-alpha",
        "2026-09-26","SYN","qt"},Quantity::from_raw(-50000000),Price(100)});
    auto result=qt_empty_model_owner_document(value);ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(result.value().at("qt_components").size(),1U);
    EXPECT_EQ(result.value().at("qt_components")[0].at("quantity_exact"),"-0.5");
    EXPECT_TRUE(result.value().at("system_components").empty());
}
TEST(QtEmptyModelOwner, V1EmptySeedStillRefuses){EXPECT_TRUE(qt_model_seed_document(sample().publication).is_error());}
TEST(QtEmptyModelOwner, MissingActualFreshEmptyBatchRefuses){auto value=sample();value.fresh_empty_batches.clear();EXPECT_TRUE(qt_empty_model_owner_document(value).is_error());}
TEST(QtEmptyModelOwner, ExtraFreshOwnerRefuses){auto value=sample();value.fresh_empty_batches.push_back({"EQ_BOOK","LIVE_EQUITY_MEAN_REVERSION","stale","2026-09-26"});EXPECT_TRUE(qt_empty_model_owner_document(value).is_error());}
TEST(QtEmptyModelOwner, WrongFreshDayRefuses){auto value=sample();value.fresh_empty_batches[0].source_day="2026-09-25";EXPECT_TRUE(qt_empty_model_owner_document(value).is_error());}
TEST(QtEmptyModelOwner, WrongFreshBookRefuses){auto value=sample();value.fresh_empty_batches[0].portfolio_id="OTHER";EXPECT_TRUE(qt_empty_model_owner_document(value).is_error());}
TEST(QtEmptyModelOwner, DuplicateFreshOwnerRefuses){auto value=sample();value.fresh_empty_batches.push_back(value.fresh_empty_batches[0]);EXPECT_TRUE(qt_empty_model_owner_document(value).is_error());}
TEST(QtEmptyModelOwner, DisabledConfigurationCannotProveOwner){auto value=sample();value.configuration_snapshot["strategies"]["empty-alpha"]["enabled_live"]=false;EXPECT_TRUE(qt_empty_model_owner_document(value).is_error());}
TEST(QtEmptyModelOwner, ZeroSystemRowCannotMasqueradeAsEmpty){auto value=sample();value.publication.system_components.push_back({{"EQ_BOOK","LIVE_EQUITY_MEAN_REVERSION","empty-alpha","2026-09-26","SYN","system"},Quantity(0),Price(100)});EXPECT_TRUE(qt_empty_model_owner_document(value).is_error());}
TEST(QtEmptyModelOwner, ForeignQtOwnerRefuses){auto value=sample();value.qt_components.push_back({{"OTHER","LIVE_EQUITY_MEAN_REVERSION","empty-alpha","2026-09-26","SYN","qt"},Quantity(1),Price(100)});EXPECT_TRUE(qt_empty_model_owner_document(value).is_error());}
TEST(QtEmptyModelOwner, ExtraWireFieldRefuses){auto doc=vectors().at("document");doc["ready"]=true;EXPECT_TRUE(canonical_qt_empty_model_owner_bytes(doc).is_error());}
TEST(QtEmptyModelOwner, SelfRehashedMalformedSeedRefuses){auto doc=vectors().at("document");doc["seed_digest"]=std::string(64,'0');EXPECT_TRUE(canonical_qt_empty_model_owner_bytes(doc).is_error());}
TEST(QtEmptyModelOwner, PreservedV1EmptyDigestAndProposalBytesStayExact){
    auto v=vectors();auto seed=qt_digest_v1(J{{"seed_rows",J::array()}});ASSERT_TRUE(seed.is_ok());EXPECT_EQ(seed.value(),v.at("v1_empty_seed_digest").get<std::string>());
    auto manifest=qt_proposal_manifest_document(sample().publication);ASSERT_TRUE(manifest.is_ok());
    EXPECT_EQ(manifest.value().dump(),v.at("proposal_canonical_bytes").get<std::string>());
}
}
