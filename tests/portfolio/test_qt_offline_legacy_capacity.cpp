#include "trade_ngin/portfolio/qt_equity_proof.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/portfolio/qt_evaluation.hpp"
#include "qt_test_build_identity.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
using namespace trade_ngin;
TEST(QtOfflineEnvelope, RetainsLegacyValid4096OwnersAndHistoryBelowEightMiB){
    using J=nlohmann::json;
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"contracts"/"qt-eval-v1.json";
    std::ifstream file(path,std::ios::binary);ASSERT_TRUE(file.good());
    auto r=trade_ngin::test::fixture_for_compiled_build(J::parse(file).at("selected_book"));
    const auto slot=r.at("context").at("slots")[0];
    const auto proposal=r.at("proposal").at("quantities")[0];
    const auto cost=r.at("component_cost_inputs")[0];
    const auto close=r.at("risk_inputs").at("closes")[0];
    r["context"]["slots"]=J::array();r["proposal"]["quantities"]=J::array();
    r["component_cost_inputs"]=J::array();r["risk_inputs"]["closes"]=J::array();
    r["risk_inputs"]["expected_observation_times"]=J::array();
    for(unsigned n=0;n<4096;++n){
        auto s=slot,p=proposal,c=cost,h=close;
        const auto owner="synthetic-owner-"+std::to_string(n);
        s["key"]["strategy_name"]=owner;p["key"]["strategy_name"]=owner;c["key"]["strategy_name"]=owner;
        r["context"]["slots"].push_back(s);r["proposal"]["quantities"].push_back(p);
        r["component_cost_inputs"].push_back(c);
        char timestamp[32];std::snprintf(timestamp,sizeof timestamp,"2026-09-24T%02u:%02u:%02uZ",n/3600,(n/60)%60,n%60);
        h["timestamp"]=timestamp;r["risk_inputs"]["closes"].push_back(h);
        r["risk_inputs"]["expected_observation_times"].push_back(timestamp);
    }
    r["risk_config"]["lookback_period"]=4096;
    const auto bytes=r.dump();ASSERT_LT(bytes.size(),8u*1024*1024);
    size_t events=0;
    auto counted=J::parse(bytes,[&](int,J::parse_event_t,J&){++events;return true;});
    ASSERT_EQ(counted,r);ASSERT_GT(events,400000u);
    // First establish actual accepted legacy semantics; no unknown fields or
    // duplicate owner/history identities manufacture this compatibility RED.
    const auto original=parse_qt_evaluation_request(bytes);ASSERT_TRUE(original.is_ok());
    ASSERT_EQ(original.value().context.slots.size(),4096u);
    auto routed=parse_qt_offline_envelope(bytes);ASSERT_TRUE(routed.is_ok());
    EXPECT_EQ(routed.value(),r);
    RecordProperty("legacy_request_bytes",std::to_string(bytes.size()));
    RecordProperty("legacy_parse_events",std::to_string(events));
}
