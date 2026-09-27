#include "trade_ngin/data/qt_desk_owner_scope.hpp"
#include <gtest/gtest.h>

namespace trade_ngin { namespace {
using J=nlohmann::json;
J owner(std::string id="ui-one",std::string book="BOOK",bool active=true,std::string lifecycle="live",std::string engine="engine-one") {
    return J{{"id",id},{"strategy_type",engine},{"portfolio_id",book},
        {"is_active",active},{"lifecycle",lifecycle},{"updated_at","2026-09-27T12:00:00Z"}};
}
J member(std::string id="ui-one",std::string book="BOOK") {
    return J{{"strategy_id",id},{"portfolio_id",book}};
}
bool admitted(const J& registry,const J& memberships) {
    return qt_desk_owner_authorized(registry,memberships,"engine-one","BOOK");
}

TEST(QtDeskOwnerScopeTest, PrimaryRegistryIdDiffersFromEngineId) {
    EXPECT_TRUE(admitted(J::array({owner()}),J::array({member()})));
}
TEST(QtDeskOwnerScopeTest, ExactSecondaryBookRegistryMembership) {
    EXPECT_TRUE(admitted(J::array({owner("ui-one","PRIMARY")}),J::array({member()})));
}
TEST(QtDeskOwnerScopeTest, EqualRegistryAndEngineIdRetainsExistingAdmission) {
    EXPECT_TRUE(admitted(J::array({owner("engine-one")}),J::array({member("engine-one")})));
}
TEST(QtDeskOwnerScopeTest, PrimaryBookStillRequiresExplicitMembership) {
    EXPECT_FALSE(admitted(J::array({owner()}),J::array()));
}
TEST(QtDeskOwnerScopeTest, ForeignRegistryIdMatchingEngineCannotAuthorizeOwner) {
    auto foreign=owner("engine-one","FOREIGN",true,"live","foreign-engine");
    EXPECT_FALSE(admitted(J::array({owner(),foreign}),J::array({member("engine-one")})));
}
TEST(QtDeskOwnerScopeTest, TwoActivePrimaryOwnersAreAmbiguous) {
    EXPECT_FALSE(admitted(J::array({owner("engine-one"),owner("ui-two")}),
        J::array({member("engine-one")})));
}
TEST(QtDeskOwnerScopeTest, TwoActiveSecondaryOwnersAreAmbiguous) {
    EXPECT_FALSE(admitted(J::array({owner("ui-one","PRIMARY"),owner("ui-two","OTHER")}),
        J::array({member(),member("ui-two")})));
}
TEST(QtDeskOwnerScopeTest, ForeignSameEngineOutsideBookDoesNotCreateAmbiguity) {
    EXPECT_TRUE(admitted(J::array({owner(),owner("ui-two","FOREIGN")}),
        J::array({member(),member("ui-two","FOREIGN")})));
}
TEST(QtDeskOwnerScopeTest, InactiveOwnerRefuses) {
    EXPECT_FALSE(admitted(J::array({owner("ui-one","BOOK",false)}),J::array({member()})));
}
TEST(QtDeskOwnerScopeTest, InactiveCollisionDoesNotCreateAmbiguity) {
    EXPECT_TRUE(admitted(J::array({owner("engine-one"),owner("ui-two","BOOK",false)}),
        J::array({member("engine-one"),member("ui-two")})));
}
TEST(QtDeskOwnerScopeTest, RetiredOwnerRefuses) {
    EXPECT_FALSE(admitted(J::array({owner("engine-one","BOOK",true,"retired")}),
        J::array({member("engine-one")})));
}
TEST(QtDeskOwnerScopeTest, DifferentBookMembershipDoesNotAuthorizeSecondaryBook) {
    EXPECT_FALSE(admitted(J::array({owner("ui-one","PRIMARY")}),J::array({member("ui-one","OTHER")})));
}
TEST(QtDeskOwnerScopeTest, DuplicateScopedRegistryIdentityRefuses) {
    EXPECT_FALSE(admitted(J::array({owner("engine-one"),owner("engine-one")}),
        J::array({member("engine-one")})));
}
} }
