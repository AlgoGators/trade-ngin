// Lane N3 (equity day 2): pure derivation of the MODEL -> verified-prior binding row.
// Vectors and expected digests come from gen_binding_vectors.py (independent Python
// canonical JSON), not from the engine under test.
#include <gtest/gtest.h>
#include "trade_ngin/data/qt_equity_model_prior_binding.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
using namespace trade_ngin;
namespace {
using J=nlohmann::json;
const char* const vectors=R"json({"actions_digest":"e5965d9ebf6974607b3e2afdc64253351139b41fb37eab3ed1724af3c5d3c3a1","v1":{"accounting_input_digest":"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee","accounting_input_id":"40000000-0000-4000-8000-000000000001","action_admission":"action_free_only","attempt_id":"50000000-0000-4000-8000-000000000001","basis_positions":[{"average_price_exact":"100","basis_evidence":{"formed_day":"2026-09-19","price_frame_id":"owned-adjusted","source_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","source_id":"qt-basis/94000000-0000-4000-8000-000000000001"},"daily_realized_pnl_exact":"9","daily_unrealized_pnl_exact":"0","key":{"date":"2026-09-24","portfolio_id":"EQUITY_MR_PORTFOLIO","portfolio_type":"qt","strategy_id":"LIVE_EQUITY_MEAN_REVERSION","strategy_name":"EQUITY_MEAN_REVERSION","symbol":"SYN"},"last_update":"2026-09-25T00:00:00Z","quantity_exact":"5"}],"book_id":"EQUITY_MR_PORTFOLIO","decision_id":"10000000-0000-4000-8000-000000000001","finalization_digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","finalization_id":"20000000-0000-4000-8000-000000000001","finalization_source_digest":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","finalization_source_id":"qt-finalization/20000000-0000-4000-8000-000000000001","mode":"verified_desk_prior","model_publication_id":"30000000-0000-4000-8000-000000000001","model_seed_digest":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","observation_digest":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","observation_id":"60000000-0000-4000-8000-000000000001","results_digest":"1111111111111111111111111111111111111111111111111111111111111111","schema_version":"qt-equity-model-prior/v1","source_day":"2026-09-24","valuation_day":"2026-09-25"},"v1_digest":"3f74cf8f6a9a68d02ff4caba0b377264cc1e209d12b5d385f126f881e51f2ecd","v2":{"accounting_input_digest":"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee","accounting_input_id":"40000000-0000-4000-8000-000000000001","action_admission":"proved_action_adjusted_prior","action_frame":{"actions_source":{"book_id":"EQUITY_MR_PORTFOLIO","content_digest":"e5965d9ebf6974607b3e2afdc64253351139b41fb37eab3ed1724af3c5d3c3a1","created_at":"2026-09-25T00:00:00+00:00","payload":{"book_id":"EQUITY_MR_PORTFOLIO","events":[],"previous_day":"2026-09-24","schema_version":"qt-equity-actions-source/v1","source_day":"2026-09-25","valuation_time":"2026-09-25T00:00:00Z"},"policy_revision":1,"policy_version":"p1","producer_id":"synthetic-producer","purpose":"actions","source_day":"2026-09-25","source_id":"qt-actions/b2000000-0000-4000-8000-000000000001","source_version":"qt-actions/b2000000-0000-4000-8000-000000000001"},"adjustments":[],"derived_model_basis_positions":[{"average_price_exact":"100","basis_frame_id":"owned-adjusted","daily_realized_pnl_exact":"9","daily_unrealized_pnl_exact":"0","key":{"date":"2026-09-25","portfolio_id":"EQUITY_MR_PORTFOLIO","portfolio_type":"system","strategy_id":"LIVE_EQUITY_MEAN_REVERSION","strategy_name":"EQUITY_MEAN_REVERSION","symbol":"SYN"},"last_update":"2026-09-25T00:00:00Z","original_basis_evidence":{"formed_day":"2026-09-19","price_frame_id":"owned-adjusted","source_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","source_id":"qt-basis/94000000-0000-4000-8000-000000000001"},"quantity_exact":"5"}],"original_action_count":1,"original_action_digest":"2222222222222222222222222222222222222222222222222222222222222222","original_basis_positions":[{"average_price_exact":"100","basis_evidence":{"formed_day":"2026-09-19","price_frame_id":"owned-adjusted","source_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","source_id":"qt-basis/94000000-0000-4000-8000-000000000001"},"daily_realized_pnl_exact":"9","daily_unrealized_pnl_exact":"0","key":{"date":"2026-09-24","portfolio_id":"EQUITY_MR_PORTFOLIO","portfolio_type":"qt","strategy_id":"LIVE_EQUITY_MEAN_REVERSION","strategy_name":"EQUITY_MEAN_REVERSION","symbol":"SYN"},"last_update":"2026-09-25T00:00:00Z","quantity_exact":"5"}],"owner":{"portfolio_id":"EQUITY_MR_PORTFOLIO","source_day":"2026-09-24","strategy_id":"LIVE_EQUITY_MEAN_REVERSION","strategy_name":"EQUITY_MEAN_REVERSION","valuation_day":"2026-09-25"},"policy_identity":{"book_id":"EQUITY_MR_PORTFOLIO","policy_version":"p1","producer_id":"synthetic-producer","purpose":"execution","version":1},"raw_capture":{"aliases":[],"bars":[],"restating_metadata":[],"terminations":[]},"schema_version":"qt-equity-model-action-frame/v1","successor_action_count":0,"successor_action_digest":"4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945"},"action_frame_digest":"1b7293a9ec65c46f7353f07deee08932f625f306584d3285b9f3f2704accea4b","attempt_id":"50000000-0000-4000-8000-000000000001","basis_positions":[{"average_price_exact":"100","basis_evidence":{"formed_day":"2026-09-19","price_frame_id":"owned-adjusted","source_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","source_id":"qt-basis/94000000-0000-4000-8000-000000000001"},"daily_realized_pnl_exact":"9","daily_unrealized_pnl_exact":"0","key":{"date":"2026-09-24","portfolio_id":"EQUITY_MR_PORTFOLIO","portfolio_type":"qt","strategy_id":"LIVE_EQUITY_MEAN_REVERSION","strategy_name":"EQUITY_MEAN_REVERSION","symbol":"SYN"},"last_update":"2026-09-25T00:00:00Z","quantity_exact":"5"}],"book_id":"EQUITY_MR_PORTFOLIO","decision_id":"10000000-0000-4000-8000-000000000001","finalization_digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","finalization_id":"20000000-0000-4000-8000-000000000001","finalization_source_digest":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","finalization_source_id":"qt-finalization/20000000-0000-4000-8000-000000000001","mode":"verified_desk_prior","model_publication_id":"30000000-0000-4000-8000-000000000001","model_seed_digest":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","observation_digest":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","observation_id":"60000000-0000-4000-8000-000000000001","results_digest":"1111111111111111111111111111111111111111111111111111111111111111","schema_version":"qt-equity-model-prior/v2","source_day":"2026-09-24","valuation_day":"2026-09-25"},"v2_digest":"8fbb68a19f54169c87a518308bae9e5a271757f179e55e5d8f78a69a0c56be34"})json";
J all(){return J::parse(vectors);}
J v1(){return all().at("v1");}
J v2(){return all().at("v2");}
constexpr auto PUB="70000000-0000-4000-8000-000000000001";
constexpr auto BOOK="EQUITY_MR_PORTFOLIO";
constexpr auto D="2026-09-25";
std::string engine_digest(const J& j){auto c=canonical_qt_desk_input_json(j);EXPECT_TRUE(c.is_ok());auto h=qt_sha256_hex(c.value());EXPECT_TRUE(h.is_ok());return h.value();}
bool refused(const J& reference,const std::string& pub=PUB,const std::string& book=BOOK,const std::string& day=D){
    return derive_qt_equity_model_prior_binding(pub,book,day,reference).is_error();}
J rehashed_v2(J reference){reference["action_frame_digest"]=engine_digest(reference.at("action_frame"));return reference;}
}

TEST(QtEquityModelPriorBinding, ActionFreeV1BindsEveryReferenceFieldAndNullActions){
    auto r=derive_qt_equity_model_prior_binding(PUB,BOOK,D,v1());ASSERT_TRUE(r.is_ok());const auto& b=r.value();
    EXPECT_EQ(b.publication_id,PUB);EXPECT_EQ(b.book_id,BOOK);EXPECT_EQ(b.source_day,D);
    EXPECT_EQ(b.strategy_id,"LIVE_EQUITY_MEAN_REVERSION");
    EXPECT_EQ(b.decision_id,v1().at("decision_id").get<std::string>());EXPECT_EQ(b.finalization_id,v1().at("finalization_id").get<std::string>());
    EXPECT_EQ(b.finalization_digest,std::string(64,'a'));EXPECT_EQ(b.finalization_source_digest,std::string(64,'b'));
    EXPECT_EQ(b.finalization_source_id,"qt-finalization/20000000-0000-4000-8000-000000000001");
    EXPECT_FALSE(b.actions_source_id.has_value());EXPECT_FALSE(b.actions_source_digest.has_value());
    EXPECT_EQ(b.replay_reference,v1());
}
TEST(QtEquityModelPriorBinding, ReplayDigestIsTheEngineCanonicalInputJsonSha256){
    auto r=derive_qt_equity_model_prior_binding(PUB,BOOK,D,v1());ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().replay_reference_digest,all().at("v1_digest").get<std::string>());
    EXPECT_EQ(r.value().replay_reference_digest,engine_digest(v1()));
}
TEST(QtEquityModelPriorBinding, ActionFrameV2BindsItsActionsSource){
    auto r=derive_qt_equity_model_prior_binding(PUB,BOOK,D,v2());ASSERT_TRUE(r.is_ok());const auto& b=r.value();
    ASSERT_TRUE(b.actions_source_id.has_value());ASSERT_TRUE(b.actions_source_digest.has_value());
    EXPECT_EQ(*b.actions_source_id,"qt-actions/b2000000-0000-4000-8000-000000000001");
    EXPECT_EQ(*b.actions_source_digest,all().at("actions_digest").get<std::string>());
    EXPECT_EQ(b.replay_reference_digest,all().at("v2_digest").get<std::string>());
    EXPECT_EQ(b.replay_reference,v2());
}
TEST(QtEquityModelPriorBinding, NonVerifiedModeOrUnknownSchemaRefuses){
    auto r=v1();r["mode"]="system_reference";EXPECT_TRUE(refused(r));
    r=v1();r["schema_version"]="qt-equity-model-prior/v3";EXPECT_TRUE(refused(r));
    EXPECT_TRUE(refused(J::object()));EXPECT_TRUE(refused(J::array()));EXPECT_TRUE(refused(J()));
}
TEST(QtEquityModelPriorBinding, MissingContractKeysRefuse){
    for(auto field:{"mode","decision_id","finalization_id","finalization_digest","finalization_source_id",
        "finalization_source_digest","book_id","valuation_day","source_day","action_admission"}){
        auto r=v1();r.erase(field);EXPECT_TRUE(refused(r))<<field;}
}
TEST(QtEquityModelPriorBinding, ForeignBookDayOrPublicationRefuses){
    EXPECT_TRUE(refused(v1(),PUB,"OTHER_BOOK"));
    EXPECT_TRUE(refused(v1(),PUB,BOOK,"2026-09-26"));
    EXPECT_TRUE(refused(v1(),PUB,BOOK,"2026-09-24"));
    EXPECT_TRUE(refused(v1(),"not-a-uuid"));
    EXPECT_TRUE(refused(v1(),"00000000-0000-0000-0000-000000000000"));
    EXPECT_TRUE(refused(v1(),"70000000-0000-4000-8000-00000000000A"));
    auto r=v1();r["source_day"]=D;EXPECT_TRUE(refused(r));
}
TEST(QtEquityModelPriorBinding, MalformedIdentityOrDigestRefuses){
    auto r=v1();r["decision_id"]="10000000-0000-4000-8000-00000000000G";EXPECT_TRUE(refused(r));
    r=v1();r["finalization_digest"]=std::string(63,'a');EXPECT_TRUE(refused(r));
    r=v1();r["finalization_source_digest"]=std::string(64,'B');EXPECT_TRUE(refused(r));
    r=v1();r["finalization_source_id"]="qt-finalization/20000000-0000-4000-8000-000000000002";EXPECT_TRUE(refused(r));
    r=v1();r["decision_id"]=1;EXPECT_TRUE(refused(r));
}
TEST(QtEquityModelPriorBinding, V1CannotCarryAnActionFrame){
    auto r=v1();r["action_frame"]=v2().at("action_frame");EXPECT_TRUE(refused(r));
    r=v1();r["action_frame_digest"]=v2().at("action_frame_digest");EXPECT_TRUE(refused(r));
    r=v1();r["action_admission"]="proved_action_adjusted_prior";EXPECT_TRUE(refused(r));
}
TEST(QtEquityModelPriorBinding, V2FrameMustBeSelfConsistent){
    auto r=v2();r["action_admission"]="action_free_only";EXPECT_TRUE(refused(r));
    r=v2();r["action_frame"]["original_action_count"]=2;EXPECT_TRUE(refused(r));  // stale frame digest
    r=v2();r.erase("action_frame_digest");EXPECT_TRUE(refused(r));
    r=v2();r["action_frame"].erase("actions_source");EXPECT_TRUE(refused(rehashed_v2(r)));
    r=v2();r["action_frame"]["actions_source"]["book_id"]="OTHER_BOOK";EXPECT_TRUE(refused(rehashed_v2(r)));
    r=v2();r["action_frame"]["actions_source"]["source_day"]="2026-09-24";EXPECT_TRUE(refused(rehashed_v2(r)));
    r=v2();r["action_frame"]["actions_source"]["purpose"]="market";EXPECT_TRUE(refused(rehashed_v2(r)));
    r=v2();r["action_frame"]["actions_source"]["content_digest"]="short";EXPECT_TRUE(refused(rehashed_v2(r)));
    r=v2();r["action_frame"]["actions_source"]["source_id"]="";EXPECT_TRUE(refused(rehashed_v2(r)));
    r=v2();r["action_frame"]["actions_source"]["source_id"]="qt-actions/other";auto ok=derive_qt_equity_model_prior_binding(PUB,BOOK,D,rehashed_v2(r));
    ASSERT_TRUE(ok.is_ok());EXPECT_EQ(*ok.value().actions_source_id,"qt-actions/other");
}
TEST(QtEquityModelPriorBinding, NonCanonicalReferenceRefusesInsteadOfHashingAnotherForm){
    auto r=v1();r["unexpected_float"]=1.5;EXPECT_TRUE(refused(r));
    r=v1();r["oversized"]=std::string(4097,'x');EXPECT_TRUE(refused(r));
}
TEST(QtEquityModelPriorBinding, AnyReferenceChangeChangesTheDigest){
    auto r=v1();r["results_digest"]=std::string(64,'2');
    auto changed=derive_qt_equity_model_prior_binding(PUB,BOOK,D,r);ASSERT_TRUE(changed.is_ok());
    EXPECT_NE(changed.value().replay_reference_digest,all().at("v1_digest").get<std::string>());
}
