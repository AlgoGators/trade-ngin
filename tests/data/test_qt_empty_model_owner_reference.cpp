#include <gtest/gtest.h>
#include "trade_ngin/data/qt_empty_model_owner_reference.hpp"
#include "empty_owner_vectors.hpp"
#include "qt_test_build_identity.hpp"
#include <limits>
using namespace trade_ngin;
namespace {
using J=nlohmann::json;
J vector(){return trade_ngin::test::fixture_for_compiled_build(J::parse(empty_owner_test::fixture));}
TEST(QtEmptyOwnerReference, ExactIndependentVariantKeepsItsDiscriminant){
 auto v=vector();auto d=v.at("document");auto result=qt_empty_model_owner_reference(d,7,TRADE_NGIN_GIT_SHA,"registry-equity",0);ASSERT_TRUE(result.is_ok());
 auto& r=result.value();EXPECT_EQ(r.size(),13U);EXPECT_EQ(r.at("schema_version"),"qt-empty-model-owner-reference/v2");
 EXPECT_EQ(r.at("owner_document_digest"),v.at("owner_digest"));EXPECT_EQ(r.at("publication_id"),d.at("publication_id"));
 EXPECT_EQ(r.at("configured_owner_names"),d.at("configured_owner_names"));EXPECT_EQ(r.at("registry_revision"),0);
}
TEST(QtEmptyOwnerReference, LargestActualTypedOrdinalsStayExact){
 auto d=vector().at("document");auto maximum=std::numeric_limits<int64_t>::max();auto r=qt_empty_model_owner_reference(d,maximum,"local","registry",maximum);ASSERT_TRUE(r.is_ok());
 EXPECT_EQ(r.value().at("publication_version"),maximum);EXPECT_EQ(r.value().at("registry_revision"),maximum);
}
TEST(QtEmptyOwnerReference, ZeroOrNegativePublicationVersionRefuses){auto d=vector().at("document");for(int v:{0,-1})EXPECT_TRUE(qt_empty_model_owner_reference(d,v,"local","registry",0).is_error());}
TEST(QtEmptyOwnerReference, NegativeRegistryRevisionRefuses){EXPECT_TRUE(qt_empty_model_owner_reference(vector().at("document"),1,"local","registry",-1).is_error());}
TEST(QtEmptyOwnerReference, MissingProducerOrRegistryRefuses){auto d=vector().at("document");EXPECT_TRUE(qt_empty_model_owner_reference(d,1,"","registry",0).is_error());EXPECT_TRUE(qt_empty_model_owner_reference(d,1,"local"," ",0).is_error());}
TEST(QtEmptyOwnerReference, SelfRehashedAndExtraAuthorityFieldsRefuse){auto d=vector().at("document");d["qt_digest"]=std::string(64,'0');EXPECT_TRUE(qt_empty_model_owner_reference(d,1,"local","registry",0).is_error());d=vector().at("document");d["available"]=true;EXPECT_TRUE(qt_empty_model_owner_reference(d,1,"local","registry",0).is_error());}
}
