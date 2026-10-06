#include "trade_ngin/portfolio/qt_equity_proof.hpp"
#include "trade_ngin/apps/qt_equity_accounting_output.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/portfolio/qt_evaluation.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/git_version.hpp"
#include "qt_test_build_identity.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

using namespace trade_ngin;
namespace {
using J=nlohmann::json;
#define fixture unbound_fixture
// Exact owned synthetic helper fixture is embedded below; no filesystem input
// or purported SQL authority is accepted by the production wire.
J fixture(){return J::parse(R"QT_FIXTURE({
  "accounting_input": {
    "accounting_input_id": "20000000-0000-0000-0000-000000000001",
    "accounting_source_id": "owned-synthetic-equity-accounting",
    "actions": [],
    "book_id": "BOOK",
    "calculation_version": "qt-equity-main08b15c/v1",
    "cost_config": {
      "explicit_fee_per_contract": "1.5",
      "max_participation": "1",
      "min_adv": "10000",
      "min_participation": "0"
    },
    "currency": "USD",
    "day_mode": "open",
    "decision_id": "10000000-0000-0000-0000-000000000001",
    "instruments": [
      {
        "asset_type": "EQUITY",
        "cost_evidence": {
          "adv_model_number": "1000000",
          "date": "2026-09-24",
          "source_digest": "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
          "source_id": "owned-synthetic-cost-history",
          "volatility_multiplier_model_number": "1"
        },
        "cost_parameters": {
          "apply_regulatory_fees": false,
          "baseline_spread_ticks": "1",
          "commission_per_unit": "0.005",
          "finra_taf_cap_per_trade": "9.79",
          "finra_taf_per_share": "0.000195",
          "max_commission_pct": "0.01",
          "max_commission_per_order": "100",
          "max_impact_bps": "100",
          "max_spread_ticks": "10",
          "max_total_implicit_bps": "0",
          "min_commission_per_order": "1",
          "min_spread_ticks": "1",
          "point_value": "1",
          "sec_fee_per_million": "20.6",
          "spread_cost_multiplier": "0.5",
          "tick_constrained": false,
          "tick_size": "0.01"
        },
        "mark": {
          "date": "2026-09-24",
          "price_frame_id": "adjusted-1",
          "price_model_number": "14",
          "source_digest": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "source_id": "owned-synthetic-close"
        },
        "reference": {
          "date": "2026-09-24",
          "price_frame_id": "adjusted-1",
          "price_model_number": "14",
          "source_digest": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "source_id": "owned-synthetic-close"
        },
        "symbol": "SYN"
      }
    ],
    "market_source_digest": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "market_source_id": "30000000-0000-0000-0000-000000000001",
    "previous_day": "2026-09-24",
    "previous_positions": [
      {
        "average_price_exact": "10",
        "basis_evidence": {
          "formed_day": "2026-09-20",
          "price_frame_id": "adjusted-1",
          "source_digest": "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
          "source_id": "owned-prior-position"
        },
        "daily_realized_pnl_exact": "9",
        "daily_unrealized_pnl_exact": "6",
        "key": {
          "date": "2026-09-24",
          "portfolio_id": "BOOK",
          "portfolio_type": "qt",
          "strategy_id": "ENGINE",
          "strategy_name": "owner-a",
          "symbol": "SYN"
        },
        "last_update": "2026-09-24T00:00:00Z",
        "quantity_exact": "1.5"
      }
    ],
    "previous_totals": [
      {
        "equity_exact": "1026",
        "initial_capital_exact": "1000",
        "strategy_id": "ENGINE",
        "total_pnl_exact": "26",
        "total_realized_pnl_exact": "30",
        "total_transaction_costs_exact": "10",
        "total_unrealized_pnl_exact": "6"
      }
    ],
    "prior_finalization_digest": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    "prior_finalization_source_id": "qt-finalization/40000000-0000-0000-0000-000000000001",
    "schema_version": "qt-equity-accounting-input/v1",
    "source_day": "2026-09-25",
    "timestamp": "2026-09-25T00:00:00Z"
  },
  "decision": {
    "book_id": "BOOK",
    "decision_id": "10000000-0000-0000-0000-000000000001",
    "model_publication_id": "50000000-0000-0000-0000-000000000001",
    "source_day": "2026-09-25"
  },
  "producer_authority": {
    "allowed_override_codes": [],
    "as_of": "2026-09-25T00:00:00Z",
    "book_id": "BOOK",
    "content_digest": "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
    "evaluator_build": "local-qt-controlled",
    "evaluator_bundle_sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    "evaluator_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "model_publication_id": "50000000-0000-0000-0000-000000000001",
    "policy_revision": 1,
    "policy_updated_at": "2026-09-20T00:00:00Z",
    "policy_version": "owned-synthetic-evaluation-policy/v1",
    "producer_id": "owned-synthetic-evaluator",
    "schema_version": "qt-input-authority/v1",
    "snapshot_id": 1,
    "source_day": "2026-09-25",
    "source_version": "owned-synthetic-evaluation/v1",
    "valid_until": "2026-09-26T00:00:00Z"
  },
  "selection": [
    {
      "asset_type": "EQUITY",
      "average_price_exact": "10",
      "basis_status": "preserved_source",
      "editable": true,
      "key": {
        "date": "2026-09-25",
        "portfolio_id": "BOOK",
        "portfolio_type": "qt_proposal",
        "strategy_id": "ENGINE",
        "strategy_name": "owner-a",
        "symbol": "SYN"
      },
      "origin": "qt_draft",
      "quantity_exact": "0.5"
    }
  ]
}
)QT_FIXTURE");}
#undef fixture
J fixture(){return trade_ngin::test::fixture_for_compiled_build(unbound_fixture());}
std::string hash(const J& value,bool source=false){
    auto bytes=source?canonical_qt_desk_source_json(value):canonical_qt_desk_input_json(value);
    if(bytes.is_error())throw std::runtime_error("test canonicalization failed");
    auto digest=qt_sha256_hex(bytes.value());
    if(digest.is_error())throw std::runtime_error("test hash failed");
    return digest.value();
}
void fingerprint(J& r,bool source=false){r.erase("context_fingerprint");r["context_fingerprint"]=hash(r,source);}
J request(){
    auto f=fixture();f["decision"].erase("model_publication_id");
    f["producer_authority"]["evaluator_build"]=TRADE_NGIN_GIT_SHA;
    J r={{"schema","qt-equity-proof/v1"},{"operation","recompute_equity_accounting"},
        {"evaluator_build",TRADE_NGIN_GIT_SHA},{"decision",f.at("decision")},
        {"selection_rows",f.at("selection")},{"accounting_input",f.at("accounting_input")},
        {"producer_authority",f.at("producer_authority")}};
    fingerprint(r);return r;
}
void reject(J r){fingerprint(r);EXPECT_TRUE(recompute_qt_equity_proof(r).is_error());}
}

TEST(QtEquityProof, FractionalActualKernelOutputHasExactClosedDigestResponse){
    auto r=request();const auto saved=r;
    auto output=recompute_qt_equity_accounting_output(r.at("decision"),r.at("selection_rows"),
        r.at("accounting_input"),r.at("producer_authority"));
    ASSERT_TRUE(output.is_ok());const auto& o=output.value();
    ASSERT_EQ(o.at("executions").size(),1u);
    EXPECT_EQ(o.at("executions")[0].at("side"),"SELL");
    EXPECT_EQ(o.at("executions")[0].at("quantity_exact"),"1");
    EXPECT_EQ(o.at("executions")[0].at("commissions_fees_exact"),"0.14");
    const auto& live=o.at("live_results")[0];
    EXPECT_EQ(live.at("daily_realized_pnl_exact"),"4");
    EXPECT_EQ(live.at("daily_unrealized_pnl_exact"),"-4");
    EXPECT_EQ(live.at("total_transaction_costs_exact"),"10.14");
    EXPECT_EQ(live.at("total_pnl_exact"),"25.86");
    EXPECT_EQ(live.at("current_portfolio_value_exact"),"1025.86");
    EXPECT_EQ(o.at("consumption").at("charges").size(),1u);
    auto financial=o;financial.erase("consumption");
    auto result=recompute_qt_equity_proof(r);ASSERT_TRUE(result.is_ok());
    const J expected={{"schema","qt-equity-proof/v1"},{"operation",r.at("operation")},
        {"evaluator_build",TRADE_NGIN_GIT_SHA},{"context_fingerprint",r.at("context_fingerprint")},
        {"calculation_version","qt-equity-main08b15c/v1"},
        {"input_digest",hash(r.at("accounting_input"))},
        {"selection_digest",hash(r.at("selection_rows"))},
        {"financial_output_digest",hash(financial)},{"output_digest",hash(o,true)}};
    EXPECT_EQ(result.value(),expected);for(const auto& v:result.value())EXPECT_TRUE(v.is_string());
    EXPECT_NE(result.value().at("output_digest"),result.value().at("financial_output_digest"));
    EXPECT_EQ(r,saved);RecordProperty("actual_equity_proof_request",r.dump());
    RecordProperty("actual_equity_proof_response",result.value().dump());
    RecordProperty("actual_equity_proof_output",o.dump());
}

TEST(QtEquityProof, QuietActualTraceIsPresentEmptyAndRecomputationDeterministic){
    auto r=request();r["selection_rows"][0]["quantity_exact"]="1.5";fingerprint(r);
    auto output=recompute_qt_equity_accounting_output(r.at("decision"),r.at("selection_rows"),r.at("accounting_input"),r.at("producer_authority"));
    ASSERT_TRUE(output.is_ok());EXPECT_TRUE(output.value().at("executions").empty());
    EXPECT_TRUE(output.value().at("consumption").at("charges").empty());
    auto a=recompute_qt_equity_proof(r),b=recompute_qt_equity_proof(r);
    ASSERT_TRUE(a.is_ok());ASSERT_TRUE(b.is_ok());EXPECT_EQ(a.value(),b.value());
    EXPECT_EQ(a.value().at("output_digest"),hash(output.value(),true));
}

TEST(QtEquityProof, BindsFingerprintAndCompiledBuild){
    auto r=request();r["context_fingerprint"]=std::string(64,'0');EXPECT_TRUE(recompute_qt_equity_proof(r).is_error());
    r=request();r["evaluator_build"]="different-compiled-build";reject(r);
    r=request();r["producer_authority"]["evaluator_build"]="different-compiled-build";reject(r);
    r=request();r["evaluator_build"]="unknown";reject(r);
    r=request();r["context_fingerprint"]=42;EXPECT_TRUE(recompute_qt_equity_proof(r).is_error());
}
TEST(QtEquityProof, RejectsUnknownAndExpectedOutputOrExternalAuthorityInputs){
    for(const auto* name:{"expected_output","output_digest","financial_output_digest","database","source_path","dsn","trusted"}){
        auto r=request();r[name]="forbidden";reject(r);
    }
    for(const auto* name:{"schema","operation","evaluator_build","decision","selection_rows","accounting_input","producer_authority"}){
        auto r=request();r.erase(name);reject(r);
    }
    auto r=request();r["schema"]="qt-equity-proof/v2";reject(r);
    r=request();r["operation"]="evaluate_selected_book";reject(r);
}
TEST(QtEquityProof, DecisionIsExactlyMinimalThreeFields){
    auto r=request();r["decision"]["model_publication_id"]=r.at("producer_authority").at("model_publication_id");reject(r);
    for(const auto* name:{"book_id","source_day","decision_id"}){r=request();r["decision"].erase(name);reject(r);}
    r=request();r["decision"]["decision_id"]="bad-uuid";reject(r);
    r=request();r["decision"]["source_day"]="2026-02-30";reject(r);
}
TEST(QtEquityProof, CompleteSelectionShapeAndFullOwnerAreRequired){
    for(const auto* name:{"key","asset_type","editable","quantity_exact","average_price_exact","origin","basis_status"}){
        auto r=request();r["selection_rows"][0].erase(name);reject(r);
    }
    auto r=request();r["selection_rows"][0]["extra"]=true;reject(r);
    r=request();r["selection_rows"][0]["asset_type"]="FUTURE";reject(r);
    r=request();r["selection_rows"][0]["key"].erase("strategy_name");reject(r);
    r=request();r["selection_rows"][0]["key"]["portfolio_id"]="OTHER";reject(r);
    r=request();r["selection_rows"].push_back(r["selection_rows"][0]);reject(r);
    r=request();r["selection_rows"]=J::array();reject(r);
}
TEST(QtEquityProof, InputAndAuthorityRemainClosedAndSemanticallyBound){
    auto r=request();r["accounting_input"]["extra"]=1;reject(r);
    r=request();r["accounting_input"]["book_id"]="OTHER";reject(r);
    r=request();r["accounting_input"]["schema_version"]="qt-futures-accounting-input/v1";reject(r);
    r=request();r["accounting_input"]["instruments"][0]["cost_parameters"].erase("min_commission_per_order");reject(r);
    r=request();r["producer_authority"]["extra"]=true;reject(r);
    r=request();r["producer_authority"].erase("evaluator_bundle_sha256");reject(r);
    r=request();r["producer_authority"]["book_id"]="OTHER";reject(r);
    r=request();r["producer_authority"]["model_publication_id"]="not-uuid";reject(r);
    r=request();r["producer_authority"]["snapshot_id"]="not-an-ordinal";reject(r);
    r=request();r["producer_authority"]["policy_revision"]=false;reject(r);
    r=request();r["producer_authority"]["valid_until"]=r["producer_authority"]["as_of"];reject(r);
}
TEST(QtEquityProof, RejectsFloatingRequestTokensIncludingRehashedAndInMemoryNonfinite){
    for(double number:{1.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}){
        auto r=request();r["producer_authority"]["policy_revision"]=number;
        if(std::isfinite(number))fingerprint(r,true);
        EXPECT_TRUE(recompute_qt_equity_proof(r).is_error());
    }
    auto r=request();r["accounting_input"]["instruments"][0]["reference"]["price_model_number"]=14.0;
    fingerprint(r,true);EXPECT_TRUE(recompute_qt_equity_proof(r).is_error());
}
TEST(QtOfflineEnvelope, RejectsDuplicatesAtEveryNestingLevelAndInvalidNumbers){
    for(const auto* bytes:{R"({"schema":"qt-equity-proof/v1","schema":"qt-eval/v1"})",
            R"({"schema":"qt-equity-proof/v1","decision":{"book_id":"A","book_id":"B"}})",
            R"({"schema":"qt-equity-proof/v1","rows":[{"key":1,"key":2}]})",
            R"({"schema":"qt-equity-proof/v1","x":NaN})",
            R"({"schema":"qt-equity-proof/v1","x":Infinity})",
            R"({"schema":"qt-equity-proof/v1","x":1e999})"})
        EXPECT_TRUE(parse_qt_offline_envelope(bytes).is_error());
}
TEST(QtOfflineEnvelope, BoundsBytesDepthAndEventsBeforeSemanticAdmission){
    EXPECT_TRUE(parse_qt_offline_envelope(std::string(8u*1024*1024+1,' ')).is_error());
    const auto deep=std::string("{\"schema\":\"qt-equity-proof/v1\",\"x\":")+std::string(34,'[')+"0"+std::string(34,']')+"}";
    EXPECT_TRUE(parse_qt_offline_envelope(deep).is_error());
    std::string many="{\"schema\":\"qt-equity-proof/v1\",\"x\":[";
    for(unsigned i=0;i<400001;++i){if(i)many+=',';many+='0';}many+="]}";
    ASSERT_LT(many.size(),8u*1024*1024);EXPECT_TRUE(parse_qt_offline_envelope(many).is_error());
    EXPECT_TRUE(parse_qt_offline_envelope("[]").is_error());EXPECT_TRUE(parse_qt_offline_envelope("").is_error());
}
TEST(QtOfflineEnvelope, RoutesFullEquityRequestWithoutChangingImmutableOperands){
    const auto r=request();auto parsed=parse_qt_offline_envelope(r.dump());
    ASSERT_TRUE(parsed.is_ok());EXPECT_EQ(parsed.value(),r);
    auto result=recompute_qt_equity_proof(parsed.value());ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().at("context_fingerprint"),r.at("context_fingerprint"));
}
TEST(QtOfflineEnvelope, ExistingEvaluationWireStillUsesOriginalSemanticParser){
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"contracts"/"qt-eval-v1.json";
    std::ifstream file(path,std::ios::binary);ASSERT_TRUE(file.good());
    auto existing=trade_ngin::test::fixture_for_compiled_build(J::parse(file).at("selected_book"));const auto bytes=existing.dump();
    auto direct=parse_qt_evaluation_request(bytes);ASSERT_TRUE(direct.is_ok());
    auto routed=parse_qt_offline_envelope(bytes);ASSERT_TRUE(routed.is_ok());EXPECT_EQ(routed.value(),existing);
    auto retained=parse_qt_evaluation_request(bytes);ASSERT_TRUE(retained.is_ok());
    EXPECT_EQ(retained.value().context_fingerprint,direct.value().context_fingerprint);
    EXPECT_EQ(retained.value().evaluator_build,direct.value().evaluator_build);
    EXPECT_TRUE(recompute_qt_equity_proof(routed.value()).is_error());
}
