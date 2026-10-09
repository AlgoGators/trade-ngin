// T-7b-2 C9e (ledger LOOP-cut-lands-by-refusal): the risk gate's cut delivered in whole contracts.
//
// deliver_cut_in_whole_contracts is the rule the PortfolioManager applies on a lap the gate cut:
// from the lap's book, remove one contract at a time until the gross notional is at or below
// factor x the lap book's; the contracts the day's request added first, held contracts after;
// within a class the removal that leaves the book nearest the gate's target in the optimizer's
// tracking error, and the best fit on notional where no candidate is in the covariance. The two
// worked days are real inputs, dumped at full precision from the futures backtest on the parent
// 2374b89f's path (the probe TVOL_C9E_DUMP of evidence run S9e_SHDUMP_D2HT, never committed):
// every number below is the log's, and the expected books are that run's C9E_CUT lines.

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "trade_ngin/portfolio/cut_delivery.hpp"

using namespace trade_ngin;

namespace {

double gross(const CutDeliveryInput& in, const std::map<std::string, double>& book) {
    double g = 0.0;
    for (const auto& [sym, q] : book) {
        auto it = in.notional_per_contract.find(sym);
        if (it != in.notional_per_contract.end() && it->second > 0.0)
            g += std::abs(q) * it->second;
    }
    return g;
}

// S9e_SHDUMP_D2HT 2025-06-25: C9E_CUT shadow=1 lap=1 design=D2HT factor=0.8713519786 notional
// lap_book=1026202.31 held=612914.56 target=894183.42 cut_book=886800.44 removed_new=2
// removed_held=0 removed: MES.v.0(new) ZF.v.0(new) | 6L.v.0 held=0 lap=2 cut=2 6M.v.0 held=2 lap=1
// cut=1 M2K.v.0 held=0 lap=2 cut=2 MES.v.0 held=1 lap=2 cut=1 MYM.v.0 held=1 lap=2 cut=2 ZF.v.0
// held=0 lap=2 cut=1 ZN.v.0 held=0 lap=1 cut=1
static CutDeliveryInput worked_2025_06_25() {
    CutDeliveryInput in;
    in.factor = 0.87135197858516089;
    in.capital = 507273.38798383001;
    in.lap_book["ZF.v.0"] = 2.0;
    in.notional_per_contract["ZF.v.0"] = 108671.875;
    in.lap_book["ZT.v.0"] = 1.0;
    in.held["ZT.v.0"] = 1.0;
    in.notional_per_contract["ZT.v.0"] = 207757.8125;
    in.lap_book["6B.v.0"] = 1.0;
    in.held["6B.v.0"] = 1.0;
    in.notional_per_contract["6B.v.0"] = 85143.75;
    in.lap_book["6L.v.0"] = 2.0;
    in.notional_per_contract["6L.v.0"] = 18100;
    in.lap_book["PL.v.0"] = 1.0;
    in.held["PL.v.0"] = 1.0;
    in.notional_per_contract["PL.v.0"] = 65524.999999999993;
    in.lap_book["MBT.v.0"] = 3.0;
    in.held["MBT.v.0"] = 3.0;
    in.notional_per_contract["MBT.v.0"] = 10622;
    in.lap_book["6M.v.0"] = 1.0;
    in.held["6M.v.0"] = 2.0;
    in.notional_per_contract["6M.v.0"] = 26090;
    in.lap_book["M2K.v.0"] = 2.0;
    in.notional_per_contract["M2K.v.0"] = 10885.5;
    in.lap_book["MNQ.v.0"] = 1.0;
    in.held["MNQ.v.0"] = 1.0;
    in.notional_per_contract["MNQ.v.0"] = 44849;
    in.lap_book["MYM.v.0"] = 2.0;
    in.held["MYM.v.0"] = 1.0;
    in.notional_per_contract["MYM.v.0"] = 21708;
    in.lap_book["ZN.v.0"] = 1.0;
    in.notional_per_contract["ZN.v.0"] = 111625;
    in.lap_book["6C.v.0"] = 1.0;
    in.held["6C.v.0"] = 1.0;
    in.notional_per_contract["6C.v.0"] = 73155;
    in.lap_book["MES.v.0"] = 2.0;
    in.held["MES.v.0"] = 1.0;
    in.notional_per_contract["MES.v.0"] = 30730;
    in.covariance_symbols = {"6B.v.0",  "6C.v.0",  "6L.v.0",  "6M.v.0",  "M2K.v.0",
                             "MBT.v.0", "MES.v.0", "MNQ.v.0", "MYM.v.0", "PL.v.0",
                             "ZF.v.0",  "ZN.v.0",  "ZT.v.0"};
    in.covariance = {
        {0.005338892707813836, 0.0021547375270608423, 0.002594587955222386, 0.0029083513707364645,
         0.004241019533642666, 0.003689318770703119, 0.003094489756637371, 0.004067174050203684,
         0.0022101135320496336, 0.00696620735771768, 0.0007153858470940069, 0.0011029001929198437,
         0.0002842291083142801},
        {0.0021547375270608423, 0.003059714819319604, 0.0025505347837969446, 0.002061130851566034,
         0.001839985268859281, 0.0014340466606829964, 0.0014520908583594673, 0.002278596729118437,
         0.0008833057334996502, 0.0030551325850337926, 0.00029603968168444175,
         0.0004703017184411521, 0.0001270821895888995},
        {0.002594587955222386, 0.0025505347837969446, 0.019912790271135943, 0.008366182843495494,
         0.015032148174826513, 0.013214983311776453, 0.011663004539666559, 0.014819030265269664,
         0.009327635180853106, 0.012162470880216188, -0.000274521490280294, -7.1362096252984e-06,
         -0.00031059952830007204},
        {0.0029083513707364645, 0.002061130851566034, 0.008366182843495494, 0.019468199910810922,
         0.01266823550416945, 0.012518481699082163, 0.00990972480087541, 0.012718628457974633,
         0.007716326978422572, 0.011585006352316895, -0.00010460626289453678, 7.789968016463617e-05,
         -0.00018751438059860944},
        {0.004241019533642666, 0.001839985268859281, 0.015032148174826513, 0.01266823550416945,
         0.06754689312812583, 0.06600208112643556, 0.04716875685740192, 0.05557953763427527,
         0.04249943575358939, 0.024490177206254897, -0.0014367454818547318, -0.0006779947916167215,
         -0.0012040411611089151},
        {0.003689318770703119, 0.0014340466606829964, 0.013214983311776453, 0.012518481699082163,
         0.06600208112643556, 0.27415087677456373, 0.0467121491819661, 0.05983582934888195,
         0.03773267414159604, 0.030962415113411884, -0.003111406296825892, -0.0035957040075010206,
         -0.0017402627216335487},
        {0.003094489756637371, 0.0014520908583594673, 0.011663004539666559, 0.00990972480087541,
         0.04716875685740192, 0.0467121491819661, 0.04295934978694611, 0.05307633410475697,
         0.03543506176470238, 0.01896377331076792, -0.001450458712163429, -0.0009261642348284925,
         -0.0010910317809037073},
        {0.004067174050203684, 0.002278596729118437, 0.014819030265269664, 0.012718628457974633,
         0.05557953763427527, 0.05983582934888195, 0.05307633410475697, 0.06984884698362631,
         0.04067560119787557, 0.024579250926339884, -0.0021297987358261097, -0.0016394047165503769,
         -0.0014878864694740938},
        {0.0022101135320496336, 0.0008833057334996502, 0.009327635180853106, 0.007716326978422572,
         0.04249943575358939, 0.03773267414159604, 0.03543506176470238, 0.04067560119787557,
         0.03398872810933892, 0.015391690661381269, -0.0009641829334254466, -0.0004112489077058881,
         -0.0007959787307902248},
        {0.00696620735771768, 0.0030551325850337926, 0.012162470880216188, 0.011585006352316895,
         0.024490177206254897, 0.030962415113411884, 0.01896377331076792, 0.024579250926339884,
         0.015391690661381269, 0.08668291392459855, 0.0005585940324861113, 0.0012642122495849957,
         8.110192809038972e-05},
        {0.0007153858470940069, 0.00029603968168444175, -0.000274521490280294,
         -0.00010460626289453678, -0.0014367454818547318, -0.003111406296825892,
         -0.001450458712163429, -0.0021297987358261097, -0.0009641829334254466,
         0.0005585940324861113, 0.0014996307983995093, 0.002115366139670544, 0.0006816219147439488},
        {0.0011029001929198437, 0.0004703017184411521, -7.1362096252984e-06, 7.789968016463617e-05,
         -0.0006779947916167215, -0.0035957040075010206, -0.0009261642348284925,
         -0.0016394047165503769, -0.0004112489077058881, 0.0012642122495849957,
         0.002115366139670544, 0.003176396955069025, 0.0008856421120399943},
        {0.0002842291083142801, 0.0001270821895888995, -0.00031059952830007204,
         -0.00018751438059860944, -0.0012040411611089151, -0.0017402627216335487,
         -0.0010910317809037073, -0.0014878864694740938, -0.0007959787307902248,
         8.110192809038972e-05, 0.0006816219147439488, 0.0008856421120399943,
         0.0003630195139874275},
    };
    return in;
}

// S9e_SHDUMP_D2HT 2025-02-27: C9E_CUT shadow=1 lap=1 design=D2HT factor=0.8744379297 notional
// lap_book=531734.38 held=0.00 target=464968.71 cut_book=325578.12 removed_new=1 removed_held=0
// removed: ZT.v.0(new) | ZF.v.0 held=0 lap=2 cut=2 ZN.v.0 held=0 lap=1 cut=1 ZT.v.0 held=0 lap=1
// cut=0
static CutDeliveryInput zt_2025_02_27() {
    CutDeliveryInput in;
    in.factor = 0.87443792970132062;
    in.capital = 500000;
    in.lap_book["ZF.v.0"] = 2.0;
    in.notional_per_contract["ZF.v.0"] = 107437.5;
    in.lap_book["ZT.v.0"] = 1.0;
    in.notional_per_contract["ZT.v.0"] = 206156.25;
    in.lap_book["ZN.v.0"] = 1.0;
    in.notional_per_contract["ZN.v.0"] = 110703.125;
    in.covariance_symbols = {"ZF.v.0", "ZN.v.0", "ZT.v.0"};
    in.covariance = {
        {0.0010827015345464678, 0.001547553312231074, 0.0004891585310017923},
        {0.001547553312231074, 0.002342029481992849, 0.0006554148518359913},
        {0.0004891585310017923, 0.0006554148518359913, 0.000253237449971507},
    };
    return in;
}

}  // namespace

// 2025-06-25 (the day T-VOL section 3.2 walked): the gate asks 0.871352 (correlation); the lap-1
// book holds six contracts the held book does not (6L 2, M2K 2, MES +1, MYM +1, ZF 2, ZN 1). The
// cut removes the added MES lot and then one added ZF lot (each the nearest to the target in
// tracking error among the added contracts); no held contract is touched.
TEST(CutDelivery, WorkedDay20250625) {
    const CutDeliveryInput in = worked_2025_06_25();
    const CutDelivery d = deliver_cut_in_whole_contracts(in);
    ASSERT_EQ(d.removed.size(), 2u);
    EXPECT_EQ(d.removed[0].first, "MES.v.0");
    EXPECT_TRUE(d.removed[0].second);
    EXPECT_EQ(d.removed[1].first, "ZF.v.0");
    EXPECT_TRUE(d.removed[1].second);
    EXPECT_EQ(d.removed_new, 2);
    EXPECT_EQ(d.removed_held, 0);
    EXPECT_NEAR(d.lap_notional, 1026202.31, 0.005);
    EXPECT_NEAR(d.held_notional, 612914.56, 0.005);
    EXPECT_NEAR(d.target_notional, 894183.42, 0.005);
    EXPECT_NEAR(d.cut_notional, 886800.44, 0.005);
    for (const auto& [sym, q] : in.lap_book) {
        const double want = std::round(q) - (sym == "MES.v.0" || sym == "ZF.v.0" ? 1.0 : 0.0);
        EXPECT_EQ(d.book.at(sym), want) << sym;
    }
    EXPECT_LE(d.cut_notional, d.target_notional);
    EXPECT_DOUBLE_EQ(gross(in, d.book), d.cut_notional);
}

// 2025-02-27 (the ZT walk of CHURN_AND_BUFFER_ANALYSIS section 1c): nothing is held; the lap-1 book
// is ZF 2, ZN 1, ZT 1 and the gate asks 0.874438. The one contract whose removal leaves the book
// nearest the target is ZT's: the cut book is ZF 2, ZN 1, ZT 0, and it is the next lap's input,
// where the head's lap-2 deadband sent the whole request back to the held (flat) ZT.
TEST(CutDelivery, ZtWalk20250227) {
    const CutDeliveryInput in = zt_2025_02_27();
    const CutDelivery d = deliver_cut_in_whole_contracts(in);
    ASSERT_EQ(d.removed.size(), 1u);
    EXPECT_EQ(d.removed[0].first, "ZT.v.0");
    EXPECT_TRUE(d.removed[0].second);
    EXPECT_EQ(d.book.at("ZF.v.0"), 2.0);
    EXPECT_EQ(d.book.at("ZN.v.0"), 1.0);
    EXPECT_EQ(d.book.at("ZT.v.0"), 0.0);
    EXPECT_DOUBLE_EQ(d.lap_notional, 531734.375);  // the log prints %.2f: 531734.38
    EXPECT_NEAR(d.target_notional, 464968.71, 0.005);
    EXPECT_DOUBLE_EQ(d.cut_notional, 325578.125);  // the log prints %.2f: 325578.12
}

// Without a covariance the best fit on notional: the smallest contract that covers the excess.
TEST(CutDelivery, BestFitWithoutCovariance) {
    CutDeliveryInput in;
    in.factor = 0.9;
    in.capital = 1000.0;
    in.lap_book = {{"A", 2.0}, {"B", 1.0}, {"C", 1.0}};
    in.notional_per_contract = {{"A", 100.0}, {"B", 50.0}, {"C", 30.0}};
    const CutDelivery d = deliver_cut_in_whole_contracts(in);  // 280 -> target 252, excess 28
    ASSERT_EQ(d.removed.size(), 1u);
    EXPECT_EQ(d.removed[0].first, "C");
    EXPECT_EQ(d.book.at("A"), 2.0);
    EXPECT_EQ(d.book.at("B"), 1.0);
    EXPECT_EQ(d.book.at("C"), 0.0);
}

// No contract covers the excess: the largest goes first, then the best fit again.
TEST(CutDelivery, LargestWhenNothingCovers) {
    CutDeliveryInput in;
    in.factor = 0.4;
    in.capital = 1000.0;
    in.lap_book = {{"A", 1.0}, {"B", 1.0}};
    in.notional_per_contract = {{"A", 100.0}, {"B", 100.0}};
    const CutDelivery d = deliver_cut_in_whole_contracts(in);  // 200 -> 80
    EXPECT_EQ(d.book.at("A"), 0.0);
    EXPECT_EQ(d.book.at("B"), 0.0);
    EXPECT_EQ(d.cut_notional, 0.0);
}

// A contract the day added goes before a held one, even when the held one would fit tighter.
TEST(CutDelivery, AddedBeforeHeld) {
    CutDeliveryInput in;
    in.factor = 0.95;
    in.capital = 1000.0;
    in.lap_book = {{"A", 1.0}, {"B", 1.0}};
    in.held = {{"A", 1.0}};
    in.notional_per_contract = {{"A", 10.0}, {"B", 100.0}};
    const CutDelivery d = deliver_cut_in_whole_contracts(in);  // 110 -> 104.5
    ASSERT_EQ(d.removed.size(), 1u);
    EXPECT_EQ(d.removed[0].first, "B");
    EXPECT_TRUE(d.removed[0].second);
    EXPECT_EQ(d.removed_new, 1);
    EXPECT_EQ(d.removed_held, 0);
    EXPECT_EQ(d.book.at("A"), 1.0);
}

// With nothing added, held contracts are cut (a leverage cut on an unchanged book).
TEST(CutDelivery, HeldWhenNothingAdded) {
    CutDeliveryInput in;
    in.factor = 0.9;
    in.capital = 1000.0;
    in.lap_book = {{"A", 2.0}, {"B", 1.0}};
    in.held = {{"A", 2.0}, {"B", 1.0}};
    in.notional_per_contract = {{"A", 10.0}, {"B", 50.0}};
    const CutDelivery d = deliver_cut_in_whole_contracts(in);  // 70 -> 63
    ASSERT_EQ(d.removed.size(), 1u);
    EXPECT_EQ(d.removed[0].first, "A");
    EXPECT_FALSE(d.removed[0].second);
    EXPECT_EQ(d.removed_held, 1);
    EXPECT_EQ(d.book.at("A"), 1.0);
}

// Inside a class the covariance decides: the removal nearest the target in tracking error, here B,
// where the best fit on notional (a tie) would have taken A.
TEST(CutDelivery, TrackingErrorOrderInsideAClass) {
    CutDeliveryInput in;
    in.factor = 0.6;
    in.capital = 1000.0;
    in.lap_book = {{"A", 1.0}, {"B", 1.0}};
    in.notional_per_contract = {{"A", 100.0}, {"B", 100.0}};
    in.covariance_symbols = {"A", "B"};
    in.covariance = {{4.0, 0.0}, {0.0, 1.0}};
    const CutDelivery d = deliver_cut_in_whole_contracts(in);  // 200 -> 120
    ASSERT_EQ(d.removed.size(), 1u);
    EXPECT_EQ(d.removed[0].first, "B");
    EXPECT_EQ(d.book.at("A"), 1.0);
    EXPECT_EQ(d.book.at("B"), 0.0);
}

// A short is cut toward zero; contracts beyond the held short are the day's.
TEST(CutDelivery, ShortTowardZero) {
    CutDeliveryInput in;
    in.factor = 0.5;
    in.capital = 1000.0;
    in.lap_book = {{"A", -2.0}};
    in.held = {{"A", -1.0}};
    in.notional_per_contract = {{"A", 10.0}};
    const CutDelivery d = deliver_cut_in_whole_contracts(in);  // 20 -> 10
    EXPECT_EQ(d.book.at("A"), -1.0);
    EXPECT_EQ(d.removed_new, 1);
}

// A position without a notional is never cut and is counted; factor 1 cuts nothing.
TEST(CutDelivery, UnknownNotionalAndNoCut) {
    CutDeliveryInput in;
    in.factor = 1.0;
    in.capital = 1000.0;
    in.lap_book = {{"A", 2.0}, {"X", 3.0}};
    in.notional_per_contract = {{"A", 10.0}};
    const CutDelivery d = deliver_cut_in_whole_contracts(in);
    EXPECT_EQ(d.unknown_notional, 1);
    EXPECT_EQ(d.book.at("A"), 2.0);
    EXPECT_EQ(d.book.at("X"), 3.0);
    EXPECT_TRUE(d.removed.empty());
    in.factor = 0.1;
    const CutDelivery e = deliver_cut_in_whole_contracts(in);
    EXPECT_EQ(e.book.at("X"), 3.0);
    EXPECT_EQ(e.book.at("A"), 0.0);
}
