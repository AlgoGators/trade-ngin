// QT runner modes on both futures runners (docs/design/qt-contract.md). The behaviour is checked
// against a database by scripts/qt_runner_check.sh; these pin the wiring in the source, as the
// other twin-runner tests do.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "trade_ngin/live/qt_desk.hpp"

namespace {
namespace fs = std::filesystem;
constexpr auto npos = std::string::npos;
const char* kRunners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                          "apps/strategies/live_portfolio.cpp"};

std::string read_source(const std::string& relative) {
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) {
            std::ifstream in(dir / relative);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
        dir = dir.parent_path();
    }
    return std::string();
}
}  // namespace

TEST(QtRunnerModes, ModeNames) {
    using trade_ngin::qt::Mode;
    EXPECT_EQ(trade_ngin::qt::mode_name(Mode::FINALIZE_SYSTEM), "finalize-system");
    EXPECT_EQ(trade_ngin::qt::mode_name(Mode::PUBLISH), "publish");
}

// Ruling 17: each book is finalised into its own rows. The model run of an editable portfolio
// starts from qt and runs --finalize-system first, which reads system and writes Day T-1 only.
TEST(QtRunnerModes, TheSystemBooksDayTMinusOneIsFinalisedOnAnEditablePortfolio) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        EXPECT_NE(src.find("if (qt_mode == qt::Mode::MODEL && qt_read_book == \"qt\") {\n"
                           "            const int rc = qt::run_finalize_system("),
                  npos)
            << runner;
        EXPECT_NE(src.find("qt_mode == qt::Mode::MODEL || qt_mode == qt::Mode::FINALIZE_SYSTEM;"),
                  npos)
            << runner << ": --finalize-system writes Day T-1";
        EXPECT_NE(src.find("qt_mode != qt::Mode::PUBLISH && qt_mode != qt::Mode::FINALIZE_SYSTEM;"),
                  npos)
            << runner << ": --finalize-system writes no Day T row";
        EXPECT_NE(src.find("if (qt_desk_editable && qt_mode != qt::Mode::FINALIZE_SYSTEM) {"), npos)
            << runner << ": --finalize-system reads the system book";
    }
}

// Ruling 29: a non-trading day is published by the model run itself, with no e-mail.
TEST(QtRunnerModes, ANonTradingDayIsPublishedByTheModelRun) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const auto rule =
            src.find("const bool qt_non_trading_day = qt_day_kind == qt::DayKind::CALENDAR_CLOSED;");
        ASSERT_NE(rule, npos) << runner;
        EXPECT_NE(src.find("\"system:non-trading-day\"", rule), npos) << runner;
        EXPECT_NE(src.find("if (qt_desk_editable && qt_mode == qt::Mode::MODEL) send_email = false;"),
                  npos)
            << runner << ": ruling 15";
    }
}

// Ruling 16: a close is stored as a zero-quantity row in qt.
TEST(QtRunnerModes, ACloseIsStoredAsAZeroQuantityRow) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        EXPECT_NE(src.find("if (!has_quantity && qt_zero_keep.count(strategy_name + \"|\" + symbol) == 0) {"),
                  npos)
            << runner;
    }
}

// ===== QT hardening (2026-10-09) =====

namespace {
std::size_t at(const std::string& src, const std::string& needle, std::size_t from = 0) {
    return src.find(needle, from);
}
}  // namespace

// Contract C1: the snapshot's bytes are Python's json.dumps(list, separators=(",", ":"),
// ensure_ascii=True), and its hash their SHA-256. The vectors were computed in Python 3.
TEST(QtHardening, TheProposalSnapshotIsPythonsBytesAndHash) {
    using trade_ngin::qt::ProposalRow;
    using trade_ngin::qt::proposal_sha256;
    using trade_ngin::qt::proposal_snapshot_json;
    EXPECT_EQ(proposal_snapshot_json({}), "[]");
    EXPECT_EQ(proposal_sha256({}),
              "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945");

    const std::vector<ProposalRow> book = {{"TREND_FOLLOWING", "6A.v.0", -2},
                                           {"TREND_FOLLOWING", "MES.v.0", 0},
                                           {"TREND_FOLLOWING", "ZN.v.0", 15}};
    EXPECT_EQ(proposal_snapshot_json(book),
              "[{\"strategy_name\":\"TREND_FOLLOWING\",\"symbol\":\"6A.v.0\",\"quantity\":-2},"
              "{\"strategy_name\":\"TREND_FOLLOWING\",\"symbol\":\"MES.v.0\",\"quantity\":0},"
              "{\"strategy_name\":\"TREND_FOLLOWING\",\"symbol\":\"ZN.v.0\",\"quantity\":15}]");
    const std::string want = "11608a8e156036f756d6a99ce817e1d28daeaf19659efd66c9896d1be45c2979";
    EXPECT_EQ(proposal_sha256(book), want);
    // The rows are sorted by (strategy_name, symbol, quantity) before hashing.
    EXPECT_EQ(proposal_sha256({book[2], book[0], book[1]}), want);
    EXPECT_NE(proposal_sha256({{"TREND_FOLLOWING", "6A.v.0", -3}, book[1], book[2]}), want);
    // Two rows of one (strategy_name, symbol) (two strategy_ids): the quantity breaks the tie,
    // as AlgoLens sorts.
    EXPECT_EQ(proposal_sha256({{"TREND_FOLLOWING", "ZN.v.0", 15}, {"TREND_FOLLOWING", "ZN.v.0", -3}}),
              "ee0d6565378bb2576f729341065862c7044c90a0b09e353881d426bb2d798e3e");

    // A backslash is escaped as two.
    EXPECT_EQ(proposal_snapshot_json({{"a\\z", "x", 1}}),
              "[{\"strategy_name\":\"a\\\\z\",\"symbol\":\"x\",\"quantity\":1}]");
    // Escapes: " \b \n \t, a control character, DEL, a two-byte and a four-byte character.
    const std::vector<ProposalRow> odd = {
        {"S\"q\b\n\t\x01\x7f", "caf\xc3\xa9\xf0\x9f\x98\x80", -1234567890123LL}};
    EXPECT_EQ(proposal_snapshot_json(odd),
              "[{\"strategy_name\":\"S\\\"q\\b\\n\\t\\u0001\\u007f\",\"symbol\":\"caf\\u00e9"
              "\\ud83d\\ude00\",\"quantity\":-1234567890123}]");
    EXPECT_EQ(proposal_sha256(odd),
              "d9158f33474b39ca8aefe88a50d5dc5c7ea8196fbfda6126c81e86a1d320b941");
}

TEST(QtHardening, Sha256KnownVectors) {
    using trade_ngin::qt::sha256_hex;
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(sha256_hex(std::string(1000000, 'a')),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// Contract C5: only the calendar closes a day; a whole-book carry on a calendar trading day is a
// feed hole.
TEST(QtHardening, OnlyTheCalendarClosesADay) {
    using trade_ngin::qt::classify_qt_day;
    using trade_ngin::qt::DayKind;
    EXPECT_EQ(classify_qt_day(6, false, true), DayKind::CALENDAR_CLOSED);   // Saturday
    EXPECT_EQ(classify_qt_day(0, false, false), DayKind::CALENDAR_CLOSED);  // Sunday, MBT printed
    EXPECT_EQ(classify_qt_day(3, true, true), DayKind::CALENDAR_CLOSED);    // a holiday
    EXPECT_EQ(classify_qt_day(3, false, true), DayKind::FEED_HOLE);         // Wednesday, no prices
    EXPECT_EQ(classify_qt_day(1, false, false), DayKind::TRADING);
}

// Item 6 / C5 in the runners: a feed hole refuses the model run of an editable portfolio before
// anything is stored, and only a calendar closure is published by the model run.
TEST(QtHardening, AFeedHoleRefusesBeforeAnythingIsStored) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const auto kind = at(src, "const qt::DayKind qt_day_kind = qt::classify_qt_day(");
        const auto refuse = at(src, "qt_day_kind == qt::DayKind::FEED_HOLE) {", kind);
        const auto metadata =
            at(src, "INFO(\"Storing live run metadata for this trading day...\");");
        ASSERT_NE(kind, npos) << runner;
        ASSERT_NE(refuse, npos) << runner;
        EXPECT_LT(refuse, metadata) << runner << ": the refusal comes before the first store";
        EXPECT_LT(at(src, "return 1;", refuse), metadata) << runner;
        EXPECT_EQ(at(src, "skip_strategy_processing || qt_t1_wday"), npos)
            << runner << ": a carried day is no longer a non-trading day by itself";
    }
}

// Item 2 (review E#2): a desk or override run deletes every non-ROLL qt execution of the date and
// clears the completion marker before it stores its own executions.
TEST(QtHardening, ADeskRunReplacesTheQtDaysExecutions) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const auto clear = at(src, "auto cleared = qt::clear_desk_day(*db, portfolio_id, "
                                   "combined_strategy_id, qt_date);");
        ASSERT_NE(clear, npos) << runner;
        const auto guard =
            src.rfind("if (qt_mode == qt::Mode::DESK || qt_mode == qt::Mode::OVERRIDE) {", clear);
        ASSERT_NE(guard, npos) << runner;
        EXPECT_LT(clear - guard, 200u) << runner << ": desk and override only";
        EXPECT_LT(clear, at(src, "db->replace_roll_day_executions(")) << runner;
        EXPECT_LT(clear, at(src, "db->store_executions(executions,")) << runner;
        EXPECT_LT(clear, at(src, "db->store_positions(strategy_positions_vec,")) << runner;
    }
}

// Item 3: a failed store is never a done over a partial book.
TEST(QtHardening, AFailedStoreFailsTheRunAndTheCommandRow) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        for (const char* site : {"qt_store_failed(\"live_run_metadata\");",
                                 "qt_store_failed(\"executions (stale rows) of \" + strategy_name);",
                                 "qt_store_failed(\"executions of \" + strategy_name);",
                                 "qt_store_failed(\"Day T-1 positions of \" + strategy_name);",
                                 "qt_store_failed(\"positions of \" + strategy_name);",
                                 "qt_store_failed(\"Day T-1 live_results\");",
                                 "qt_store_failed(\"Day T-1 equity_curve\");",
                                 "qt_store_failed(\"Day T-1 live_results metrics\");",
                                 "qt_store_failed(\"live_results and equity_curve\");"}) {
            EXPECT_NE(at(src, site), npos) << runner << ": " << site;
        }
        // desk/override: the failure check comes before moved_by and the marker, and fails the row
        const auto check =
            at(src, "if (!qt_store_failures.empty()) {\n                return qt_fail(");
        ASSERT_NE(check, npos) << runner;
        const auto moved = at(src, "auto moved = qt::write_moved_by(");
        EXPECT_LT(check, moved) << runner;
        EXPECT_LT(moved, at(src, ": qt::mark_qt_day(")) << runner;
        // model: no seed from a partial system day; every mode exits non-zero
        EXPECT_NE(at(src, "} else if (save_result.is_error() || !qt_store_failures.empty()) {"),
                  npos)
            << runner;
        EXPECT_NE(at(src, "qt_store_failure_list() + \"); exiting 1\");"), npos) << runner;
        // publish: refused unless the day is complete and its last desk command finished
        const auto blocker = at(src, "auto blocker = qt::publish_blocker(*db, portfolio_id, "
                                     "combined_strategy_id, qt_date);");
        ASSERT_NE(blocker, npos) << runner;
        EXPECT_NE(at(src, "if (!blocker.value().empty()) return qt_refuse(blocker.value());",
                     blocker),
                  npos)
            << runner;
    }
}

// Item 4 (review E#3, E#11): publish builds its e-mail and CSV from the stored qt day.
TEST(QtHardening, PublishReadsTheStoredQtDay) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const auto load = at(src, "auto stored_fills = qt::load_book_executions(*db, portfolio_id, "
                                  "combined_strategy_id,");
        ASSERT_NE(load, npos) << runner;
        const auto export_call = at(src, "auto current_export_result =");
        EXPECT_LT(load, export_call) << runner;
        EXPECT_LT(at(src, "auto stored_day = qt::load_book_results(*db, portfolio_id, "
                          "combined_strategy_id, \"qt\","),
                  export_call)
            << runner;
        const auto positions = at(src, "strategy_positions_map[sleeve] = stored.value();");
        EXPECT_LT(positions, load) << runner;
        EXPECT_LT(at(src, "rebuild_combined_positions(positions, strategy_positions_map);",
                     positions),
                  load)
            << runner;
        EXPECT_NE(at(src, "stored_value(\"current_portfolio_value\", current_portfolio_value);"),
                  npos)
            << runner;
    }
}

// Item 5 / contract C3: a published day is frozen.
TEST(QtHardening, APublishedDayIsFrozen) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const auto frozen = at(src, "\"; a published day is frozen (contract C3)\");");
        ASSERT_NE(frozen, npos) << runner;
        EXPECT_LT(frozen, at(src, "auto approved = qt::check_override_decision(*db, *qt_audit);"))
            << runner << ": checked before the decision is read";
        EXPECT_NE(at(src, "return qt_refuse(qt_date + \" was already published at \" + "
                          "already.value());"),
                  npos)
            << runner << ": a second publish is refused";
        const auto kept =
            at(src, "\"; a model re-run never rewrites a published day's qt_proposal and qt\");");
        ASSERT_NE(kept, npos) << runner;
        EXPECT_LT(kept, at(src, "auto proposal = qt::copy_book_day(")) << runner;
    }
}

// Item 7: the publish e-mail is sent at most once.
TEST(QtHardening, ThePublishEmailIsSentOnce) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const auto sent =
            at(src, "qt_emailed = true;\n                        if (qt_mode == qt::Mode::PUBLISH) {");
        ASSERT_NE(sent, npos) << runner;
        EXPECT_LT(at(src, "auto marked = qt::mark_email_sent(*db, qt_audit_id);", sent) - sent, 300u)
            << runner << ": email_sent_at is written right after the send";
        const auto before = at(src, "auto sent = qt::publish_email_sent(*db, portfolio_id, qt_date);");
        ASSERT_NE(before, npos) << runner;
        const auto off = at(src, "send_email = false;", before);
        ASSERT_NE(off, npos) << runner;
        EXPECT_LT(off, at(src, "auto book = qt::load_book(*db, portfolio_id, combined_strategy_id, "
                               "\"qt\", qt_date);"))
            << runner;
        EXPECT_NE(at(src, "if (!qt_emailed && !qt_email_logged && !qt_sent_before) {"), npos)
            << runner;
    }
}
