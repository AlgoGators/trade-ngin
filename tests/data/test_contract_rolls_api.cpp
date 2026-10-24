// The per-contract roll API (contract_rolls.hpp) and its switch. The switch, portfolio.json's
// per_contract_rolls, is off in both templates, parsed strictly, and not written to the stored run
// metadata (AppConfig::to_json), so turning nothing on moves nothing. The API is checked on the clone
// (read only, TRADE_NGIN_TEST_DSN) for the GC example's outlook and the micros' "waiting on data".
#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <unistd.h>

#include "contract_rolls_test_util.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/data/contract_rolls.hpp"

using namespace trade_ngin;
using namespace trade_ngin::contract_rolls;
using contract_rolls_test::d;

namespace {

std::filesystem::path template_dir() {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / "config_template/defaults.json")) return dir / "config_template";
        dir = dir.parent_path();
    }
    return {};
}

/// A copy of config_template with the conservative portfolio.json's switch set to `value` (raw JSON
/// text; empty removes the key).
std::filesystem::path template_with_switch(const std::string& value) {
    namespace fs = std::filesystem;
    const fs::path src = template_dir();
    const fs::path dst = fs::temp_directory_path() / ("r2_switch_" + std::to_string(::getpid()));
    fs::remove_all(dst);
    fs::copy(src, dst, fs::copy_options::recursive);
    const fs::path pj = dst / "portfolios/conservative/portfolio.json";
    nlohmann::json j = nlohmann::json::parse(std::ifstream(pj));
    if (value.empty()) {
        j.erase("per_contract_rolls");
    } else {
        j["per_contract_rolls"] = nlohmann::json::parse(value);
    }
    std::ofstream(pj) << j.dump(2);
    return dst;
}

/// A copy of config_template with the conservative portfolio.json's `key` set to `value` (raw JSON
/// text; empty removes the key).
std::filesystem::path template_with(const std::string& key, const std::string& value) {
    namespace fs = std::filesystem;
    const fs::path src = template_dir();
    const fs::path dst = fs::temp_directory_path() / ("r4a_setting_" + std::to_string(::getpid()));
    fs::remove_all(dst);
    fs::copy(src, dst, fs::copy_options::recursive);
    const fs::path pj = dst / "portfolios/conservative/portfolio.json";
    nlohmann::json j = nlohmann::json::parse(std::ifstream(pj));
    if (value.empty()) {
        j.erase(key);
    } else {
        j[key] = nlohmann::json::parse(value);
    }
    std::ofstream(pj) << j.dump(2);
    return dst;
}

}  // namespace

// Rolls step 4, ruling 19: per_contract_switch_bar is "held" in both templates, "held" when absent,
// "vendor" when set so, refused otherwise, and not written to the stored run metadata.
TEST(PerContractRollsSwitch, TheSwitchBarSettingIsHeldInBothTemplatesAndParsedStrictly) {
    const auto dir = template_dir();
    ASSERT_FALSE(dir.empty()) << "config_template/ not found; this test must not skip";
    for (const char* book : {"base", "conservative"}) {
        std::ifstream in(dir / "portfolios" / book / "portfolio.json");
        const nlohmann::json j = nlohmann::json::parse(in);
        ASSERT_TRUE(j.contains("per_contract_switch_bar")) << book;
        EXPECT_EQ(j.at("per_contract_switch_bar"), "held") << book;
    }
    for (const auto& [value, expected] : std::vector<std::pair<std::string, std::string>>{
             {"\"vendor\"", "vendor"}, {"\"held\"", "held"}, {"", "held"}}) {
        const auto d = template_with("per_contract_switch_bar", value);
        auto loaded = ConfigLoader::load(d, "conservative");
        ASSERT_TRUE(loaded.is_ok()) << loaded.error()->what();
        EXPECT_EQ(loaded.value().per_contract_switch_bar, expected) << value;
        EXPECT_FALSE(loaded.value().to_json().contains("per_contract_switch_bar"));
        std::filesystem::remove_all(d);
    }
    for (const char* bad : {"\"target\"", "true", "1"}) {
        const auto d = template_with("per_contract_switch_bar", bad);
        auto loaded = ConfigLoader::load(d, "conservative");
        ASSERT_TRUE(loaded.is_error()) << bad;
        EXPECT_NE(std::string(loaded.error()->what()).find("per_contract_switch_bar"),
                  std::string::npos);
        std::filesystem::remove_all(d);
    }
}

TEST(PerContractRollsSwitch, OffInBothTemplatesAndNotInTheStoredConfig) {
    const auto dir = template_dir();
    ASSERT_FALSE(dir.empty()) << "config_template/ not found; this test must not skip";
    for (const char* book : {"base", "conservative"}) {
        std::ifstream in(dir / "portfolios" / book / "portfolio.json");
        const nlohmann::json j = nlohmann::json::parse(in);
        ASSERT_TRUE(j.contains("per_contract_rolls")) << book;
        EXPECT_EQ(j.at("per_contract_rolls"), false) << book;
    }
    auto loaded = ConfigLoader::load(dir, "conservative");
    ASSERT_TRUE(loaded.is_ok()) << loaded.error()->what();
    EXPECT_FALSE(loaded.value().per_contract_rolls);
    EXPECT_FALSE(loaded.value().to_json().contains("per_contract_rolls"));
}

TEST(PerContractRollsSwitch, ParsedTrueAbsentFalseAndRefusedWhenNotABoolean) {
    {
        const auto dir = template_with_switch("true");
        auto loaded = ConfigLoader::load(dir, "conservative");
        ASSERT_TRUE(loaded.is_ok()) << loaded.error()->what();
        EXPECT_TRUE(loaded.value().per_contract_rolls);
        EXPECT_FALSE(loaded.value().to_json().contains("per_contract_rolls"));
        std::filesystem::remove_all(dir);
    }
    {
        const auto dir = template_with_switch("");
        auto loaded = ConfigLoader::load(dir, "conservative");
        ASSERT_TRUE(loaded.is_ok()) << loaded.error()->what();
        EXPECT_FALSE(loaded.value().per_contract_rolls);
        std::filesystem::remove_all(dir);
    }
    {
        const auto dir = template_with_switch("\"yes\"");
        auto loaded = ConfigLoader::load(dir, "conservative");
        ASSERT_TRUE(loaded.is_error());
        EXPECT_NE(std::string(loaded.error()->what()).find("per_contract_rolls"), std::string::npos);
        std::filesystem::remove_all(dir);
    }
}

TEST(ContractRollsApi, OutlookGivesTheDeadlineWindowLastActionableBarAndTarget) {
    // GCQ6 held, GCU6 (off the cycle) and GCZ6 print; on 2026-07-27 the outlook is GCQ6's dates and
    // the target GCZ6.
    std::vector<ContractBar> rows;
    std::vector<VendorBar> grid;
    for (const char* dt : {"2026-07-24", "2026-07-26", "2026-07-27", "2026-07-28"}) {
        rows.push_back(contract_rolls_test::bar("GC", "GCQ6", "q", dt, "2026-08-27", 3300, 80000));
        rows.push_back(contract_rolls_test::bar("GC", "GCU6", "u", dt, "2026-09-28", 3310, 50000));
        if (std::string(dt) != "2026-07-28") {
            rows.push_back(contract_rolls_test::bar("GC", "GCZ6", "z", dt, "2026-12-29", 3350, 9000));
        }
        rows.push_back(contract_rolls_test::bar("GC", "GCG7", "g", dt, "2027-02-24", 3370, 100));
        VendorBar v;
        v.date = d(dt);
        v.symbol = "GC.v.0";
        v.instrument_id = "q";
        grid.push_back(v);
    }
    auto book = std::make_shared<ContractBook>(ContractBook::build(rows).value());
    const TradingCalendar cal = contract_rolls_test::shipped_calendar();
    RuleInputs in;
    in.root = "GC";
    in.grid = grid;
    in.book = book.get();
    in.cal = &cal;
    in.cycle = "GJMQZ";
    auto held = apply_roll_rule(in);
    ASSERT_TRUE(held.is_ok());
    const ContractRollSeries series(held.value(), book, "GJMQZ", std::nullopt, cal);
    EXPECT_EQ(series.days().size(), 4u);
    EXPECT_EQ(series.report().rolls, 0);

    auto o = series.outlook(d("2026-07-27"));
    ASSERT_TRUE(o.is_ok()) << o.error()->what();
    EXPECT_EQ(o.value().held.raw_symbol, "GCQ6");
    EXPECT_EQ(ymd(o.value().dates.deadline), "2026-07-31");
    EXPECT_EQ(ymd(o.value().dates.window_first), "2026-07-24");
    EXPECT_EQ(ymd(o.value().dates.last_actionable), "2026-07-29");
    ASSERT_TRUE(o.value().target.has_value());
    EXPECT_EQ(o.value().target->raw_symbol, "GCZ6");
    EXPECT_TRUE(o.value().target_is_cycle_front);
    EXPECT_EQ(o.value().cycle_front_symbol, "GCZ6");
    // A date between grid bars reads the latest bar on or before it.
    EXPECT_EQ(series.outlook(d("2026-07-25")).value().date, d("2026-07-25"));
    // On 07-28 the front has no bar: the target is the next cycle contract, flagged.
    auto o28 = series.outlook(d("2026-07-28"));
    ASSERT_TRUE(o28.is_ok());
    EXPECT_EQ(o28.value().target->raw_symbol, "GCG7");
    EXPECT_FALSE(o28.value().target_is_cycle_front);
    EXPECT_EQ(o28.value().cycle_front_symbol, "GCZ6");
    // Before the first date: refused.
    EXPECT_TRUE(series.outlook(d("2026-07-23")).is_error());
}

class ContractRollsApiDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string c = contract_rolls_test::dsn();
        if (c.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        try {
            conn_ = std::make_unique<pqxx::connection>(c);
            pqxx::read_transaction t(*conn_);
            const auto r = t.exec(
                "SELECT count(*) FILTER (WHERE expiration IS NULL), count(*) FROM "
                "futures_data.contract_ohlcv_1d WHERE root IN ('GC', 'MES')");
            if (r[0][1].as<int64_t>() == 0 || r[0][0].as<int64_t>() > 0) {
                GTEST_SKIP() << "no contract bars with an expiration";
            }
        } catch (const std::exception& e) {
            GTEST_SKIP() << "database unreachable: " << e.what();
        }
    }
    std::unique_ptr<pqxx::connection> conn_;
};

TEST_F(ContractRollsApiDbTest, BuildsTheGcSeriesAndItsOutlook) {
    pqxx::read_transaction t(*conn_);
    const FuturesCalendar& cal = contract_rolls_test::shipped_calendars();
    auto s = build_held_series(t, "GC", d("2026-07-01"), d("2026-08-14"), cal);
    ASSERT_TRUE(s.is_ok()) << s.error()->what();
    int rolls = 0;
    for (const auto& day : s.value().days()) {
        if (day.roll) {
            ++rolls;
            EXPECT_EQ(ymd(day.date), "2026-07-29");
            EXPECT_EQ(day.trigger, RollTrigger::CROSSOVER);
            ASSERT_TRUE(day.old_close.has_value());
        }
    }
    EXPECT_EQ(rolls, 1);
    auto o = s.value().outlook(d("2026-07-27"));
    ASSERT_TRUE(o.is_ok()) << o.error()->what();
    EXPECT_EQ(o.value().held.raw_symbol, "GCQ6");
    EXPECT_EQ(o.value().target->raw_symbol, "GCZ6");
    EXPECT_EQ(ymd(o.value().dates.last_actionable), "2026-07-29");
    auto start = series_start(t, "GC");
    ASSERT_TRUE(start.is_ok());
    EXPECT_LE(start.value(), d("2010-06-08"));
    EXPECT_TRUE(build_held_series(t, "XX", d("2026-07-01"), d("2026-07-02"), cal).is_error());
}

TEST_F(ContractRollsApiDbTest, TheMicrosBeforeTheChangeAreWaitingOnData) {
    pqxx::read_transaction t(*conn_);
    const FuturesCalendar& cal = contract_rolls_test::shipped_calendars();
    auto before = build_held_series(t, "MES", d("2026-01-05"), d("2026-03-06"), cal);
    if (before.is_ok()) GTEST_SKIP() << "ES contracts are loaded on this database";
    EXPECT_EQ(before.error()->code(), ErrorCode::DATA_NOT_FOUND);
    EXPECT_NE(std::string(before.error()->what()).find("waiting on data"), std::string::npos);
    auto start = series_start(t, "MES");
    ASSERT_TRUE(start.is_ok());
    EXPECT_EQ(ymd(start.value()), "2026-02-22");
    auto from_change = build_held_series(t, "MES", start.value(), d("2026-10-02"), cal);
    ASSERT_TRUE(from_change.is_ok()) << from_change.error()->what();
    EXPECT_EQ(from_change.value().days().front().held.raw_symbol, "MESH6");
}
