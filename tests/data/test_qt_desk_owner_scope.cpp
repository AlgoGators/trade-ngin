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
// N5: model publication admits incubating scopes; the QT desk stays live-only.
TEST(QtDeskOwnerScopeTest, IncubatingActiveOwnerIsNotDeskAuthorized) {
    EXPECT_FALSE(admitted(J::array({owner("engine-one","BOOK",true,"incubating")}),
        J::array({member("engine-one")})));
    EXPECT_FALSE(admitted(J::array({owner("ui-one","PRIMARY",true,"incubating")}),J::array({member()})));
}
TEST(QtDeskOwnerScopeTest, IncubatingCollisionNeitherAuthorizesNorCreatesAmbiguity) {
    EXPECT_TRUE(admitted(J::array({owner("engine-one"),owner("ui-two","BOOK",true,"incubating")}),
        J::array({member("engine-one"),member("ui-two")})));
    EXPECT_FALSE(admitted(J::array({owner("engine-one","BOOK",false),owner("ui-two","BOOK",true,"incubating")}),
        J::array({member("engine-one"),member("ui-two")})));
}
// N5 r2 (F2): every engine in a desk selection, editable or carried, needs a live owner.
J selection_row(std::string engine,bool editable) {
    return J{{"key",{{"portfolio_id","BOOK"},{"strategy_id",engine},{"strategy_name","NAME"},{"date","2026-09-22"},
        {"symbol","SYN"},{"portfolio_type","qt_proposal"}}},{"editable",editable}};
}
TEST(QtDeskOwnerScopeTest, SelectionOfLiveEnginesIsAuthorized) {
    const auto registry=J::array({owner("ui-one"),owner("ui-two","BOOK",true,"live","engine-two")});
    const auto members=J::array({member("ui-one"),member("ui-two")});
    EXPECT_TRUE(qt_desk_selection_owners_authorized(registry,members,
        J::array({selection_row("engine-one",true),selection_row("engine-two",false)}),"BOOK"));
}
TEST(QtDeskOwnerScopeTest, CarriedRowOfIncubatingEngineRefusesSelection) {
    const auto registry=J::array({owner("ui-one"),owner("ui-two","BOOK",true,"incubating","engine-two")});
    const auto members=J::array({member("ui-one"),member("ui-two")});
    EXPECT_FALSE(qt_desk_selection_owners_authorized(registry,members,
        J::array({selection_row("engine-one",true),selection_row("engine-two",false)}),"BOOK"));
}
TEST(QtDeskOwnerScopeTest, CarriedRowOfUnregisteredEngineRefusesSelection) {
    EXPECT_FALSE(qt_desk_selection_owners_authorized(J::array({owner()}),J::array({member()}),
        J::array({selection_row("engine-one",true),selection_row("engine-unregistered",false)}),"BOOK"));
}
TEST(QtDeskOwnerScopeTest, CarriedRowOfInactiveOrRetiredEngineRefusesSelection) {
    for(const auto& carried:{owner("ui-two","BOOK",false,"live","engine-two"),owner("ui-two","BOOK",true,"retired","engine-two")})
        EXPECT_FALSE(qt_desk_selection_owners_authorized(J::array({owner(),carried}),
            J::array({member(),member("ui-two")}),
            J::array({selection_row("engine-one",true),selection_row("engine-two",false)}),"BOOK"));
}
TEST(QtDeskOwnerScopeTest, CarriedRowOfLivePrimaryBookEngineNeedsNoEditingMembership) {
    // The desk fixtures' "mixed" book: a held, non-editable component of another live engine whose primary
    // book is this book, without a membership row (editing it would need one; carrying it does not).
    EXPECT_TRUE(qt_desk_selection_owners_authorized(
        J::array({owner(),owner("IMMUTABLE","BOOK",true,"live","IMMUTABLE")}),J::array({member()}),
        J::array({selection_row("engine-one",true),selection_row("IMMUTABLE",false)}),"BOOK"));
}
// N5 r2 (F6): the empty-owner desk facts rule (qt_desk_current_facts.cpp) is live-only.
TEST(QtDeskOwnerScopeTest, EmptyOwnerRegistryIsLiveActiveAndUnchangedOnly) {
    auto row=owner("inc_meanrev","EQUITY_MR_PORTFOLIO",true,"live","LIVE_EQUITY_MEAN_REVERSION");
    row["runtime_revision"]=3;
    const J engine="LIVE_EQUITY_MEAN_REVERSION";
    EXPECT_TRUE(qt_desk_empty_owner_registry_eligible(row,engine,J(3)));
    auto incubating=row;incubating["lifecycle"]="incubating";
    EXPECT_FALSE(qt_desk_empty_owner_registry_eligible(incubating,engine,J(3)));
    auto retired=row;retired["lifecycle"]="retired";
    EXPECT_FALSE(qt_desk_empty_owner_registry_eligible(retired,engine,J(3)));
    auto inactive=row;inactive["is_active"]=false;
    EXPECT_FALSE(qt_desk_empty_owner_registry_eligible(inactive,engine,J(3)));
    EXPECT_FALSE(qt_desk_empty_owner_registry_eligible(row,engine,J(4)));
    EXPECT_FALSE(qt_desk_empty_owner_registry_eligible(row,J("LIVE_OTHER"),J(3)));
}
TEST(QtDeskOwnerScopeTest, DifferentBookMembershipDoesNotAuthorizeSecondaryBook) {
    EXPECT_FALSE(admitted(J::array({owner("ui-one","PRIMARY")}),J::array({member("ui-one","OTHER")})));
}
TEST(QtDeskOwnerScopeTest, DuplicateScopedRegistryIdentityRefuses) {
    EXPECT_FALSE(admitted(J::array({owner("engine-one"),owner("engine-one")}),
        J::array({member("engine-one")})));
}
} }
